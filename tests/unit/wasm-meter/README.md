# Fuel injector regression checks

These checks are regression evidence, not a proof of equivalence. See
[the correctness argument and its limits](CORRECTNESS.md) for the intended
preservation contract, concrete resource-limit differences, and outstanding
proof obligations.

Run after building Redis with stock WAMR FastJIT:

```sh
python3 tests/unit/wasm-meter/run.py
```

The runner needs a C compiler, Python 3 and WABT's `wat2wasm`. It builds only a
small standalone verifier in a temporary directory and links the existing WAMR
archive. It does not rebuild or modify any dependency. Use `--build-dir PATH`
to retain generated Wasm and the verifier, or `--wamr-library PATH` to select an
existing FastJIT archive. A 30 second process timeout catches missing runaway
checks.

Expected costs are derived from the fixture's original dynamic instructions,
including control delimiters reached by fallthrough. Tests cover budgets equal
to the exact cost, larger than necessary and `INT64_MAX`. For fixtures ending
with an unchecked finite tail, one-short budgets must return the correct value
and publish exactly -1 fuel. An empty function with zero starting fuel must
trap at entry. Native test
imports inspect published fuel and simulate a nested spend to detect stale
cached counters. They also verify that exhaustion stops execution before native
side effects. Branch/function returns, if joins, typed loops, multivalue results,
direct and indirect recursion, every backedge form, repeated trap recovery and
the shared startup budget are exercised in actual stock WAMR FastJIT.

Function entry rejects non-positive fuel before any original instruction.
Loop backedges, direct imports, all indirect calls, and explicit `unreachable`
retain their exhaustion checks. Direct guest calls and function exits debit
and publish fuel without checking it; finite leaf and caller tails may return
a negative balance. Tests assert exact negative debt through nested returns,
rejection by the next guest entry, direct and table-import host guards,
function-label branches, mixed loop/function tables, and recovery after reset.
Successful calls retain exact accounting; fuel traps saturate shared fuel at
zero and set the exhaustion marker. These cases specify the new checkpoint
semantics explicitly rather than requiring every underfunded finite call to
trap. Guest/import function indices crossing 127/128 test call classification.

Loop-header prepayment checks cover equal-arm branches, typed loop parameters,
zero iterations, unchanged memory/division trap ordering, and signed-LEB prefix
cost boundaries. A guest progress global makes the precise backedge checkpoint
observable when the next header has been prepaid into a negative local balance.
A funded last backedge followed by a final exiting header now returns the exact
negative balance; an underfunded backedge still traps before the next header.

Caller-paid leaf tests cover direct fixed-cost callees containing ordinary
loads/stores, guest global effects, division traps, locals, multiple results,
and bulk memory operations. Entry gates must run before callee effects, and a
successful leaf's full cost must be deducted before the next guest or host call.
Leaf accounting also survives conditional exits and if joins, remains accurate
at loop backedges and explicit traps, and reaches ordinary metered guest callees.
Tests distinguish negative, zero and one-unit-positive callee entry; they retain
ordinary trap categories and effects without asserting private fuel values on
ordinary traps. Original exported, table and start entries remain metered even
when a directly called private clone exists. Original/clone function indices
crossing 127/128, wide local/global indices, and original validation of invalid
call/export/start/element references guard the appended function namespace.

Nested direct-call tree tests cover forward references, shared/repeated children,
multiple arguments/results, void signatures and live operands below a call.
Exhaustive small budgets assert the original guest effects and entry checkpoints
through the guarded fast path and its metered fallback, including finite negative
tails. Guards inside if arms and loop bodies retain join accounting and exact
backedge exhaustion after the full subtree cost. Ordinary nested division traps,
host budget publication and mutation,
indirect callees, recursive cycles, loop descendants and explicit trap checkpoints
retain their original behavior. Exported, table and startup entries still select
the original metered functions. Generated signature indices 63, 64, 127 and 128
exercise signed-LEB block types; a 4,096-function forward chain exercises deep
analysis without executing to that stack depth. An 80-level doubled-child DAG
has a dynamic cost exceeding both 32-bit and signed 64-bit ranges: its low-budget
run must exhaust promptly rather than selecting an unmetered wrapped-cost tree.
