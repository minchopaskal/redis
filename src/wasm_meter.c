/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#include "wasm_meter.h"
#include "wasm_meter_plan.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* MVP numeric instructions, sign extension, saturating conversions and bulk
 * memory only. Unknown opcodes fail closed. Original function/local/global
 * indices are unchanged: private state is appended and updates are stack-neutral.
 * Keep remaining fuel in an appended i64 local in every function. Debit each
 * executed straight-line segment once, just before its terminating control
 * instruction. Plain blocks and equal-cost linear if/else arms need no separate
 * debit. Publish before ordinary calls and exits, and reload after calls so
 * callees spend the same shared budget. Direct calls to fixed-cost linear
 * leaves instead use private, uninstrumented clones. The caller checks entry,
 * then charges the leaf's exact cost after a successful return, keeping fuel
 * local across the call. Original functions remain metered for other entries.
 * Extend this to straight-line acyclic direct-call trees only when remaining
 * fuel covers their full transitive cost; otherwise call the metered original.
 * This guard guarantees every skipped nested entry check would have passed.
 * Typed if parameters/results preserve call arguments without extra locals.
 * An unconditional loop backedge doubles as its fuel check: branch back while
 * spent fuel is within budget, else take the cold exit.
 * For a loop with a forward-exit header and a constant-cost body containing
 * equal-cost if/else arms, prepay the header at initial entry and the backedge.
 * This removes its separate fuel-local update across the body's control-flow
 * joins. Exclude that prepaid cost from the backedge check, retaining the
 * original exhaustion timing and exact accounting after the header executes.
 * Keep original integer comparisons adjacent to if/br_if by debiting before
 * the comparison; these pure instructions commute with the stack-neutral
 * debit, preserving FastJIT's compare-and-branch fusion.
 * Check fuel at function entry, loop backedges, imported/indirect calls and
 * explicit traps. Direct guest calls rely on the callee's entry check. Returns
 * publish even a negative balance: finite guest tails may finish past budget,
 * but the next entry, backedge or host call detects the debt. Indirect calls
 * stay guarded because their tables may contain imported host functions.
 * Every cycle crosses an entry or backedge check. Negative debt can unwind
 * existing frames, each with at most one module-sized acyclic tail. Redis's
 * 64 KiB WAMR stack and 16 MiB input cap bound this by 2^40 units, even allowing
 * one frame per stack byte, plus one leaf and one prepaid header. Starting fuel
 * <= INT64_MAX therefore cannot underflow INT64_MIN before a check or a finite return.
 * Ordinary guest traps, including traps in specialized leaves, may leave a
 * cached counter unpublished; Redis resets it before the next invocation and
 * only inspects the exhausted flag. Moving a leaf's entry gate to the caller
 * can change its ordering relative to native stack-overflow checks.
 * A void block around each function provides one cold exhaustion exit. br_if
 * lets FastJIT retain cached values on fallthrough. Original branches to the
 * function label skip the added block; inner labels keep their indices.
 * A unit is one original opcode, including control delimiters when reached by
 * fallthrough. Bulk operations cost one; fuel bounds guest instructions, not
 * time spent inside a Redis host command. */
#define MAX_OUTPUT (64U * 1024U * 1024U)
typedef struct { const uint8_t *p, *end; int bad; } reader;
typedef struct { uint8_t *p; size_t len, cap; int bad; } buffer;
typedef struct { const uint8_t *p; size_t len; unsigned id; } section;
typedef struct { uint32_t cost, clone, calls; } fixed_call;
typedef struct {
    const uint8_t *input;
    uint32_t *params, *functions;
    fixed_call *fixed;
    uint32_t type_count, function_count, import_functions, clone_count;
} signatures;
typedef struct { uint32_t global, local; } meter;

static unsigned byte(reader *r) {
    if (r->p == r->end) { r->bad = 1; return 0; }
    return *r->p++;
}
static uint32_t u32(reader *r) {
    uint32_t n = 0;
    for (unsigned i = 0; i < 5; i++) {
        unsigned b = byte(r);
        if (i == 4 && (b & 0xf0)) { r->bad = 1; return 0; }
        n |= (uint32_t)(b & 127) << (7 * i);
        if (!(b & 128)) return n;
    }
    r->bad = 1;
    return 0;
}
static void skip(reader *r, size_t n) {
    if (n > (size_t)(r->end - r->p)) r->bad = 1;
    else r->p += n;
}
static void signedLeb(reader *r, unsigned bits) {
    for (unsigned i = 0; i < (bits + 6) / 7; i++) {
        unsigned b = byte(r);
        if (!(b & 128)) {
            if (i == bits / 7) {
                unsigned mask = 0x7f & ~((1U << (bits % 7)) - 1);
                unsigned sign = 1U << ((bits - 1) % 7);
                if ((b & mask) != ((b & sign) ? mask : 0)) r->bad = 1;
            }
            return;
        }
    }
    r->bad = 1;
}
static void put(buffer *b, const void *p, size_t len) {
    if (b->bad) return;
    if (len > MAX_OUTPUT - b->len) { b->bad = 1; return; }
    size_t need = b->len + len;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need) cap *= 2;
        void *next = realloc(b->p, cap);
        if (!next) { b->bad = 1; return; }
        b->p = next; b->cap = cap;
    }
    if (len) memcpy(b->p + b->len, p, len);
    b->len = need;
}
static void emit(buffer *b, unsigned v) { uint8_t c = v; put(b, &c, 1); }
static void leb(buffer *b, uint64_t v, int signed_positive) {
    do {
        unsigned c = v & 127;
        v >>= 7;
        if (v || (signed_positive && (c & 64))) emit(b, c | 128);
        else { emit(b, c); break; }
    } while (1);
}
static void name(buffer *b, const char *s) { leb(b, strlen(s), 0); put(b, s, strlen(s)); }
/* Negative constants are only used for a bounded prepaid loop prefix. */
static void negativeLeb(buffer *b, uint32_t magnitude) {
    int64_t value = -(int64_t)magnitude;
    do {
        unsigned c = (uint64_t)value & 127;
        /* Division with floor rounding avoids relying on signed right shift. */
        int64_t next = value / 128;
        if (value % 128) next--;
        if (next == -1 && (c & 64)) { emit(b, c); break; }
        emit(b, c | 128);
        value = next;
    } while (1);
}
static void emitSection(buffer *out, unsigned id, buffer *payload) {
    if (payload->bad) { out->bad = 1; return; }
    emit(out, id); leb(out, payload->len, 0); put(out, payload->p, payload->len);
}
/* check: 0 only debits; 1 branches to exhaustion; -1 branches back to a loop
 * while fuel remains. The latter fuses the original backedge with the check. */
static void charge(buffer *b, const meter *m, uint32_t cost,
                   int check, unsigned branch_depth, uint32_t prepaid) {
    emit(b, 0x20); leb(b, m->local, 0);    /* local.get fuel */
    emit(b, 0x42); leb(b, cost, 1);        /* i64.const cost */
    emit(b, 0x7d);                        /* i64.sub */
    emit(b, check ? 0x22 : 0x21);         /* local.tee or local.set fuel */
    leb(b, m->local, 0);
    if (check) {
        emit(b, 0x42);                    /* i64.const -prepaid */
        if (prepaid) negativeLeb(b, prepaid);
        else emit(b, 0);
        emit(b, check > 0 ? 0x53 : 0x59); /* i64.lt_s or i64.ge_s */
        emit(b, 0x0d); leb(b, branch_depth, 0); /* br_if exhaustion or loop */
    }
}
static void publish(buffer *b, const meter *m) {
    emit(b, 0x20); leb(b, m->local, 0);    /* local.get fuel */
    emit(b, 0x24); leb(b, m->global, 0);   /* global.set fuel */
}
static void reload(buffer *b, const meter *m) {
    emit(b, 0x23); leb(b, m->global, 0);   /* global.get fuel */
    emit(b, 0x21); leb(b, m->local, 0);    /* local.set fuel */
}
/* Every entered function has at least one chargeable original instruction.
 * The same gate precedes calls to private leaves; do not require their entire
 * cost up front, because a finite tail may return a negative balance. */
static void entryCheck(buffer *b, const meter *m, unsigned depth) {
    emit(b, 0x20); leb(b, m->local, 0); /* local.get fuel */
    emit(b, 0x42); emit(b, 0);         /* i64.const 0 */
    emit(b, 0x57);                    /* i64.le_s */
    emit(b, 0x0d); leb(b, depth, 0);  /* br_if exhaustion */
}

static void types(reader *r, signatures *s) {
    s->type_count = u32(r);
    if (r->bad || s->type_count > (size_t)(r->end - r->p) / 3) { r->bad = 1; return; }
    s->params = calloc(s->type_count, sizeof(*s->params));
    if (s->type_count && !s->params) { r->bad = 1; return; }
    for (uint32_t i = 0; i < s->type_count && !r->bad; i++) {
        if (byte(r) != 0x60) { r->bad = 1; break; }
        s->params[i] = u32(r); skip(r, s->params[i]);
        uint32_t results = u32(r); skip(r, results);
    }
    if (r->p != r->end) r->bad = 1;
}
static void functions(reader *r, signatures *s) {
    s->function_count = u32(r);
    if (r->bad || s->function_count > (size_t)(r->end - r->p)) { r->bad = 1; return; }
    s->functions = calloc(s->function_count, sizeof(*s->functions));
    if (s->function_count && !s->functions) { r->bad = 1; return; }
    for (uint32_t i = 0; i < s->function_count && !r->bad; i++) {
        s->functions[i] = u32(r);
        if (s->functions[i] >= s->type_count) r->bad = 1;
    }
    if (r->p != r->end) r->bad = 1;
}

static void limits(reader *r) {
    unsigned flags = u32(r);
    if (flags > 1) r->bad = 1; /* no memory64/shared/custom page sizes */
    u32(r);
    if (flags & 1) u32(r);
}
static uint32_t importedGlobals(reader *r, signatures *s) {
    uint32_t count = u32(r), globals = 0;
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t n = u32(r); skip(r, n);
        n = u32(r); skip(r, n);
        switch (byte(r)) {
        case 0: u32(r); s->import_functions++; break;
        case 1: byte(r); limits(r); break;
        case 2: limits(r); break;
        case 3: byte(r); byte(r); globals++; break;
        default: r->bad = 1;
        }
    }
    if (r->p != r->end) r->bad = 1;
    return globals;
}
static int reservedExports(reader *r) {
    uint32_t count = u32(r);
    const char *prefix = "__redis_meter_";
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t n = u32(r);
        const uint8_t *p = r->p;
        skip(r, n);
        if (!r->bad && n >= strlen(prefix) && !memcmp(p, prefix, strlen(prefix))) return 1;
        byte(r); u32(r);
    }
    if (r->p != r->end) r->bad = 1;
    return 0;
}

/* Decode immediates without interpreting them; WAMR checks their semantics.
 * Return whether this opcode ends a segment. */
static int instruction(reader *r, unsigned op) {
    switch (op) {
    case 0x00: case 0x05: case 0x0b: case 0x0f: return 1;
    case 0x02: signedLeb(r, 33); return 0;
    case 0x03: case 0x04: signedLeb(r, 33); return 1;
    case 0x0c: case 0x0d: case 0x10: u32(r); return 1;
    case 0x0e: {
        uint32_t count = u32(r);
        if (count >= (size_t)(r->end - r->p)) { r->bad = 1; return 1; }
        for (uint32_t i = 0; i <= count && !r->bad; i++) u32(r);
        return 1;
    }
    case 0x11: u32(r); u32(r); return 1;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x3f: case 0x40: u32(r); return 0;
    case 0x41: signedLeb(r, 32); return 0;
    case 0x42: signedLeb(r, 64); return 0;
    case 0x43: skip(r, 4); return 0;
    case 0x44: skip(r, 8); return 0;
    case 0xfc: {
        unsigned sub = u32(r);
        if (sub <= 7) return 0; /* saturating conversions */
        if (sub == 8 || sub == 10) { u32(r); u32(r); return 0; }
        if (sub == 9 || sub == 11) { u32(r); return 0; }
        r->bad = 1; return 0;
    }
    default:
        if (op >= 0x28 && op <= 0x3e) { u32(r); u32(r); return 0; }
        if (op == 0x01 || op == 0x1a || op == 0x1b || (op >= 0x45 && op <= 0xc4)) return 0;
        r->bad = 1; return 0;
    }
}
/* Resolve fixed-cost functions bottom-up over reverse direct-call edges. Each
 * body and edge is visited a bounded number of times: no recursive C traversal
 * or repeated whole-graph scan. Unresolved cycles and their callers remain
 * ineligible, as do callers of host/control-flow/otherwise ineligible functions.
 * A shared subgraph can yield an exponentially large execution cost, so reject
 * totals above UINT32_MAX rather than wrapping or changing ordinary metering.
 * Ordinary memory/arithmetic traps retain deferred debit semantics. Validate
 * every original direct target before adding clones; other original indices
 * still require the caller's full Wasm validation. */
static void prepareCalls(reader *r, signatures *s) {
    typedef struct { uint32_t pending, first; } call_node;
    typedef struct { uint32_t caller, next; } call_edge;
    uint32_t count = u32(r);
    uint64_t original_count = (uint64_t)s->import_functions + count;
    if (r->bad || count != s->function_count || original_count > UINT32_MAX ||
        count > (size_t)(r->end - r->p) / 3) { r->bad = 1; return; }
    s->fixed = calloc(count, sizeof(*s->fixed));
    call_node *nodes = calloc(count, sizeof(*nodes));
    uint32_t *queue = calloc(count, sizeof(*queue));
    buffer edges = {0};
    if (count && (!s->fixed || !nodes || !queue)) { r->bad = 1; goto done; }
    for (uint32_t i = 0; i < count; i++) nodes[i].first = UINT32_MAX;
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t len = u32(r);
        const uint8_t *start = r->p;
        skip(r, len);
        if (r->bad) break;
        reader f = {start, start + len, 0};
        uint32_t locals = u32(&f);
        for (uint32_t j = 0; j < locals && !f.bad; j++) { u32(&f); byte(&f); }
        uint32_t cost = 0;
        unsigned last = 0;
        int linear = 1;
        while (f.p < f.end && !f.bad) {
            const uint8_t *opcode = f.p;
            unsigned op = byte(&f);
            int boundary = instruction(&f, op);
            cost++;
            if (op == 0x02 || (boundary && op != 0x10 &&
                              !(op == 0x0b && f.p == f.end)))
                linear = 0;
            if (op == 0x10) {
                reader target = {opcode + 1, f.p, 0};
                uint32_t index = u32(&target);
                if (target.bad || index >= original_count) { f.bad = 1; break; }
                if (index < s->import_functions) linear = 0;
                else {
                    uint32_t callee = index - s->import_functions;
                    s->fixed[callee].clone = 1; /* referenced by an original call */
                    s->fixed[i].calls++;
                    call_edge edge = {i, nodes[callee].first};
                    uint32_t edge_index = (uint32_t)(edges.len / sizeof(edge));
                    put(&edges, &edge, sizeof(edge));
                    if (edges.bad) { f.bad = 1; break; }
                    nodes[callee].first = edge_index;
                }
            }
            last = op;
        }
        if (f.bad || last != 0x0b) { r->bad = 1; break; }
        if (linear) s->fixed[i].cost = cost;
        nodes[i].pending = s->fixed[i].calls;
    }
    if (r->p != r->end) r->bad = 1;
    if (r->bad) goto done;
    uint32_t next = 0, queued = 0;
    for (uint32_t i = 0; i < count; i++)
        if (s->fixed[i].cost && !nodes[i].pending) queue[queued++] = i;
    const call_edge *links = (const call_edge *)edges.p;
    while (next < queued) {
        uint32_t callee = queue[next++];
        for (uint32_t edge = nodes[callee].first; edge != UINT32_MAX;
             edge = links[edge].next) {
            uint32_t caller = links[edge].caller;
            fixed_call *parent = &s->fixed[caller];
            if (!parent->cost) continue;
            if (s->fixed[callee].cost > UINT32_MAX - parent->cost) {
                parent->cost = 0; /* overflowed parents must not resolve */
                continue;
            }
            parent->cost += s->fixed[callee].cost;
            if (!--nodes[caller].pending) queue[queued++] = caller;
        }
    }
    for (uint32_t i = 0; i < count; i++)
        if (nodes[i].pending || !s->fixed[i].cost) s->fixed[i].clone = 0;
    /* Preserve all prior leaf-clone indices; append call trees after them. */
    for (unsigned tree = 0; tree < 2 && !r->bad; tree++) {
        for (uint32_t i = 0; i < count; i++) {
            fixed_call *callee = &s->fixed[i];
            if (!callee->clone || (callee->calls != 0) != tree) continue;
            uint64_t index = original_count + s->clone_count;
            if (index >= UINT32_MAX) { r->bad = 1; break; }
            callee->clone = (uint32_t)index;
            s->clone_count++;
        }
    }
done:
    free(edges.p); free(queue); free(nodes);
}
static void functionSection(buffer *out, const section *original, const signatures *s) {
    buffer b = {0};
    reader r = {original->p, original->p + original->len, 0};
    u32(&r);
    leb(&b, (uint64_t)s->function_count + s->clone_count, 0);
    put(&b, r.p, r.end - r.p);
    for (unsigned tree = 0; tree < 2; tree++)
        for (uint32_t i = 0; i < s->function_count; i++)
            if (s->fixed[i].clone && (s->fixed[i].calls != 0) == tree)
                leb(&b, s->functions[i], 0);
    emitSection(out, 3, &b);
    free(b.p);
}
#include "wasm_meter_body.h"

static void code(reader *r, buffer *out, uint32_t global, const signatures *s) {
    uint32_t count = u32(r);
    if (count != s->function_count) { r->bad = 1; return; }
    leb(out, (uint64_t)count + s->clone_count, 0);
    reader originals = *r;
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t len = u32(r);
        const uint8_t *start = r->p;
        skip(r, len);
        if (r->bad) break;
        reader f = {start, start + len, 0};
        buffer b = {0};
        plannedBody(&f, &b, global, i, s, 0);
        if (f.bad || b.bad) r->bad = 1;
        else { leb(out, b.len, 0); put(out, b.p, b.len); }
        free(b.p);
    }
    if (r->p != r->end) r->bad = 1;
    /* Keep existing leaf bodies/indices verbatim, then append tree clones. */
    for (unsigned tree = 0; tree < 2 && !r->bad; tree++) {
        reader copies = originals;
        for (uint32_t i = 0; i < count && !r->bad; i++) {
            uint32_t len = u32(&copies);
            const uint8_t *start = copies.p;
            skip(&copies, len);
            if (copies.bad) { r->bad = 1; break; }
            if (!s->fixed[i].clone || (s->fixed[i].calls != 0) != tree) continue;
            reader f = {start, start + len, 0};
            buffer b = {0};
            plannedBody(&f, &b, global, i, s, tree ? 2 : 1);
            if (f.bad || b.bad) r->bad = 1;
            else { leb(out, b.len, 0); put(out, b.p, b.len); }
            free(b.p);
        }
    }
}
static void globals(buffer *out, const section *s, uint64_t fuel) {
    buffer b = {0};
    uint32_t count = 0;
    reader r = {NULL, NULL, 0};
    if (s) { r.p = s->p; r.end = s->p + s->len; count = u32(&r); }
    if (count > UINT32_MAX - 2 || r.bad) { out->bad = 1; return; }
    leb(&b, count + 2, 0);
    if (s) put(&b, r.p, r.end - r.p);
    emit(&b, 0x7e); emit(&b, 1); emit(&b, 0x42); leb(&b, fuel, 1); emit(&b, 0x0b);
    emit(&b, 0x7f); emit(&b, 1); emit(&b, 0x41); emit(&b, 0); emit(&b, 0x0b);
    emitSection(out, 6, &b); free(b.p);
}
static void exports(buffer *out, const section *s, uint32_t index) {
    buffer b = {0};
    uint32_t count = 0;
    reader r = {NULL, NULL, 0};
    if (s) { r.p = s->p; r.end = s->p + s->len; count = u32(&r); }
    if (count > UINT32_MAX - 2 || r.bad) { out->bad = 1; return; }
    leb(&b, count + 2, 0);
    if (s) put(&b, r.p, r.end - r.p);
    name(&b, WASM_METER_FUEL); emit(&b, 3); leb(&b, index, 0);
    name(&b, WASM_METER_EXHAUSTED); emit(&b, 3); leb(&b, index + 1, 0);
    emitSection(out, 7, &b); free(b.p);
}

int wasmMeterInject(const uint8_t *input, size_t length, uint64_t fuel,
                    uint8_t **output, size_t *output_length,
                    char *error, size_t error_length) {
    *output = NULL; *output_length = 0;
    const char *failure = "Invalid or unsupported Wasm for instruction metering";
    buffer result = {0};
    signatures sig = {.input = input};
    section original_code = {0};
    if (length < 8 || length > WASM_METER_MAX_INPUT || fuel > INT64_MAX ||
        memcmp(input, "\0asm\1\0\0\0", 8)) goto fail;
    reader r = {input + 8, input + length, 0};
    uint32_t imported = 0, defined = 0;
    unsigned seen = 0;
    /* First pass checks lengths/names and computes the original global space. */
    while (r.p < r.end && !r.bad) {
        unsigned id = byte(&r);
        uint32_t len = u32(&r);
        const uint8_t *p = r.p;
        skip(&r, len);
        if (r.bad || id > 12 || (id && (seen & (1U << id)))) goto fail;
        seen |= 1U << id;
        reader s = {p, p + len, 0};
        if (id == 1) types(&s, &sig);
        if (id == 2) imported = importedGlobals(&s, &sig);
        if (id == 3) functions(&s, &sig);
        if (id == 6) defined = u32(&s);
        if (id == 7 && reservedExports(&s)) {
            failure = "Reserved __redis_meter_ export name"; goto fail;
        }
        if (id == 10) original_code = (section){p, len, id};
        if (s.bad) goto fail;
    }
    if (r.bad || (uint64_t)imported + defined > UINT32_MAX - 2) goto fail;
    if (original_code.p) {
        reader c = {original_code.p, original_code.p + original_code.len, 0};
        prepareCalls(&c, &sig);
        if (c.bad) goto fail;
    } else if (sig.function_count) goto fail;
    uint32_t index = imported + defined;
    int have_globals = 0, have_exports = 0;
    put(&result, input, 8);
    r.p = input + 8;
    while (r.p < r.end && !result.bad) {
        unsigned id = byte(&r);
        uint32_t len = u32(&r);
        section s = {r.p, len, id};
        skip(&r, len);
        if (id >= 7 && !have_globals) { globals(&result, NULL, fuel); have_globals = 1; }
        if (id >= 8 && !have_exports) { exports(&result, NULL, index); have_exports = 1; }
        if (id == 3 && sig.clone_count) functionSection(&result, &s, &sig);
        else if (id == 6) { globals(&result, &s, fuel); have_globals = 1; }
        else if (id == 7) { exports(&result, &s, index); have_exports = 1; }
        else if (id == 10) {
            reader c = {s.p, s.p + s.len, 0};
            buffer b = {0};
            code(&c, &b, index, &sig);
            if (c.bad) result.bad = 1;
            else emitSection(&result, 10, &b);
            free(b.p);
        } else { emit(&result, id); leb(&result, len, 0); put(&result, s.p, s.len); }
    }
    if (!have_globals) globals(&result, NULL, fuel);
    if (!have_exports) exports(&result, NULL, index);
    if (result.bad) goto fail;
    free(sig.params); free(sig.functions); free(sig.fixed);
    *output = result.p; *output_length = result.len;
    return 1;
fail:
    free(sig.params); free(sig.functions); free(sig.fixed);
    free(result.p);
    if (error_length) snprintf(error, error_length, "%s", failure);
    return 0;
}
