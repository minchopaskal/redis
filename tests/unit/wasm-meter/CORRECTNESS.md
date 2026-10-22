# Fuel instrumentation: preservation argument and limits

This is a manual correctness argument and audit record, **not a formal proof of
the C implementation**. Regression tests and validating the generated module
do not establish semantic equivalence for all inputs. The reviewed
`src/wasm_meter.c` has SHA-256
`b9f88dc576c15fc853dffb2c69ab69258f5f9176b72dc9c183ef87019281496d`.

## The claim that needs proving

Let `M` be the original module and `T(M, F)` its instrumented module with fuel
`F`. The useful target is preservation of original program observations:

- function results;
- original linear memory, globals, tables, and data-segment state;
- ordered host calls, their arguments, results, and guest effects;
- ordinary guest traps and the original effects preceding them.

The projection `P` removes the two private globals, appended fuel locals,
metering operations and administrative control frames, and maps a private
clone back to its original function. Compare states at corresponding original
instruction boundaries, not at every injected instruction.

The intended conditional statement is:

```text
P(execution of T(M, F)) = execution of M
```

under these conditions:

1. The original module is valid, uses the supported subset and is accepted by
   the injector. Both modules successfully load and instantiate.
2. Corresponding calls start with the same original state and arguments.
   Host operations produce corresponding results and effects, cannot observe
   private meter state or compiled call frames, and do not distinguish timings.
3. No injected exhaustion trap occurs in the execution being compared.
4. Corresponding allocation outcomes agree, including `memory.grow`; neither
   execution encounters an instrumentation-dependent stack/resource limit.
5. The runtime executes the supported Wasm operations correctly. Where the
   specification allows nondeterministic results, choices are coupled; a
   universal claim of identical arithmetic NaN payloads is not made.

For executions interrupted by fuel, retain the other assumptions and replace
condition 3 with this separate target: guest effects before interruption form a
prefix of an original execution. This does not imply transaction rollback or
termination of a host call that never returns.

These are theorems to establish about the implementation. The argument below
explains why the intended rewrite rules satisfy them; it does not certify that
the parser and emitter implement those rules for every accepted binary.

## Why unconditional equivalence is false

Fuel traps deliberately change behavior. Additional exports, code size, debug
locations and runtime resource consumption are also observable differences.
Valid Wasm outside the accepted subset, or using reserved export names, may be
rejected.

Two differences were reproduced with the unmodified stock WAMR FastJIT archive
`c074257a175df8c134bc201ca41c3d87d1d497fa36f3557c28d4b6c9124d183b`:

| Input / limit | Original | Injected |
| --- | --- | --- |
| Function with 65,534 `i32` locals | Loads | Fails loading: `local count too large` |
| Recursive function, 64 KiB execution stack, argument 176, `INT64_MAX` fuel | Returns `7` | Operand-stack overflow; exhaustion flag remains zero |

The new `i64` local consumes two WAMR local cells. Instrumentation also changes
compiled frame sizes. For the recursive fixture, the measured last successful
arguments were 219 and 175 respectively. These values describe this fixture and
runtime build, not universal recursion limits. Increasing the configured stack
would move the boundary; it would not establish unconditional equivalence.

## Preservation by rewrite rule

The proof method is a simulation: each original operation is matched by a finite
sequence of transformed operations with the same projected state and effects.
Extra meter operations are silent under `P` when they do not trap. This uses the
Wasm abstract-machine approach described in the
[execution conventions](https://webassembly.github.io/spec/core/exec/conventions.html).

### Private state and unchanged indices

`analyze`, `globals`, `functionSection`, and `code` append state and functions.
Existing local, global, type, function, export, start and element references keep
their original meanings. Original validation must happen **before** appending:
otherwise an invalid index could become a valid reference to private state.
`wasmEngineCreate` does this using a separate copy, because WAMR may mutate its
input. It also validates the generated module by loading it again.

The obligation is stronger than type validity: all original definitions and
references must remain unchanged except the explicitly permitted rewrites.

### Meter arithmetic

`charge`, `publish`, `reload`, and a passing `entryCheck` consume only their own
temporary operands and access only private locals/globals. If the original
operand stack is `S`, a completed nontrapping update leaves exactly `S`.
Consequently they preserve original memory, globals, locals and host effects.

This proves semantic silence of the intended sequences, independently of whether
the numeric fuel cost is correct. Correct fuel accounting is a separate claim.

### Branch labels and returns

The exhaustion block sits immediately inside the implicit function label.
For an original branch depth `d` at original label count `D`, the mapping is:

```text
mapped(d) = d       when d < D - 1
mapped(d) = d + 1   when d = D - 1  (the function label)
```

`branchTarget` applies this to `br`, `br_if` and every `br_table` target.
The original branch condition/index and branch arguments are untouched.
Publishing before a possible function exit is stack-neutral, including when a
conditional branch falls through. The final function `end` becomes a publish
followed by `return`, preserving its results and avoiding the cold trap path.

A fused unconditional loop branch first computes a private fuel predicate.
When it passes, the injected `br_if` takes the same original loop label with the
same arguments. When it fails, execution reaches the exhaustion path. A loop's
label arguments and a block's result arguments must not be conflated. The
[control-instruction rules](https://webassembly.github.io/spec/core/exec/instructions.html#control-instructions)
define these stack effects.

### Grouped segments, comparisons and prepaid headers

`equalIf` retains the original entire `if/else`; it changes only meter placement.
Both accepted arms are linear, have equal cost and contain no calls or branches.
Thus the original condition still selects the original arm and its effects.
Ordinary trapping instructions remain in their original order.

Hoisting a debit before an integer comparison preserves its operands. Those
comparisons have no guest side effects and cannot trap. A passing debit therefore
commutes with the comparison; an exhaustion trap stops before any additional
observable guest effect.

`loopPrefix` leaves original header/body instructions intact. Its arithmetic
invariant is more subtle: after prepaying a header of cost `H`, the stored local
is `L = R - H`, where `R` is the balance before that header. Executing the header
consumes this credit without a second debit. For the backedge, let `U` be the
balance after the current header and before the body of cost `B`:

```text
L_next = U - B - H
L_next >= -H  iff  U - B >= 0
```

The adjusted comparison preserves that checkpoint, assuming no signed wrap.
On successful return the final prepaid header has executed. On an ordinary trap
partway through it, the private balance need not match a per-instruction counter.

### Calls and private clones

Ordinary calls retain their original target and signature. Publishing before and
reloading after a call changes private state only. Indirect calls retain their
table, selector, signature check and original function identities.

A leaf clone has the original signature, locals and body. Hence its return
values and original effects are identical, subject to the resource assumptions.
Its caller performs the entry gate and charges its fixed cost after success.

For a nested fixed-cost tree, prove equivalence by induction over the resolved
acyclic call graph. Leaves establish the base case. A parent retains every
original non-call instruction in order; each rewritten direct call goes to a
same-signature clone already covered by the induction hypothesis. Therefore the
parent has the same results, effects and ordinary traps.

The surrounding typed `if` uses the callee's exact parameters and results.
Each arm consumes the existing call arguments and returns the same result types,
leaving values below those arguments intact. The fast arm calls the clone; the
fallback calls the original metered function. Original branch instructions are
never placed inside this extra `if`, so their label depths do not gain another
offset.

The full-cost guard also has a separate fuel argument. For a resolved callee:

```text
K(f) = original opcode cost of f + sum K(callee) over its direct call sites
```

If the remaining balance covers `K(f)`, every nested entry occurs before all of
that positive cost has been spent. Therefore every skipped entry check would
pass. Costs count repeated call sites separately. Cycles, control flow,
host/indirect calls, explicit traps, unresolved descendants and overflowing
costs must remain ineligible. This reasoning relies on `prepareCalls` classifying
and calculating correctly; it is not a formal verification of that C routine.

### Composition

Starting from related states, a silent meter sequence preserves the relation;
an original operation makes the same projected transition; a rewritten branch
selects the corresponding continuation; and a clone call has the same behavior
by induction. Induction over a finite execution then gives the intended
observation equality. If a meter traps first, the already-matched observations
give a prefix instead. Resource failures and host observations excluded above
cannot be silently removed from this argument.

## Fuel correctness is a distinct obligation

Preservation with ample fuel does not establish that costs or checks are correct.
The current policy checks function entry, loop backedges, host/indirect calls and
explicit traps. Finite tails may finish with negative fuel. It is **not** a
strict policy that traps immediately before instruction `F + 1`.

An interruption proof must additionally show that every possible guest cycle
crosses a positive-cost check, private clones are finite, costs cannot wrap, and
host code cannot replenish or corrupt fuel outside its contract. Structured loop
branches and recursive calls cover guest cycles in the supported subset. The
signed-underflow bound relies on Redis's bounded module and execution stack; it
must not be assumed for arbitrary callers of the standalone injection API.
Time inside a host command and the work done by one bulk-memory opcode are not
bounded by an instruction count.

## Evidence and remaining work

The existing focused suite contains 6,776 assertions about explicit results,
costs and checkpoints. Running it against old and new injectors was useful
regression evidence, but did not compare all cases to the original bytecode.

An independent original-versus-injected FastJIT run on 2026-10-22 passed 399
invocations / 8,578 assertions, including 41 ordinary-trap cases and 49 cases
with host traces. It compared results, complete linear memory and size, original
exported globals, host arguments/results, and ordinary trap text. At host calls
it also compared the first 16 memory bytes and the exported progress global;
five cases exercised two calls with changed state between them.
Cases included branches, typed loops, multivalue calls,
leaf/tree calls, mixed numerical results, memory growth, and invalid indirect
calls. No injected exhaustion occurred. This is finite evidence, not a proof;
unexported state, every accepted module and every environment are not covered.

To claim a proof of the implementation, the missing work includes:

1. A precise reference fuel policy, including deferred charges and every
   exhaustion checkpoint, separate from original Wasm semantics.
2. A formal model of the supported instructions and rewrite rules, and checked
   simulation proofs for those rules.
3. A bridge from emitted bytes to that model: either verify the C decoder,
   classifier and emitter, or independently validate each transformation.
   A small checker could verify unchanged original operations and operands,
   index/label mappings, permitted meter sequences, clone signatures/call graph,
   and cost certificates. Its own soundness would still need proving.
4. Explicit resource and host assumptions, plus continued differential testing
   of the actual WAMR execution path to detect implementation mistakes.

Decoding into an explicit instruction/control-flow representation and applying
small named passes would make these obligations easier to review than the
current interleaved scanning, optimization and byte emission. Neither that
refactor nor an independent checked validator is implemented by this document.
