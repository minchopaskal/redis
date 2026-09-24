/* GCRA algorithm adapted from src/gcra.c (Redis Ltd., RSALv2/SSPLv1/AGPLv3).
 * Freestanding C guest: Redis provides time, blob persistence and expiration. */
#include "../../sdk/wasm/c/redis_wasm.h"
#include <limits.h>

static uint8_t input[8192], packed[8192], reply[1024];
static redis_wasm_slice items[7];
static const redis_wasm_slice type = {(const uint8_t *)"gcra_v1", 7};

static redis_wasm_slice str(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return (redis_wasm_slice){(const uint8_t *)s, n};
}

static int error(const char *s) { redis_wasm_error(s); return 0; }
static int host_error(void) {
    int n = redis_wasm_host_last_error_read(reply, sizeof(reply));
    if (n > 0 && n <= (int)sizeof(reply)) redis_wasm_host_error_reply(reply, n);
    else redis_wasm_error("GCRA host operation failed");
    return 0;
}

static int command(redis_wasm_slice *argv, unsigned argc) {
    unsigned pos = 4;
    redis_wasm_put_u32(packed, argc);
    for (unsigned i = 0; i < argc; i++) {
        if (argv[i].len > sizeof(packed) - pos - 4) return -1;
        redis_wasm_put_u32(packed + pos, argv[i].len);
        pos += 4;
        redis_wasm_copy(packed + pos, argv[i].data, argv[i].len);
        pos += argv[i].len;
    }
    int n = redis_wasm_host_call(packed, pos);
    if (n <= 0 || n > (int)sizeof(reply) ||
        redis_wasm_host_reply_read(reply, n, 0) != n) return -1;
    if (reply[0] == '-' || reply[0] == '!') {
        redis_wasm_host_reply_raw(reply, n);
        return -2;
    }
    return n;
}

static int number(redis_wasm_slice s, int64_t *out) {
    int64_t v = 0;
    if (!s.len) return 0;
    for (unsigned i = 0; i < s.len; i++) {
        unsigned d = s.data[i] - '0';
        if (d > 9 || v > (INT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

/* Decimal seconds with at most six fractional digits, without floating point. */
static int period_us(redis_wasm_slice s, int64_t *out) {
    unsigned dot = 0;
    while (dot < s.len && s.data[dot] != '.') dot++;
    int64_t whole, fraction = 0;
    if (!number((redis_wasm_slice){s.data, dot}, &whole) || whole >= 1000000000000LL)
        return 0;
    unsigned digits = dot == s.len ? 0 : s.len - dot - 1;
    if (dot < s.len && (!digits || digits > 6 ||
        !number((redis_wasm_slice){s.data + dot + 1, digits}, &fraction))) return 0;
    for (; digits < 6; digits++) fraction *= 10;
    *out = whole * 1000000 + fraction;
    return *out > 0;
}

static int clock_us(int64_t *now) {
    redis_wasm_slice args[] = {str("TIME")};
    int n = command(args, 1);
    if (n < 0) return n;
    if (n < 4 || reply[0] != '*' || reply[1] != '2') return -1;
    unsigned p = 4;
    int64_t parts[2];
    for (int i = 0; i < 2; i++) {
        if (p >= (unsigned)n || reply[p++] != '$') return -1;
        unsigned start = p;
        while (p < (unsigned)n && reply[p] != '\r') p++;
        int64_t len;
        if (!number((redis_wasm_slice){reply + start, p - start}, &len) || len > 20)
            return -1;
        p += 2;
        if (p + len + 2 > (unsigned)n ||
            !number((redis_wasm_slice){reply + p, (uint32_t)len}, &parts[i])) return -1;
        p += len + 2;
    }
    if (parts[0] > INT64_MAX / 1000000 || parts[1] >= 1000000) return -1;
    /* Match the native command's millisecond clock snapshot granularity. */
    *now = parts[0] * 1000000 + (parts[1] / 1000) * 1000;
    return 0;
}

static unsigned decimal(uint8_t *dst, int64_t value) {
    uint8_t tmp[20];
    unsigned p = 0, n = 0;
    if (value < 0) { dst[p++] = '-'; value = -value; }
    do { tmp[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) dst[p++] = tmp[--n];
    return p;
}

REDIS_WASM_EXPORT("redis_abi_version") int32_t redis_abi_version(void) { return 1; }
REDIS_WASM_EXPORT("redis_init") int32_t redis_init(void) {
    return redis_wasm_register_blob_type("gcra_v1") ||
           redis_wasm_register_function("GCRA", "GCRA");
}

REDIS_WASM_EXPORT("GCRA") int32_t gcra(void) {
    redis_wasm_input in;
    if (redis_wasm_read_input(input, sizeof(input), items, 7, &in) ||
        in.keys_count != 1 || (in.count != 4 && in.count != 6))
        return error("GCRA expects one key, max_burst, tokens_per_period, period [TOKENS count]");
    int64_t capacity, rate, period, cost = 1;
    if (!number(items[1], &capacity) || capacity == INT64_MAX ||
        !number(items[2], &rate) || rate == 0 || !period_us(items[3], &period))
        return error("Invalid GCRA parameters (period: decimal seconds, up to six fractional digits)");
    capacity++;
    if (in.count == 6) {
        if (items[4].len != 6) return error("Expected TOKENS count");
        const char *word = "tokens";
        for (unsigned i = 0; i < 6; i++)
            if ((items[4].data[i] | 32) != word[i]) return error("Expected TOKENS count");
        if (!number(items[5], &cost) || cost == 0) return error("TOKENS must be positive");
    }
    int64_t interval = period / rate;
    if (period % rate >= rate / 2 + rate % 2) interval++;
    if (!interval) interval = 1;
    if (interval > INT64_MAX / capacity || interval > INT64_MAX / cost)
        return error("GCRA parameters overflow microsecond arithmetic");
    int64_t now;
    int rc = clock_us(&now);
    if (rc < 0) return rc == -2 ? 0 : host_error();
    redis_wasm_slice exists[] = {str("EXISTS"), items[0]};
    rc = command(exists, 2);
    if (rc < 0) return rc == -2 ? 0 : host_error();
    int64_t tat = now;
    uint8_t state[12] = {'G','C','R',1};
    if (rc != 4 || reply[0] != ':' || (reply[1] != '0' && reply[1] != '1'))
        return error("Unexpected EXISTS reply");
    if (reply[1] == '1') {
        if (redis_wasm_blob_read(items[0], type, state, sizeof(state)) != sizeof(state))
            return host_error();
        if (state[0] != 'G' || state[1] != 'C' || state[2] != 'R' || state[3] != 1)
            return error("Invalid GCRA blob version");
        uint64_t stored = (uint64_t)redis_wasm_u32(state + 4) |
                          (uint64_t)redis_wasm_u32(state + 8) << 32;
        if (stored > INT64_MAX) return error("Invalid GCRA theoretical arrival time");
        tat = stored;
    }
    int64_t increment = interval * cost, variance = interval * capacity;
    int64_t base = now > tat ? now : tat;
    if (base > INT64_MAX - increment) return error("GCRA timestamp overflow");
    int64_t next = base + increment;
    int64_t diff = now - (next - variance);
    int64_t limited = diff < 0, retry = -1, ttl;
    if (limited) {
        if (increment <= variance) retry = (-diff) / 1000000 + ((-diff) % 1000000 != 0);
        ttl = tat > now ? tat - now : 0;
    } else {
        ttl = next - now;
        redis_wasm_put_u32(state + 4, (uint32_t)next);
        redis_wasm_put_u32(state + 8, (uint32_t)((uint64_t)next >> 32));
        /* Expire no earlier than the stored TAT, including sub-ms intervals. */
        if (redis_wasm_host_blob_write_expire(items[0].data, items[0].len, type.data, type.len,
                              state, sizeof(state), next / 1000 + (next % 1000 != 0)))
            return host_error();
    }
    int64_t remaining = variance > ttl ? (variance - ttl) / interval : 0;
    int64_t values[] = {limited, capacity, remaining, retry,
                       ttl / 1000000 + (ttl % 1000000 != 0)};
    unsigned pos = 4;
    reply[0] = '*'; reply[1] = '5'; reply[2] = '\r'; reply[3] = '\n';
    for (unsigned i = 0; i < 5; i++) {
        reply[pos++] = ':';
        pos += decimal(reply + pos, values[i]);
        reply[pos++] = '\r'; reply[pos++] = '\n';
    }
    return redis_wasm_host_reply_raw(reply, pos) < 0 ? 1 : 0;
}
