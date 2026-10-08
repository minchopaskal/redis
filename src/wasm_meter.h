/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#ifndef REDIS_WASM_METER_H
#define REDIS_WASM_METER_H
#include <stddef.h>
#include <stdint.h>

#define WASM_METER_FUEL "__redis_meter_fuel"
#define WASM_METER_EXHAUSTED "__redis_meter_exhausted"
#define WASM_METER_MAX_INPUT (16U * 1024U * 1024U)

/* The caller MUST validate the original module with WAMR before injection.
 * In particular, invalid original global indices must not become references to
 * the appended meter globals. This bounded decoder is not a Wasm validator.
 * Returns 1 on success; *output is owned by the caller and freed with free(). */
int wasmMeterInject(const uint8_t *input, size_t length, uint64_t fuel,
                    uint8_t **output, size_t *output_length,
                    char *error, size_t error_length);
#endif
