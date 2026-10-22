/* Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_export.h"
#include "wasm_meter.h"

static unsigned checks, host_calls;
static uint64_t host_fuel;
static const char *current_case;

static void fail(const char *why) {
    fprintf(stderr, "FAIL [%s]: %s\n", current_case ? current_case : "setup", why);
    exit(1);
}
static void require(int condition, const char *why) {
    if (!condition) fail(why);
    checks++;
}
static void *global_data(wasm_module_inst_t instance, const char *name) {
    wasm_global_inst_t global;
    if (!wasm_runtime_get_export_global_inst(instance, name, &global)) fail(name);
    return global.global_data;
}
static uint64_t fuel_of(wasm_module_inst_t instance) {
    uint64_t fuel;
    memcpy(&fuel, global_data(instance, WASM_METER_FUEL), sizeof(fuel));
    return fuel;
}
static int32_t observe(wasm_exec_env_t env, int32_t argument) {
    host_calls++;
    host_fuel = fuel_of(wasm_runtime_get_module_inst(env));
    require(host_fuel <= INT64_MAX, "host entered with negative cached fuel");
    return argument * 2;
}
/* A native test import modifies the shared budget, just as a nested guest
 * invocation can. The caller must reload it rather than overwrite the spend. */
static int32_t spend(wasm_exec_env_t env, int32_t argument) {
    int32_t result = observe(env, argument);
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(env);
    require(host_fuel >= 3, "native test spend needs three fuel");
    uint64_t fuel = host_fuel - 3;
    memcpy(global_data(instance, WASM_METER_FUEL), &fuel, sizeof(fuel));
    return result;
}
static NativeSymbol imports[] = {
    {"observe", observe, "(i)i", NULL}, {"spend", spend, "(i)i", NULL}
};
typedef struct {
    uint8_t *bytes;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t env;
} Module;

static uint8_t *read_file(const char *directory, const char *name, size_t *length) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s.wasm", directory, name);
    FILE *file = fopen(path, "rb");
    if (!file) fail("open fixture");
    if (fseek(file, 0, SEEK_END)) fail("seek fixture");
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET)) fail("measure fixture");
    uint8_t *bytes = malloc(size ? (size_t)size : 1);
    if (!bytes || fread(bytes, 1, size, file) != (size_t)size) fail("read fixture");
    fclose(file);
    *length = size;
    return bytes;
}
static Module load_module(const char *directory, const char *name, uint64_t initial,
                          int should_instantiate) {
    current_case = name;
    size_t length, output_length;
    uint8_t *original = read_file(directory, name, &length);
    uint8_t *validation = malloc(length);
    if (!validation) fail("validation allocation");
    memcpy(validation, original, length);
    char error[512] = {0};
    wasm_module_t validated = wasm_runtime_load(validation, length, error, sizeof(error));
    if (!validated) { fprintf(stderr, "%s\n", error); fail("original fixture validation"); }
    wasm_runtime_unload(validated);
    free(validation);
    Module m = {0};
    require(wasmMeterInject(original, length, initial, &m.bytes, &output_length,
                            error, sizeof(error)), "injection");
    free(original);
    m.module = wasm_runtime_load(m.bytes, output_length, error, sizeof(error));
    if (!m.module) { fprintf(stderr, "%s\n", error); fail("injected fixture validation"); }
    m.instance = wasm_runtime_instantiate(m.module, 65536, 0, error, sizeof(error));
    if (!should_instantiate) {
        require(!m.instance, "startup should exhaust its initial fuel");
        require(strstr(error, "unreachable") != NULL, "startup exhaustion trap");
        wasm_runtime_unload(m.module);
        free(m.bytes);
        return (Module){0};
    }
    if (!m.instance) { fprintf(stderr, "%s\n", error); fail("instantiation"); }
    m.env = wasm_runtime_create_exec_env(m.instance, 65536);
    require(m.env != NULL, "execution environment");
    return m;
}
static void destroy(Module m) {
    wasm_runtime_destroy_exec_env(m.env);
    wasm_runtime_deinstantiate(m.instance);
    wasm_runtime_unload(m.module);
    free(m.bytes);
}
static void reset(Module m, uint64_t fuel) {
    int32_t zero = 0;
    wasm_runtime_clear_exception(m.instance);
    memcpy(global_data(m.instance, WASM_METER_FUEL), &fuel, sizeof(fuel));
    memcpy(global_data(m.instance, WASM_METER_EXHAUSTED), &zero, sizeof(zero));
    host_calls = 0;
    host_fuel = UINT64_MAX;
}
static int exhausted(Module m) {
    int32_t flag;
    memcpy(&flag, global_data(m.instance, WASM_METER_EXHAUSTED), sizeof(flag));
    return flag;
}
static int invoke(Module m, const char *name, unsigned argc, wasm_val_t *args,
                  unsigned resultc, wasm_val_t *results) {
    current_case = name;
    wasm_function_inst_t function = wasm_runtime_lookup_function(m.instance, name);
    require(function != NULL, "function export");
    return wasm_runtime_call_wasm_a(m.env, function, resultc, results, argc, args);
}
static void expect_exhausted(Module m) {
    require(exhausted(m) == 1, "exhaustion marker");
    require(fuel_of(m.instance) == 0, "exhausted shared fuel is saturated at zero");
    const char *error = wasm_runtime_get_exception(m.instance);
    require(error && strstr(error, "unreachable"), "exhaustion trap category");
}
static void exact(Module m, const char *name, unsigned argc, wasm_val_t *args,
                  unsigned resultc, const wasm_val_t *expected, uint64_t cost) {
    /* These finite fixtures finish with an unchecked tail. One-short now
     * succeeds with exactly -1 remaining; zero-budget entry still traps. */
    uint64_t budgets[] = {cost, cost + 17, INT64_MAX, cost - 1};
    for (unsigned k = 0; k < sizeof(budgets) / sizeof(*budgets); k++) {
        reset(m, budgets[k]);
        wasm_val_t results[2] = {{0}, {0}};
        if (!budgets[k]) {
            require(!invoke(m, name, argc, args, resultc, results), "zero-budget entry must trap");
            expect_exhausted(m);
            continue;
        }
        require(invoke(m, name, argc, args, resultc, results), "funded checkpoints and finite tail must succeed");
        for (unsigned i = 0; i < resultc; i++) {
            require(results[i].kind == expected[i].kind, "result type preserved");
            if (expected[i].kind == WASM_I32)
                require(results[i].of.i32 == expected[i].of.i32, "i32 result preserved");
            else require(results[i].of.i64 == expected[i].of.i64, "i64 result preserved");
        }
        uint64_t remaining = fuel_of(m.instance);
        if (remaining != budgets[k] - cost) {
            fprintf(stderr, "%s: budget=%" PRIu64 " remaining=%" PRIu64 " expected=%" PRIu64 "\n",
                    name, budgets[k], remaining, budgets[k] - cost);
            fail("exact dynamic opcode accounting");
        }
        require(!exhausted(m), "successful call must not set exhaustion");
    }
}
static void scalar(Module m, const char *name, int has_arg, int argument,
                   int expected, uint64_t cost) {
    wasm_val_t arg = {.kind = WASM_I32, .of.i32 = argument};
    wasm_val_t result = {.kind = WASM_I32, .of.i32 = expected};
    exact(m, name, has_arg, has_arg ? &arg : NULL, 1, &result, cost);
}
static void accounting(Module m) {
    exact(m, "empty", 0, NULL, 0, NULL, 1);
    scalar(m, "value", 0, 0, 42, 2);
    scalar(m, "linear", 1, 10, 10, 6);
    scalar(m, "if_else", 1, 1, 12, 7);
    scalar(m, "if_else", 1, 0, 7, 5);
    scalar(m, "if_join", 1, 1, 9, 7);
    scalar(m, "if_join", 1, 0, 9, 4);
    scalar(m, "equal_if", 1, 1, 12, 7);
    scalar(m, "equal_if", 1, 0, 15, 7);
    scalar(m, "equal_if_skip_end", 1, 1, 17, 7);
    scalar(m, "equal_if_skip_end", 1, 0, 19, 7);
    scalar(m, "nested_if", 1, 1, 11, 8);
    scalar(m, "nested_if", 1, 0, 13, 5);
    scalar(m, "branch_in_arm", 1, 1, 17, 6);
    scalar(m, "branch_in_arm", 1, 0, 19, 9);
    scalar(m, "call_in_arm", 1, 2, 4, 6);
    scalar(m, "call_in_arm", 1, 0, 0, 6);
    scalar(m, "block_linear", 0, 0, 33, 9);
    scalar(m, "function_br", 0, 0, 17, 2);
    scalar(m, "function_br_if", 1, 1, 17, 3);
    scalar(m, "function_br_if", 1, 0, 19, 6);
    scalar(m, "function_table", 1, 0, 23, 5);
    scalar(m, "function_table", 1, 1, 23, 4);
    scalar(m, "explicit_return", 0, 0, 31, 2);
    for (int n = 0; n <= 12; n++) {
        scalar(m, "counted", 1, n, n, 9 * n + 8);
        scalar(m, "call_loop", 1, n, n, 12 * n + 8);
        scalar(m, "recursive", 1, n, n, 10 * n + 5);
        scalar(m, "recursive_indirect", 1, n, n, 11 * n + 5);
        if (n) {
            scalar(m, "counted_if", 1, n, n, 8 * n + 4);
            scalar(m, "counted_table", 1, n, n, 8 * n + 4);
        }
    }
    scalar(m, "direct", 1, 11, 24, 9);
    scalar(m, "indirect", 1, 11, 24, 10);
    scalar(m, "twice", 1, 11, 13, 12);
    scalar(m, "hosted", 1, 11, 23, 5);
    scalar(m, "host_spend", 1, 11, 23, 8);
    scalar(m, "host_after_branch", 1, 1, 20, 8);
    scalar(m, "host_after_branch", 1, 0, 20, 7);
    wasm_val_t args[3] = {{.kind=WASM_I32, .of.i32=123},
                         {.kind=WASM_I64, .of.i64=INT64_C(876543210123)},
                         {.kind=WASM_I32, .of.i32=0}};
    exact(m, "multi", 2, args, 2, args, 3);
    exact(m, "multi_br", 2, args, 2, args, 3);
    exact(m, "multi_table", 3, args, 2, args, 6);
    args[2].of.i32 = 1;
    exact(m, "multi_table", 3, args, 2, args, 5);
    exact(m, "typed_loop", 2, args, 2, args, 29);
    exact(m, "typed_loop_br", 2, args, 2, args, 41);
}
static void host_boundaries(Module m) {
    wasm_val_t arg = {.kind=WASM_I32, .of.i32=7}, result;
    for (uint64_t fuel = 0; fuel < 2; fuel++) {
        reset(m, fuel);
        require(!invoke(m, "hosted", 1, &arg, 1, &result), "underfunded native call must trap");
        require(host_calls == 0, "host side effects must not run after exhaustion");
        expect_exhausted(m);
    }
    reset(m, 2);
    require(invoke(m, "hosted", 1, &arg, 1, &result), "finite tail after native call may return debt");
    require(host_calls == 1 && host_fuel == 0, "publish exact remaining fuel before native call");
    require(result.of.i32 == 15, "native result survives unchecked tail");
    require(fuel_of(m.instance) == (uint64_t)-3 && !exhausted(m), "tail publishes exact debt without exhaustion");
    for (int branch = 0; branch < 2; branch++) {
        arg.of.i32 = branch;
        reset(m, branch ? 6 : 5);
        require(!invoke(m, "host_after_branch", 1, &arg, 1, &result), "join must retain debt");
        require(!host_calls, "if/else debt checked before native call");
        expect_exhausted(m);
        reset(m, 3);
        require(!invoke(m, "call_in_arm", 1, &arg, 1, &result), "call in either arm must trap before entry");
        require(!host_calls, "equal arm lengths must not defer native-call checks");
        expect_exhausted(m);
    }
}
static void termination_and_reuse(Module m) {
    const char *names[] = {"spin_br", "spin_if", "spin_table", "spin_mixed_table",
        "spin_nested", "spin_values", "spin_call", "cycle", "cycle_indirect"};
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
            reset(m, 64);
            wasm_val_t result;
            unsigned resultc = !strcmp(names[i], "spin_values");
            require(!invoke(m, names[i], 0, NULL, resultc, &result), "runaway must trap");
            expect_exhausted(m);
            scalar(m, "value", 0, 0, 42, 2);
        }
        const char *traps[] = {"trap", "memory_trap"};
        for (unsigned i = 0; i < 2; i++) {
            reset(m, 100);
            require(!invoke(m, traps[i], 0, NULL, 0, NULL), "ordinary guest trap preserved");
            require(!exhausted(m), "ordinary trap must not become fuel exhaustion");
            scalar(m, "value", 0, 0, 42, 2);
        }
        wasm_val_t args[2] = {{.kind=WASM_I32, .of.i32=123},
                             {.kind=WASM_I64, .of.i64=INT64_C(876543210123)}};
        wasm_val_t results[2];
        reset(m, 64);
        require(!invoke(m, "spin_values_br", 2, args, 2, results), "unconditional typed backedge must trap");
        expect_exhausted(m);
        scalar(m, "value", 0, 0, 42, 2);
    }
    /* Two separate successful entries share a budget until the host resets it. */
    reset(m, 4);
    wasm_val_t result;
    require(invoke(m, "value", 0, NULL, 1, &result), "first shared-budget entry");
    require(invoke(m, "value", 0, NULL, 1, &result), "second shared-budget entry");
    require(fuel_of(m.instance) == 0, "successful returns publish cumulative spend");
    require(!invoke(m, "value", 0, NULL, 1, &result), "third shared-budget entry must exhaust");
    expect_exhausted(m);
}
static void checkpoint_semantics(const char *directory) {
    Module m = load_module(directory, "checkpoints", 1000000, 1);
    wasm_val_t arg = {.kind=WASM_I32, .of.i32=7}, result;
    struct { const char *name; uint64_t fuel, cost; int result; } tails[] = {
        {"leaf", 1, 4, 8}, {"middle", 3, 9, 16}, {"outer", 5, 14, 19}
    };
    for (unsigned i = 0; i < sizeof(tails) / sizeof(*tails); i++) {
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        reset(m, tails[i].fuel);
        require(invoke(m, tails[i].name, 1, &arg, 1, &result), "finite leaf and caller tails return debt");
        require(result.of.i32 == tails[i].result, "nested unchecked result preserved");
        require(fuel_of(m.instance) == tails[i].fuel - tails[i].cost, "negative debt propagates through every caller");
        require(!exhausted(m), "finite debt alone does not set exhaustion marker");
        require(!invoke(m, "effect_leaf", 1, &arg, 1, &result), "next function entry must detect published debt");
        expect_exhausted(m);
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        require(progress == 0, "negative entry stops before any original guest instruction");
        scalar(m, "leaf", 1, 7, 8, 4);
    }
    int32_t progress = 0;
    memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
    reset(m, 0);
    require(!invoke(m, "effect_leaf", 1, &arg, 1, &result), "zero entry stops guest code");
    expect_exhausted(m);
    memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
    require(progress == 0, "zero entry has no guest side effect");
    reset(m, 3); /* first call publishes 1, effect_leaf spends 8 */
    require(!invoke(m, "effect_twice", 1, &arg, 1, &result), "second guest entry rejects first callee's debt");
    expect_exhausted(m);
    memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
    require(progress == 1, "only first callee executed its guest effect");

    const char *host_after[] = {"direct_host_after_debt", "indirect_host_after_debt"};
    for (unsigned i = 0; i < 2; i++) {
        reset(m, 3); /* leaf enters with 1 and returns -3 */
        require(!invoke(m, host_after[i], 1, &arg, 1, &result), "host guard must detect returned callee debt");
        require(host_calls == 0, "direct and table-import host effects are guarded");
        expect_exhausted(m);
        scalar(m, host_after[i], 1, 7, 16, 8 + i);
    }
    for (uint64_t fuel = 1; fuel <= 2; fuel++) {
        reset(m, fuel);
        require(!invoke(m, "indirect_host", 1, &arg, 1, &result), "underfunded table import must trap");
        require(!host_calls, "indirect guard rejects debt before import effects");
        expect_exhausted(m);
    }
    reset(m, 3);
    require(invoke(m, "indirect_host", 1, &arg, 1, &result), "fully funded indirect import may have an unchecked final tail");
    require(host_calls == 1 && host_fuel == 0, "indirect import sees published zero, never negative");
    require(result.of.i32 == 14 && fuel_of(m.instance) == UINT64_MAX && !exhausted(m), "indirect import tail publishes -1");

    reset(m, 3); /* the indirect call itself is funded, but callee entry sees 0 */
    require(!invoke(m, "indirect_leaf", 1, &arg, 1, &result), "indirect guest callee retains entry gate");
    expect_exhausted(m);
    reset(m, 4);
    require(invoke(m, "indirect_leaf", 1, &arg, 1, &result), "indirect leaf may return debt after funded entry");
    require(result.of.i32 == 8 && fuel_of(m.instance) == (uint64_t)-4 && !exhausted(m), "indirect guest debt accounting");

    reset(m, 3);
    require(!invoke(m, "unreachable_after_debt", 1, &arg, 0, NULL), "explicit unreachable retains fuel check");
    expect_exhausted(m);
    reset(m, 8);
    require(!invoke(m, "unreachable_after_debt", 1, &arg, 0, NULL), "funded explicit unreachable remains guest trap");
    require(!exhausted(m) && fuel_of(m.instance) == 0, "funded trap publishes exact cost without fuel exhaustion");

    arg.of.i32 = 1;
    reset(m, 4);
    require(invoke(m, "mixed_exit", 1, &arg, 1, &result), "mixed table may return through function label");
    require(result.of.i32 == 77 && fuel_of(m.instance) == 0, "mixed table exit publishes exact spend");
    reset(m, 3);
    require(!invoke(m, "mixed_exit", 1, &arg, 1, &result), "any loop target retains mixed-table check even on exit selection");
    expect_exhausted(m);
    arg.of.i32 = 0;
    reset(m, 64);
    require(!invoke(m, "mixed_exit", 1, &arg, 1, &result), "mixed table loop selection stays bounded");
    expect_exhausted(m);
    scalar(m, "leaf", 1, 7, 8, 4);
    destroy(m);
}
static void leaf_calls(const char *directory) {
    Module m = load_module(directory, "leaf-calls", 1000000, 1);
    uint32_t *memory = wasm_runtime_addr_app_to_native(m.instance, 0);
    require(memory != NULL, "leaf fixture memory");
    *memory = 77;
    scalar(m, "read", 1, 0, 77, 3);
    scalar(m, "read_direct", 1, 0, 77, 6);
    scalar(m, "read_table", 1, 0, 77, 7);
    wasm_val_t write_args[2] = {{.kind=WASM_I32, .of.i32=0}, {.kind=WASM_I32, .of.i32=99}};
    exact(m, "write", 2, write_args, 1, write_args + 1, 5);
    exact(m, "write_direct", 2, write_args, 1, write_args + 1, 9);
    require(*memory == 99, "specialized leaf preserves memory writes");
    scalar(m, "effect_div", 1, 6, 20, 11);
    scalar(m, "effect_direct", 1, 6, 20, 14);
    scalar(m, "effect_twice", 1, 6, 6, 26);
    scalar(m, "effect_host", 1, 6, 40, 15);
    scalar(m, "effect_if", 1, 1, 17, 18);
    scalar(m, "effect_if", 1, 0, 19, 19);
    scalar(m, "effect_br_if", 1, 1, 20, 15);
    scalar(m, "effect_br_if", 1, 0, 19, 18);
    scalar(m, "effect_guest", 1, 6, 20, 23);
    wasm_val_t pair_args[2] = {{.kind=WASM_I32, .of.i32=7},
                             {.kind=WASM_I64, .of.i64=INT64_C(876543210123)}};
    wasm_val_t pair_results[2] = {{.kind=WASM_I64, .of.i64=INT64_C(876543210123)},
                                {.kind=WASM_I32, .of.i32=10}};
    exact(m, "pair", 2, pair_args, 2, pair_results, 9);
    exact(m, "pair_direct", 2, pair_args, 2, pair_results, 13);
    scalar(m, "fill_direct", 1, 8, 8, 9);
    require(memory[0] == UINT32_C(0x07070707) && memory[1] == UINT32_C(0x07070707),
            "bulk-memory leaf retains its original effect and one-opcode charge");

    wasm_val_t arg = {.kind=WASM_I32, .of.i32=6}, result;
    for (uint64_t fuel = 1; fuel <= 3; fuel++) {
        reset(m, fuel);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        *memory = 77;
        int ok = invoke(m, "effect_direct", 1, &arg, 1, &result);
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        if (fuel <= 2) {
            require(!ok, "negative and zero specialized-callee entry must trap");
            expect_exhausted(m);
            require(progress == 0 && *memory == 77, "callee entry gate precedes global and memory effects");
        } else {
            require(ok && result.of.i32 == 20, "one positive entry unit permits a finite leaf");
            require(progress == 1 && *memory == 6, "positive entry executes leaf effects once");
            require(fuel_of(m.instance) == (uint64_t)-11 && !exhausted(m), "caller charges full successful leaf and publishes exact debt");
        }
    }
    const uint64_t twice_budgets[] = {3, 14, 15};
    for (unsigned i = 0; i < 3; i++) {
        reset(m, twice_budgets[i]);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        int ok = invoke(m, "effect_twice", 1, &arg, 1, &result);
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        if (i < 2) {
            require(!ok, "first leaf is charged before checking the next specialized call");
            require(progress == 1, "negative or zero second entry cannot execute leaf effects");
            expect_exhausted(m);
        } else {
            require(ok && result.of.i32 == 6 && progress == 2, "positive second entry preserves consecutive results and effects");
            require(fuel_of(m.instance) == (uint64_t)-11 && !exhausted(m), "consecutive leaves debit each cost once");
        }
    }
    reset(m, 14);
    require(invoke(m, "effect_host", 1, &arg, 1, &result), "host call follows successfully charged leaf");
    require(host_calls == 1 && host_fuel == 0, "host sees accurate local budget after a specialized leaf");
    require(result.of.i32 == 40 && fuel_of(m.instance) == UINT64_MAX, "host result and final debt survive leaf accounting");
    reset(m, 3);
    require(!invoke(m, "effect_host", 1, &arg, 1, &result), "leaf debt is checked before following host effects");
    require(!host_calls, "host is not called after a leaf spends beyond budget");
    expect_exhausted(m);

    /* Successful leaf costs must remain accounted for through structured
     * control and at every later observable checkpoint. */
    const char *exit_names[] = {"effect_if", "effect_br_if"};
    const uint64_t exit_costs[][2] = {{19, 18}, {18, 15}};
    for (unsigned kind = 0; kind < 2; kind++) {
        for (int take = 0; take < 2; take++) {
            arg.of.i32 = take;
            reset(m, 3);
            require(invoke(m, exit_names[kind], 1, &arg, 1, &result), "leaf debt survives either conditional exit path");
            require(result.of.i32 == (take ? (kind ? 20 : 17) : 19), "leaf result and selected exit value preserved");
            require(fuel_of(m.instance) == 3 - exit_costs[kind][take] && !exhausted(m), "conditional exit pays the complete leaf cost once");
        }
    }
    for (unsigned funded = 0; funded < 2; funded++) {
        reset(m, 19 + funded);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        require(!invoke(m, "effect_backedge", 0, NULL, 0, NULL), "leaf loop remains bounded at exact backedge budget");
        expect_exhausted(m);
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        require(progress == (int32_t)(2 + funded), "leaf cost is charged before a backedge can repeat header effects");
    }
    arg.of.i32 = 6;
    for (uint64_t budget = 14; budget <= 15; budget++) {
        reset(m, budget);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        int ok = invoke(m, "effect_guest", 1, &arg, 1, &result);
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        if (budget == 14) {
            require(!ok && progress == 1, "ordinary guest entry sees zero after the preceding leaf's complete cost");
            expect_exhausted(m);
        } else {
            require(ok && progress == 101 && result.of.i32 == 20, "one positive unit permits the ordinary guest effects");
            require(fuel_of(m.instance) == (uint64_t)-8 && !exhausted(m), "ordinary guest return preserves caller's prepaid leaf debt");
        }
    }

    /* A trap inside the leaf may leave private accounting unpublished. Check
     * trap kind and original effects, not the private shared-fuel snapshot. */
    for (unsigned division = 0; division < 2; division++) {
        arg.of.i32 = division ? 0 : 65536;
        reset(m, 3);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        *memory = 77;
        require(!invoke(m, division ? "effect_direct" : "read_direct", 1, &arg, 1, &result), "ordinary leaf trap remains observable");
        require(!exhausted(m), "ordinary leaf trap is not fuel exhaustion");
        const char *error = wasm_runtime_get_exception(m.instance);
        require(error && strstr(error, division ? "zero" : "out of bounds"), "leaf retains its trap category");
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        require(progress == (int32_t)division && *memory == (division ? 0U : 77U), "effects before an ordinary trap are preserved");
        scalar(m, "read_direct", 1, 0, division ? 0 : 77, 6);
    }
    arg.of.i32 = 0;
    reset(m, 0);
    require(!invoke(m, "read", 1, &arg, 1, &result), "export still selects the metered original leaf");
    expect_exhausted(m);
    reset(m, 3);
    require(!invoke(m, "read_table", 1, &arg, 1, &result), "table still selects original leaf with its zero-entry gate");
    expect_exhausted(m);
    reset(m, 4);
    require(invoke(m, "read_table", 1, &arg, 1, &result), "funded table call preserves original leaf accounting");
    require(fuel_of(m.instance) == (uint64_t)-3 && !exhausted(m), "table target remains metered exactly once");
    destroy(m);

    load_module(directory, "leaf-start", 0, 0);
    m = load_module(directory, "leaf-start", 1, 1);
    require(fuel_of(m.instance) == (uint64_t)-2, "start selects metered original even when a private clone exists");
    require(!invoke(m, "call_start", 0, NULL, 0, NULL), "startup debt is retained before specialized call");
    expect_exhausted(m);
    reset(m, 2);
    require(invoke(m, "call_start", 0, NULL, 0, NULL), "positive clone entry preserves finite startup helper");
    require(fuel_of(m.instance) == (uint64_t)-3 && !exhausted(m), "specialized startup-helper call has exact cost five");
    reset(m, 0);
    require(!invoke(m, "start_again", 0, NULL, 0, NULL), "exported startup helper retains original entry gate");
    expect_exhausted(m);
    destroy(m);
}
static void call_trees(const char *directory) {
    Module m = load_module(directory, "call-trees", 1000000, 1);
    wasm_val_t pair_args[2] = {{.kind=WASM_I32, .of.i32=7},
                             {.kind=WASM_I64, .of.i64=INT64_C(876543210123)}};
    wasm_val_t pair_results[2] = {{.kind=WASM_I64, .of.i64=INT64_C(876543210123)},
                                {.kind=WASM_I32, .of.i32=10}};
    exact(m, "pair_tree", 2, pair_args, 2, pair_results, 13);
    scalar(m, "stack_tree", 1, 7, 109, 17);
    scalar(m, "diamond_tree", 1, 7, 32, 34);
    scalar(m, "void_tree", 0, 0, 17, 7);
    scalar(m, "effect_tree", 1, 7, 7, 25);
    scalar(m, "effect_middle", 1, 7, 7, 22);
    scalar(m, "effect_table", 1, 7, 7, 26);
    scalar(m, "tree_if", 1, 1, 7, 28);
    scalar(m, "tree_if", 1, 0, 9, 28);
    for (int n = 0; n <= 4; n++) scalar(m, "tree_loop", 1, n, n, 34 * n + 8);
    scalar(m, "host_tree", 1, 7, 14, 6);
    scalar(m, "spend_tree", 1, 7, 14, 9);
    scalar(m, "indirect_tree", 1, 7, 8, 11);

    /* Caller -> middle -> leaf -> reused leaf. Each leaf costs nine; the
     * middle spends two before its first entry and one before its second.
     * These independently derived cutoffs describe the original checkpoints,
     * including successful returns with debt when only the last entry is paid.
     * Exercise original exported/table entries as well as selected direct calls. */
    const char *effect_names[] = {"effect_tree", "effect_middle", "effect_table"};
    const unsigned entry_prefix[] = {2, 0, 3};
    const unsigned costs[] = {25, 22, 26};
    uint32_t *memory = wasm_runtime_addr_app_to_native(m.instance, 0);
    require(memory != NULL, "call-tree fixture memory");
    wasm_val_t arg = {.kind=WASM_I32, .of.i32=7}, result;
    for (unsigned i = 0; i < 3; i++) {
        for (uint64_t budget = 0; budget <= 29; budget++) {
            reset(m, budget);
            int32_t progress = 0;
            memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
            *memory = 77;
            int ok = invoke(m, effect_names[i], 1, &arg, 1, &result);
            memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
            int expected_progress = budget >= entry_prefix[i] + 3;
            expected_progress += budget >= entry_prefix[i] + 13;
            require(progress == expected_progress, "tree guard/fallback preserves every original leaf entry and effect");
            require(*memory == (expected_progress ? 7U : 77U), "tree fallback preserves memory effect ordering");
            if (expected_progress < 2) {
                require(!ok, "underfunded nested entry must trap");
                expect_exhausted(m);
            } else {
                require(ok && result.of.i32 == 7, "funded final nested entry may finish with debt");
                require(fuel_of(m.instance) == budget - costs[i] && !exhausted(m), "tree fast and fallback paths charge the complete subtree once");
            }
        }
    }
    /* Values below arguments, multiple results and void results must survive
     * the low-fuel branch as well as the completely funded fast branch. */
    reset(m, 7);
    wasm_val_t results[2];
    require(invoke(m, "pair_tree", 2, pair_args, 2, results), "multivalue tree fallback succeeds with a funded last entry");
    require(results[0].of.i64 == pair_results[0].of.i64 && results[1].of.i32 == 10,
            "multivalue tree fallback retains argument and result order");
    require(fuel_of(m.instance) == (uint64_t)-6 && !exhausted(m), "multivalue tree fallback preserves exact debt");
    reset(m, 11);
    require(invoke(m, "stack_tree", 1, &arg, 1, &result) && result.of.i32 == 109,
            "tree fallback preserves live operand below call arguments");
    require(fuel_of(m.instance) == (uint64_t)-6 && !exhausted(m), "live-operand fallback charges exact subtree");
    reset(m, 4);
    require(invoke(m, "void_tree", 0, NULL, 1, &result) && result.of.i32 == 17,
            "void tree fallback preserves live operand below call");
    require(fuel_of(m.instance) == (uint64_t)-3 && !exhausted(m), "void fallback charges exact subtree");
    for (int take = 0; take < 2; take++) {
        arg.of.i32 = take;
        reset(m, 17);
        require(invoke(m, "tree_if", 1, &arg, 1, &result) && result.of.i32 == (take ? 7 : 9),
                "nested typed guard fallback retains its enclosing if arm");
        require(fuel_of(m.instance) == (uint64_t)-11 && !exhausted(m), "if join retains the fallback tree's exact debt");
    }
    arg.of.i32 = 1;
    for (uint64_t budget = 0; budget <= 43; budget++) {
        reset(m, budget);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        int ok = invoke(m, "tree_loop", 1, &arg, 1, &result);
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        require(progress == (budget >= 11) + (budget >= 21), "loop tree guard preserves nested leaf entry effects");
        if (budget < 36) {
            require(!ok, "loop must charge the entire tree before its backedge check");
            expect_exhausted(m);
        } else {
            require(ok && result.of.i32 == 1, "funded backedge after tree may execute final exiting header");
            require(fuel_of(m.instance) == budget - 42 && !exhausted(m), "tree within loop retains exact final-header debt");
        }
    }

    /* Ordinary traps after a nested guest effect retain their category on
     * either path. Private fuel publication on an ordinary trap is unspecified. */
    const uint64_t trap_budgets[] = {4, 5, 16, 17, 100};
    arg.of.i32 = 0;
    for (unsigned i = 0; i < sizeof(trap_budgets) / sizeof(*trap_budgets); i++) {
        reset(m, trap_budgets[i]);
        int32_t progress = 0;
        memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
        *memory = 77;
        require(!invoke(m, "trap_tree", 1, &arg, 1, &result), "nested ordinary trap is retained");
        memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
        if (trap_budgets[i] == 4) {
            expect_exhausted(m);
            require(!progress && *memory == 77, "underfunded nested leaf cannot perform effects before trapping");
        } else {
            const char *error = wasm_runtime_get_exception(m.instance);
            require(!exhausted(m) && error && strstr(error, "zero"), "ordinary nested division trap is not fuel exhaustion");
            require(progress == 1 && !*memory, "nested effects preceding ordinary trap are preserved");
        }
    }
    arg.of.i32 = 7;
    for (uint64_t budget = 0; budget <= 8; budget++) {
        reset(m, budget);
        int ok = invoke(m, "host_tree", 1, &arg, 1, &result);
        if (budget < 4) {
            require(!ok && !host_calls, "host descendant cannot be bypassed by a tree clone");
            expect_exhausted(m);
        } else {
            require(ok && result.of.i32 == 14 && host_calls == 1 && host_fuel == budget - 4,
                    "host descendant sees the exact published nested budget");
            require(fuel_of(m.instance) == budget - 6 && !exhausted(m), "host descendant returns exact nested debt");
        }
        reset(m, budget);
        ok = invoke(m, "indirect_tree", 1, &arg, 1, &result);
        if (budget < 6) {
            require(!ok, "indirect descendant retains its original guest entry check");
            expect_exhausted(m);
        } else {
            require(ok && result.of.i32 == 8, "funded indirect descendant still succeeds");
            require(fuel_of(m.instance) == budget - 11 && !exhausted(m), "indirect descendant retains exact accounting");
        }
    }
    for (uint64_t budget = 14; budget <= 15; budget++) {
        reset(m, budget);
        require(!invoke(m, "trap_checkpoint_tree", 1, &arg, 0, NULL), "explicit trap descendant still traps");
        if (budget == 14) expect_exhausted(m);
        else require(!exhausted(m) && fuel_of(m.instance) == 0,
                     "explicit trap descendant retains its funded checkpoint");
    }
    const char *cycles[] = {"cycle_tree", "loop_tree"};
    for (unsigned i = 0; i < sizeof(cycles) / sizeof(*cycles); i++) {
        reset(m, 64);
        require(!invoke(m, cycles[i], 0, NULL, 0, NULL), "cyclic descendant must remain bounded");
        expect_exhausted(m);
    }
    destroy(m);

    m = load_module(directory, "tree-types", 1000000, 1);
    const unsigned type_indices[] = {63, 64, 127, 128};
    pair_results[1].of.i32 = 7;
    for (unsigned i = 0; i < sizeof(type_indices) / sizeof(*type_indices); i++) {
        char name[64];
        snprintf(name, sizeof(name), "tree_type_%u", type_indices[i]);
        exact(m, name, 2, pair_args, 2, pair_results, 11);
        reset(m, 7);
        require(invoke(m, name, 2, pair_args, 2, results), "signed type-index fallback validates and runs");
        require(results[0].of.i64 == pair_results[0].of.i64 && results[1].of.i32 == 7,
                "signed type-index fallback retains multiple results");
        require(fuel_of(m.instance) == (uint64_t)-4 && !exhausted(m), "signed type-index fallback retains exact debt");
    }
    destroy(m);
    const char *analysis_cases[] = {"deep-tree", "expensive-tree"};
    const char *analysis_exports[] = {"deep_tree", "expensive_tree"};
    for (unsigned i = 0; i < 2; i++) {
        m = load_module(directory, analysis_cases[i], 1000000, 1);
        reset(m, 64);
        require(!invoke(m, analysis_exports[i], 0, NULL, 0, NULL), "deep or overflowing-cost graph retains low-fuel checkpoints");
        expect_exhausted(m);
        destroy(m);
    }
    load_module(directory, "tree-start", 2, 0);
    m = load_module(directory, "tree-start", 3, 1);
    require(fuel_of(m.instance) == (uint64_t)-4, "nested startup retains exact finite debt");
    require(!invoke(m, "call_tree_start", 0, NULL, 0, NULL), "nested startup debt is retained before subsequent entry");
    expect_exhausted(m);
    reset(m, 9);
    require(invoke(m, "call_tree_start", 0, NULL, 0, NULL), "funded direct nested startup helper succeeds");
    require(fuel_of(m.instance) == 0 && !exhausted(m), "nested startup helper costs are charged exactly once");
    reset(m, 0);
    require(!invoke(m, "tree_start", 0, NULL, 0, NULL), "exported nested startup original retains its entry gate");
    expect_exhausted(m);
    destroy(m);
}
static void startup(const char *directory) {
    load_module(directory, "start-loop", 64, 0);
    load_module(directory, "ctors-loop", 64, 0);
    Module m = load_module(directory, "start-finite", 2, 1);
    require(fuel_of(m.instance) == UINT64_MAX, "finite implicit start may publish negative debt");
    require(!invoke(m, "initialize", 0, NULL, 0, NULL), "next initializer detects startup debt");
    expect_exhausted(m);
    int32_t progress;
    memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
    require(progress == 7, "rejected initializer cannot execute its guest effect");
    destroy(m);
    m = load_module(directory, "start-finite", 6, 1);
    require(fuel_of(m.instance) == 3, "implicit start publishes its spend");
    require(invoke(m, "initialize", 0, NULL, 0, NULL), "explicit initializer shares start budget");
    require(fuel_of(m.instance) == 0, "lifecycle shared budget spent exactly");
    wasm_val_t result;
    require(!invoke(m, "ok", 0, NULL, 1, &result), "lifecycle fuel is not renewed implicitly");
    expect_exhausted(m);
    scalar(m, "ok", 0, 0, 42, 2);
    destroy(m);
}
static void rejection(const char *directory) {
    current_case = "rejection";
    size_t length, output_length = 123;
    uint8_t *original = read_file(directory, "fixtures", &length), *output = (void *)1;
    char error[512];
    require(!wasmMeterInject(original, length, (uint64_t)INT64_MAX + 1,
                             &output, &output_length, error, sizeof(error)), "reject signed fuel overflow");
    require(!output && !output_length, "failure clears output ownership");
    free(original);
    original = read_file(directory, "reserved", &length);
    output = (void *)1; output_length = 123;
    require(!wasmMeterInject(original, length, 64, &output, &output_length,
                             error, sizeof(error)), "private export prefix is reserved");
    require(!output && !output_length, "reserved-name failure clears output ownership");
    free(original);
    const uint8_t invalid_local[] = {0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,
                                    3,2,1,0,10,6,1,4,0,32,0,11};
    uint8_t copy[sizeof(invalid_local)];
    memcpy(copy, invalid_local, sizeof(copy));
    require(!wasm_runtime_load(copy, sizeof(copy), error, sizeof(error)),
            "validate original first: appended private local must not legitimize invalid local");
    const char *invalid_refs[] = {"invalid-call", "invalid-export", "invalid-start", "invalid-element"};
    for (unsigned i = 0; i < sizeof(invalid_refs) / sizeof(*invalid_refs); i++) {
        current_case = invalid_refs[i];
        original = read_file(directory, invalid_refs[i], &length);
        uint8_t *validation = malloc(length);
        require(validation != NULL, "invalid original validation copy");
        memcpy(validation, original, length);
        wasm_module_t invalid = wasm_runtime_load(validation, length, error, sizeof(error));
        require(!invalid, "validate original first: cloned indices must not legitimize invalid original references");
        free(validation);
        if (!i) {
            output = (void *)1; output_length = 123;
            require(!wasmMeterInject(original, length, 64, &output, &output_length,
                                     error, sizeof(error)), "injector rejects direct calls beyond the original function space");
            require(!output && !output_length, "invalid call clears output ownership");
        }
        free(original);
    }
    original = read_file(directory, "leaf-calls", &length);
    output = (void *)1; output_length = 123;
    require(!wasmMeterInject(original, length - 1, 64, &output, &output_length,
                             error, sizeof(error)), "truncated leaf module fails closed");
    require(!output && !output_length, "malformed leaf input clears output ownership");
    free(original);
    const uint8_t unknown[] = {0,97,115,109,1,0,0,0,1,4,1,96,0,0,
                              3,2,1,0,10,5,1,3,0,255,11};
    output = (void *)1; output_length = 123;
    require(!wasmMeterInject(unknown, sizeof(unknown), 64, &output, &output_length,
                             error, sizeof(error)), "unknown instruction fails closed");
    require(!output && !output_length, "decode failure clears output ownership");
}
static void prefix_cases(const char *directory) {
    Module m = load_module(directory, "prefix", 1000000, 1);
    for (int n = 0; n <= 7; n++) {
        /* Four header opcodes on every entry. The fixture costs are:
         * simple body: six for the equal-arm if, four for i++, one backedge.
         * typed body additionally advances its i64 loop parameter by two ops. */
        scalar(m, "prefix_simple", 1, n, n, 15 * n + 8);
        scalar(m, "prefix_typed", 1, n, 123 + n, 17 * n + 9);
    }
    scalar(m, "prefix_memory", 1, 0, 0, 13);
    scalar(m, "prefix_division", 1, 0, 0, 14);
    const char *traps[] = {"prefix_memory", "prefix_division"};
    const char *messages[] = {"out of bounds", "zero"};
    const uint64_t checkpoint[] = {26, 27};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned enough = 0; enough < 2; enough++) {
            uint64_t budget = checkpoint[i] - 1 + enough;
            reset(m, budget);
            int32_t progress = 0;
            memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
            wasm_val_t arg = {.kind=WASM_I32, .of.i32=1}, result;
            require(!invoke(m, traps[i], 1, &arg, 1, &result), "prefix must trap on its second entry");
            if (!enough) expect_exhausted(m);
            else {
                require(!exhausted(m), "prepaid next header must not move exhaustion earlier");
                const char *error = wasm_runtime_get_exception(m.instance);
                require(error && strstr(error, messages[i]), "ordinary prefix trap is preserved");
                require(fuel_of(m.instance) == budget, "guest trap leaves unpublished shared fuel intact");
            }
            memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
            require(progress == 1, "first loop body ran before the tested checkpoint");
            scalar(m, "prefix_simple", 1, 2, 2, 38);
        }
    }
    destroy(m);
    m = load_module(directory, "prefix-boundaries", 1000000, 1);
    const unsigned prefixes[] = {63, 64, 65, 127, 128, 129, 8191, 8192, 8193};
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++) {
        unsigned prefix = prefixes[i];
        char name[64];
        snprintf(name, sizeof(name), "prefix_%u", prefix);
        for (int n = 0; n <= 2; n++)
            scalar(m, name, 1, n, n, prefix * (n + 1) + 11 * n + 4);
        /* The first backedge has consumed 2 + prefix + 11 original ops.
         * At that exact budget it must enter the next header, whose progress
         * increment makes the negative signed-LEB threshold observable. */
        for (unsigned enough = 0; enough < 2; enough++) {
            reset(m, prefix + 12 + enough);
            int32_t progress = 0;
            memcpy(global_data(m.instance, "progress"), &progress, sizeof(progress));
            wasm_val_t arg = {.kind=WASM_I32, .of.i32=1}, result;
            if (!enough) {
                require(!invoke(m, name, 1, &arg, 1, &result), "underfunded backedge must exhaust");
                expect_exhausted(m);
            } else {
                require(invoke(m, name, 1, &arg, 1, &result), "final exiting header may return negative debt");
                require(result.of.i32 == 1, "partial prefix result preserved");
                require(fuel_of(m.instance) == (uint64_t)-(int64_t)(prefix + 2), "final prefix debt is exact");
                require(!exhausted(m), "unchecked final prefix tail has not observed exhaustion");
            }
            memcpy(&progress, global_data(m.instance, "progress"), sizeof(progress));
            require(progress == (int32_t)(1 + enough), "signed threshold preserves the exact backedge checkpoint");
        }
    }
    destroy(m);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    RuntimeInitArgs init = {0};
    init.running_mode = Mode_Fast_JIT;
    init.mem_alloc_type = Alloc_With_System_Allocator;
    init.native_module_name = "env";
    init.native_symbols = imports;
    init.n_native_symbols = sizeof(imports) / sizeof(*imports);
    require(wasm_runtime_full_init(&init), "stock FastJIT initialization");
    Module m = load_module(argv[1], "fixtures", 1000000, 1);
    accounting(m);
    host_boundaries(m);
    termination_and_reuse(m);
    destroy(m);
    startup(argv[1]);
    checkpoint_semantics(argv[1]);
    leaf_calls(argv[1]);
    call_trees(argv[1]);
    prefix_cases(argv[1]);
    rejection(argv[1]);
    m = load_module(argv[1], "wide-indices", INT64_MAX, 1);
    require(fuel_of(m.instance) == INT64_MAX, "signed maximum initial fuel encoding");
    wasm_val_t args[3] = {{.kind=WASM_I32, .of.i32=123},
                         {.kind=WASM_I64, .of.i64=INT64_C(876543210123)},
                         {.kind=WASM_I32, .of.i32=0}};
    exact(m, "wide", 3, args, 1, args + 1, 6);
    exact(m, "wide_call", 3, args, 1, args + 1, 11);
    destroy(m);
    m = load_module(argv[1], "clone-indices", 1000000, 1);
    scalar(m, "clone_index", 1, 7, 8, 7);
    destroy(m);
    m = load_module(argv[1], "wide-calls", 1000000, 1);
    scalar(m, "wide_guest", 1, 7, 8, 7);
    scalar(m, "wide_host", 1, 7, 14, 3);
    reset(m, 1);
    wasm_val_t host_arg = {.kind=WASM_I32, .of.i32=7}, host_result;
    require(!invoke(m, "wide_host", 1, &host_arg, 1, &host_result), "last wide import index remains guarded");
    require(host_calls == 0, "wide import classification stops host effects");
    expect_exhausted(m);
    destroy(m);
    wasm_runtime_destroy();
    printf("PASS: %u checks in stock WAMR FastJIT: exact finite fuel, INT64_MAX, "
           "branches/joins/multivalue, calls/recursion/native synchronization, "
           "all loop edges, explicit checkpoint/debt semantics, trap recovery and shared startup fuel.\n", checks);
    return 0;
}
