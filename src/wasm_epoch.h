/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#ifndef REDIS_WASM_EPOCH_H
#define REDIS_WASM_EPOCH_H
#include <stddef.h>
#include <stdint.h>

#define WASM_EPOCH_MODULE "redis"
#define WASM_EPOCH_POLL "__epoch_poll"
#define WASM_EPOCH_POLL_STRIDE 1024
#define WASM_EPOCH_MAX_INPUT (16U * 1024U * 1024U)

/* The caller MUST validate the original module with WAMR before injection.
 * Otherwise invalid function/local/global indices might address private meter
 * state. The trusted redis.__epoch_poll import returns a positive polling
 * stride, or raises a WAMR exception when the invocation's epoch expires.
 * The host reads CLOCK_MONOTONIC directly at each probe; no timer thread is
 * required. The polling countdown only samples checks, not instruction fuel.
 * Unlike fuel, epoch interruption measures elapsed time nondeterministically;
 * it does not count guest instructions or preempt native Redis commands.
 * Returns 1 on success; *output is owned by the caller and freed with free(). */
int wasmEpochInject(const uint8_t *input, size_t length,
                    uint8_t **output, size_t *output_length,
                    char *error, size_t error_length);
#endif
