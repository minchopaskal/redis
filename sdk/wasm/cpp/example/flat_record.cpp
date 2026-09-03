#include "../redis_wasm.hpp"

using redis::wasm::BlobType;
using redis::wasm::FlatBlob;
using redis::wasm::InputBuffer;
using redis::wasm::Slice;

template <typename Number>
struct FlatRecord {
    uint32_t id;
    Number x;
    Number y;
    Number z;
    uint32_t flags;
};

using IntRecord = FlatRecord<int32_t>;

static InputBuffer<8192, 16> input;
static uint8_t blob_scratch[256];
static uint8_t reply_scratch[64];
static const char record_type_name[] = "cpp_flat_record_i32";

static BlobType record_type() {
    return BlobType(redis::wasm::literal(record_type_name));
}

static bool parse_i32(Slice text, int32_t &out) {
    if (text.size == 0) return false;
    uint32_t cursor = 0;
    bool negative = false;
    if (text.data[0] == '-') {
        negative = true;
        cursor++;
    }
    if (cursor == text.size) return false;
    int64_t value = 0;
    for (; cursor < text.size; cursor++) {
        uint8_t c = text.data[cursor];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        int64_t limit = negative ? 2147483648LL : 2147483647LL;
        if (value > limit) return false;
    }
    out = static_cast<int32_t>(negative ? -value : value);
    return true;
}

REDIS_WASM_EXPORT("redis_abi_version")
int32_t redis_abi_version() {
    return REDIS_WASM_ABI_VERSION;
}

REDIS_WASM_EXPORT("redis_init")
int32_t redis_init() {
    if (!record_type().register_type()) return 1;
    if (!redis::wasm::register_function("cpp_record_set", "cpp_record_set"))
        return 2;
    if (!redis::wasm::register_function("cpp_record_sum", "cpp_record_sum"))
        return 3;
    return 0;
}

REDIS_WASM_EXPORT("cpp_record_set")
int32_t cpp_record_set() {
    if (!input.read() || input.keys_count() != 1 || input.args_count() != 5) {
        redis::wasm::error("cpp_record_set expects key, id, x, y, z, and flags");
        return 0;
    }

    int32_t id, x, y, z, flags;
    if (!parse_i32(input.arg(0), id) || id < 0 ||
        !parse_i32(input.arg(1), x) ||
        !parse_i32(input.arg(2), y) ||
        !parse_i32(input.arg(3), z) ||
        !parse_i32(input.arg(4), flags) || flags < 0) {
        redis::wasm::error("record fields must be signed 32-bit integers");
        return 0;
    }

    IntRecord record = {
        static_cast<uint32_t>(id), x, y, z, static_cast<uint32_t>(flags)
    };
    FlatBlob<IntRecord> blob(record_type());
    if (!blob.write(input.key(0), record, blob_scratch, sizeof(blob_scratch))) {
        redis::wasm::error("failed writing templated C++ flat record");
        return 0;
    }
    redis::wasm::status("C++ record stored");
    return 0;
}

REDIS_WASM_EXPORT("cpp_record_sum")
int32_t cpp_record_sum() {
    if (!input.read() || input.keys_count() != 1 || input.args_count() != 0) {
        redis::wasm::error("cpp_record_sum expects one key");
        return 0;
    }
    IntRecord record = {};
    FlatBlob<IntRecord> blob(record_type());
    if (!blob.read(input.key(0), record, blob_scratch, sizeof(blob_scratch))) {
        redis::wasm::error("failed reading templated C++ flat record");
        return 0;
    }
    int64_t sum = static_cast<int64_t>(record.x) + record.y + record.z;
    if (sum < 0 || sum > 0xffffffffLL ||
        !redis::wasm::reply_integer(static_cast<uint32_t>(sum),
                                    reply_scratch, sizeof(reply_scratch)))
        return 1;
    return 0;
}
