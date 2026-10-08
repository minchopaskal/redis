/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#include "wasm_meter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* MVP numeric instructions, sign extension, saturating conversions and bulk
 * memory only. Unknown opcodes fail closed. No index renumbering: two globals
 * are appended, and inserted checks are balanced, stack-neutral expressions.
 * Prepay straight-line segments, splitting at ALL calls/control transfers.
 * Loop checks are inside the loop, so every backedge pays. A unit is one
 * original opcode, including control delimiters when reached by fallthrough.
 * A trap may prepay the remainder of its segment. Bulk operations cost one;
 * fuel bounds guest instructions, not time spent inside a Redis host command. */
#define MAX_OUTPUT (64U * 1024U * 1024U)
typedef struct { const uint8_t *p, *end; int bad; } reader;
typedef struct { uint8_t *p; size_t len, cap; int bad; } buffer;
typedef struct { const uint8_t *p; size_t len; unsigned id; } section;

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
static void charge(buffer *b, uint32_t index, uint32_t cost) {
    emit(b, 0x23); leb(b, index, 0);        /* global.get fuel */
    emit(b, 0x42); leb(b, cost, 1);         /* i64.const cost */
    emit(b, 0x54);                         /* i64.lt_u */
    emit(b, 0x04); emit(b, 0x40);          /* if () */
    emit(b, 0x41); emit(b, 1);             /* i32.const 1 */
    emit(b, 0x24); leb(b, index + 1, 0);   /* global.set exhausted */
    emit(b, 0x00); emit(b, 0x0b);          /* unreachable; end */
    emit(b, 0x23); leb(b, index, 0);
    emit(b, 0x42); leb(b, cost, 1);
    emit(b, 0x7d);                         /* i64.sub */
    emit(b, 0x24); leb(b, index, 0);
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
static void body(reader *r, buffer *out, uint32_t global) {
    const uint8_t *locals = r->p;
    uint32_t count = u32(r);
    for (uint32_t i = 0; i < count && !r->bad; i++) { u32(r); byte(r); }
    put(out, locals, r->p - locals);
    const uint8_t *start = r->p;
    unsigned depth = 1;
    uint32_t cost = 0;
    while (r->p < r->end && !r->bad) {
        unsigned op = byte(r);
        int boundary = instruction(r, op);
        if (op >= 0x02 && op <= 0x04 && ++depth > 512) r->bad = 1;
        if (op == 0x0b && --depth == 0 && r->p != r->end) r->bad = 1;
        cost++;
        if (boundary) {
            charge(out, global, cost);
            put(out, start, r->p - start);
            start = r->p; cost = 0;
        }
        if (out->bad) r->bad = 1;
    }
    if (depth != 0 || cost || r->p != r->end) r->bad = 1;
}
static void code(reader *r, buffer *out, uint32_t global) {
    uint32_t count = u32(r);
    leb(out, count, 0);
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t len = u32(r);
        const uint8_t *start = r->p;
        skip(r, len);
        if (r->bad) break;
        reader f = {start, start + len, 0};
        buffer b = {0};
        body(&f, &b, global);
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
        if (id == 2) imported = importedGlobals(&s);
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
            code(&c, &b, index);
            if (c.bad) result.bad = 1;
            else emitSection(&result, 10, &b);
            free(b.p);
        } else { emit(&result, id); leb(&result, len, 0); put(&result, s.p, s.len); }
    }
    if (!have_globals) globals(&result, NULL, fuel);
    if (!have_exports) exports(&result, NULL, index);
    if (result.bad) goto fail;
    *output = result.p; *output_length = result.len;
    return 1;
fail:
    free(result.p);
    if (error_length) snprintf(error, error_length, "%s", failure);
    return 0;
}
