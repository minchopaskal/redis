# What is proved, and what is checked

This directory contains a small Lean 4 proof of metering erasure and control-flow
simulation, plus checkers connected to receipts from the production C emitter.
It is **not a formal verification of the complete C injector, WebAssembly
decoder, or WAMR**. The distinction matters: a correctly typed module and a
successful byte comparison alone do not prove preserved program behavior.

The toolchain is pinned to Lean 4.35.0 in `lean-toolchain`; no Mathlib or other
Lean packages are required. `lake build` checks the generic proofs. It also
prints their foundational axiom dependencies through `AxiomAudit.lean`. There
are no project-declared axioms, admitted proofs, `unsafe` proof definitions, or
`native_decide` proofs. Some standard library/tactic proofs use Lean's ordinary
foundational axioms (`propext`, `Quot.sound`, and `Classical.choice`), which are
reported rather than hidden.

## The semantic core

`Metering.lean` defines separate guest and private meter state. Its execution
theorems preserve the complete guest state and ordered observations when private
steps pass. If a checkpoint fails, the preceding observations and state form an
original execution prefix. Results and ordinary trap outcomes can be represented
in the guest state or observations. Their actual Wasm semantics is a parameter;
the model does not invent semantics for individual Wasm instructions.

It also proves concrete supporting facts: inserting the exhaustion label keeps
every original label lookup correct under the depth remapping; typed dispatch
preserves unrelated stack operands when its arms implement the same call;
appended clone indices recover their original identity; a fully funded positive
cost leaves positive fuel at every proper prefix; and the prepaid-loop threshold
is algebraically equivalent to the original checkpoint.

`ControlFlow.lean` goes beyond replaying a preselected instruction list. An
executable checker verifies a finite source graph, target graph and origin map:

- Guest operations and the ordered lists of projected successor locations agree.
- Private meter and fuel-guard edges keep the same original location.
- Target halts map to source halts, and all target indices are covered.

The operational semantics makes branch choices during execution. The checked
simulation theorem applies to **every finite target run**, preserving its guest
state and observations in a source run. Completed target runs correspond to
source halts; fuel interruption corresponds to a source prefix. A separate rank
checker proves that private-step chains are bounded and cannot contain a cycle.
Guest loops remain allowed. This is forward simulation and private-step progress,
not a proof that every source execution is reproduced or that fuel bounds all
host work. `CheckerChecks.lean` kernel-checks rejection of altered operations,
successor order/multiplicity, dangling targets and private-only cycles.

`Calls.lean` separately proves interprocedural preservation in a normalized
module model. A call explicitly executes its callee and then its continuation,
with signature-indexed frame entry/exit relations. Induction on a finite
execution proves both parts, so it covers recursive calls and many-to-one clone
origin mappings without assuming that callees already behave equivalently.
The theorem covers completed normal returns, not interrupted or trapping call
executions. Its local module-correspondence premises are not yet produced by the
actual-byte CFG adapter; that adapter's concrete coverage remains no-call bodies.

Fuel is modeled with mathematical integers. Relating it to Wasm `i64` still
requires the documented no-wrap bound. Finite returns with negative fuel are
intentional; immediate exhaustion before every over-budget instruction is not
the implemented policy.

## Connection to the actual emitter

`receipt.c` records the named operations and actual output of the production
emitter. `check_receipt.py` independently parses the original and output modules,
checks their correspondence and operation contexts, and validates original
indices, private clone identities/signatures/costs, and ordered source coverage.
`Audit.lean` independently encodes the named operations and compares every
function-body output byte with the real output. It also proves that a successful
receipt recovers the original source bytes. Every operation's source position,
including private operations that consume no source bytes, must equal the current
source cursor; this binds its branch-depth context to its actual placement.

The streaming emitter can produce a begin event and partial operation events
before injection fails. Discard every event on failure: only a complete receipt
from a successful module injection is a certificate candidate.

There are two different forms of evidence:

1. The compiled Lean checker executes on receipts. Its generic correctness
   theorems are kernel-checked, but executing the checker is not itself a kernel
   proof term for each particular input.
2. `kernel_receipt.py` emits concrete Lean data and `by decide` theorems for
   selected actual receipts. Lean's kernel reduces those certificates without
   using `native_decide`.

`check_cfg.py` additionally constructs graphs from the real original module and
validated emitter receipt. It supports functions **without direct or indirect
calls**, including structured blocks, loops, conditionals, branch tables,
returns, original ordinary traps and fused backedges. It also covers no-call
private clones. It expands named meter macros, retains every original operation,
and derives successors from structured labels. Every call-containing body is
explicitly listed as excluded from this CFG check, rather than assigning calls
an assumed equivalence. All bodies can still receive byte-encoding checks.

The CFG adapter first validates both modules with WABT, then checks the actual
bytes with the receipt checker and Lean reference encoder. It emits `bytes.json`,
`graphs.json`, `ActualModuleCFG.lean` and `report.json`. The graph executable checks
every selected graph. With `--kernel`, each graph under the explicit
operation-count limit also gets a concrete `by decide` proof;
larger graphs are explicitly reported as executable-only. `GraphAudit.lean`
connects accepted graph records to both the simulation and progress checkers.

## Reproduce

With the pinned Lean toolchain available, from this directory:

```sh
lake build
lake build audit graphAudit
python3 ../run.py --build-dir /tmp/wasm-proof-fixtures
python3 run_certificates.py --fixture-dir /tmp/wasm-proof-fixtures --build-dir /tmp/wasm-proof-receipts
python3 check_cfg.py /tmp/wasm-proof-fixtures/fixtures.wasm /tmp/wasm-proof-receipts/fixtures.injected.wasm /tmp/wasm-proof-receipts/fixtures.receipt.jsonl /tmp/wasm-proof-graphs --kernel
```

The C verifier requires the already-built stock FastJIT archive. The receipt
runner additionally needs WABT and a C compiler. If Lean is installed outside
`PATH`, the receipt runner accepts `--lean-root /path/to/toolchain`, and the CFG
adapter accepts `--lake /path/to/toolchain/bin/lake`.

## Toward an independent load-time verifier

C could emit the original bytes, instrumented bytes and a transformation
certificate. A verifier must treat that certificate, including any supplied Lean
models, as an untrusted proposal. To remove the current Python/model trust gap,
Lean would need to decode the actual bytes itself or check decoding certificates,
then verify permitted transformations under a proved semantic preservation
theorem. A compiled checker could perform this validation at load time; generated
Lean definitions and kernel-reduced proof terms remain an offline alternative.

The current implementation runs offline. It neither adds Lean to the Redis load
path nor establishes the full byte-to-Wasm-model connection: Python decoding and
the interpretation of emitted macros remain trusted boundaries.

## Trusted boundary and limits

The remaining boundary includes the independent Python Wasm decoder and graph
adapter; the interpretation of byte-level Wasm meter macros as the abstract
private actions; Wasm instruction, call-stack and structured-control semantics;
and WAMR's execution of those instructions. The reference encoder is executable
Lean, but its equivalence to the intended Wasm semantics is not proved here.
The project also trusts Lean's kernel and, for executable-only checks, the Lean
compiler/runtime and host tools.

The guest model excludes observations of the added private exports, debug/call
frame identities and timing. It assumes matching resource outcomes. Extra locals
and larger frames can make a module fail to load or overflow the stack earlier;
the concrete counterexamples in `../CORRECTNESS.md` remain valid. No proof here
claims unconditional equivalence at those resource boundaries, verifies the C
parser for all binaries, or establishes a strict per-instruction fuel limit.
