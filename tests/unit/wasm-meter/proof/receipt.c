/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
/* Audit the production planner/emitter; no alternate instrumentation path. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include "wasm_meter_plan.h"

static void record(const wasmMeterAuditEvent *event);
#define WASM_METER_AUDIT(event) record(event)
#include "wasm_meter.c"

static void bytes(const uint8_t *data, size_t length) {
    putchar('[');
    for (size_t i = 0; i < length; i++) printf("%s%u", i ? "," : "", data[i]);
    putchar(']');
}

static void record(const wasmMeterAuditEvent *event) {
    const wasmMeterPlanFunction *f = event->function;
    if (event->phase == WASM_METER_AUDIT_BODY_BEGIN) {
        printf("{\"event\":\"begin\",\"original\":%u,\"emitted\":%u,"
               "\"type\":%u,\"offset\":%u,\"length\":%u,"
               "\"global\":%u,\"local\":%u,\"cloneKind\":%u}\n",
               f->original_function_index, f->emitted_function_index,
               f->type_index, f->source_body_offset, f->source_body_length,
               f->fuel_global, f->fuel_local, f->clone_kind);
        return;
    }
    if (event->phase == WASM_METER_AUDIT_BODY_END) {
        printf("{\"event\":\"end\",\"output\":");
        bytes(event->output_bytes, event->output_length);
        puts("}");
        return;
    }
    const wasmMeterPlanOp *op = event->operation;
    printf("{\"event\":\"op\",\"kind\":%u,\"sourceOffset\":%u,"
           "\"sourceLength\":%u,\"outputOffset\":%zu,\"args\":[",
           op->kind, op->source_offset, op->source_length, event->output_offset);
    switch (op->kind) {
    case WASM_METER_PLAN_DECLARE_FUEL_LOCAL:
        printf("%u,%u", op->as.locals.original_groups, op->as.locals.count_bytes); break;
    case WASM_METER_PLAN_CHARGE:
        printf("%u,%d,%u,%u", op->as.charge.cost, op->as.charge.check,
               op->as.charge.depth, op->as.charge.prepaid); break;
    case WASM_METER_PLAN_ENTRY_CHECK:
        printf("%u", op->as.entry.depth); break;
    case WASM_METER_PLAN_BRANCH_REMAP:
        printf("%u,%u", op->as.branch.opcode, op->as.branch.depth);
        for (uint32_t i = 0; i < op->as.branch.target_count; i++)
            printf(",%u", f->branch_targets[op->as.branch.target_start + i]);
        break;
    case WASM_METER_PLAN_CLONE_DISPATCH:
        printf("%u,%u,%u,%u,%u", op->as.dispatch.original,
               op->as.dispatch.clone, op->as.dispatch.type,
               op->as.dispatch.cost, op->as.dispatch.tree); break;
    case WASM_METER_PLAN_JUMP_EXHAUSTION:
        printf("%u", op->as.jump.depth); break;
    case WASM_METER_PLAN_CLONE_CALL:
        printf("%u,%u", op->as.clone_call.original, op->as.clone_call.clone); break;
    default: break;
    }
    printf("],\"output\":");
    bytes(event->output_bytes, event->output_length);
    puts("}");
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: receipt original.wasm injected.wasm\n"); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in || fseek(in, 0, SEEK_END)) return 2;
    long length = ftell(in);
    if (length < 0 || fseek(in, 0, SEEK_SET)) return 2;
    uint8_t *input = malloc((size_t)length);
    if (!input || fread(input, 1, (size_t)length, in) != (size_t)length) return 2;
    fclose(in);
    uint8_t *output = NULL;
    size_t output_length = 0;
    char error[256];
    if (!wasmMeterInject(input, (size_t)length, INT64_MAX, &output,
                         &output_length, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); free(input); return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(output, 1, output_length, out) != output_length || fclose(out)) return 2;
    free(output); free(input);
    return 0;
}
