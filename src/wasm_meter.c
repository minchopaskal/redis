/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#include "wasm_meter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* MVP numeric instructions, sign extension, saturating conversions and bulk
 * memory only. Unknown opcodes fail closed. Original function/local/global
 * indices are unchanged: private state is appended and checks are stack-neutral.
 * Cache fuel in an appended i64 local when a function has enough checks to
 * amortize synchronization and no calls inside loops. Publish at calls/exits,
 * reload after calls, and publish on exhaustion. Otherwise charge the global
 * directly. Ordinary guest traps may leave a cached counter unpublished;
 * Redis resets it before the next invocation and only inspects the trap flag.
 * A void block around each function provides one cold exhaustion exit. br_if
 * lets FastJIT retain cached values on fallthrough. Original branches to the
 * function label skip the added block; inner labels keep their indices.
 * Prepay straight-line segments, splitting at ALL calls/control transfers.
 * Loop checks are inside the loop, so every backedge pays. A unit is one
 * original opcode, including control delimiters when reached by fallthrough.
 * A trap may prepay the remainder of its segment. Bulk operations cost one;
 * fuel bounds guest instructions, not time spent inside a Redis host command. */
#define MAX_OUTPUT (64U * 1024U * 1024U)
typedef struct { const uint8_t *p, *end; int bad; } reader;
typedef struct { uint8_t *p; size_t len, cap; int bad; } buffer;
typedef struct { const uint8_t *p; size_t len; unsigned id; } section;
typedef struct {
    uint32_t *params, *functions;
    uint32_t type_count, function_count;
} signatures;
typedef struct { uint32_t global, local; int cached; } meter;

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
static void emitSection(buffer *out, unsigned id, buffer *payload) {
    if (payload->bad) { out->bad = 1; return; }
    emit(out, id); leb(out, payload->len, 0); put(out, payload->p, payload->len);
}
static void charge(buffer *b, const meter *m, uint32_t cost, unsigned fail_depth) {
    unsigned get = m->cached ? 0x20 : 0x23;
    unsigned set = m->cached ? 0x21 : 0x24;
    uint32_t index = m->cached ? m->local : m->global;
    emit(b, get); leb(b, index, 0);        /* local.get or global.get fuel */
    emit(b, 0x42); leb(b, cost, 1);         /* i64.const cost */
    emit(b, 0x54);                         /* i64.lt_u */
    emit(b, 0x0d); leb(b, fail_depth, 0);  /* br_if exhaustion block */
    emit(b, get); leb(b, index, 0);
    emit(b, 0x42); leb(b, cost, 1);
    emit(b, 0x7d);                         /* i64.sub */
    emit(b, set); leb(b, index, 0);
}
static void publish(buffer *b, const meter *m) {
    if (!m->cached) return;
    emit(b, 0x20); leb(b, m->local, 0);    /* local.get fuel */
    emit(b, 0x24); leb(b, m->global, 0);   /* global.set fuel */
}
static void reload(buffer *b, const meter *m) {
    if (!m->cached) return;
    emit(b, 0x23); leb(b, m->global, 0);   /* global.get fuel */
    emit(b, 0x21); leb(b, m->local, 0);    /* local.set fuel */
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
static uint32_t importedGlobals(reader *r) {
    uint32_t count = u32(r), globals = 0;
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t n = u32(r); skip(r, n);
        n = u32(r); skip(r, n);
        switch (byte(r)) {
        case 0: u32(r); break;
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
    case 0x02: case 0x03: case 0x04: signedLeb(r, 33); return 1;
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
/* Select storage from original code only. Calls nested in loops make frequent
 * publish/reload pairs expensive; tiny functions cannot amortize the local. */
static int analyze(reader r, uint32_t params, meter *m) {
    uint32_t count = u32(&r);
    uint64_t locals = params;
    for (uint32_t i = 0; i < count && !r.bad; i++) { locals += u32(&r); byte(&r); }
    if (r.bad || locals >= UINT32_MAX) return 0;
    m->local = (uint32_t)locals;
    unsigned labels[512] = {0}, depth = 1, loops = 0;
    uint32_t checks = 0, calls = 0;
    int loop_calls = 0;
    while (r.p < r.end && !r.bad) {
        unsigned op = byte(&r);
        if (instruction(&r, op)) checks++;
        if (op == 0x10 || op == 0x11) { calls++; if (loops) loop_calls = 1; }
        if (op >= 0x02 && op <= 0x04) {
            if (depth == 512) return 0;
            labels[depth++] = op;
            if (op == 0x03) loops++;
        } else if (op == 0x0b) {
            if (!depth) return 0;
            if (labels[--depth] == 0x03) loops--;
            if (!depth && r.p != r.end) return 0;
        }
    }
    if (r.bad || depth || r.p != r.end) return 0;
    m->cached = !loop_calls && checks > (uint64_t)calls * 2 + 2;
    return 1;
}
static int exitsFunction(reader r, unsigned op, unsigned depth) {
    uint32_t n = op == 0x0e ? u32(&r) : 0;
    for (uint32_t i = 0; i <= n && !r.bad; i++)
        if (u32(&r) == depth - 1) return 1;
    return 0;
}
static void body(reader *r, buffer *out, uint32_t global, uint32_t params) {
    meter m = {.global = global};
    if (!analyze(*r, params, &m)) { r->bad = 1; return; }
    uint32_t count = u32(r);
    const uint8_t *locals = r->p;
    for (uint32_t i = 0; i < count && !r->bad; i++) { u32(r); byte(r); }
    leb(out, count + m.cached, 0);
    put(out, locals, r->p - locals);
    if (m.cached) { emit(out, 1); emit(out, 0x7e); } /* one i64 fuel local */
    reload(out, &m);
    emit(out, 0x02); emit(out, 0x40);       /* block exhaustion (void) */
    const uint8_t *start = r->p;
    unsigned depth = 1;
    uint32_t cost = 0;
    while (r->p < r->end && !r->bad) {
        const uint8_t *opcode = r->p;
        unsigned op = byte(r);
        int boundary = instruction(r, op);
        cost++;
        if (boundary) {
            charge(out, &m, cost, depth - 1);
            if (op == 0x0b && depth == 1) {
                put(out, start, opcode - start);
                publish(out, &m);
                emit(out, 0x0f);           /* return before cold exhaustion exit */
            } else if (op == 0x0c || op == 0x0d || op == 0x0e) {
                put(out, start, opcode - start);
                reader targets = {opcode + 1, r->p, 0};
                if (exitsFunction(targets, op, depth)) publish(out, &m);
                emit(out, op);
                if (op == 0x0e) {
                    uint32_t n = u32(&targets);
                    leb(out, n, 0);
                    for (uint32_t i = 0; i <= n && !targets.bad; i++)
                        branchTarget(&targets, out, depth);
                } else branchTarget(&targets, out, depth);
                if (targets.bad) r->bad = 1;
            } else if (op == 0x10 || op == 0x11 || op == 0x0f || op == 0x00) {
                put(out, start, opcode - start);
                publish(out, &m);
                put(out, opcode, r->p - opcode);
                if (op == 0x10 || op == 0x11) reload(out, &m);
            } else put(out, start, r->p - start);
            start = r->p; cost = 0;
        }
        if (op >= 0x02 && op <= 0x04 && ++depth > 512) r->bad = 1;
        if (op == 0x0b && --depth == 0 && r->p != r->end) r->bad = 1;
        if (out->bad) r->bad = 1;
    }
    if (depth != 0 || cost || r->p != r->end) r->bad = 1;
    emit(out, 0x0b);                       /* end exhaustion block */
    publish(out, &m);
    emit(out, 0x41); emit(out, 1);          /* i32.const 1 */
    emit(out, 0x24); leb(out, global + 1, 0); /* global.set exhausted */
    emit(out, 0x00); emit(out, 0x0b);       /* unreachable; end function */
}
static void code(reader *r, buffer *out, uint32_t global, const signatures *s) {
    uint32_t count = u32(r);
    if (count != s->function_count) { r->bad = 1; return; }
    leb(out, count, 0);
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t len = u32(r);
        const uint8_t *start = r->p;
        skip(r, len);
        if (r->bad) break;
        reader f = {start, start + len, 0};
        buffer b = {0};
        body(&f, &b, global, s->params[s->functions[i]]);
        if (f.bad || b.bad) r->bad = 1;
        else { leb(out, b.len, 0); put(out, b.p, b.len); }
        free(b.p);
    }
    if (r->p != r->end) r->bad = 1;
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
        if (id == 2) imported = importedGlobals(&s);
        if (id == 3) functions(&s, &sig);
        if (id == 6) defined = u32(&s);
        if (id == 7 && reservedExports(&s)) {
            failure = "Reserved __redis_meter_ export name"; goto fail;
        }
        if (s.bad) goto fail;
    }
    if (r.bad || (uint64_t)imported + defined > UINT32_MAX - 2) goto fail;
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
        if (id == 6) { globals(&result, &s, fuel); have_globals = 1; }
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
    free(sig.params); free(sig.functions);
    *output = result.p; *output_length = result.len;
    return 1;
fail:
    free(sig.params); free(sig.functions);
    free(result.p);
    if (error_length) snprintf(error, error_length, "%s", failure);
    return 0;
}
