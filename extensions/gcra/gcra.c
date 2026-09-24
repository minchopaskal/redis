/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* WASM implementation of the GCRA algorithm in src/gcra.c.
 * Redis provides the clock, blob persistence and key expiration. */
#include "../../sdk/wasm/c/redis_wasm.h"
#include <limits.h>

#define GCRA_INPUT_SIZE 8192
#define GCRA_REPLY_SIZE 1024
#define GCRA_MAX_ARGS 7
#define GCRA_ERR -1
#define GCRA_ERR_REPLIED -2

static uint8_t input_buffer[GCRA_INPUT_SIZE];
static uint8_t command_buffer[GCRA_INPUT_SIZE];
static uint8_t reply_buffer[GCRA_REPLY_SIZE];
static redis_wasm_slice input_items[GCRA_MAX_ARGS];
static const redis_wasm_slice blob_type = {(const uint8_t *)"gcra_v1", 7};

static redis_wasm_slice gcraStringSlice(const char *str) {
    uint32_t len = 0;
    while (str[len]) len++;
    return (redis_wasm_slice){(const uint8_t *)str, len};
}

static int gcraReplyError(const char *message) {
    redis_wasm_error(message);
    return 0;
}

static int gcraReplyHostError(void) {
    int n = redis_wasm_host_last_error_read(reply_buffer, sizeof(reply_buffer));
    if (n > 0 && n <= (int)sizeof(reply_buffer)) {
        redis_wasm_host_error_reply(reply_buffer, n);
    } else {
        redis_wasm_error("GCRA host operation failed");
    }
    return 0;
}

/* Return the reply length, or GCRA_ERR_REPLIED if a Redis error was forwarded. */
static int gcraCall(redis_wasm_slice *argv, unsigned argc) {
    unsigned pos = 4;
    redis_wasm_put_u32(command_buffer, argc);
    for (unsigned i = 0; i < argc; i++) {
        if (pos > sizeof(command_buffer) - 4 ||
            argv[i].len > sizeof(command_buffer) - pos - 4)
        {
            return GCRA_ERR;
        }
        redis_wasm_put_u32(command_buffer + pos, argv[i].len);
        pos += 4;
        redis_wasm_copy(command_buffer + pos, argv[i].data, argv[i].len);
        pos += argv[i].len;
    }
    int n = redis_wasm_host_call(command_buffer, pos);
    if (n <= 0 || n > (int)sizeof(reply_buffer) ||
        redis_wasm_host_reply_read(reply_buffer, n, 0) != n)
    {
        return GCRA_ERR;
    }
    if (reply_buffer[0] == '-' || reply_buffer[0] == '!') {
        redis_wasm_host_reply_raw(reply_buffer, n);
        return GCRA_ERR_REPLIED;
    }
    return n;
}

static int gcraParseInteger(redis_wasm_slice str, int64_t *value) {
    int64_t result = 0;
    if (str.len == 0) return 0;
    for (unsigned i = 0; i < str.len; i++) {
        unsigned digit = str.data[i] - '0';
        if (digit > 9 || result > (INT64_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    *value = result;
    return 1;
}

/* Decimal seconds with at most six fractional digits, without floating point. */
static int gcraParsePeriod(redis_wasm_slice s, int64_t *out) {
    unsigned dot = 0;
    while (dot < s.len && s.data[dot] != '.') dot++;
    int64_t whole, fraction = 0;
    if (!gcraParseInteger((redis_wasm_slice){s.data, dot}, &whole) || whole >= 1000000000000LL)
        return 0;
    unsigned digits = dot == s.len ? 0 : s.len - dot - 1;
    if (dot < s.len && (!digits || digits > 6 ||
        !gcraParseInteger((redis_wasm_slice){s.data + dot + 1, digits}, &fraction)))
    {
        return 0;
    }
    for (; digits < 6; digits++) fraction *= 10;
    *out = whole * 1000000 + fraction;
    return *out > 0;
}

/* TIME returns Unix seconds and microseconds as two RESP bulk strings. */
static int gcraGetTime(int64_t *now) {
    redis_wasm_slice args[] = {gcraStringSlice("TIME")};
    int n = gcraCall(args, 1);
    if (n < 0) return n;
    if (n < 4 || reply_buffer[0] != '*' || reply_buffer[1] != '2') return GCRA_ERR;
    unsigned p = 4;
    int64_t parts[2];
    for (int i = 0; i < 2; i++) {
        if (p >= (unsigned)n || reply_buffer[p++] != '$') return GCRA_ERR;
        unsigned start = p;
        while (p < (unsigned)n && reply_buffer[p] != '\r') p++;
        int64_t len;
        if (!gcraParseInteger((redis_wasm_slice){reply_buffer + start, p - start}, &len) || len > 20) {
            return GCRA_ERR;
        }
        p += 2;
        if (p + len + 2 > (unsigned)n ||
            !gcraParseInteger((redis_wasm_slice){reply_buffer + p, (uint32_t)len}, &parts[i]))
        {
            return GCRA_ERR;
        }
        p += len + 2;
    }
    if (parts[1] >= 1000000 || parts[0] > (INT64_MAX - parts[1]) / 1000000) return GCRA_ERR;
    /* Match the native command's millisecond clock snapshot granularity. */
    *now = parts[0] * 1000000 + (parts[1] / 1000) * 1000;
    return 0;
}

static unsigned gcraFormatInteger(uint8_t *dst, int64_t value) {
    uint8_t tmp[20];
    unsigned p = 0, n = 0;
    if (value < 0) {
        dst[p++] = '-';
        value = -value;
    }
    do {
        tmp[n++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (n) dst[p++] = tmp[--n];
    return p;
}

REDIS_WASM_EXPORT(REDIS_WASM_ABI_MARKER)
void gcraAbiVersion(void) {}

REDIS_WASM_EXPORT("_initialize")
void gcraInitialize(void) {
    if (redis_wasm_register_blob_type("gcra_v1") != REDIS_WASM_OK)
        __builtin_trap();
    if (redis_wasm_create_command("GCRA", "GCRA", -5, 1) != REDIS_WASM_OK)
        __builtin_trap();
}

/* GCRA key max_burst tokens_per_period period [TOKENS count].
 * FCALL GCRA 1 key ... uses the same implementation. */
REDIS_WASM_EXPORT("GCRA")
int32_t gcraCommand(void) {
    redis_wasm_input in;
    if (redis_wasm_read_input(input_buffer, sizeof(input_buffer), input_items,
                             GCRA_MAX_ARGS, &in) != REDIS_WASM_OK ||
        in.keys_count != 1 || (in.count != 4 && in.count != 6))
    {
        return gcraReplyError("GCRA expects one key, max_burst, tokens_per_period, period [TOKENS count]");
    }

    /* GCRA parameters. Burst capacity includes one sustained-rate token. */
    int64_t capacity, tokens_per_period, period_us, num_tokens = 1;
    if (!gcraParseInteger(input_items[1], &capacity) || capacity == INT64_MAX ||
        !gcraParseInteger(input_items[2], &tokens_per_period) || tokens_per_period == 0 ||
        !gcraParsePeriod(input_items[3], &period_us))
    {
        return gcraReplyError("Invalid GCRA parameters (period: decimal seconds, up to six fractional digits)");
    }
    capacity++;
    if (in.count == 6) {
        if (input_items[4].len != 6) return gcraReplyError("Expected TOKENS count");
        const char *word = "tokens";
        for (unsigned i = 0; i < 6; i++) {
            if ((input_items[4].data[i] | 32) != word[i]) return gcraReplyError("Expected TOKENS count");
        }
        if (!gcraParseInteger(input_items[5], &num_tokens) || num_tokens == 0) {
            return gcraReplyError("TOKENS must be positive");
        }
    }

    /* Round to microseconds, with a minimum emission interval of one. */
    int64_t emission_interval_us = period_us / tokens_per_period;
    if (period_us % tokens_per_period >= tokens_per_period / 2 + tokens_per_period % 2) {
        emission_interval_us++;
    }
    if (emission_interval_us == 0) emission_interval_us = 1;
    if (emission_interval_us > INT64_MAX / capacity || emission_interval_us > INT64_MAX / num_tokens) {
        return gcraReplyError("GCRA parameters overflow microsecond arithmetic");
    }

    int64_t now;
    int rc = gcraGetTime(&now);
    if (rc < 0) return rc == GCRA_ERR_REPLIED ? 0 : gcraReplyHostError();
    redis_wasm_slice exists[] = {gcraStringSlice("EXISTS"), input_items[0]};
    rc = gcraCall(exists, 2);
    if (rc < 0) return rc == GCRA_ERR_REPLIED ? 0 : gcraReplyHostError();
    int64_t tat_us = now;
    uint8_t state[12] = {'G', 'C', 'R', 1};
    if (rc != 4 || reply_buffer[0] != ':' || (reply_buffer[1] != '0' && reply_buffer[1] != '1')) {
        return gcraReplyError("Unexpected EXISTS reply");
    }
    if (reply_buffer[1] == '1') {
        if (redis_wasm_blob_read(input_items[0], blob_type, state, sizeof(state)) != sizeof(state)) {
            return gcraReplyHostError();
        }
        if (state[0] != 'G' || state[1] != 'C' || state[2] != 'R' || state[3] != 1) {
            return gcraReplyError("Invalid GCRA blob version");
        }
        uint64_t stored = (uint64_t)redis_wasm_u32(state + 4) |
                          (uint64_t)redis_wasm_u32(state + 8) << 32;
        if (stored > INT64_MAX) return gcraReplyError("Invalid GCRA theoretical arrival time");
        tat_us = stored;
    }

    /* Advance the theoretical arrival time by the request cost. Subtracting
     * burst variance gives the earliest arrival time we can allow. */
    int64_t increment_us = emission_interval_us * num_tokens;
    int64_t variance_us = emission_interval_us * capacity;
    int64_t base_us = now > tat_us ? now : tat_us;
    if (base_us > INT64_MAX - increment_us) return gcraReplyError("GCRA timestamp overflow");
    int64_t new_tat_us = base_us + increment_us;
    int64_t diff_us = now - (new_tat_us - variance_us);
    int64_t limited = diff_us < 0, retry_after_s = -1, ttl_us;
    if (limited) {
        /* Costs exceeding total capacity can never be retried. */
        if (increment_us <= variance_us) {
            retry_after_s = (-diff_us) / 1000000 + ((-diff_us) % 1000000 != 0);
        }
        ttl_us = tat_us > now ? tat_us - now : 0;
    } else {
        ttl_us = new_tat_us - now;
        redis_wasm_put_u32(state + 4, (uint32_t)new_tat_us);
        redis_wasm_put_u32(state + 8, (uint32_t)((uint64_t)new_tat_us >> 32));
        /* Expire no earlier than the stored TAT, including sub-ms intervals. */
        int64_t expire_at_ms = new_tat_us / 1000 + (new_tat_us % 1000 != 0);
        if (redis_wasm_host_blob_write_expire(input_items[0].data, input_items[0].len,
                                            blob_type.data, blob_type.len,
                                            state, sizeof(state), expire_at_ms) != REDIS_WASM_OK)
        {
            return gcraReplyHostError();
        }
    }
    int64_t remaining = variance_us > ttl_us ? (variance_us - ttl_us) / emission_interval_us : 0;
    int64_t values[] = {limited, capacity, remaining, retry_after_s,
                       ttl_us / 1000000 + (ttl_us % 1000000 != 0)};
    unsigned pos = 4;
    redis_wasm_copy(reply_buffer, (const uint8_t *)"*5\r\n", pos);
    for (unsigned i = 0; i < 5; i++) {
        reply_buffer[pos++] = ':';
        pos += gcraFormatInteger(reply_buffer + pos, values[i]);
        reply_buffer[pos++] = '\r';
        reply_buffer[pos++] = '\n';
    }
    return redis_wasm_host_reply_raw(reply_buffer, pos) < 0 ? 1 : 0;
}
