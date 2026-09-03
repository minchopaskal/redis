/*
 * Copyright (c) 2026 Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/*
 * WAMR-backed Redis Functions engine.
 *
 * This is intentionally a small proof of concept. It implements the binary
 * module lifecycle and the basic host ABI, but not function flags or the
 * proposed WASM-owned keyspace blob type.
 */

#include "functions.h"
#include "function_wasm.h"
#include "sha256.h"
#include "wasm_export.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define WASM_ENGINE_NAME "WASM"
#define WASM_ABI_VERSION 1
#define WASM_RUNTIME_POOL_SIZE (64U * 1024U * 1024U)
#define WASM_MAX_MEMORY_PAGES 128U /* 8 MiB at the WebAssembly 64 KiB page size. */
#define WASM_EXEC_STACK_SIZE (64U * 1024U)
#define WASM_LOAD_INSTRUCTION_LIMIT 10000000
#define WASM_CALL_INSTRUCTION_LIMIT 100000000
#define WASM_ERROR_BUF_SIZE 256
#define WASM_RESP_MAX_DEPTH 128
#define WASM_RESP_ENTRY_SIZE 16

typedef struct wasmEngineCtx {
    void *pool;
    size_t pool_size;
} wasmEngineCtx;

typedef struct wasmLibraryCtx {
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    sds binary;
    sds name;
    sds blob_owner;
    dict *blob_types;
    size_t refs;
} wasmLibraryCtx;

typedef struct wasmFunctionCtx {
    wasmLibraryCtx *library;
    wasm_function_inst_t function;
} wasmFunctionCtx;

typedef enum wasmHostPhase {
    WASM_HOST_LOAD,
    WASM_HOST_CALL,
} wasmHostPhase;

typedef struct wasmHostCtx {
    wasmHostPhase phase;
    wasmLibraryCtx *library;
    functionLibInfo *li;
    scriptRunCtx *run_ctx;
    size_t nkeys;
    sds input;
    sds staged_reply;
    sds last_error;
    sds final_reply;
} wasmHostCtx;

typedef struct wasmRespEntry {
    uint32_t tag;
    uint32_t offset;
    uint32_t length;
    uint32_t child_count;
} wasmRespEntry;

typedef struct wasmRespParser {
    const unsigned char *start;
    const unsigned char *p;
    const unsigned char *end;
    wasmRespEntry *entries;
    size_t entries_len;
    size_t entries_cap;
    int saw_resp3;
} wasmRespParser;

/* Functions contexts are recreated by FUNCTION FLUSH and DEBUG RELOAD. WAMR's
 * process-wide signal/allocator state is not safely reinitializable, so keep
 * the runtime alive and only recreate the Redis engine wrapper. */
static wasmEngineCtx *wasm_global_ctx;

static dictType wasmBlobTypeDictType = {
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .keyDestructor = dictSdsDestructor,
};

static uint32_t readU32LE(const unsigned char *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void writeU32LE(unsigned char *p, uint32_t value) {
    p[0] = value & 0xff;
    p[1] = (value >> 8) & 0xff;
    p[2] = (value >> 16) & 0xff;
    p[3] = (value >> 24) & 0xff;
}

static wasmHostCtx *wasmGetHostCtx(wasm_exec_env_t exec_env) {
    return wasm_runtime_get_user_data(exec_env);
}

static int wasmGetGuestBuffer(wasm_exec_env_t exec_env, uint32_t offset, uint32_t len, void **ptr) {
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(exec_env);
    if (len == 0) {
        *ptr = NULL;
        return C_OK;
    }
    if (!wasm_runtime_validate_app_addr(instance, offset, len)) {
        wasm_runtime_set_exception(instance, "out of bounds guest memory access");
        return C_ERR;
    }
    *ptr = wasm_runtime_addr_app_to_native(instance, offset);
    if (!*ptr) {
        wasm_runtime_set_exception(instance, "invalid guest memory address");
        return C_ERR;
    }
    return C_OK;
}

static void wasmSetLastError(wasmHostCtx *ctx, const char *error) {
    if (ctx->last_error) sdsfree(ctx->last_error);
    ctx->last_error = sdsnew(error);
}

static void wasmSetLastErrorSds(wasmHostCtx *ctx, sds error) {
    if (ctx->last_error) sdsfree(ctx->last_error);
    ctx->last_error = error;
}

static int32_t wasmReadSds(wasm_exec_env_t exec_env, sds value, uint32_t dst, uint32_t cap, uint32_t offset) {
    if (!value)
        return 0;
    if (offset > sdslen(value))
        return -1;

    size_t remaining = sdslen(value) - offset;
    if (remaining > INT32_MAX)
        return -1;
    if (cap == 0)
        return remaining ? -(int32_t)remaining : 0;

    size_t to_copy = remaining < cap ? remaining : cap;
    void *out;
    if (wasmGetGuestBuffer(exec_env, dst, to_copy, &out) != C_OK)
        return -1;
    if (to_copy) memcpy(out, value + offset, to_copy);
    return (int32_t)to_copy;
}

static int parseUnsignedLine(const unsigned char *p, const unsigned char *end,
                             const unsigned char **after, long long *value)
{
    const unsigned char *q = p;
    int negative = 0;
    unsigned long long n = 0;

    if (q < end && *q == '-') {
        negative = 1;
        q++;
    }
    if (q == end || *q < '0' || *q > '9')
        return C_ERR;
    while (q < end && *q >= '0' && *q <= '9') {
        unsigned digit = *q++ - '0';
        if (n > (ULLONG_MAX - digit) / 10)
            return C_ERR;
        n = n * 10 + digit;
    }
    if (q + 1 >= end || q[0] != '\r' || q[1] != '\n')
        return C_ERR;
    if ((!negative && n > LLONG_MAX) ||
        (negative && n > (unsigned long long)LLONG_MAX + 1))
        return C_ERR;
    if (negative && n == (unsigned long long)LLONG_MAX + 1)
        *value = LLONG_MIN;
    else
        *value = negative ? -(long long)n : (long long)n;
    *after = q + 2;
    return C_OK;
}

static int findLineEnd(const unsigned char *p, const unsigned char *end,
                       const unsigned char **line_end)
{
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') {
            *line_end = p;
            return C_OK;
        }
        if (*p == '\r' || *p == '\n')
            return C_ERR;
        p++;
    }
    return C_ERR;
}

static int appendRespEntry(wasmRespParser *parser, uint32_t tag, size_t offset,
                           size_t length, uint32_t child_count, size_t *index)
{
    if (offset > UINT32_MAX || length > UINT32_MAX)
        return C_ERR;
    if (parser->entries_len == parser->entries_cap) {
        size_t new_cap = parser->entries_cap ? parser->entries_cap * 2 : 16;
        if (new_cap > INT32_MAX / WASM_RESP_ENTRY_SIZE)
            return C_ERR;
        parser->entries = zrealloc(parser->entries, new_cap * sizeof(*parser->entries));
        parser->entries_cap = new_cap;
    }
    *index = parser->entries_len++;
    parser->entries[*index] = (wasmRespEntry) {
        .tag = tag,
        .offset = (uint32_t)offset,
        .length = (uint32_t)length,
        .child_count = child_count,
    };
    return C_OK;
}

static int parseRespValue(wasmRespParser *parser, unsigned depth);

static int parseRespAggregate(wasmRespParser *parser, unsigned depth, unsigned char tag,
                              long long count, const unsigned char *value_start,
                              const unsigned char *children_start)
{
    if (count < 0) {
        if (tag != '*' || count != -1)
            return C_ERR;
        size_t null_index;
        return appendRespEntry(parser, tag, value_start - parser->start,
                               parser->p - value_start, 0, &null_index);
    }
    uint64_t elements = (uint64_t)count;
    if (tag == '%' || tag == '|') elements *= 2;
    if (elements > UINT32_MAX)
        return C_ERR;

    size_t entry_index;
    if (appendRespEntry(parser, tag, value_start - parser->start, 0,
                        (uint32_t)elements, &entry_index) != C_OK)
        return C_ERR;
    for (uint64_t i = 0; i < elements; i++) {
        if (parseRespValue(parser, depth + 1) != C_OK)
            return C_ERR;
    }
    if (tag == '|') {
        if (parseRespValue(parser, depth + 1) != C_OK)
            return C_ERR;
    }
    parser->entries[entry_index].length = (uint32_t)(parser->p - value_start);
    UNUSED(children_start);
    return C_OK;
}

static int parseRespValue(wasmRespParser *parser, unsigned depth) {
    if (depth > WASM_RESP_MAX_DEPTH || parser->p >= parser->end)
        return C_ERR;

    const unsigned char *value_start = parser->p;
    unsigned char tag = *parser->p++;
    const unsigned char *line_end;
    const unsigned char *after;
    long long len;
    size_t entry_index;

    switch (tag) {
    case '+':
    case '-':
    case ':':
    case ',':
    case '(':
        if (findLineEnd(parser->p, parser->end, &line_end) != C_OK)
            return C_ERR;
        if (tag == ',') parser->saw_resp3 = 1;
        if (tag == '(') parser->saw_resp3 = 1;
        if (appendRespEntry(parser, tag, parser->p - parser->start,
                            line_end - parser->p, 0, &entry_index) != C_OK)
            return C_ERR;
        parser->p = line_end + 2;
        return C_OK;
    case '_':
        parser->saw_resp3 = 1;
        if (parser->p + 2 > parser->end || parser->p[0] != '\r' || parser->p[1] != '\n')
            return C_ERR;
        if (appendRespEntry(parser, tag, parser->p - parser->start, 0, 0,
                            &entry_index) != C_OK)
            return C_ERR;
        parser->p += 2;
        return C_OK;
    case '#':
        parser->saw_resp3 = 1;
        if (parser->p + 3 > parser->end ||
            (parser->p[0] != 't' && parser->p[0] != 'f') ||
            parser->p[1] != '\r' || parser->p[2] != '\n')
            return C_ERR;
        if (appendRespEntry(parser, tag, parser->p - parser->start, 1, 0,
                            &entry_index) != C_OK)
            return C_ERR;
        parser->p += 3;
        return C_OK;
    case '$':
    case '!':
    case '=':
        if (tag != '$') parser->saw_resp3 = 1;
        if (parseUnsignedLine(parser->p, parser->end, &after, &len) != C_OK)
            return C_ERR;
        if (len == -1 && tag == '$') {
            if (appendRespEntry(parser, tag, after - parser->start, 0, 0,
                                &entry_index) != C_OK)
                return C_ERR;
            parser->p = after;
            return C_OK;
        }
        if (len < 0 || (uint64_t)len > (uint64_t)(parser->end - after) ||
            (size_t)(parser->end - after) < (size_t)len + 2)
            return C_ERR;
        if (after[len] != '\r' || after[len + 1] != '\n')
            return C_ERR;
        if (tag == '=' && len < 4)
            return C_ERR;
        if (appendRespEntry(parser, tag, after - parser->start, (size_t)len, 0,
                            &entry_index) != C_OK)
            return C_ERR;
        parser->p = after + len + 2;
        return C_OK;
    case '*':
    case '~':
    case '%':
    case '|':
    case '>':
        if (tag != '*') parser->saw_resp3 = 1;
        if (parseUnsignedLine(parser->p, parser->end, &after, &len) != C_OK)
            return C_ERR;
        parser->p = after;
        return parseRespAggregate(parser, depth, tag, len, value_start, after);
    default:
        return C_ERR;
    }
}

static int validateResp(const void *data, size_t len, wasmRespEntry **entries,
                        size_t *entry_count, int *saw_resp3)
{
    wasmRespParser parser = {
        .start = data,
        .p = data,
        .end = (const unsigned char *)data + len,
    };
    if (len == 0 || parseRespValue(&parser, 0) != C_OK || parser.p != parser.end) {
        zfree(parser.entries);
        return C_ERR;
    }
    if (entries) {
        *entries = parser.entries;
        *entry_count = parser.entries_len;
    } else {
        zfree(parser.entries);
    }
    if (saw_resp3) *saw_resp3 = parser.saw_resp3;
    return C_OK;
}

static sds drainClientReply(client *c) {
    sds reply = sdsnewlen(c->buf, c->bufpos);
    c->bufpos = 0;
    while (listLength(c->reply)) {
        clientReplyBlock *block = listNodeValue(listFirst(c->reply));
        reply = sdscatlen(reply, block->buf, block->used);
        listDelNode(c->reply, listFirst(c->reply));
    }
    c->reply_bytes = c->reply_bytes_shared = c->reply_bytes_unshared = 0;
    return reply;
}

static void cleanupScriptClient(client *c) {
    for (int i = 0; i < c->argc; i++)
        decrRefCount(c->argv[i]);
    zfree(c->argv);
    c->argc = c->argv_len = 0;
    c->user = NULL;
    c->argv = NULL;
    c->all_argv_len_sum = 0;
    resetClient(c, 1);
}

/* Takes ownership of argv and every object it contains. */
static int wasmRunRedisCommand(wasmHostCtx *ctx, robj **argv, int argc, sds *reply) {
    client *c = ctx->run_ctx->c;
    c->argv = argv;
    c->argc = c->argv_len = argc;

    sds call_error = NULL;
    scriptCall(ctx->run_ctx, &call_error);
    if (call_error) {
        wasmSetLastErrorSds(ctx, call_error);
        cleanupScriptClient(c);
        return C_ERR;
    }
    *reply = drainClientReply(c);
    cleanupScriptClient(c);
    return C_OK;
}

static sds packInput(robj **keys, size_t nkeys, robj **args, size_t nargs) {
    if (nkeys + nargs > UINT32_MAX)
        return NULL;
    size_t total = 4;
    for (size_t i = 0; i < nkeys + nargs; i++) {
        robj *o = i < nkeys ? keys[i] : args[i - nkeys];
        size_t len = stringObjectLen(o);
        if (len > UINT32_MAX || total > SIZE_MAX - 4 - len)
            return NULL;
        total += 4 + len;
    }
    sds packed = sdsnewlen(NULL, total);
    unsigned char *p = (unsigned char *)packed;
    writeU32LE(p, (uint32_t)(nkeys + nargs));
    p += 4;
    for (size_t i = 0; i < nkeys + nargs; i++) {
        robj *o = i < nkeys ? keys[i] : args[i - nkeys];
        robj *decoded = getDecodedObject(o);
        size_t len = sdslen(decoded->ptr);
        char *str = decoded->ptr;
        writeU32LE(p, (uint32_t)len);
        p += 4;
        memcpy(p, str, len);
        p += len;
        decrRefCount(decoded);
    }
    return packed;
}

static void wasmLibraryRelease(wasmLibraryCtx *library) {
    serverAssert(library->refs > 0);
    if (--library->refs != 0)
        return;
    if (library->exec_env) wasm_runtime_destroy_exec_env(library->exec_env);
    if (library->instance) wasm_runtime_deinstantiate(library->instance);
    if (library->module) wasm_runtime_unload(library->module);
    dictRelease(library->blob_types);
    sdsfree(library->binary);
    sdsfree(library->name);
    sdsfree(library->blob_owner);
    zfree(library);
}

static int validateNoArgsI32(wasmLibraryCtx *library, wasm_function_inst_t function) {
    wasm_valkind_t result_type;
    if (!function ||
        wasm_func_get_param_count(function, library->instance) != 0 ||
        wasm_func_get_result_count(function, library->instance) != 1)
        return C_ERR;
    wasm_func_get_result_types(function, library->instance, &result_type);
    return result_type == WASM_I32 ? C_OK : C_ERR;
}

static int validateNoArgsVoid(wasmLibraryCtx *library, wasm_function_inst_t function) {
    return function &&
           wasm_func_get_param_count(function, library->instance) == 0 &&
           wasm_func_get_result_count(function, library->instance) == 0
        ? C_OK : C_ERR;
}

static int callNoArgsI32(wasmLibraryCtx *library, wasm_function_inst_t function,
                         int instruction_limit, int32_t *result, sds *err)
{
    wasm_val_t wasm_result = {.kind = WASM_I32};
    wasm_runtime_clear_exception(library->instance);
    wasm_runtime_set_instruction_count_limit(library->exec_env, instruction_limit);
    if (!wasm_runtime_call_wasm_a(library->exec_env, function, 1, &wasm_result, 0, NULL)) {
        const char *exception = wasm_runtime_get_exception(library->instance);
        *err = sdscatprintf(sdsempty(), "%s", exception ? exception : "unknown WebAssembly trap");
        return C_ERR;
    }
    *result = wasm_result.of.i32;
    return C_OK;
}

static int callNoArgsVoid(wasmLibraryCtx *library, wasm_function_inst_t function,
                          int instruction_limit, sds *err)
{
    wasm_runtime_clear_exception(library->instance);
    wasm_runtime_set_instruction_count_limit(library->exec_env, instruction_limit);
    if (!wasm_runtime_call_wasm_a(library->exec_env, function, 0, NULL, 0, NULL)) {
        const char *exception = wasm_runtime_get_exception(library->instance);
        *err = sdscatprintf(sdsempty(), "%s", exception ? exception : "unknown WebAssembly trap");
        return C_ERR;
    }
    return C_OK;
}

static void wasmLog(wasm_exec_env_t exec_env, int32_t level, int32_t ptr, int32_t len) {
    void *msg;
    if (ptr < 0 || len < 0 ||
        wasmGetGuestBuffer(exec_env, (uint32_t)ptr, (uint32_t)len, &msg) != C_OK)
        return;
    int redis_level = LL_NOTICE;
    if (level <= 0) redis_level = LL_DEBUG;
    else if (level >= 2) redis_level = LL_WARNING;
    serverLog(redis_level, "WASM: %.*s", len, (const char *)msg);
}

static int32_t wasmInputLen(wasm_exec_env_t exec_env) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    return ctx && ctx->phase == WASM_HOST_CALL && sdslen(ctx->input) <= INT32_MAX
        ? (int32_t)sdslen(ctx->input) : -1;
}

static int32_t wasmInputRead(wasm_exec_env_t exec_env, int32_t dst, int32_t cap) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    if (!ctx || ctx->phase != WASM_HOST_CALL || dst < 0 || cap < 0)
        return -1;
    if ((size_t)cap < sdslen(ctx->input))
        return sdslen(ctx->input) <= INT32_MAX ? -(int32_t)sdslen(ctx->input) : -1;
    return wasmReadSds(exec_env, ctx->input, (uint32_t)dst, (uint32_t)cap, 0);
}

static int32_t wasmKeysCount(wasm_exec_env_t exec_env) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    if (!ctx || ctx->phase != WASM_HOST_CALL || ctx->nkeys > INT32_MAX)
        return -1;
    return (int32_t)ctx->nkeys;
}

static int unpackCommand(wasm_exec_env_t exec_env, uint32_t offset, uint32_t len,
                         robj ***argv, int *argc, sds *err)
{
    unsigned char *data;
    if (len < 4 || wasmGetGuestBuffer(exec_env, offset, len, (void **)&data) != C_OK) {
        *err = sdsnew("invalid packed command buffer");
        return C_ERR;
    }
    uint32_t count = readU32LE(data);
    if (count == 0 || count > INT_MAX) {
        *err = sdsnew("packed command must contain at least one argument");
        return C_ERR;
    }
    data += 4;
    len -= 4;
    robj **objects = zcalloc(sizeof(*objects) * count);
    for (uint32_t i = 0; i < count; i++) {
        if (len < 4) goto malformed;
        uint32_t item_len = readU32LE(data);
        data += 4;
        len -= 4;
        if (item_len > len) goto malformed;
        objects[i] = createStringObject((char *)data, item_len);
        data += item_len;
        len -= item_len;
    }
    if (len != 0) goto malformed;
    *argv = objects;
    *argc = (int)count;
    return C_OK;

malformed:
    for (uint32_t i = 0; i < count; i++)
        if (objects[i]) decrRefCount(objects[i]);
    zfree(objects);
    *err = sdsnew("malformed packed command buffer");
    return C_ERR;
}

static int32_t wasmCall(wasm_exec_env_t exec_env, int32_t argv_ptr, int32_t argv_len) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    if (!ctx || ctx->phase != WASM_HOST_CALL || argv_ptr < 0 || argv_len < 0)
        return -1;

    if (ctx->staged_reply) {
        sdsfree(ctx->staged_reply);
        ctx->staged_reply = NULL;
    }
    if (ctx->last_error) {
        sdsfree(ctx->last_error);
        ctx->last_error = NULL;
    }

    sds unpack_error = NULL;
    robj **argv;
    int argc;
    if (unpackCommand(exec_env, (uint32_t)argv_ptr, (uint32_t)argv_len,
                      &argv, &argc, &unpack_error) != C_OK)
    {
        wasmSetLastErrorSds(ctx, unpack_error);
        return -1;
    }
    if (wasmRunRedisCommand(ctx, argv, argc, &ctx->staged_reply) != C_OK)
        return -1;
    if (sdslen(ctx->staged_reply) > INT32_MAX) {
        wasmSetLastError(ctx, "command reply is too large");
        sdsfree(ctx->staged_reply);
        ctx->staged_reply = NULL;
        return -1;
    }
    return (int32_t)sdslen(ctx->staged_reply);
}

static int32_t wasmReplyRead(wasm_exec_env_t exec_env, int32_t dst, int32_t cap, int32_t offset) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    if (!ctx || ctx->phase != WASM_HOST_CALL || dst < 0 || cap < 0 || offset < 0)
        return -1;
    return wasmReadSds(exec_env, ctx->staged_reply, (uint32_t)dst, (uint32_t)cap,
                       (uint32_t)offset);
}

static int32_t wasmLastErrorRead(wasm_exec_env_t exec_env, int32_t dst, int32_t cap) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    if (!ctx || dst < 0 || cap < 0)
        return -1;
    if ((size_t)cap < (ctx->last_error ? sdslen(ctx->last_error) : 0))
        return -(int32_t)sdslen(ctx->last_error);
    return wasmReadSds(exec_env, ctx->last_error, (uint32_t)dst, (uint32_t)cap, 0);
}

static int32_t wasmParseResp(wasm_exec_env_t exec_env, int32_t src, int32_t src_len,
                             int32_t out, int32_t out_cap)
{
    void *input;
    void *output;
    wasmRespEntry *entries = NULL;
    size_t entry_count = 0;
    if (src < 0 || src_len < 0 || out < 0 || out_cap < 0 ||
        wasmGetGuestBuffer(exec_env, (uint32_t)src, (uint32_t)src_len, &input) != C_OK ||
        validateResp(input, (size_t)src_len, &entries, &entry_count, NULL) != C_OK)
        return -1;

    size_t needed = entry_count * WASM_RESP_ENTRY_SIZE;
    if (needed > INT32_MAX) {
        zfree(entries);
        return -1;
    }
    if ((size_t)out_cap < needed) {
        zfree(entries);
        return -(int32_t)needed;
    }
    if (wasmGetGuestBuffer(exec_env, (uint32_t)out, (uint32_t)needed, &output) != C_OK) {
        zfree(entries);
        return -1;
    }
    unsigned char *p = output;
    for (size_t i = 0; i < entry_count; i++, p += WASM_RESP_ENTRY_SIZE) {
        writeU32LE(p, entries[i].tag);
        writeU32LE(p + 4, entries[i].offset);
        writeU32LE(p + 8, entries[i].length);
        writeU32LE(p + 12, entries[i].child_count);
    }
    zfree(entries);
    return (int32_t)needed;
}

static int32_t wasmReplyRaw(wasm_exec_env_t exec_env, int32_t ptr, int32_t len) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    void *reply;
    if (!ctx || ctx->phase != WASM_HOST_CALL || ptr < 0 || len < 0 ||
        ctx->final_reply ||
        wasmGetGuestBuffer(exec_env, (uint32_t)ptr, (uint32_t)len, &reply) != C_OK ||
        validateResp(reply, (size_t)len, NULL, NULL, NULL) != C_OK)
        return -1;
    ctx->final_reply = sdsnewlen(reply, len);
    return 0;
}

static void wasmSimpleReply(wasm_exec_env_t exec_env, int32_t ptr, int32_t len, int error) {
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    void *message;
    if (!ctx || ctx->phase != WASM_HOST_CALL || ptr < 0 || len < 0 ||
        ctx->final_reply ||
        wasmGetGuestBuffer(exec_env, (uint32_t)ptr, (uint32_t)len, &message) != C_OK)
        return;
    const unsigned char *p = message;
    for (int32_t i = 0; i < len; i++) {
        if (p[i] == '\r' || p[i] == '\n')
            return;
    }
    char prefix = error ? '-' : '+';
    ctx->final_reply = sdsnewlen(&prefix, 1);
    ctx->final_reply = sdscatlen(ctx->final_reply, message, len);
    ctx->final_reply = sdscatlen(ctx->final_reply, "\r\n", 2);
}

static void wasmStatusReply(wasm_exec_env_t exec_env, int32_t ptr, int32_t len) {
    wasmSimpleReply(exec_env, ptr, len, 0);
}

static void wasmErrorReply(wasm_exec_env_t exec_env, int32_t ptr, int32_t len) {
    wasmSimpleReply(exec_env, ptr, len, 1);
}

static int32_t wasmRegisterFunction(wasm_exec_env_t exec_env, int32_t name_ptr,
                                    int32_t name_len, int32_t export_ptr,
                                    int32_t export_len)
{
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    void *name_data;
    void *export_data;
    if (!ctx || ctx->phase != WASM_HOST_LOAD || name_ptr < 0 || name_len <= 0 ||
        export_ptr < 0 || export_len <= 0 ||
        wasmGetGuestBuffer(exec_env, (uint32_t)name_ptr, (uint32_t)name_len, &name_data) != C_OK ||
        wasmGetGuestBuffer(exec_env, (uint32_t)export_ptr, (uint32_t)export_len, &export_data) != C_OK ||
        memchr(name_data, '\0', name_len) || memchr(export_data, '\0', export_len))
        return -1;

    sds name = sdsnewlen(name_data, name_len);
    sds export_name = sdsnewlen(export_data, export_len);
    wasm_function_inst_t function = wasm_runtime_lookup_function(ctx->library->instance, export_name);
    if (validateNoArgsI32(ctx->library, function) != C_OK) {
        wasmSetLastError(ctx, "registered export must have type () -> i32");
        sdsfree(name);
        sdsfree(export_name);
        return -1;
    }

    wasmFunctionCtx *function_ctx = zmalloc(sizeof(*function_ctx));
    function_ctx->library = ctx->library;
    function_ctx->function = function;
    ctx->library->refs++;
    sds err = NULL;
    if (functionLibCreateFunction(name, function_ctx, ctx->li, NULL, 0, &err) != C_OK) {
        wasmLibraryRelease(ctx->library);
        zfree(function_ctx);
        wasmSetLastErrorSds(ctx, err);
        sdsfree(name);
        sdsfree(export_name);
        return -1;
    }
    sdsfree(export_name);
    return 0;
}

static int wasmValidBlobTypeName(sds name) {
    if (sdslen(name) == 0)
        return C_ERR;
    for (size_t i = 0; i < sdslen(name); i++) {
        unsigned char c = name[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return C_ERR;
    }
    return C_OK;
}

static sds wasmRegisteredBlobType(wasm_exec_env_t exec_env, wasmHostCtx *ctx,
                                  int32_t type_ptr, int32_t type_len)
{
    void *type_data;
    if (!ctx || type_ptr < 0 || type_len <= 0 ||
        wasmGetGuestBuffer(exec_env, (uint32_t)type_ptr, (uint32_t)type_len,
                           &type_data) != C_OK)
        return NULL;

    sds requested = sdsnewlen(type_data, type_len);
    dictEntry *entry = dictFind(ctx->library->blob_types, requested);
    sdsfree(requested);
    if (!entry) {
        wasmSetLastError(ctx, "WASM blob type is not registered by this library");
        return NULL;
    }
    return dictGetKey(entry);
}

static int32_t wasmBlobRegister(wasm_exec_env_t exec_env, int32_t type_ptr,
                                int32_t type_len)
{
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    void *type_data;
    if (!ctx || ctx->phase != WASM_HOST_LOAD || type_ptr < 0 ||
        type_len <= 0 || type_len > 128 ||
        wasmGetGuestBuffer(exec_env, (uint32_t)type_ptr, (uint32_t)type_len,
                           &type_data) != C_OK ||
        memchr(type_data, '\0', type_len))
        return -1;

    sds type = sdsnewlen(type_data, type_len);
    if (wasmValidBlobTypeName(type) != C_OK) {
        wasmSetLastError(ctx, "WASM blob type names may contain only letters, numbers, and underscores");
        sdsfree(type);
        return -1;
    }
    if (dictAdd(ctx->library->blob_types, type, NULL) != DICT_OK) {
        wasmSetLastError(ctx, "WASM blob type is already registered");
        sdsfree(type);
        return -1;
    }
    return 0;
}

static int wasmAuthorizeBlobRead(wasmHostCtx *ctx, sds key) {
    robj **argv = zmalloc(sizeof(*argv) * 2);
    argv[0] = createStringObject("TYPE", 4);
    argv[1] = createStringObject(key, sdslen(key));
    sds reply = NULL;
    int result = wasmRunRedisCommand(ctx, argv, 2, &reply);
    sdsfree(reply);
    return result;
}

static wasmBlob *wasmLookupBlob(wasmHostCtx *ctx, sds key, sds type) {
    if (wasmAuthorizeBlobRead(ctx, key) != C_OK)
        return NULL;

    kvobj *value = dbFind(ctx->run_ctx->c->db, key);
    if (!value) {
        wasmSetLastError(ctx, "WASM blob key does not exist");
        return NULL;
    }
    if (value->type != OBJ_WASM) {
        wasmSetLastError(ctx, "WRONGTYPE key does not hold a WASM blob");
        return NULL;
    }
    wasmBlob *blob = value->ptr;
    if (sdscmp(blob->owner, ctx->library->blob_owner) != 0) {
        wasmSetLastError(ctx, "WASM blob is owned by another module");
        return NULL;
    }
    if (sdscmp(blob->type, type) != 0) {
        wasmSetLastError(ctx, "WASM blob has a different registered type");
        return NULL;
    }
    return blob;
}

static int wasmGetBlobLookupArgs(wasm_exec_env_t exec_env, int32_t key_ptr,
                                 int32_t key_len, int32_t type_ptr,
                                 int32_t type_len, wasmHostCtx **ctx_out,
                                 sds *key_out, sds *type_out)
{
    wasmHostCtx *ctx = wasmGetHostCtx(exec_env);
    void *key_data;
    if (!ctx || ctx->phase != WASM_HOST_CALL || key_ptr < 0 || key_len < 0 ||
        wasmGetGuestBuffer(exec_env, (uint32_t)key_ptr, (uint32_t)key_len,
                           &key_data) != C_OK)
        return C_ERR;
    sds type = wasmRegisteredBlobType(exec_env, ctx, type_ptr, type_len);
    if (!type) return C_ERR;
    *ctx_out = ctx;
    *key_out = sdsnewlen(key_data, key_len);
    *type_out = type;
    return C_OK;
}

static int32_t wasmBlobLen(wasm_exec_env_t exec_env, int32_t key_ptr,
                           int32_t key_len, int32_t type_ptr, int32_t type_len)
{
    wasmHostCtx *ctx;
    sds key, type;
    if (wasmGetBlobLookupArgs(exec_env, key_ptr, key_len, type_ptr, type_len,
                              &ctx, &key, &type) != C_OK)
        return -1;
    wasmBlob *blob = wasmLookupBlob(ctx, key, type);
    sdsfree(key);
    if (!blob || sdslen(blob->payload) > INT32_MAX)
        return -1;
    return (int32_t)sdslen(blob->payload);
}

static int32_t wasmBlobRead(wasm_exec_env_t exec_env, int32_t key_ptr,
                            int32_t key_len, int32_t type_ptr, int32_t type_len,
                            int32_t dst, int32_t cap, int32_t offset)
{
    wasmHostCtx *ctx;
    sds key, type;
    if (dst < 0 || cap < 0 || offset < 0 ||
        wasmGetBlobLookupArgs(exec_env, key_ptr, key_len, type_ptr, type_len,
                              &ctx, &key, &type) != C_OK)
        return -1;
    wasmBlob *blob = wasmLookupBlob(ctx, key, type);
    sdsfree(key);
    if (!blob)
        return -1;
    return wasmReadSds(exec_env, blob->payload, (uint32_t)dst, (uint32_t)cap,
                       (uint32_t)offset);
}

static void wasmSetReplyError(wasmHostCtx *ctx, sds reply, const char *fallback) {
    if (reply && sdslen(reply) > 1 && (reply[0] == '-' || reply[0] == '!')) {
        size_t len = 1;
        while (len < sdslen(reply) && reply[len] != '\r' && reply[len] != '\n')
            len++;
        wasmSetLastErrorSds(ctx, sdsnewlen(reply + 1, len - 1));
    } else {
        wasmSetLastError(ctx, fallback);
    }
}

static int32_t wasmBlobWrite(wasm_exec_env_t exec_env, int32_t key_ptr,
                             int32_t key_len, int32_t type_ptr, int32_t type_len,
                             int32_t src, int32_t src_len)
{
    wasmHostCtx *ctx;
    sds key, type;
    void *payload_data;
    if (src < 0 || src_len < 0 ||
        wasmGetBlobLookupArgs(exec_env, key_ptr, key_len, type_ptr, type_len,
                              &ctx, &key, &type) != C_OK)
        return -1;
    if (wasmGetGuestBuffer(exec_env, (uint32_t)src, (uint32_t)src_len,
                           &payload_data) != C_OK) {
        sdsfree(key);
        return -1;
    }

    robj *key_object = createStringObject(key, sdslen(key));
    robj *blob_object = createWasmBlobObject(sdsdup(ctx->library->blob_owner),
                                             sdsdup(type),
                                             sdsnewlen(payload_data, src_len));
    sds dump = createRawDumpPayload(blob_object, key_object,
                                    ctx->run_ctx->c->db->id,
                                    DUMP_PAYLOAD_SKIP_KEY_META, src_len);
    decrRefCount(blob_object);
    sdsfree(key);

    robj **argv = zmalloc(sizeof(*argv) * 5);
    argv[0] = createStringObject("RESTORE", 7);
    argv[1] = key_object;
    argv[2] = createStringObject("0", 1);
    argv[3] = createStringObject(dump, sdslen(dump));
    argv[4] = createStringObject("REPLACE", 7);
    sdsfree(dump);

    sds reply = NULL;
    if (wasmRunRedisCommand(ctx, argv, 5, &reply) != C_OK)
        return -1;
    if (!reply || sdslen(reply) == 0 || reply[0] == '-' || reply[0] == '!') {
        wasmSetReplyError(ctx, reply, "Failed writing WASM blob");
        sdsfree(reply);
        return -1;
    }
    sdsfree(reply);
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static NativeSymbol wasmNativeSymbols[] = {
    {"log", wasmLog, "(iii)", NULL},
    {"input_len", wasmInputLen, "()i", NULL},
    {"input_read", wasmInputRead, "(ii)i", NULL},
    {"keys_count", wasmKeysCount, "()i", NULL},
    {"call", wasmCall, "(ii)i", NULL},
    {"reply_read", wasmReplyRead, "(iii)i", NULL},
    {"last_error_read", wasmLastErrorRead, "(ii)i", NULL},
    {"parse_resp", wasmParseResp, "(iiii)i", NULL},
    {"reply_raw", wasmReplyRaw, "(ii)i", NULL},
    {"status_reply", wasmStatusReply, "(ii)", NULL},
    {"error_reply", wasmErrorReply, "(ii)", NULL},
    {"register_function", wasmRegisterFunction, "(iiii)i", NULL},
    {"blob_register", wasmBlobRegister, "(ii)i", NULL},
    {"blob_len", wasmBlobLen, "(iiii)i", NULL},
    {"blob_read", wasmBlobRead, "(iiiiiii)i", NULL},
    {"blob_write", wasmBlobWrite, "(iiiiii)i", NULL},
};
#pragma GCC diagnostic pop

static void wasmHostCtxFree(wasmHostCtx *ctx) {
    sdsfree(ctx->input);
    sdsfree(ctx->staged_reply);
    sdsfree(ctx->last_error);
    sdsfree(ctx->final_reply);
}

static int wasmEngineCreate(void *engine_ctx, functionLibInfo *li, sds blob,
                            size_t timeout, sds *err)
{
    UNUSED(engine_ctx);
    UNUSED(timeout);
    const unsigned char *bytes = (unsigned char *)blob;
    size_t length = sdslen(blob);
    if (length && bytes[0] == '\n') {
        bytes++;
        length--;
    }
    if (length > UINT32_MAX || length < 8) {
        *err = sdsnew("Invalid WebAssembly module");
        return C_ERR;
    }

    wasmLibraryCtx *library = zcalloc(sizeof(*library));
    library->refs = 1;
    library->name = sdsdup(li->name);
    library->binary = sdsnewlen(bytes, length);
    unsigned char owner[SHA256_BLOCK_SIZE];
    SHA256_CTX sha256;
    sha256_init(&sha256);
    sha256_update(&sha256, (unsigned char *)library->binary,
                  sdslen(library->binary));
    sha256_final(&sha256, owner);
    serverAssert(SHA256_BLOCK_SIZE == WASM_BLOB_OWNER_LEN);
    library->blob_owner = sdsnewlen(owner, sizeof(owner));
    library->blob_types = dictCreate(&wasmBlobTypeDictType);

    char error_buf[WASM_ERROR_BUF_SIZE] = {0};
    library->module = wasm_runtime_load((uint8_t *)library->binary,
                                        (uint32_t)sdslen(library->binary),
                                        error_buf, sizeof(error_buf));
    if (!library->module) {
        *err = sdscatprintf(sdsempty(), "Error loading WebAssembly module: %s", error_buf);
        goto error;
    }

    InstantiationArgs args = {
        .default_stack_size = WASM_EXEC_STACK_SIZE,
        .host_managed_heap_size = 0,
        .max_memory_pages = WASM_MAX_MEMORY_PAGES,
    };
    library->instance = wasm_runtime_instantiate_ex(library->module, &args,
                                                     error_buf, sizeof(error_buf));
    if (!library->instance) {
        *err = sdscatprintf(sdsempty(), "Error instantiating WebAssembly module: %s", error_buf);
        goto error;
    }
    wasm_memory_inst_t memory = wasm_runtime_lookup_memory(library->instance, "memory");
    if (!memory) {
        *err = sdsnew("WebAssembly module must export memory as 'memory'");
        goto error;
    }
    if (wasm_memory_get_cur_page_count(memory) > WASM_MAX_MEMORY_PAGES ||
        wasm_memory_get_max_page_count(memory) > WASM_MAX_MEMORY_PAGES)
    {
        *err = sdsnew("WebAssembly memory exceeds the 8 MiB limit");
        goto error;
    }
    library->exec_env = wasm_runtime_create_exec_env(library->instance, WASM_EXEC_STACK_SIZE);
    if (!library->exec_env) {
        *err = sdsnew("Failed creating WebAssembly execution environment");
        goto error;
    }

    wasm_function_inst_t abi = wasm_runtime_lookup_function(library->instance, "redis_abi_version");
    wasm_function_inst_t init = wasm_runtime_lookup_function(library->instance, "redis_init");
    if (validateNoArgsI32(library, abi) != C_OK || validateNoArgsI32(library, init) != C_OK) {
        *err = sdsnew("WebAssembly module must export redis_abi_version and redis_init as () -> i32");
        goto error;
    }

    wasmHostCtx host_ctx = {
        .phase = WASM_HOST_LOAD,
        .library = library,
        .li = li,
    };
    wasm_runtime_set_user_data(library->exec_env, &host_ctx);
    int32_t result;
    sds call_error = NULL;
    wasm_function_inst_t initialize = wasm_runtime_lookup_function(library->instance, "_initialize");
    if (initialize) {
        if (validateNoArgsVoid(library, initialize) != C_OK) {
            *err = sdsnew("Optional WebAssembly _initialize export must have type () -> void");
            wasmHostCtxFree(&host_ctx);
            goto error;
        }
        if (callNoArgsVoid(library, initialize, WASM_LOAD_INSTRUCTION_LIMIT, &call_error) != C_OK) {
            *err = sdscatprintf(sdsempty(), "Error running WebAssembly _initialize: %s", call_error);
            sdsfree(call_error);
            wasmHostCtxFree(&host_ctx);
            goto error;
        }
    }
    if (callNoArgsI32(library, abi, WASM_LOAD_INSTRUCTION_LIMIT, &result, &call_error) != C_OK) {
        *err = sdscatprintf(sdsempty(), "Error reading WebAssembly ABI version: %s", call_error);
        sdsfree(call_error);
        wasmHostCtxFree(&host_ctx);
        goto error;
    }
    if (result != WASM_ABI_VERSION) {
        *err = sdscatprintf(sdsempty(), "Unsupported WebAssembly ABI version: %i", result);
        wasmHostCtxFree(&host_ctx);
        goto error;
    }
    if (callNoArgsI32(library, init, WASM_LOAD_INSTRUCTION_LIMIT, &result, &call_error) != C_OK) {
        *err = sdscatprintf(sdsempty(), "Error running redis_init: %s", call_error);
        sdsfree(call_error);
        wasmHostCtxFree(&host_ctx);
        goto error;
    }
    if (result != 0) {
        *err = host_ctx.last_error
            ? sdscatprintf(sdsempty(), "redis_init failed: %s", host_ctx.last_error)
            : sdscatprintf(sdsempty(), "redis_init returned %i", result);
        wasmHostCtxFree(&host_ctx);
        goto error;
    }
    wasmHostCtxFree(&host_ctx);
    wasm_runtime_set_user_data(library->exec_env, NULL);
    wasmLibraryRelease(library); /* Registered functions now own the library. */
    return C_OK;

error:
    wasmLibraryRelease(library);
    return C_ERR;
}

static void wasmEngineCall(scriptRunCtx *run_ctx, void *engine_ctx,
                           void *compiled_function, robj **keys, size_t nkeys,
                           robj **args, size_t nargs)
{
    UNUSED(engine_ctx);
    wasmFunctionCtx *function_ctx = compiled_function;
    wasmLibraryCtx *library = function_ctx->library;
    wasmHostCtx host_ctx = {
        .phase = WASM_HOST_CALL,
        .library = library,
        .run_ctx = run_ctx,
        .nkeys = nkeys,
        .input = packInput(keys, nkeys, args, nargs),
    };
    if (!host_ctx.input) {
        addReplyError(run_ctx->original_client, "Failed packing WebAssembly input");
        return;
    }

    /* Command calls are staged as RESP3 for the guest. */
    scriptSetResp(run_ctx, 3);
    wasm_runtime_set_user_data(library->exec_env, &host_ctx);
    int32_t result;
    sds call_error = NULL;
    if (callNoArgsI32(library, function_ctx->function, WASM_CALL_INSTRUCTION_LIMIT,
                      &result, &call_error) != C_OK)
    {
        addReplyErrorFormat(run_ctx->original_client, "WebAssembly function '%s' trapped: %s",
                            run_ctx->funcname, call_error);
        serverLog(LL_WARNING, "WebAssembly function '%s' trapped: %s",
                  run_ctx->funcname, call_error);
        sdsfree(call_error);
        goto done;
    }
    if (result != 0) {
        if (host_ctx.last_error)
            addReplyErrorFormat(run_ctx->original_client,
                                "WebAssembly function '%s' failed: %s",
                                run_ctx->funcname, host_ctx.last_error);
        else
            addReplyErrorFormat(run_ctx->original_client,
                                "WebAssembly function '%s' returned %i",
                                run_ctx->funcname, result);
        goto done;
    }
    if (!host_ctx.final_reply) {
        addReplyNull(run_ctx->original_client);
        goto done;
    }

    int saw_resp3 = 0;
    if (validateResp(host_ctx.final_reply, sdslen(host_ctx.final_reply),
                     NULL, NULL, &saw_resp3) != C_OK)
    {
        addReplyError(run_ctx->original_client,
                      "WebAssembly function produced an invalid reply");
        goto done;
    }
    if (run_ctx->original_client->resp == 2 && saw_resp3) {
        addReplyError(run_ctx->original_client,
                      "WebAssembly function produced a RESP3-only reply for a RESP2 client");
        goto done;
    }
    addReplyProto(run_ctx->original_client, host_ctx.final_reply,
                  sdslen(host_ctx.final_reply));

done:
    wasm_runtime_set_user_data(library->exec_env, NULL);
    wasmHostCtxFree(&host_ctx);
}

static size_t wasmEngineGetUsedMemory(void *engine_ctx) {
    UNUSED(engine_ctx);
    mem_alloc_info_t info;
    if (!wasm_runtime_get_mem_alloc_info(&info))
        return 0;
    return info.total_size - info.total_free_size;
}

static size_t wasmEngineFunctionMemoryOverhead(void *compiled_function) {
    return zmalloc_size(compiled_function);
}

static size_t wasmEngineMemoryOverhead(void *engine_ctx) {
    return zmalloc_size(engine_ctx);
}

static void wasmEngineFreeFunction(void *engine_ctx, void *compiled_function) {
    UNUSED(engine_ctx);
    wasmFunctionCtx *function_ctx = compiled_function;
    wasmLibraryRelease(function_ctx->library);
    zfree(function_ctx);
}

static void wasmEngineFreeCtx(void *engine_ctx) {
    UNUSED(engine_ctx);
}

int wasmEngineInitEngine(void) {
    if (!wasm_global_ctx) {
        wasmEngineCtx *ctx = zmalloc(sizeof(*ctx));
        ctx->pool_size = WASM_RUNTIME_POOL_SIZE;
        ctx->pool = zmalloc(ctx->pool_size);

        RuntimeInitArgs init_args = {0};
        init_args.mem_alloc_type = Alloc_With_Pool;
        init_args.mem_alloc_option.pool.heap_buf = ctx->pool;
        init_args.mem_alloc_option.pool.heap_size = (uint32_t)ctx->pool_size;
        init_args.native_module_name = "redis";
        init_args.native_symbols = wasmNativeSymbols;
        init_args.n_native_symbols = sizeof(wasmNativeSymbols) / sizeof(wasmNativeSymbols[0]);
        init_args.running_mode = Mode_Interp;
        if (!wasm_runtime_full_init(&init_args)) {
            serverLog(LL_WARNING, "Failed initializing WAMR");
            zfree(ctx->pool);
            zfree(ctx);
            return C_ERR;
        }
        wasm_global_ctx = ctx;
    }

    engine *wasm_engine = zmalloc(sizeof(*wasm_engine));
    *wasm_engine = (engine) {
        .engine_ctx = wasm_global_ctx,
        .create = wasmEngineCreate,
        .call = wasmEngineCall,
        .get_used_memory = wasmEngineGetUsedMemory,
        .get_function_memory_overhead = wasmEngineFunctionMemoryOverhead,
        .get_engine_memory_overhead = wasmEngineMemoryOverhead,
        .free_function = wasmEngineFreeFunction,
        .free_ctx = wasmEngineFreeCtx,
    };
    return functionsRegisterEngine(WASM_ENGINE_NAME, wasm_engine);
}
