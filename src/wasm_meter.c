/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#include "wasm_meter.h"
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

static void branchTarget(reader *r, buffer *out, unsigned depth) {
    uint32_t target = u32(r);
    if (target >= depth) r->bad = 1;
    /* The extra block sits immediately inside the original function label. */
    if (target == depth - 1) target++;
    leb(out, target, 0);
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
/* A linear if/else with equally costly arms has a constant cost regardless
 * of its condition. Fold the whole diamond into the surrounding segment,
 * avoiding fuel-local spills at the if and its merge. Reject nested control,
 * calls and explicit traps; ordinary trapping instructions retain the same
 * deferred-accounting behavior as any other straight-line segment. */
static int equalIf(reader *r, uint32_t *cost) {
    reader scan = *r;
    uint32_t then_cost = 0, arm_cost = 0;
    int have_else = 0;
    while (scan.p < scan.end && !scan.bad) {
        unsigned op = byte(&scan);
        arm_cost++;
        if (op == 0x05) {
            if (have_else) return 0;
            have_else = 1;
            then_cost = arm_cost;
            arm_cost = 0;
        } else if (op == 0x0b) {
            if (!have_else || then_cost != arm_cost) return 0;
            *r = scan;
            *cost = arm_cost; /* one arm plus its else/end delimiter */
            return 1;
        } else if (op == 0x02 || instruction(&scan, op)) return 0;
    }
    return 0;
}
/* Append fuel after all original parameters and locals. */
static int analyze(reader r, uint32_t params, meter *m) {
    uint32_t count = u32(&r);
    uint64_t locals = params;
    for (uint32_t i = 0; i < count && !r.bad; i++) { locals += u32(&r); byte(&r); }
    if (r.bad || locals >= UINT32_MAX || count == UINT32_MAX) return 0;
    m->local = (uint32_t)locals;
    return 1;
}
/* Check every possible loop backedge, including br_if fallthrough and mixed
 * br_table targets. Function-label exits still publish the local balance. */
static int branchCheck(reader r, unsigned op, unsigned depth,
                       const unsigned *labels, int *exits) {
    uint32_t n = op == 0x0e ? u32(&r) : 0;
    int check = 0;
    *exits = 0;
    for (uint32_t i = 0; i <= n && !r.bad; i++) {
        uint32_t target = u32(&r);
        if (target >= depth) return 1; /* branchTarget rejects it later */
        if (target == depth - 1) *exits = 1;
        if (labels[depth - 1 - target] == 0x03) check = 1;
    }
    return check;
}
/* A straight-line header ending in a forward br_if has the same cost on every
 * entry. Prepay it at initial entry and the backedge rather than updating fuel
 * in the header. Only select bodies with one or more equal-cost linear if/else
 * arms, otherwise straight-line code and a final unconditional br 0. Their
 * constant cost already needs just one body debit, but their control-flow joins
 * force FastJIT to reload the header's fuel-local update. Simple linear loops
 * can retain that value already and do not benefit reliably from this change.
 *
 * The backedge compares against -prefix, preserving its original check point:
 * charging the next prefix must not cause earlier exhaustion. The header then
 * consumes that credit before its forward branch. Reject calls, other control
 * boundaries, plain blocks and nested loops. Lookahead therefore does not
 * recursively scan whole loop subtrees. */
static uint32_t loopPrefix(reader r, unsigned depth, const unsigned *labels,
                           const uint8_t **prefix_end) {
    reader header = r;
    uint32_t cost = 0;
    const uint8_t *end = NULL;
    while (header.p < header.end && !header.bad) {
        const uint8_t *opcode = header.p;
        unsigned op = byte(&header);
        int boundary = instruction(&header, op);
        cost++;
        if (op == 0x02 || header.bad) return 0;
        if (!boundary) continue;
        if (op != 0x0d) return 0;
        reader target = {opcode + 1, header.p, 0};
        uint32_t label = u32(&target);
        /* depth is the label count before adding this loop. Label 0 is the
         * loop; label == depth is the implicit function label. */
        if (target.bad || !label || label >= depth ||
            labels[depth - label] == 0x03) return 0;
        end = opcode;
        break;
    }
    if (!end) return 0;
    r = header;
    int have_if = 0;
    while (r.p < r.end && !r.bad) {
        const uint8_t *opcode = r.p;
        unsigned op = byte(&r);
        int boundary = instruction(&r, op);
        if (r.bad || op == 0x02) return 0;
        if (op == 0x04) {
            uint32_t arm_cost;
            if (!equalIf(&r, &arm_cost)) return 0;
            have_if = 1;
        } else if (op == 0x0c) {
            reader target = {opcode + 1, r.p, 0};
            if (!have_if || u32(&target) != 0 || target.bad ||
                r.p == r.end || *r.p != 0x0b) return 0;
            *prefix_end = end;
            return cost;
        } else if (boundary) return 0;
    }
    return 0;
}
static void body(reader *r, buffer *out, uint32_t global, uint32_t params,
                 const signatures *s) {
    meter m = {.global = global};
    if (!analyze(*r, params, &m)) { r->bad = 1; return; }
    uint32_t count = u32(r);
    const uint8_t *locals = r->p;
    for (uint32_t i = 0; i < count && !r->bad; i++) { u32(r); byte(r); }
    leb(out, count + 1, 0);
    put(out, locals, r->p - locals);
    emit(out, 1); emit(out, 0x7e); /* one i64 fuel local */
    reload(out, &m);
    emit(out, 0x02); emit(out, 0x40);       /* block exhaustion (void) */
    entryCheck(out, &m, 0);
    const uint8_t *start = r->p, *previous = NULL;
    unsigned previous_op = 0;
    unsigned labels[512] = {0}, depth = 1;
    uint32_t prefixes[512] = {0};
    const uint8_t *prefix_ends[512] = {0};
    uint32_t cost = 0;
    while (r->p < r->end && !r->bad) {
        const uint8_t *opcode = r->p;
        unsigned op = byte(r);
        int boundary = instruction(r, op);
        cost++;
        uint32_t arm_cost;
        if (op == 0x04 && equalIf(r, &arm_cost)) {
            cost += arm_cost;
            previous = NULL; /* the last original instruction was the if's end */
            continue; /* the complete if/else leaves label depth unchanged */
        }
        if (boundary) {
            uint32_t entering_prefix = 0;
            const uint8_t *entering_end = NULL;
            if (op == 0x03)
                entering_prefix = loopPrefix(*r, depth, labels, &entering_end);
            if (op == 0x0d && prefix_ends[depth - 1] == opcode)
                cost -= prefixes[depth - 1];
            int exits = 0;
            int check = op == 0x11 || op == 0x00;
            const fixed_call *callee = NULL;
            uint32_t callee_type = 0;
            if (op == 0x10) {
                reader target = {opcode + 1, r->p, 0};
                uint32_t index = u32(&target);
                if (target.bad || index >= (uint64_t)s->import_functions + s->function_count) {
                    r->bad = 1; break;
                }
                check = index < s->import_functions;
                if (!check && s->fixed[index - s->import_functions].clone) {
                    callee = &s->fixed[index - s->import_functions];
                    callee_type = s->functions[index - s->import_functions];
                }
            }
            if (op >= 0x0c && op <= 0x0e) {
                reader targets = {opcode + 1, r->p, 0};
                check = branchCheck(targets, op, depth, labels, &exits);
            }
            /* A debit between a comparison and its branch forces FastJIT to
             * materialize a Boolean and compare it again. Keep the pair intact.
             * The comparison's operands remain below our stack-neutral update. */
            const uint8_t *debit = opcode;
            if ((op == 0x0d || op == 0x04) && previous && previous >= start &&
                previous_op >= 0x45 && previous_op <= 0x5a) debit = previous;
            put(out, start, debit - start);
            if (op == 0x0c && check && !exits) {
                reader target = {opcode + 1, r->p, 0};
                uint32_t backedge = u32(&target);
                if (target.bad || backedge >= depth) { r->bad = 1; break; }
                uint32_t prepaid = prefixes[depth - 1 - backedge];
                charge(out, &m, cost + prepaid, -1, backedge, prepaid);
                emit(out, 0x0c); leb(out, depth - 1, 0); /* br exhaustion */
            } else {
                if (cost || entering_prefix || check)
                    charge(out, &m, cost + entering_prefix, check, depth - 1, 0);
                put(out, debit, opcode - debit);
                if (op == 0x0b && depth == 1) {
                    publish(out, &m);
                    emit(out, 0x0f);           /* return before cold exhaustion exit */
                } else if (op == 0x0c || op == 0x0d || op == 0x0e) {
                    reader targets = {opcode + 1, r->p, 0};
                    if (exits) publish(out, &m);
                    emit(out, op);
                    if (op == 0x0e) {
                        uint32_t n = u32(&targets);
                        leb(out, n, 0);
                        for (uint32_t i = 0; i <= n && !targets.bad; i++)
                            branchTarget(&targets, out, depth);
                    } else branchTarget(&targets, out, depth);
                    if (targets.bad) r->bad = 1;
                } else if (callee) {
                    if (callee->calls) {
                        /* The original signature supplies this if's block
                         * parameters and results. Both arms consume the same
                         * existing arguments, including mixed/multivalue types. */
                        emit(out, 0x20); leb(out, m.local, 0);
                        emit(out, 0x42); leb(out, callee->cost, 1);
                        emit(out, 0x59); /* i64.ge_s: entire tree is funded */
                        emit(out, 0x04); leb(out, callee_type, 1);
                    } else entryCheck(out, &m, depth - 1);
                    emit(out, 0x10); leb(out, callee->clone, 0);
                    charge(out, &m, callee->cost, 0, 0, 0);
                    if (callee->calls) {
                        emit(out, 0x05); /* else: preserve low-fuel behavior */
                        publish(out, &m);
                        put(out, opcode, r->p - opcode);
                        reload(out, &m);
                        emit(out, 0x0b);
                    }
                } else if (op == 0x10 || op == 0x11 || op == 0x0f || op == 0x00) {
                    publish(out, &m);
                    put(out, opcode, r->p - opcode);
                    if (op == 0x10 || op == 0x11) reload(out, &m);
                } else put(out, opcode, r->p - opcode);
            }
            if (op == 0x03 && depth < 512) {
                prefixes[depth] = entering_prefix;
                prefix_ends[depth] = entering_end;
            }
            start = r->p; cost = 0;
        }
        if (op >= 0x02 && op <= 0x04) {
            if (depth == 512) { r->bad = 1; break; }
            if (op != 0x03) { prefixes[depth] = 0; prefix_ends[depth] = NULL; }
            labels[depth++] = op;
        }
        if (op == 0x0b) {
            if (!depth) { r->bad = 1; break; }
            if (--depth == 0 && r->p != r->end) r->bad = 1;
        }
        previous = opcode;
        previous_op = op;
        if (out->bad) r->bad = 1;
    }
    if (depth != 0 || cost || r->p != r->end) r->bad = 1;
    emit(out, 0x0b);                       /* end exhaustion block */
    emit(out, 0x42); emit(out, 0);          /* saturate remaining fuel at zero */
    emit(out, 0x24); leb(out, global, 0);
    emit(out, 0x41); emit(out, 1);          /* i32.const 1 */
    emit(out, 0x24); leb(out, global + 1, 0); /* global.set exhausted */
    emit(out, 0x00); emit(out, 0x0b);       /* unreachable; end function */
}
/* A funded tree clone bypasses all nested meters. Its only edits are direct
 * calls to other selected clones; signatures, locals and other bytes stay put. */
static void cloneBody(reader *r, buffer *out, const signatures *s) {
    const uint8_t *start = r->p;
    uint32_t locals = u32(r);
    for (uint32_t i = 0; i < locals && !r->bad; i++) { u32(r); byte(r); }
    while (r->p < r->end && !r->bad) {
        const uint8_t *opcode = r->p;
        unsigned op = byte(r);
        instruction(r, op);
        if (op != 0x10) continue;
        reader target = {opcode + 1, r->p, 0};
        uint32_t index = u32(&target);
        if (target.bad || index < s->import_functions ||
            index >= (uint64_t)s->import_functions + s->function_count ||
            !s->fixed[index - s->import_functions].clone) { r->bad = 1; break; }
        put(out, start, opcode - start);
        emit(out, 0x10); leb(out, s->fixed[index - s->import_functions].clone, 0);
        start = r->p;
    }
    put(out, start, r->p - start);
}
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
        body(&f, &b, global, s->params[s->functions[i]], s);
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
            if (!tree) { leb(out, len, 0); put(out, start, len); }
            else {
                reader f = {start, start + len, 0};
                buffer b = {0};
                cloneBody(&f, &b, s);
                if (f.bad || b.bad) r->bad = 1;
                else { leb(out, b.len, 0); put(out, b.p, b.len); }
                free(b.p);
            }
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
    signatures sig = {0};
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
