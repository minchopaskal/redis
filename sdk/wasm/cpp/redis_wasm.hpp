#ifndef REDIS_WASM_CPP_SDK_HPP
#define REDIS_WASM_CPP_SDK_HPP

#include "../c/redis_wasm.h"

namespace redis::wasm {

struct Slice {
    const uint8_t *data;
    uint32_t size;

    constexpr redis_wasm_slice c_slice() const { return {data, size}; }
};

inline Slice literal(const char *value) {
    uint32_t len = 0;
    while (value[len]) ++len;
    return {reinterpret_cast<const uint8_t *>(value), len};
}

template <uint32_t BufferSize, uint32_t MaxItems>
class InputBuffer {
public:
    bool read() {
        redis_wasm_input input;
        if (redis_wasm_read_input(buffer_, BufferSize, items_, MaxItems,
                                  &input) != REDIS_WASM_OK)
            return false;
        count_ = input.count;
        keys_ = input.keys_count;
        return true;
    }

    uint32_t count() const { return count_; }
    uint32_t keys_count() const { return keys_; }
    uint32_t args_count() const { return count_ - keys_; }

    Slice item(uint32_t index) const {
        return {items_[index].data, items_[index].len};
    }

    Slice key(uint32_t index) const { return item(index); }
    Slice arg(uint32_t index) const { return item(keys_ + index); }

private:
    uint8_t buffer_[BufferSize];
    redis_wasm_slice items_[MaxItems];
    uint32_t count_;
    uint32_t keys_;
};

class BlobType {
public:
    explicit BlobType(Slice name) : name_(name) {}

    bool register_type() const {
        return redis_wasm_host_blob_register(name_.data, name_.size) == 0;
    }

    Slice name() const { return name_; }

private:
    Slice name_;
};

/* FlatBlob intentionally stores trivially-copyable T bytes. Blob ownership is
 * tied to the module hash, so a different binary (and potentially different
 * C++ ABI/layout) cannot read the value accidentally. */
template <typename T>
class FlatBlob {
    static_assert(__is_trivially_copyable(T),
                  "FlatBlob requires a trivially-copyable type");

public:
    explicit FlatBlob(BlobType type) : type_(type) {}

    bool write(Slice key, const T &value, uint8_t *scratch,
               uint32_t scratch_size) const {
        constexpr uint32_t header_size = 12;
        if (scratch_size < header_size + sizeof(T)) return false;
        scratch[0] = 'C';
        scratch[1] = 'X';
        scratch[2] = 'X';
        scratch[3] = 'F';
        redis_wasm_put_u32(scratch + 4, 1);
        redis_wasm_put_u32(scratch + 8, sizeof(T));
        redis_wasm_copy(scratch + header_size,
                        reinterpret_cast<const uint8_t *>(&value), sizeof(T));
        return redis_wasm_blob_write(key.c_slice(), type_.name().c_slice(),
                                     scratch, header_size + sizeof(T)) == 0;
    }

    bool read(Slice key, T &value, uint8_t *scratch,
              uint32_t scratch_size) const {
        int32_t len = redis_wasm_blob_read(key.c_slice(), type_.name().c_slice(),
                                          scratch, scratch_size);
        constexpr uint32_t expected = 12 + sizeof(T);
        if (len != static_cast<int32_t>(expected) ||
            scratch[0] != 'C' || scratch[1] != 'X' ||
            scratch[2] != 'X' || scratch[3] != 'F' ||
            redis_wasm_u32(scratch + 4) != 1 ||
            redis_wasm_u32(scratch + 8) != sizeof(T))
            return false;
        redis_wasm_copy(reinterpret_cast<uint8_t *>(&value),
                        scratch + 12, sizeof(T));
        return true;
    }

private:
    BlobType type_;
};

inline bool register_function(const char *name, const char *export_name) {
    return redis_wasm_register_function(name, export_name) == 0;
}

inline void status(const char *message) { redis_wasm_status(message); }
inline void error(const char *message) { redis_wasm_error(message); }

inline bool reply_integer(uint32_t value, uint8_t *scratch, uint32_t size) {
    return redis_wasm_reply_integer(value, scratch, size) == 0;
}

} // namespace redis::wasm

#endif
