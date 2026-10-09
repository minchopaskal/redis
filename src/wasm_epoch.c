/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#include "wasm_epoch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* A trusted native import checks the host's epoch deadline. The common path
 * counts loop backedges, not individual Wasm instructions. For loops whose
 * backedges are all unconditional, replace the original backedge with a
 * countdown and conditional branch to that same loop. Only zero falls through
 * to the native probe, then branches back. This avoids an additional hot
 * branch or an if/block merge that discards FastJIT's cached values.
 * Loops with conditional/table backedges instead poll at their header.
 * Every function containing an original call also updates a private shared
 * entry counter, covering loop-free recursive/indirect call cycles. Leaf
 * functions below 512 original opcodes need no entry poll. Larger functions
 * and loop bodies probe unconditionally at entry/header, bounding delay from
 * repeated large callees and sampled iterations with large bodies. Functions
 * with more than 32 static call sites also sample every original call using
 * the shared counter. Accumulating across invocations/functions prevents flat
 * sequences of small leaf calls from repeatedly resetting a local sample.
 * Both counters amortize native probes; neither is an instruction budget.
 * MVP numeric instructions, sign extension, saturating conversions and bulk
 * memory are supported; unknown opcodes/element forms fail closed.
 * Appending a function import shifts defined function indices by one. Rewrite
 * calls, function exports, start and element vectors (and debug name maps).
 * Existing types, locals, globals and branch labels keep their indices. */
#define MAX_OUTPUT (64U * 1024U * 1024U)
#define MAX_DEPTH 512
#define LARGE_BODY_OPS 512
#define MANY_CALL_SITES 32
typedef struct { const uint8_t *p, *end; int bad; } reader;
typedef struct { uint8_t *p; size_t len, cap; int bad; } buffer;
typedef struct { const uint8_t *p; size_t len; unsigned id; } section;
typedef struct {
    uint32_t *params, *functions;
    uint32_t type_count, function_count, imported_functions, imported_globals;
} signatures;
typedef struct { uint32_t start; int generic, large; } loop_info;
typedef struct {
    loop_info *loops;
    uint32_t count, cap, local, calls, opcodes;
    int local_needed;
} body_info;

static unsigned byte(reader *r) {
    if (r->p == r->end) { r->bad = 1; return 0; }
    return *r->p++;
}
static uint32_t u32(reader *r) {
    uint32_t n = 0;
    for (unsigned i = 0; i < 5; i++) {
        unsigned c = byte(r);
        if (i == 4 && (c & 0xf0)) { r->bad = 1; return 0; }
        n |= (uint32_t)(c & 127) << (7 * i);
        if (!(c & 128)) return n;
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
        unsigned c = byte(r);
        if (!(c & 128)) {
            if (i == bits / 7) {
                unsigned mask = 0x7f & ~((1U << (bits % 7)) - 1);
                unsigned sign = 1U << ((bits - 1) % 7);
                if ((c & mask) != ((c & sign) ? mask : 0)) r->bad = 1;
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
static int sameName(const uint8_t *p, uint32_t n, const char *name) {
    return n == strlen(name) && !memcmp(p, name, n);
}
static void copyName(reader *r, buffer *out) {
    uint32_t n = u32(r);
    const uint8_t *p = r->p;
    skip(r, n);
    if (!r->bad) { leb(out, n, 0); put(out, p, n); }
}
static uint32_t functionIndex(uint32_t index, const signatures *s, reader *r) {
    if (index >= s->imported_functions) {
        if (index == UINT32_MAX) r->bad = 1;
        else index++;
    }
    return index;
}

static void types(reader *r, signatures *s) {
    s->type_count = u32(r);
    if (r->bad || s->type_count == UINT32_MAX ||
        s->type_count > (size_t)(r->end - r->p) / 3) { r->bad = 1; return; }
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
    if (flags > 1) r->bad = 1; /* no shared memory, memory64 or custom pages */
    u32(r);
    if (flags & 1) u32(r);
}
static void imports(reader *r, signatures *s) {
    uint32_t count = u32(r);
    if (count == UINT32_MAX) { r->bad = 1; return; }
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t module_len = u32(r);
        const uint8_t *module = r->p;
        skip(r, module_len);
        uint32_t field_len = u32(r);
        const uint8_t *field = r->p;
        skip(r, field_len);
        if (r->bad) break;
        if (sameName(module, module_len, WASM_EPOCH_MODULE) &&
            sameName(field, field_len, WASM_EPOCH_POLL)) { r->bad = 1; break; }
        switch (byte(r)) {
        case 0: u32(r); s->imported_functions++; break;
        case 1: byte(r); limits(r); break;
        case 2: limits(r); break;
        case 3: byte(r); byte(r); s->imported_globals++; break;
        default: r->bad = 1;
        }
    }
    if (r->p != r->end) r->bad = 1;
}
static int reservedExports(reader *r) {
    uint32_t count = u32(r);
    const char *prefix = "__redis_epoch_";
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

/* Decode original instructions. The original WAMR load checks semantics. */
static void instruction(reader *r, unsigned op) {
    switch (op) {
    case 0x00: case 0x01: case 0x05: case 0x0b: case 0x0f:
    case 0x1a: case 0x1b: return;
    case 0x02: case 0x03: case 0x04: signedLeb(r, 33); return;
    case 0x0c: case 0x0d: case 0x10: u32(r); return;
    case 0x0e: {
        uint32_t count = u32(r);
        if (count >= (size_t)(r->end - r->p)) { r->bad = 1; return; }
        for (uint32_t i = 0; i <= count && !r->bad; i++) u32(r);
        return;
    }
    case 0x11: u32(r); u32(r); return;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x3f: case 0x40: u32(r); return;
    case 0x41: signedLeb(r, 32); return;
    case 0x42: signedLeb(r, 64); return;
    case 0x43: skip(r, 4); return;
    case 0x44: skip(r, 8); return;
    case 0xfc: {
        unsigned sub = u32(r);
        if (sub <= 7) return; /* saturating conversions */
        if (sub == 8 || sub == 10) { u32(r); u32(r); return; }
        if (sub == 9 || sub == 11) { u32(r); return; }
        r->bad = 1; return;
    }
    default:
        if (op >= 0x28 && op <= 0x3e) { u32(r); u32(r); return; }
        if (op >= 0x45 && op <= 0xc4) return;
        r->bad = 1;
    }
}
static int addLoop(body_info *f) {
    if (f->count == f->cap) {
        size_t cap = f->cap ? (size_t)f->cap * 2 : 16;
        if (cap > MAX_OUTPUT / sizeof(*f->loops)) return 0;
        void *next = realloc(f->loops, cap * sizeof(*f->loops));
        if (!next) return 0;
        f->loops = next; f->cap = (uint32_t)cap;
    }
    f->loops[f->count++] = (loop_info){.start = f->opcodes};
    return 1;
}
static void backedge(reader *r, body_info *f, const int32_t *labels,
                     unsigned depth, unsigned op) {
    uint32_t count = op == 0x0e ? u32(r) : 0;
    if (count >= (size_t)(r->end - r->p)) { r->bad = 1; return; }
    for (uint32_t i = 0; i <= count && !r->bad; i++) {
        uint32_t target = u32(r);
        if (target >= depth) { r->bad = 1; break; }
        int32_t loop = labels[depth - 1 - target];
        if (loop >= 0 && op != 0x0c) f->loops[loop].generic = 1;
    }
}
static int analyze(reader r, uint32_t params, body_info *f) {
    uint32_t groups = u32(&r);
    uint64_t locals = params;
    for (uint32_t i = 0; i < groups && !r.bad; i++) { locals += u32(&r); byte(&r); }
    if (r.bad || locals >= UINT32_MAX) return 0;
    f->local = (uint32_t)locals;
    int32_t labels[MAX_DEPTH];
    labels[0] = -1;
    unsigned depth = 1;
    while (r.p < r.end && !r.bad) {
        const uint8_t *opcode = r.p;
        unsigned op = byte(&r);
        instruction(&r, op);
        f->opcodes++;
        if (op == 0x10 || op == 0x11) f->calls++;
        if (op == 0x0c || op == 0x0d || op == 0x0e) {
            reader targets = {opcode + 1, r.p, 0};
            backedge(&targets, f, labels, depth, op);
            if (targets.bad || targets.p != targets.end) r.bad = 1;
        }
        if (op >= 0x02 && op <= 0x04) {
            if (depth == MAX_DEPTH) { r.bad = 1; break; }
            int32_t loop = -1;
            if (op == 0x03) {
                if (!addLoop(f)) { r.bad = 1; break; }
                loop = (int32_t)f->count - 1;
            }
            labels[depth++] = loop;
        } else if (op == 0x0b) {
            if (!depth) { r.bad = 1; break; }
            depth--;
            int32_t loop = labels[depth];
            if (loop >= 0) {
                loop_info *info = &f->loops[loop];
                info->large = f->opcodes - info->start + 1 >= LARGE_BODY_OPS;
            }
            if (!depth && r.p != r.end) r.bad = 1;
        }
    }
    for (uint32_t i = 0; i < f->count; i++)
        if (!f->loops[i].large) f->local_needed = 1;
    return !r.bad && !depth && r.p == r.end;
}
static void probe(buffer *out, uint32_t function) {
    emit(out, 0x10); leb(out, function, 0); /* trusted call () -> i32 */
}
static void localDecrement(buffer *out, uint32_t local) {
    emit(out, 0x20); leb(out, local, 0);
    emit(out, 0x41); emit(out, 1);
    emit(out, 0x6b);                       /* i32.sub */
    emit(out, 0x22); leb(out, local, 0);   /* local.tee counter */
}
static void entryPoll(buffer *out, uint32_t global, uint32_t function) {
    emit(out, 0x23); leb(out, global, 0);
    emit(out, 0x41); emit(out, 1);
    emit(out, 0x6b);
    emit(out, 0x24); leb(out, global, 0);
    emit(out, 0x23); leb(out, global, 0);
    emit(out, 0x45);                       /* i32.eqz */
    emit(out, 0x04); emit(out, 0x40);
    /* A native exception skips the returned value. Leave the shared counter
     * positive so the next invocation cannot underflow it and miss polls. */
    emit(out, 0x41); leb(out, WASM_EPOCH_POLL_STRIDE, 1);
    emit(out, 0x24); leb(out, global, 0);
    probe(out, function);
    emit(out, 0x24); leb(out, global, 0);
    emit(out, 0x0b);
}
static void headerPoll(buffer *out, uint32_t local, uint32_t function) {
    localDecrement(out, local);
    emit(out, 0x45);
    emit(out, 0x04); emit(out, 0x40);      /* cold native check */
    probe(out, function);
    emit(out, 0x21); leb(out, local, 0);
    emit(out, 0x0b);
}
static void body(reader *r, buffer *out, uint32_t global,
                 uint32_t params, const signatures *s) {
    body_info f = {0};
    if (!analyze(*r, params, &f)) { r->bad = 1; goto done; }
    uint32_t groups = u32(r);
    const uint8_t *locals = r->p;
    for (uint32_t i = 0; i < groups && !r->bad; i++) { u32(r); byte(r); }
    if (groups == UINT32_MAX) { r->bad = 1; goto done; }
    leb(out, groups + f.local_needed, 0);
    put(out, locals, r->p - locals);
    if (f.local_needed) { emit(out, 1); emit(out, 0x7f); } /* appended i32 counter */
    if (f.opcodes >= LARGE_BODY_OPS) {
        probe(out, s->imported_functions); emit(out, 0x1a);
    } else if (f.calls) entryPoll(out, global, s->imported_functions);
    if (f.local_needed) {
        emit(out, 0x41); leb(out, WASM_EPOCH_POLL_STRIDE, 1);
        emit(out, 0x21); leb(out, f.local, 0);
    }
    int32_t labels[MAX_DEPTH];
    labels[0] = -1;
    unsigned depth = 1;
    uint32_t next_loop = 0;
    while (r->p < r->end && !r->bad && !out->bad) {
        const uint8_t *opcode = r->p;
        unsigned op = byte(r);
        instruction(r, op);
        if (r->bad) break;
        reader immediate = {opcode + 1, r->p, 0};
        if ((op == 0x10 || op == 0x11) && f.calls > MANY_CALL_SITES)
            entryPoll(out, global, s->imported_functions);
        if (op == 0x10) {
            uint32_t index = functionIndex(u32(&immediate), s, &immediate);
            emit(out, op); leb(out, index, 0);
        } else if (op == 0x0c) {
            uint32_t target = u32(&immediate);
            if (target >= depth) { r->bad = 1; break; }
            int32_t loop = labels[depth - 1 - target];
            if (loop >= 0 && !f.loops[loop].generic && !f.loops[loop].large) {
                localDecrement(out, f.local);
                emit(out, 0x0d); leb(out, target, 0); /* original hot backedge */
                probe(out, s->imported_functions);
                emit(out, 0x21); leb(out, f.local, 0);
                emit(out, 0x0c); leb(out, target, 0);
            } else put(out, opcode, r->p - opcode);
        } else put(out, opcode, r->p - opcode);
        if (immediate.bad) { r->bad = 1; break; }
        if (op >= 0x02 && op <= 0x04) {
            if (depth == MAX_DEPTH) { r->bad = 1; break; }
            int32_t loop = -1;
            if (op == 0x03) {
                if (next_loop == f.count) { r->bad = 1; break; }
                loop = (int32_t)next_loop++;
            }
            labels[depth++] = loop;
            if (loop >= 0) {
                if (f.loops[loop].large) {
                    probe(out, s->imported_functions); emit(out, 0x1a);
                } else if (f.loops[loop].generic)
                    headerPoll(out, f.local, s->imported_functions);
            }
        } else if (op == 0x0b) {
            if (!depth) { r->bad = 1; break; }
            depth--;
            if (!depth && r->p != r->end) r->bad = 1;
        }
    }
    if (depth || next_loop != f.count || r->p != r->end) r->bad = 1;
done:
    free(f.loops);
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
        body(&f, &b, global, s->params[s->functions[i]], s);
        if (f.bad || b.bad) r->bad = 1;
        else { leb(out, b.len, 0); put(out, b.p, b.len); }
        free(b.p);
    }
    if (r->p != r->end) r->bad = 1;
}
static void addType(buffer *out, const section *s) {
    buffer b = {0};
    uint32_t count = 0;
    reader r = {NULL, NULL, 0};
    if (s) { r.p = s->p; r.end = s->p + s->len; count = u32(&r); }
    if (count == UINT32_MAX || r.bad) { out->bad = 1; return; }
    leb(&b, count + 1, 0);
    if (s) put(&b, r.p, r.end - r.p);
    emit(&b, 0x60); emit(&b, 0); emit(&b, 1); emit(&b, 0x7f); /* () -> i32 */
    emitSection(out, 1, &b); free(b.p);
}
static void addImport(buffer *out, const section *s, uint32_t type) {
    buffer b = {0};
    uint32_t count = 0;
    reader r = {NULL, NULL, 0};
    if (s) { r.p = s->p; r.end = s->p + s->len; count = u32(&r); }
    if (count == UINT32_MAX || r.bad) { out->bad = 1; return; }
    leb(&b, count + 1, 0);
    if (s) put(&b, r.p, r.end - r.p);
    name(&b, WASM_EPOCH_MODULE); name(&b, WASM_EPOCH_POLL);
    emit(&b, 0); leb(&b, type, 0);
    emitSection(out, 2, &b); free(b.p);
}
static void addGlobal(buffer *out, const section *s) {
    buffer b = {0};
    uint32_t count = 0;
    reader r = {NULL, NULL, 0};
    if (s) { r.p = s->p; r.end = s->p + s->len; count = u32(&r); }
    if (count == UINT32_MAX || r.bad) { out->bad = 1; return; }
    leb(&b, count + 1, 0);
    if (s) put(&b, r.p, r.end - r.p);
    emit(&b, 0x7f); emit(&b, 1); emit(&b, 0x41);
    leb(&b, WASM_EPOCH_POLL_STRIDE, 1); emit(&b, 0x0b);
    emitSection(out, 6, &b); free(b.p);
}
static void exports(reader *r, buffer *out, const signatures *s) {
    uint32_t count = u32(r);
    leb(out, count, 0);
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        copyName(r, out);
        unsigned kind = byte(r);
        uint32_t index = u32(r);
        if (kind == 0) index = functionIndex(index, s, r);
        emit(out, kind); leb(out, index, 0);
    }
    if (r->p != r->end) r->bad = 1;
}
static void expression(reader *r, buffer *out) {
    const uint8_t *start = r->p;
    unsigned op = byte(r);
    switch (op) {
    case 0x23: u32(r); break;
    case 0x41: signedLeb(r, 32); break;
    case 0x42: signedLeb(r, 64); break;
    case 0x43: skip(r, 4); break;
    case 0x44: skip(r, 8); break;
    default: r->bad = 1;
    }
    if (byte(r) != 0x0b) r->bad = 1;
    if (!r->bad) put(out, start, r->p - start);
}
static void elements(reader *r, buffer *out, const signatures *s) {
    uint32_t count = u32(r);
    leb(out, count, 0);
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t mode = u32(r);
        if (mode > 3) { r->bad = 1; break; } /* no reference-type expressions */
        leb(out, mode, 0);
        if (mode == 2) leb(out, u32(r), 0);
        if (mode == 0 || mode == 2) expression(r, out);
        if (mode) {
            unsigned kind = byte(r);
            if (kind) { r->bad = 1; break; }
            emit(out, kind);
        }
        uint32_t n = u32(r);
        if (n > (size_t)(r->end - r->p)) { r->bad = 1; break; }
        leb(out, n, 0);
        for (uint32_t j = 0; j < n && !r->bad; j++)
            leb(out, functionIndex(u32(r), s, r), 0);
    }
    if (r->p != r->end) r->bad = 1;
}
static void nameMap(reader *r, buffer *out, const signatures *s, int remap) {
    uint32_t count = u32(r);
    leb(out, count, 0);
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        uint32_t index = u32(r);
        if (remap) index = functionIndex(index, s, r);
        leb(out, index, 0); copyName(r, out);
    }
}
static void custom(reader *r, buffer *out, const signatures *s) {
    uint32_t n = u32(r);
    const uint8_t *section_name = r->p;
    skip(r, n);
    if (r->bad) return;
    leb(out, n, 0); put(out, section_name, n);
    if (!sameName(section_name, n, "name")) {
        put(out, r->p, r->end - r->p); r->p = r->end; return;
    }
    while (r->p < r->end && !r->bad) {
        unsigned id = byte(r);
        uint32_t len = u32(r);
        const uint8_t *start = r->p;
        skip(r, len);
        if (r->bad) break;
        reader names = {start, start + len, 0};
        buffer b = {0};
        if (id == 1) nameMap(&names, &b, s, 1);
        else if (id == 2 || id == 3) {
            uint32_t count = u32(&names);
            leb(&b, count, 0);
            for (uint32_t i = 0; i < count && !names.bad; i++) {
                leb(&b, functionIndex(u32(&names), s, &names), 0);
                nameMap(&names, &b, s, 0);
            }
        } else { put(&b, names.p, names.end - names.p); names.p = names.end; }
        if (names.bad || names.p != names.end || b.bad) r->bad = 1;
        else { emit(out, id); leb(out, b.len, 0); put(out, b.p, b.len); }
        free(b.p);
    }
}

int wasmEpochInject(const uint8_t *input, size_t length,
                    uint8_t **output, size_t *output_length,
                    char *error, size_t error_length) {
    *output = NULL; *output_length = 0;
    const char *failure = "Invalid or unsupported Wasm for epoch metering";
    buffer result = {0};
    signatures sig = {0};
    if (length < 8 || length > WASM_EPOCH_MAX_INPUT ||
        memcmp(input, "\0asm\1\0\0\0", 8)) goto fail;
    reader r = {input + 8, input + length, 0};
    uint32_t defined_globals = 0;
    unsigned seen = 0;
    /* Validate sizes and compute the original function/global spaces. */
    while (r.p < r.end && !r.bad) {
        unsigned id = byte(&r);
        uint32_t len = u32(&r);
        const uint8_t *start = r.p;
        skip(&r, len);
        if (r.bad || id > 12 || (id && (seen & (1U << id)))) goto fail;
        seen |= 1U << id;
        reader s = {start, start + len, 0};
        if (id == 1) types(&s, &sig);
        if (id == 2) imports(&s, &sig);
        if (id == 3) functions(&s, &sig);
        if (id == 6) defined_globals = u32(&s);
        if (id == 7 && reservedExports(&s)) {
            failure = "Reserved __redis_epoch_ export name"; goto fail;
        }
        if (s.bad) goto fail;
    }
    if (r.bad || (uint64_t)sig.imported_globals + defined_globals >= UINT32_MAX ||
        (uint64_t)sig.imported_functions + sig.function_count >= UINT32_MAX) goto fail;
    uint32_t global = sig.imported_globals + defined_globals;
    int have_types = 0, have_imports = 0, have_globals = 0;
    put(&result, input, 8);
    r.p = input + 8;
    while (r.p < r.end && !r.bad && !result.bad) {
        unsigned id = byte(&r);
        uint32_t len = u32(&r);
        section s = {r.p, len, id};
        skip(&r, len);
        if (r.bad) break;
        if (id >= 2 && !have_types) { addType(&result, NULL); have_types = 1; }
        if (id >= 3 && !have_imports) { addImport(&result, NULL, sig.type_count); have_imports = 1; }
        if (id >= 7 && !have_globals) { addGlobal(&result, NULL); have_globals = 1; }
        if (id == 1) { addType(&result, &s); have_types = 1; }
        else if (id == 2) { addImport(&result, &s, sig.type_count); have_imports = 1; }
        else if (id == 6) { addGlobal(&result, &s); have_globals = 1; }
        else if (id == 0 || id == 7 || id == 8 || id == 9 || id == 10) {
            reader payload = {s.p, s.p + s.len, 0};
            buffer b = {0};
            if (id == 0) custom(&payload, &b, &sig);
            else if (id == 7) exports(&payload, &b, &sig);
            else if (id == 8) {
                leb(&b, functionIndex(u32(&payload), &sig, &payload), 0);
                if (payload.p != payload.end) payload.bad = 1;
            } else if (id == 9) elements(&payload, &b, &sig);
            else code(&payload, &b, global, &sig);
            if (payload.bad) result.bad = 1;
            else emitSection(&result, id, &b);
            free(b.p);
        } else { emit(&result, id); leb(&result, len, 0); put(&result, s.p, s.len); }
    }
    if (!have_types) addType(&result, NULL);
    if (!have_imports) addImport(&result, NULL, sig.type_count);
    if (!have_globals) addGlobal(&result, NULL);
    if (r.bad || result.bad) goto fail;
    free(sig.params); free(sig.functions);
    *output = result.p; *output_length = result.len;
    return 1;
fail:
    free(sig.params); free(sig.functions);
    free(result.p);
    if (error_length) snprintf(error, error_length, "%s", failure);
    return 0;
}
