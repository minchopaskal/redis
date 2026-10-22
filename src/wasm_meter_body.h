/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
/* Internal implementation, included once after the bounded Wasm decoder.
 * Decoding, placement analysis, and encoding are separate stages. The plan's
 * source spans consume each original body byte exactly once, in order; private
 * operations have empty source spans. No emitter operation selects a placement. */

/* arg/aux are decoded call indices or a slice of decoded branch targets.
 * Byte offsets are relative to the original module, not temporary buffers. */
typedef struct {
    uint32_t offset, arg, aux;
    uint8_t opcode, boundary;
} decoded_instruction;
typedef struct {
    const uint8_t *source;
    uint32_t offset, length, groups, count_bytes, locals_bytes, fuel_local;
    decoded_instruction *instructions;
    size_t count, capacity;
    uint32_t *targets;
    size_t target_count, target_capacity;
    int bad;
} decoded_body;
#define METER_PLAN_CHUNK_OPS 1024U
typedef struct {
    wasmMeterPlanOp *ops;
    size_t count, capacity;
    int bad;
    int (*consume)(void *context, const wasmMeterPlanOp *ops, size_t count);
    void *context;
} emission_plan;

static void *reserveRecords(void *records, size_t *capacity, size_t needed, size_t size) {
    if (needed <= *capacity) return records;
    if (needed > SIZE_MAX / size) return NULL;
    size_t next = *capacity ? *capacity : 32;
    while (next < needed) {
        if (next > SIZE_MAX / 2) { next = needed; break; }
        next *= 2;
    }
    if (next > SIZE_MAX / size) next = needed;
    void *grown = realloc(records, next * size);
    if (!grown) return NULL;
    *capacity = next;
    return grown;
}
static void freeDecodedBody(decoded_body *d) {
    free(d->instructions); free(d->targets);
}
static uint32_t instructionEnd(const decoded_body *d, size_t index) {
    return index + 1 < d->count ? d->instructions[index + 1].offset : d->offset + d->length;
}
static void decodeBody(reader source, const uint8_t *module, uint32_t params, decoded_body *d) {
    d->source = source.p;
    d->offset = (uint32_t)(source.p - module);
    d->length = (uint32_t)(source.end - source.p);
    d->groups = u32(&source);
    d->count_bytes = (uint32_t)(source.p - d->source);
    uint64_t locals = params;
    for (uint32_t i = 0; i < d->groups && !source.bad; i++) {
        locals += u32(&source); byte(&source);
    }
    if (source.bad || locals >= UINT32_MAX || d->groups == UINT32_MAX) {
        d->bad = 1; return;
    }
    d->locals_bytes = (uint32_t)(source.p - d->source);
    d->fuel_local = (uint32_t)locals;
    while (source.p < source.end && !source.bad) {
        const uint8_t *begin = source.p;
        decoded_instruction op = {.offset = (uint32_t)(begin - module)};
        op.opcode = byte(&source);
        op.boundary = instruction(&source, op.opcode);
        if (source.bad) break;
        if (op.opcode == 0x10) {
            reader immediate = {begin + 1, source.p, 0};
            op.arg = u32(&immediate);
            if (immediate.bad) { source.bad = 1; break; }
        } else if (op.opcode >= 0x0c && op.opcode <= 0x0e) {
            reader immediate = {begin + 1, source.p, 0};
            uint32_t count = op.opcode == 0x0e ? u32(&immediate) + 1 : 1;
            op.arg = (uint32_t)d->target_count;
            op.aux = count;
            void *targets = reserveRecords(d->targets, &d->target_capacity,
                                           d->target_count + count, sizeof(*d->targets));
            if (!targets) { source.bad = 1; break; }
            d->targets = targets;
            for (uint32_t i = 0; i < count && !immediate.bad; i++)
                d->targets[d->target_count++] = u32(&immediate);
            if (immediate.bad) { source.bad = 1; break; }
        }
        void *instructions = reserveRecords(d->instructions, &d->capacity,
                                            d->count + 1, sizeof(*d->instructions));
        if (!instructions) { source.bad = 1; break; }
        d->instructions = instructions;
        d->instructions[d->count++] = op;
    }
    d->bad = source.bad || source.p != source.end;
}
/* A bounded plan sink avoids retaining a whole function's expansion before
 * the output-size limit can reject it. Placement produces the same ordered
 * typed operations regardless of chunk boundaries; only the consumer encodes
 * bytes. Audit operation pointers remain valid during their synchronous call. */
static void flushPlan(emission_plan *p) {
    if (p->bad || !p->count) return;
    if (!p->consume(p->context, p->ops, p->count)) p->bad = 1;
    p->count = 0;
}
static void planOp(emission_plan *p, wasmMeterPlanOp op) {
    if (p->count == METER_PLAN_CHUNK_OPS) flushPlan(p);
    if (p->bad) return;
    void *ops = reserveRecords(p->ops, &p->capacity, p->count + 1, sizeof(*p->ops));
    if (!ops) { p->bad = 1; return; }
    p->ops = ops;
    p->ops[p->count++] = op;
}
static void planPrivate(emission_plan *p, wasmMeterPlanKind kind, uint32_t offset) {
    planOp(p, (wasmMeterPlanOp){.kind = kind, .source_offset = offset});
}
static void planCopy(emission_plan *p, uint32_t offset, uint32_t length) {
    if (length) planOp(p, (wasmMeterPlanOp){.kind = WASM_METER_PLAN_COPY_ORIGINAL,
                                         .source_offset = offset, .source_length = length});
}
static void planCharge(emission_plan *p, uint32_t offset, uint32_t length,
                       uint32_t cost, int check, unsigned depth, uint32_t prepaid) {
    wasmMeterPlanOp op = {.kind = WASM_METER_PLAN_CHARGE,
                         .source_offset = offset, .source_length = length};
    op.as.charge.cost = cost; op.as.charge.check = check;
    op.as.charge.depth = depth; op.as.charge.prepaid = prepaid;
    planOp(p, op);
}
static void planEntry(emission_plan *p, uint32_t offset, unsigned depth) {
    wasmMeterPlanOp op = {.kind = WASM_METER_PLAN_ENTRY_CHECK, .source_offset = offset};
    op.as.entry.depth = depth;
    planOp(p, op);
}
/* A linear if/else is indivisible for placement when its two arms have the
 * same charge, including the reached else/end delimiter. Its original bytes
 * remain a single part of the surrounding CopyOriginal range. */
static size_t equalIf(const decoded_body *d, size_t index, uint32_t *cost) {
    uint32_t then_cost = 0, arm_cost = 0;
    int have_else = 0;
    for (size_t i = index + 1; i < d->count; i++) {
        const decoded_instruction *op = &d->instructions[i];
        arm_cost++;
        if (op->opcode == 0x05) {
            if (have_else) return SIZE_MAX;
            have_else = 1; then_cost = arm_cost; arm_cost = 0;
        } else if (op->opcode == 0x0b) {
            if (!have_else || then_cost != arm_cost) return SIZE_MAX;
            *cost = arm_cost;
            return i;
        } else if (op->opcode == 0x02 || op->boundary) return SIZE_MAX;
    }
    return SIZE_MAX;
}
static int branchCheck(const decoded_body *d, const decoded_instruction *op,
                       unsigned depth, const unsigned *labels, int *exits) {
    int check = 0;
    *exits = 0;
    for (uint32_t i = 0; i < op->aux; i++) {
        uint32_t target = d->targets[op->arg + i];
        if (target >= depth) return 1; /* placement rejects this target below */
        if (target == depth - 1) *exits = 1;
        if (labels[depth - 1 - target] == 0x03) check = 1;
    }
    return check;
}
/* Inspect decoded records only. Calls, nested control, other exits, and plain
 * blocks disqualify prepayment. Therefore every successful backedge next runs
 * precisely this header, and every normal loop exit has consumed its credit. */
static uint32_t loopPrefix(const decoded_body *d, size_t index, unsigned depth,
                           const unsigned *labels, size_t *prefix_end) {
    uint32_t cost = 0;
    size_t header_end = SIZE_MAX;
    for (size_t i = index + 1; i < d->count; i++) {
        const decoded_instruction *op = &d->instructions[i];
        cost++;
        if (op->opcode == 0x02) return 0;
        if (!op->boundary) continue;
        if (op->opcode != 0x0d) return 0;
        uint32_t label = d->targets[op->arg];
        if (!label || label >= depth || labels[depth - label] == 0x03) return 0;
        header_end = i;
        break;
    }
    if (header_end == SIZE_MAX) return 0;
    int have_if = 0;
    for (size_t i = header_end + 1; i < d->count; i++) {
        const decoded_instruction *op = &d->instructions[i];
        if (op->opcode == 0x02) return 0;
        if (op->opcode == 0x04) {
            uint32_t arm_cost;
            size_t end = equalIf(d, i, &arm_cost);
            if (end == SIZE_MAX) return 0;
            i = end; have_if = 1;
        } else if (op->opcode == 0x0c) {
            if (!have_if || d->targets[op->arg] != 0 || i + 1 == d->count ||
                d->instructions[i + 1].opcode != 0x0b) return 0;
            *prefix_end = header_end;
            return cost;
        } else if (op->boundary) return 0;
    }
    return 0;
}
/* Placement analysis: produce typed operations without writing output bytes.
 * The cursor marks the next original byte not yet assigned to a consuming op.
 * Equal-arm folding changes charges only; all its bytes are copied intact. */
static void planBody(const decoded_body *d, const signatures *s, emission_plan *p) {
    wasmMeterPlanOp locals = {.kind = WASM_METER_PLAN_DECLARE_FUEL_LOCAL,
                             .source_offset = d->offset, .source_length = d->locals_bytes};
    locals.as.locals.original_groups = d->groups;
    locals.as.locals.count_bytes = d->count_bytes;
    planOp(p, locals);
    uint32_t start = d->offset + d->locals_bytes;
    planPrivate(p, WASM_METER_PLAN_RELOAD, start);
    planPrivate(p, WASM_METER_PLAN_OPEN_EXHAUSTION, start);
    planEntry(p, start, 0);
    size_t previous = SIZE_MAX;
    unsigned labels[512] = {0}, depth = 1;
    uint32_t prefixes[512] = {0};
    size_t prefix_ends[512];
    for (size_t i = 0; i < 512; i++) prefix_ends[i] = SIZE_MAX;
    uint32_t cost = 0;
    for (size_t i = 0; i < d->count && !p->bad; i++) {
        const decoded_instruction *record = &d->instructions[i];
        unsigned op = record->opcode;
        uint32_t opcode = record->offset, end = instructionEnd(d, i);
        cost++;
        uint32_t arm_cost;
        if (op == 0x04) {
            size_t equal_end = equalIf(d, i, &arm_cost);
            if (equal_end != SIZE_MAX) {
                cost += arm_cost;
                previous = SIZE_MAX;
                i = equal_end;
                continue;
            }
        }
        if (record->boundary) {
            uint32_t entering_prefix = 0;
            size_t entering_end = SIZE_MAX;
            if (op == 0x03)
                entering_prefix = loopPrefix(d, i, depth, labels, &entering_end);
            if (op == 0x0d && prefix_ends[depth - 1] == i)
                cost -= prefixes[depth - 1];
            int exits = 0;
            int check = op == 0x11 || op == 0x00;
            const fixed_call *callee = NULL;
            uint32_t callee_type = 0;
            if (op == 0x10) {
                uint32_t index = record->arg;
                if (index >= (uint64_t)s->import_functions + s->function_count) {
                    p->bad = 1; break;
                }
                check = index < s->import_functions;
                if (!check && s->fixed[index - s->import_functions].clone) {
                    callee = &s->fixed[index - s->import_functions];
                    callee_type = s->functions[index - s->import_functions];
                }
            }
            if (op >= 0x0c && op <= 0x0e)
                check = branchCheck(d, record, depth, labels, &exits);
            uint32_t debit = opcode;
            if ((op == 0x0d || op == 0x04) && previous != SIZE_MAX) {
                const decoded_instruction *comparison = &d->instructions[previous];
                if (comparison->offset >= start && comparison->opcode >= 0x45 &&
                    comparison->opcode <= 0x5a) debit = comparison->offset;
            }
            planCopy(p, start, debit - start);
            if (op == 0x0c && check && !exits) {
                uint32_t backedge = d->targets[record->arg];
                if (backedge >= depth) { p->bad = 1; break; }
                uint32_t prepaid = prefixes[depth - 1 - backedge];
                planCharge(p, opcode, end - opcode, cost + prepaid, -1, backedge, prepaid);
                wasmMeterPlanOp jump = {.kind = WASM_METER_PLAN_JUMP_EXHAUSTION,
                                       .source_offset = end};
                jump.as.jump.depth = depth - 1;
                planOp(p, jump);
            } else {
                if (cost || entering_prefix || check)
                    planCharge(p, debit, 0, cost + entering_prefix, check, depth - 1, 0);
                planCopy(p, debit, opcode - debit);
                if (op == 0x0b && depth == 1) {
                    planPrivate(p, WASM_METER_PLAN_PUBLISH, opcode);
                    planOp(p, (wasmMeterPlanOp){.kind = WASM_METER_PLAN_RETURN,
                                              .source_offset = opcode, .source_length = end - opcode});
                } else if (op >= 0x0c && op <= 0x0e) {
                    for (uint32_t t = 0; t < record->aux; t++)
                        if (d->targets[record->arg + t] >= depth) p->bad = 1;
                    if (p->bad) break;
                    if (exits) planPrivate(p, WASM_METER_PLAN_PUBLISH, opcode);
                    wasmMeterPlanOp branch = {.kind = WASM_METER_PLAN_BRANCH_REMAP,
                                             .source_offset = opcode, .source_length = end - opcode};
                    branch.as.branch.opcode = op; branch.as.branch.depth = depth;
                    branch.as.branch.target_start = record->arg;
                    branch.as.branch.target_count = record->aux;
                    planOp(p, branch);
                } else if (callee) {
                    if (!callee->calls) planEntry(p, opcode, depth - 1);
                    wasmMeterPlanOp dispatch = {.kind = WASM_METER_PLAN_CLONE_DISPATCH,
                                               .source_offset = opcode, .source_length = end - opcode};
                    dispatch.as.dispatch.original = record->arg;
                    dispatch.as.dispatch.clone = callee->clone;
                    dispatch.as.dispatch.type = callee_type;
                    dispatch.as.dispatch.cost = callee->cost;
                    dispatch.as.dispatch.tree = callee->calls != 0;
                    planOp(p, dispatch);
                } else if (op == 0x10 || op == 0x11 || op == 0x0f || op == 0x00) {
                    planPrivate(p, WASM_METER_PLAN_PUBLISH, opcode);
                    planCopy(p, opcode, end - opcode);
                    if (op == 0x10 || op == 0x11)
                        planPrivate(p, WASM_METER_PLAN_RELOAD, end);
                } else planCopy(p, opcode, end - opcode);
            }
            if (op == 0x03 && depth < 512) {
                prefixes[depth] = entering_prefix;
                prefix_ends[depth] = entering_end;
            }
            start = end; cost = 0;
        }
        if (op >= 0x02 && op <= 0x04) {
            if (depth == 512) { p->bad = 1; break; }
            if (op != 0x03) { prefixes[depth] = 0; prefix_ends[depth] = SIZE_MAX; }
            labels[depth++] = op;
        }
        if (op == 0x0b) {
            if (!depth) { p->bad = 1; break; }
            if (--depth == 0 && i + 1 != d->count) p->bad = 1;
        }
        previous = i;
    }
    if (depth != 0 || cost || start != d->offset + d->length) p->bad = 1;
    planPrivate(p, WASM_METER_PLAN_COLD_EXIT, d->offset + d->length);
}
/* Clones retain their locals/signature and every non-call byte. A call-tree
 * clone only substitutes each selected direct target with its private clone. */
static void planClone(const decoded_body *d, const signatures *s, int tree, emission_plan *p) {
    uint32_t start = d->offset;
    if (tree) {
        for (size_t i = 0; i < d->count && !p->bad; i++) {
            const decoded_instruction *op = &d->instructions[i];
            if (op->opcode != 0x10) continue;
            uint32_t index = op->arg;
            if (index < s->import_functions ||
                index >= (uint64_t)s->import_functions + s->function_count ||
                !s->fixed[index - s->import_functions].clone) { p->bad = 1; break; }
            planCopy(p, start, op->offset - start);
            uint32_t end = instructionEnd(d, i);
            wasmMeterPlanOp call = {.kind = WASM_METER_PLAN_CLONE_CALL,
                                   .source_offset = op->offset, .source_length = end - op->offset};
            call.as.clone_call.original = index;
            call.as.clone_call.clone = s->fixed[index - s->import_functions].clone;
            planOp(p, call);
            start = end;
        }
    }
    planCopy(p, start, d->offset + d->length - start);
}

#ifdef WASM_METER_AUDIT
static void auditPlan(wasmMeterAuditPhase phase, const wasmMeterPlanFunction *function,
                      const wasmMeterPlanOp *op, const buffer *out, size_t offset) {
    wasmMeterAuditEvent event = {.phase = phase, .function = function, .operation = op,
                                .output_offset = offset, .output_length = out->len - offset,
                                .output_bytes = out->p ? out->p + offset : NULL};
    WASM_METER_AUDIT(&event);
}
#endif

/* Encoding is deliberately mechanical: it consumes typed operations and does
 * not scan original instructions or make metering-placement decisions. */
typedef struct {
    buffer *out;
    const wasmMeterPlanFunction *function;
} emission_target;
static int emitPlan(void *context, const wasmMeterPlanOp *ops, size_t count) {
    emission_target *target = context;
    buffer *out = target->out;
    const wasmMeterPlanFunction *function = target->function;
    meter m = {.global = function->fuel_global, .local = function->fuel_local};
    for (size_t i = 0; i < count && !out->bad; i++) {
        const wasmMeterPlanOp *op = &ops[i];
        const uint8_t *source = function->source_body + (op->source_offset - function->source_body_offset);
#ifdef WASM_METER_AUDIT
        size_t offset = out->len;
#endif
        switch (op->kind) {
        case WASM_METER_PLAN_COPY_ORIGINAL:
            put(out, source, op->source_length);
            break;
        case WASM_METER_PLAN_DECLARE_FUEL_LOCAL:
            leb(out, op->as.locals.original_groups + 1, 0);
            put(out, source + op->as.locals.count_bytes,
                op->source_length - op->as.locals.count_bytes);
            emit(out, 1); emit(out, 0x7e);
            break;
        case WASM_METER_PLAN_OPEN_EXHAUSTION:
            emit(out, 0x02); emit(out, 0x40);
            break;
        case WASM_METER_PLAN_CHARGE:
            charge(out, &m, op->as.charge.cost, op->as.charge.check,
                   op->as.charge.depth, op->as.charge.prepaid);
            break;
        case WASM_METER_PLAN_PUBLISH:
            publish(out, &m);
            break;
        case WASM_METER_PLAN_RELOAD:
            reload(out, &m);
            break;
        case WASM_METER_PLAN_ENTRY_CHECK:
            entryCheck(out, &m, op->as.entry.depth);
            break;
        case WASM_METER_PLAN_BRANCH_REMAP:
            emit(out, op->as.branch.opcode);
            if (op->as.branch.opcode == 0x0e) leb(out, op->as.branch.target_count - 1, 0);
            for (uint32_t j = 0; j < op->as.branch.target_count; j++) {
                uint32_t target = function->branch_targets[op->as.branch.target_start + j];
                if (target == op->as.branch.depth - 1) target++;
                leb(out, target, 0);
            }
            break;
        case WASM_METER_PLAN_RETURN:
            emit(out, 0x0f);
            break;
        case WASM_METER_PLAN_CLONE_DISPATCH:
            if (op->as.dispatch.tree) {
                emit(out, 0x20); leb(out, m.local, 0);
                emit(out, 0x42); leb(out, op->as.dispatch.cost, 1);
                emit(out, 0x59);
                emit(out, 0x04); leb(out, op->as.dispatch.type, 1);
            }
            emit(out, 0x10); leb(out, op->as.dispatch.clone, 0);
            charge(out, &m, op->as.dispatch.cost, 0, 0, 0);
            if (op->as.dispatch.tree) {
                emit(out, 0x05);
                publish(out, &m);
                put(out, source, op->source_length);
                reload(out, &m);
                emit(out, 0x0b);
            }
            break;
        case WASM_METER_PLAN_JUMP_EXHAUSTION:
            emit(out, 0x0c); leb(out, op->as.jump.depth, 0);
            break;
        case WASM_METER_PLAN_COLD_EXIT:
            emit(out, 0x0b);
            emit(out, 0x42); emit(out, 0);
            emit(out, 0x24); leb(out, m.global, 0);
            emit(out, 0x41); emit(out, 1);
            emit(out, 0x24); leb(out, m.global + 1, 0);
            emit(out, 0x00); emit(out, 0x0b);
            break;
        case WASM_METER_PLAN_CLONE_CALL:
            emit(out, 0x10); leb(out, op->as.clone_call.clone, 0);
            break;
        }
#ifdef WASM_METER_AUDIT
        if (!out->bad) auditPlan(WASM_METER_AUDIT_OPERATION, function, op, out, offset);
#endif
    }
    return !out->bad;
}
static void plannedBody(reader *r, buffer *out, uint32_t global, uint32_t index,
                        const signatures *s, unsigned clone_kind) {
    decoded_body d = {0};
    emission_plan p = {0};
    decodeBody(*r, s->input, s->params[s->functions[index]], &d);
    if (d.bad) r->bad = 1;
    else {
        wasmMeterPlanFunction function = {
            .original_function_index = s->import_functions + index,
            .emitted_function_index = clone_kind ? s->fixed[index].clone : s->import_functions + index,
            .type_index = s->functions[index],
            .source_body_offset = d.offset, .source_body_length = d.length,
            .fuel_global = global, .fuel_local = clone_kind ? UINT32_MAX : d.fuel_local,
            .clone_kind = clone_kind, .source_body = d.source,
            .branch_targets = d.targets, .branch_target_count = d.target_count
        };
        emission_target target = {.out = out, .function = &function};
        p.consume = emitPlan; p.context = &target;
#ifdef WASM_METER_AUDIT
        auditPlan(WASM_METER_AUDIT_BODY_BEGIN, &function, NULL, out, 0);
#endif
        if (clone_kind) planClone(&d, s, clone_kind == 2, &p);
        else planBody(&d, s, &p);
        flushPlan(&p);
        if (p.bad) r->bad = 1;
        else {
#ifdef WASM_METER_AUDIT
            auditPlan(WASM_METER_AUDIT_BODY_END, &function, NULL, out, 0);
#endif
            r->p = r->end;
        }
    }
    free(p.ops); freeDecodedBody(&d);
}
