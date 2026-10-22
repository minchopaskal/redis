/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#ifndef REDIS_WASM_METER_PLAN_H
#define REDIS_WASM_METER_PLAN_H

#include <stddef.h>
#include <stdint.h>

/* Internal, typed bytecode-emission vocabulary. Source spans are offsets in
 * the original module; output spans in audit events are offsets in one emitted
 * function body, including its local declarations and final end instruction. */
typedef enum {
    WASM_METER_PLAN_COPY_ORIGINAL,
    WASM_METER_PLAN_DECLARE_FUEL_LOCAL,
    WASM_METER_PLAN_OPEN_EXHAUSTION,
    WASM_METER_PLAN_CHARGE,
    WASM_METER_PLAN_PUBLISH,
    WASM_METER_PLAN_RELOAD,
    WASM_METER_PLAN_ENTRY_CHECK,
    WASM_METER_PLAN_BRANCH_REMAP,
    WASM_METER_PLAN_RETURN,
    WASM_METER_PLAN_CLONE_DISPATCH,
    WASM_METER_PLAN_JUMP_EXHAUSTION,
    WASM_METER_PLAN_COLD_EXIT,
    WASM_METER_PLAN_CLONE_CALL
} wasmMeterPlanKind;

typedef struct {
    wasmMeterPlanKind kind;
    uint32_t source_offset;
    uint32_t source_length;
    union {
        struct { uint32_t original_groups, count_bytes; } locals;
        struct { uint32_t cost, depth, prepaid; int check; } charge;
        struct { uint32_t depth; } entry;
        struct { uint32_t opcode, depth, target_start, target_count; } branch;
        struct { uint32_t original, clone, type, cost, tree; } dispatch;
        struct { uint32_t depth; } jump;
        struct { uint32_t original, clone; } clone_call;
    } as;
} wasmMeterPlanOp;

typedef struct {
    uint32_t original_function_index;
    uint32_t emitted_function_index;
    uint32_t type_index;
    uint32_t source_body_offset;
    uint32_t source_body_length;
    uint32_t fuel_global;
    uint32_t fuel_local; /* UINT32_MAX for an uninstrumented clone. */
    uint32_t clone_kind; /* 0 original, 1 leaf clone, 2 call-tree clone. */
    const uint8_t *source_body;
    const uint32_t *branch_targets;
    size_t branch_target_count;
} wasmMeterPlanFunction;

typedef enum {
    WASM_METER_AUDIT_BODY_BEGIN,
    WASM_METER_AUDIT_OPERATION,
    WASM_METER_AUDIT_BODY_END
} wasmMeterAuditPhase;

typedef struct {
    wasmMeterAuditPhase phase;
    const wasmMeterPlanFunction *function;
    const wasmMeterPlanOp *operation; /* NULL at body boundaries. */
    size_t output_offset;
    size_t output_length;
    const uint8_t *output_bytes; /* This event's bytes, valid during callback. */
} wasmMeterAuditEvent;

/* An audit harness may define WASM_METER_AUDIT(event_pointer) before including
 * wasm_meter.c. Events are synchronous; pointers must not be retained. Normal
 * builds define no callback and perform no audit serialization or hashing. */

#endif
