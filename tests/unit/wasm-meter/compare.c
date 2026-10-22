/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
/* Refactor regression evidence: compare the actual byte-producing C functions,
 * independently of the model/proofs and without loading WAMR. Some corpus
 * inputs deliberately violate the API's original-validation precondition:
 * matching acceptance here does not establish that such a module is valid. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_meter.h"

int wasmMeterInjectBaseline(const uint8_t *, size_t, uint64_t, uint8_t **,
                            size_t *, char *, size_t);

static unsigned cases, accepted, rejected;
static const char *current_file, *current_variant;
static uint64_t current_fuel;

static void require(int condition, const char *why) {
    if (condition) return;
    fprintf(stderr, "FAIL %s [%s, fuel=%" PRIu64 "]: %s\n",
            current_file, current_variant, current_fuel, why);
    exit(1);
}

static void compare(const uint8_t *input, size_t length, uint64_t fuel,
                    const char *variant) {
    current_variant = variant;
    current_fuel = fuel;
    uint8_t *original = malloc(length ? length : 1);
    require(original != NULL, "input snapshot allocation");
    if (length) memcpy(original, input, length);
    uint8_t *old_output = (void *)1, *new_output = (void *)1;
    size_t old_length = 123, new_length = 123;
    char old_error[512] = {0}, new_error[512] = {0};
    int old_ok = wasmMeterInjectBaseline(input, length, fuel, &old_output,
                                         &old_length, old_error, sizeof(old_error));
    require(!length || !memcmp(original, input, length), "baseline modified input");
    int new_ok = wasmMeterInject(input, length, fuel, &new_output,
                                 &new_length, new_error, sizeof(new_error));
    require(!length || !memcmp(original, input, length), "current injector modified input");
    require(old_ok == new_ok, "acceptance/rejection differs");
    if (old_ok) {
        require(old_output && new_output, "successful output ownership");
        require(old_length == new_length, "output length differs");
        if (memcmp(old_output, new_output, old_length)) {
            size_t at = 0;
            while (at < old_length && old_output[at] == new_output[at]) at++;
            fprintf(stderr, "first differing output byte %zu: baseline=%02x current=%02x\n",
                    at, old_output[at], new_output[at]);
            require(0, "output bytes differ");
        }
        free(old_output);
        free(new_output);
        accepted++;
    } else {
        require(!old_output && !new_output && !old_length && !new_length,
                "failure must clear output ownership and length");
        require(!strcmp(old_error, new_error), "rejection diagnostic differs");
        rejected++;
    }
    cases++;
    free(original);
}

static void fixture(const char *path) {
    current_file = path;
    current_variant = "read";
    current_fuel = 0;
    FILE *file = fopen(path, "rb");
    require(file != NULL, "open fixture");
    require(!fseek(file, 0, SEEK_END), "seek fixture");
    long size = ftell(file);
    require(size >= 0 && !fseek(file, 0, SEEK_SET), "measure fixture");
    size_t length = (size_t)size;
    uint8_t *bytes = malloc(length ? length : 1);
    require(bytes != NULL, "fixture allocation");
    require(fread(bytes, 1, length, file) == length, "read fixture");
    fclose(file);

    const uint64_t budgets[] = {0, 1, 63, 64, 127, 128, 8191, 8192, INT64_MAX};
    for (unsigned i = 0; i < sizeof(budgets) / sizeof(*budgets); i++)
        compare(bytes, length, budgets[i], "complete");
    compare(bytes, length, (uint64_t)INT64_MAX + 1, "invalid signed fuel");
    /* These are parser-equivalence cases. Some shortened prefixes are complete
     * valid modules; compare both acceptance and complete output in that case. */
    size_t cuts[] = {0, 1, 7, 8, length / 2, length ? length - 1 : 0};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(*cuts); i++)
        if (cuts[i] < length) compare(bytes, cuts[i], 64, "truncated/prefix");
    if (length) {
        bytes[0] ^= 0xff;
        compare(bytes, length, 64, "invalid magic");
    }
    free(bytes);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    for (int i = 1; i < argc; i++) fixture(argv[i]);
    const uint8_t unknown_opcode[] = {0,97,115,109,1,0,0,0,1,4,1,96,0,0,
                                      3,2,1,0,10,5,1,3,0,255,11};
    const uint8_t unknown_section[] = {0,97,115,109,1,0,0,0,13,0};
    const uint8_t invalid_length[] = {0,97,115,109,1,0,0,0,1,128,128,128,128,16};
    current_file = "embedded malformed inputs";
    compare(unknown_opcode, sizeof(unknown_opcode), 64, "unknown opcode");
    compare(unknown_section, sizeof(unknown_section), 64, "unknown section");
    compare(invalid_length, sizeof(invalid_length), 64, "overflowing section length");
    printf("PASS: %u byte-comparison cases (%u identical outputs, %u identical rejections) across %d fixtures.\n",
           cases, accepted, rejected, argc - 1);
    return 0;
}
