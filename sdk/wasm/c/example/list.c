#include "../redis_wasm.h"

#define BUFFER_SIZE 16384
#define MAX_ITEMS 256

static uint8_t input_buffer[BUFFER_SIZE];
static uint8_t blob_buffer[BUFFER_SIZE];
static uint8_t reply_buffer[64];
static redis_wasm_slice input_items[MAX_ITEMS];

static const uint8_t list_type_name[] = "c_linked_list";
static const redis_wasm_slice list_type = {
    list_type_name, sizeof(list_type_name) - 1
};

static int write_node(uint8_t *buffer, uint32_t cap, uint32_t *cursor,
                      int32_t prev, int32_t next, redis_wasm_slice value)
{
    if (*cursor > cap || cap - *cursor < 12 ||
        value.len > cap - *cursor - 12)
        return REDIS_WASM_BUFFER_TOO_SMALL;
    redis_wasm_put_u32(buffer + *cursor, (uint32_t)prev);
    redis_wasm_put_u32(buffer + *cursor + 4, (uint32_t)next);
    redis_wasm_put_u32(buffer + *cursor + 8, value.len);
    redis_wasm_copy(buffer + *cursor + 12, value.data, value.len);
    *cursor += 12 + value.len;
    return REDIS_WASM_OK;
}

static int validate_list(uint8_t *buffer, uint32_t len, uint32_t *count_out,
                         uint32_t *tail_next_offset)
{
    if (len < 12 || buffer[0] != 'C' || buffer[1] != 'L' ||
        buffer[2] != 'S' || buffer[3] != 'T' ||
        redis_wasm_u32(buffer + 4) != 1)
        return REDIS_WASM_INVALID_INPUT;

    uint32_t count = redis_wasm_u32(buffer + 8);
    uint32_t cursor = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (cursor > len || len - cursor < 12)
            return REDIS_WASM_INVALID_INPUT;
        int32_t prev = (int32_t)redis_wasm_u32(buffer + cursor);
        int32_t next = (int32_t)redis_wasm_u32(buffer + cursor + 4);
        uint32_t value_len = redis_wasm_u32(buffer + cursor + 8);
        if (prev != (i == 0 ? -1 : (int32_t)i - 1) ||
            next != (i + 1 == count ? -1 : (int32_t)i + 1) ||
            value_len > len - cursor - 12)
            return REDIS_WASM_INVALID_INPUT;
        if (i + 1 == count) *tail_next_offset = cursor + 4;
        cursor += 12 + value_len;
    }
    if (cursor != len)
        return REDIS_WASM_INVALID_INPUT;
    *count_out = count;
    return REDIS_WASM_OK;
}

REDIS_WASM_EXPORT("redis_abi_version")
int32_t redis_abi_version(void) {
    return REDIS_WASM_ABI_VERSION;
}

REDIS_WASM_EXPORT("redis_init")
int32_t redis_init(void) {
    if (redis_wasm_register_blob_type((const char *)list_type_name) != 0)
        return 1;
    if (redis_wasm_register_function("c_list_create", "c_list_create") != 0)
        return 2;
    if (redis_wasm_register_function("c_list_push", "c_list_push") != 0)
        return 3;
    if (redis_wasm_register_function("c_list_len", "c_list_len") != 0)
        return 4;
    return 0;
}

REDIS_WASM_EXPORT("c_list_create")
int32_t c_list_create(void) {
    redis_wasm_input input;
    if (redis_wasm_read_input(input_buffer, sizeof(input_buffer), input_items,
                              MAX_ITEMS, &input) != REDIS_WASM_OK ||
        input.keys_count != 1 || input.count <= 1) {
        redis_wasm_error("c_list_create expects one key and one or more values");
        return 0;
    }

    blob_buffer[0] = 'C';
    blob_buffer[1] = 'L';
    blob_buffer[2] = 'S';
    blob_buffer[3] = 'T';
    redis_wasm_put_u32(blob_buffer + 4, 1);
    uint32_t count = input.count - 1;
    redis_wasm_put_u32(blob_buffer + 8, count);
    uint32_t cursor = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (write_node(blob_buffer, sizeof(blob_buffer), &cursor,
                       i == 0 ? -1 : (int32_t)i - 1,
                       i + 1 == count ? -1 : (int32_t)i + 1,
                       input.items[i + 1]) != REDIS_WASM_OK) {
            redis_wasm_error("serialized list exceeds the C SDK buffer");
            return 0;
        }
    }
    if (redis_wasm_blob_write(input.items[0], list_type,
                              blob_buffer, cursor) != 0) {
        redis_wasm_error("failed writing C linked-list blob");
        return 0;
    }
    redis_wasm_status("C list created");
    return 0;
}

REDIS_WASM_EXPORT("c_list_push")
int32_t c_list_push(void) {
    redis_wasm_input input;
    if (redis_wasm_read_input(input_buffer, sizeof(input_buffer), input_items,
                              MAX_ITEMS, &input) != REDIS_WASM_OK ||
        input.keys_count != 1 || input.count != 2) {
        redis_wasm_error("c_list_push expects one key and one value");
        return 0;
    }

    int32_t len = redis_wasm_blob_read(input.items[0], list_type,
                                       blob_buffer, sizeof(blob_buffer));
    if (len < 0) {
        redis_wasm_error("failed reading C linked-list blob");
        return 0;
    }
    uint32_t count, tail_next_offset = 0;
    if (validate_list(blob_buffer, len, &count, &tail_next_offset) != REDIS_WASM_OK ||
        count == 0) {
        redis_wasm_error("invalid C linked-list blob");
        return 0;
    }
    redis_wasm_put_u32(blob_buffer + tail_next_offset, count);
    uint32_t cursor = (uint32_t)len;
    if (write_node(blob_buffer, sizeof(blob_buffer), &cursor,
                   (int32_t)count - 1, -1, input.items[1]) != REDIS_WASM_OK) {
        redis_wasm_error("serialized list exceeds the C SDK buffer");
        return 0;
    }
    redis_wasm_put_u32(blob_buffer + 8, count + 1);
    if (redis_wasm_blob_write(input.items[0], list_type,
                              blob_buffer, cursor) != 0) {
        redis_wasm_error("failed updating C linked-list blob");
        return 0;
    }
    redis_wasm_status("C list updated");
    return 0;
}

REDIS_WASM_EXPORT("c_list_len")
int32_t c_list_len(void) {
    redis_wasm_input input;
    if (redis_wasm_read_input(input_buffer, sizeof(input_buffer), input_items,
                              MAX_ITEMS, &input) != REDIS_WASM_OK ||
        input.keys_count != 1 || input.count != 1) {
        redis_wasm_error("c_list_len expects one key");
        return 0;
    }
    int32_t len = redis_wasm_blob_read(input.items[0], list_type,
                                       blob_buffer, sizeof(blob_buffer));
    uint32_t count, tail_next_offset = 0;
    if (len < 0 ||
        validate_list(blob_buffer, len, &count, &tail_next_offset) != REDIS_WASM_OK) {
        redis_wasm_error("invalid C linked-list blob");
        return 0;
    }
    if (redis_wasm_reply_integer(count, reply_buffer,
                                 sizeof(reply_buffer)) != 0)
        return 1;
    return 0;
}
