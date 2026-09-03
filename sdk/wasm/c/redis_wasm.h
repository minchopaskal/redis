#ifndef REDIS_WASM_C_SDK_H
#define REDIS_WASM_C_SDK_H

#include <stddef.h>
#include <stdint.h>

#define REDIS_WASM_ABI_VERSION 1
#define REDIS_WASM_OK 0
#define REDIS_WASM_ERR (-1)
#define REDIS_WASM_BUFFER_TOO_SMALL (-2)
#define REDIS_WASM_INVALID_INPUT (-3)

#define REDIS_WASM_EXPORT(name) \
    __attribute__((export_name(name), used))

#define REDIS_WASM_IMPORT(name) \
    __attribute__((import_module("redis"), import_name(name)))

typedef struct {
    const uint8_t *data;
    uint32_t len;
} redis_wasm_slice;

typedef struct {
    redis_wasm_slice *items;
    uint32_t count;
    uint32_t keys_count;
} redis_wasm_input;

REDIS_WASM_IMPORT("input_len")
int32_t redis_wasm_host_input_len(void);
REDIS_WASM_IMPORT("input_read")
int32_t redis_wasm_host_input_read(uint8_t *dst, int32_t cap);
REDIS_WASM_IMPORT("keys_count")
int32_t redis_wasm_host_keys_count(void);
REDIS_WASM_IMPORT("call")
int32_t redis_wasm_host_call(const uint8_t *argv, int32_t len);
REDIS_WASM_IMPORT("reply_read")
int32_t redis_wasm_host_reply_read(uint8_t *dst, int32_t cap, int32_t offset);
REDIS_WASM_IMPORT("last_error_read")
int32_t redis_wasm_host_last_error_read(uint8_t *dst, int32_t cap);
REDIS_WASM_IMPORT("reply_raw")
int32_t redis_wasm_host_reply_raw(const uint8_t *reply, int32_t len);
REDIS_WASM_IMPORT("status_reply")
void redis_wasm_host_status_reply(const uint8_t *message, int32_t len);
REDIS_WASM_IMPORT("error_reply")
void redis_wasm_host_error_reply(const uint8_t *message, int32_t len);
REDIS_WASM_IMPORT("register_function")
int32_t redis_wasm_host_register_function(const uint8_t *name, int32_t name_len,
                                          const uint8_t *export_name,
                                          int32_t export_len);
REDIS_WASM_IMPORT("blob_register")
int32_t redis_wasm_host_blob_register(const uint8_t *type, int32_t type_len);
REDIS_WASM_IMPORT("blob_len")
int32_t redis_wasm_host_blob_len(const uint8_t *key, int32_t key_len,
                                 const uint8_t *type, int32_t type_len);
REDIS_WASM_IMPORT("blob_read")
int32_t redis_wasm_host_blob_read(const uint8_t *key, int32_t key_len,
                                  const uint8_t *type, int32_t type_len,
                                  uint8_t *dst, int32_t cap, int32_t offset);
REDIS_WASM_IMPORT("blob_write")
int32_t redis_wasm_host_blob_write(const uint8_t *key, int32_t key_len,
                                   const uint8_t *type, int32_t type_len,
                                   const uint8_t *src, int32_t src_len);

static inline uint32_t redis_wasm_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void redis_wasm_put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline void redis_wasm_copy(uint8_t *dst, const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

static inline int32_t redis_wasm_read_input(uint8_t *buffer, uint32_t cap,
                                            redis_wasm_slice *items,
                                            uint32_t item_cap,
                                            redis_wasm_input *out)
{
    int32_t required = redis_wasm_host_input_len();
    int32_t keys = redis_wasm_host_keys_count();
    if (required < 4 || keys < 0 || (uint32_t)required > cap)
        return REDIS_WASM_BUFFER_TOO_SMALL;
    if (redis_wasm_host_input_read(buffer, required) != required)
        return REDIS_WASM_ERR;

    uint32_t count = redis_wasm_u32(buffer);
    if (count > item_cap || (uint32_t)keys > count)
        return REDIS_WASM_BUFFER_TOO_SMALL;
    uint32_t cursor = 4;
    for (uint32_t i = 0; i < count; i++) {
        if (cursor > (uint32_t)required - 4)
            return REDIS_WASM_INVALID_INPUT;
        uint32_t len = redis_wasm_u32(buffer + cursor);
        cursor += 4;
        if (len > (uint32_t)required - cursor)
            return REDIS_WASM_INVALID_INPUT;
        items[i].data = buffer + cursor;
        items[i].len = len;
        cursor += len;
    }
    if (cursor != (uint32_t)required)
        return REDIS_WASM_INVALID_INPUT;
    out->items = items;
    out->count = count;
    out->keys_count = (uint32_t)keys;
    return REDIS_WASM_OK;
}

static inline int32_t redis_wasm_register_function(const char *name,
                                                   const char *export_name)
{
    uint32_t name_len = 0, export_len = 0;
    while (name[name_len]) name_len++;
    while (export_name[export_len]) export_len++;
    return redis_wasm_host_register_function((const uint8_t *)name, name_len,
                                             (const uint8_t *)export_name,
                                             export_len);
}

static inline int32_t redis_wasm_register_blob_type(const char *type) {
    uint32_t len = 0;
    while (type[len]) len++;
    return redis_wasm_host_blob_register((const uint8_t *)type, len);
}

static inline int32_t redis_wasm_blob_read(redis_wasm_slice key,
                                           redis_wasm_slice type,
                                           uint8_t *dst, uint32_t cap)
{
    int32_t len = redis_wasm_host_blob_len(key.data, key.len, type.data, type.len);
    if (len < 0) return REDIS_WASM_ERR;
    if ((uint32_t)len > cap) return REDIS_WASM_BUFFER_TOO_SMALL;
    return redis_wasm_host_blob_read(key.data, key.len, type.data, type.len,
                                    dst, len, 0);
}

static inline int32_t redis_wasm_blob_write(redis_wasm_slice key,
                                            redis_wasm_slice type,
                                            const uint8_t *src, uint32_t len)
{
    return redis_wasm_host_blob_write(key.data, key.len, type.data, type.len,
                                     src, len);
}

static inline void redis_wasm_status(const char *message) {
    uint32_t len = 0;
    while (message[len]) len++;
    redis_wasm_host_status_reply((const uint8_t *)message, len);
}

static inline void redis_wasm_error(const char *message) {
    uint32_t len = 0;
    while (message[len]) len++;
    redis_wasm_host_error_reply((const uint8_t *)message, len);
}

static inline int32_t redis_wasm_reply_integer(uint32_t value,
                                               uint8_t *buffer,
                                               uint32_t cap)
{
    if (cap < 4) return REDIS_WASM_BUFFER_TOO_SMALL;
    uint8_t digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (uint8_t)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    if (count + 3 > cap) return REDIS_WASM_BUFFER_TOO_SMALL;
    buffer[0] = ':';
    for (uint32_t i = 0; i < count; i++)
        buffer[1 + i] = digits[count - i - 1];
    buffer[1 + count] = '\r';
    buffer[2 + count] = '\n';
    return redis_wasm_host_reply_raw(buffer, count + 3);
}

#endif
