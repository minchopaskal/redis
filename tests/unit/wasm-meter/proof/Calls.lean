import ControlFlow

/-!
Interprocedural preservation for normalized module graphs, including private
clones and recursive calls. A call executes its callee and then its continuation;
callee equivalence is proved by induction on that execution, never assumed.

The trusted lowering boundary is explicit: `Correspondence` must be established
from the actual Wasm bodies, including operand stacks, labels, argument/result
movement, call resolution, and clone origins. This file does not prove that
lowering or connect it to the current no-call JSON graph checker. `State` contains
guest memory, globals, locals and result values; `Frame` saves caller state.
`enter`/`leave` give the same signature-indexed frame semantics on both sides.
Imports may be represented by guest operations with their actual host semantics.
Runs here finish at a normal return: traps are not silently converted to returns.
The theorem concerns guest behavior, not fuel bounds or native resource failures.
-/

namespace WasmMeterProof.Calls

inductive Node where
  | guest (op : GuestOp) (successors : List Nat)
  | call (callee next : Nat)
  | ret
  | meter (action : MeterOp) (next : Nat)
  | guard (minimum fast slow : Nat)

structure Function (Sig : Type) where
  signature : Sig
  entry : Nat
  body : Nat → Option Node

abbrev Module (Sig : Type) := Nat → Function Sig

/-- Undefined functions can have empty bodies. The origin map is many-to-one:
several private clones may correspond to a single original function. -/
structure Correspondence {Sig : Type} (source target : Module Sig) where
  origin : Nat → Nat
  point : Nat → Nat → Nat
  signature : ∀ f, (target f).signature = (source (origin f)).signature
  entry : ∀ f, point f (target f).entry = (source (origin f)).entry
  guest : ∀ f pc op successors,
    (target f).body pc = some (.guest op successors) →
    (source (origin f)).body (point f pc) =
      some (.guest op (successors.map (point f)))
  call : ∀ f pc callee next,
    (target f).body pc = some (.call callee next) →
    (source (origin f)).body (point f pc) =
      some (.call (origin callee) (point f next))
  ret : ∀ f pc, (target f).body pc = some .ret →
    (source (origin f)).body (point f pc) = some .ret
  meter : ∀ f pc action next,
    (target f).body pc = some (.meter action next) → point f next = point f pc
  guard : ∀ f pc minimum fast slow,
    (target f).body pc = some (.guard minimum fast slow) →
      point f fast = point f pc ∧ point f slow = point f pc

section Execution

variable {Sig State Event Frame : Type}
variable (step : CFG.Semantics State Event)
variable (enter : Sig → State → Frame → State → Prop)
variable (leave : Sig → Frame → State → State → Prop)

inductive SourceRun (program : Module Sig) :
    Nat → Nat → State → List Event → State → Prop where
  | ret {f pc s} (fetch : (program f).body pc = some .ret) :
      SourceRun program f pc s [] s
  | guest {f pc op successors s observations middle choice next trace final}
      (fetch : (program f).body pc = some (.guest op successors))
      (action : step op s observations middle choice)
      (edge : successors[choice]? = some next)
      (tail : SourceRun program f next middle trace final) :
      SourceRun program f pc s (observations ++ trace) final
  | call {f pc callee next s frame start returned resumed events trace final}
      (fetch : (program f).body pc = some (.call callee next))
      (entry : enter (program callee).signature s frame start)
      (body : SourceRun program callee (program callee).entry start events returned)
      (exit : leave (program callee).signature frame returned resumed)
      (tail : SourceRun program f next resumed trace final) :
      SourceRun program f pc s (events ++ trace) final

/-- Fuel-local initialization and restoration across frames are deliberately
abstract: they affect only private state. This permits both original metered
functions and clones without a private local, without assuming a fuel policy. -/
inductive TargetRun (program : Module Sig) :
    Nat → Nat → MeterState → State → List Event → MeterState → State → Prop where
  | ret {f pc m s} (fetch : (program f).body pc = some .ret) :
      TargetRun program f pc m s [] m s
  | guest {f pc op successors m s observations middle choice next trace finalMeter final}
      (fetch : (program f).body pc = some (.guest op successors))
      (action : step op s observations middle choice)
      (edge : successors[choice]? = some next)
      (tail : TargetRun program f next m middle trace finalMeter final) :
      TargetRun program f pc m s (observations ++ trace) finalMeter final
  | call {f pc callee next m s frame start startMeter returned returnedMeter
          resumed resumedMeter events trace finalMeter final}
      (fetch : (program f).body pc = some (.call callee next))
      (entry : enter (program callee).signature s frame start)
      (body : TargetRun program callee (program callee).entry startMeter start
        events returnedMeter returned)
      (exit : leave (program callee).signature frame returned resumed)
      (tail : TargetRun program f next resumedMeter resumed trace finalMeter final) :
      TargetRun program f pc m s (events ++ trace) finalMeter final
  | meter {f pc action next m nextMeter s trace finalMeter final}
      (fetch : (program f).body pc = some (.meter action next))
      (actionStep : runMeter action m = some nextMeter)
      (tail : TargetRun program f next nextMeter s trace finalMeter final) :
      TargetRun program f pc m s trace finalMeter final
  | guard {f pc minimum fast slow m s trace finalMeter final}
      (fetch : (program f).body pc = some (.guard minimum fast slow))
      (tail : TargetRun program f
        (if Int.ofNat minimum ≤ m.cachedFuel then fast else slow)
        m s trace finalMeter final) :
      TargetRun program f pc m s trace finalMeter final

variable {step enter leave}

/-- The induction is on a finite run, not on the call graph. In the call case,
both the callee execution and caller continuation are induction hypotheses;
recursive and mutually recursive modules therefore need no acyclicity premise. -/
theorem module_preserves_execution {source target : Module Sig}
    (aligned : Correspondence source target)
    {f pc m s trace finalMeter final}
    (execution : TargetRun step enter leave target f pc m s trace finalMeter final) :
    SourceRun step enter leave source (aligned.origin f) (aligned.point f pc)
      s trace final := by
  induction execution with
  | ret fetch => exact SourceRun.ret (aligned.ret _ _ fetch)
  | @guest f pc op successors m s observations middle choice next trace
      finalMeter final fetch action edge tail ih =>
      refine SourceRun.guest (aligned.guest f pc op successors fetch) action
        (next := aligned.point f next) ?_ ih
      simp only [List.getElem?_map, edge, Option.map_some]
  | @call f pc callee next m s frame start startMeter returned returnedMeter
      resumed resumedMeter events trace finalMeter final fetch entry body exit tail
      calleeIH tailIH =>
      apply SourceRun.call (aligned.call f pc callee next fetch)
      · simpa only [aligned.signature callee] using entry
      · simpa only [aligned.entry callee] using calleeIH
      · simpa only [aligned.signature callee] using exit
      · exact tailIH
  | @meter f pc action next m nextMeter s trace finalMeter final fetch actionStep tail ih =>
      simpa only [aligned.meter f pc action next fetch] using ih
  | @guard f pc minimum fast slow m s trace finalMeter final fetch tail ih =>
      obtain ⟨fastOrigin, slowOrigin⟩ := aligned.guard f pc minimum fast slow fetch
      split at ih <;> simpa only [fastOrigin, slowOrigin] using ih

/-- A target entry, whether original or private clone, returns the same guest
state and observations as its original function when it completes normally. -/
theorem entry_preserves_execution {source target : Module Sig}
    (aligned : Correspondence source target)
    {f m s trace finalMeter final}
    (execution : TargetRun step enter leave target f (target f).entry
      m s trace finalMeter final) :
    SourceRun step enter leave source (aligned.origin f) (source (aligned.origin f)).entry
      s trace final := by
  simpa only [aligned.entry f] using module_preserves_execution aligned execution

end Execution

/- A genuine self-recursive module: its guest step chooses the base case or
recursive call from the current state. This witnesses arbitrary finite call
depth in the semantics, rather than merely allowing an unused cycle in a graph. -/
private def recursiveModule : Module Unit := fun _ =>
  { signature := (), entry := 0, body := fun pc => match pc with
      | 0 => some (.guest [] [1, 2])
      | 1 => some (.call 0 2)
      | 2 => some .ret
      | _ => none }

private def recursiveStep : CFG.Semantics Nat Unit := fun _ n events result choice =>
  events = [] ∧ result = n - 1 ∧ choice = (if n = 0 then 1 else 0)

private theorem recursive_runs (m : MeterState) (n : Nat) :
    TargetRun recursiveStep (fun _ s (_ : Unit) start => start = s)
      (fun _ (_ : Unit) returned resumed => resumed = returned)
      recursiveModule 0 0 m n [] m 0 := by
  induction n with
  | zero =>
      refine TargetRun.guest (choice := 1) (middle := 0) (next := 2)
        (observations := []) (trace := []) rfl ?_ rfl (TargetRun.ret rfl)
      simp [recursiveStep]
  | succ n ih =>
      refine TargetRun.guest (choice := 0) (middle := n)
        (observations := []) (trace := []) rfl ?_ rfl ?_
      · simp [recursiveStep]
      · exact TargetRun.call (frame := ()) (startMeter := m)
          (returnedMeter := m) (resumedMeter := m) rfl rfl ih rfl (TargetRun.ret rfl)

end WasmMeterProof.Calls
