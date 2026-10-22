import Std

/-!
# A small semantic core for the Wasm meter emission plan

This file does not define the WebAssembly binary format or prove the C injector.
`GuestOp` is a normalized original operation supplied by the audit bridge.
`GuestExec` describes an already-selected finite guest execution path; it is
not an interpreter which executes every instruction of a function in file order.
Control-flow/path selection and the correspondence between emitted Wasm bytes
and these typed steps are explicit obligations of that bridge.

The results below are quantified over an arbitrary guest transition relation.
Private metering steps use a separate state, and a passing step is therefore
silent with respect to guest state and observable effects. Both successful
execution and exhaustion have kernel-checked erasure results. No assumption
that private fuel is nonnegative on return is needed.
-/

namespace WasmMeterProof

abbrev GuestOp := List Nat

inductive MeterOp where
  | debit (cost : Nat)
  | checkNonnegative
  | checkPositive
  | checkPrepaid (credit : Nat)
  | publish
  | reload
  deriving DecidableEq, Repr

structure MeterState where
  cachedFuel : Int
  publishedFuel : Int
  deriving DecidableEq, Repr

/- Mathematical integers model fuel here. Connecting this model to Wasm i64
requires the separately documented no-wrap bound; it is not assumed as an axiom. -/
def runMeter : MeterOp → MeterState → Option MeterState
  | .debit cost, m => some { m with cachedFuel := m.cachedFuel - Int.ofNat cost }
  | .checkNonnegative, m => if 0 ≤ m.cachedFuel then some m else none
  | .checkPositive, m => if 0 < m.cachedFuel then some m else none
  | .checkPrepaid credit, m => if -Int.ofNat credit ≤ m.cachedFuel then some m else none
  | .publish, m => some { m with publishedFuel := m.cachedFuel }
  | .reload, m => some { m with cachedFuel := m.publishedFuel }

inductive PlanStep where
  | guest (op : GuestOp)
  | meter (op : MeterOp)
  deriving DecidableEq, Repr

abbrev Plan := List PlanStep

def erase : Plan → List GuestOp
  | [] => []
  | .guest op :: rest => op :: erase rest
  | .meter _ :: rest => erase rest

def checkErasure (original : List GuestOp) (plan : Plan) : Bool :=
  decide (erase plan = original)

theorem checkErasure_sound {original : List GuestOp} {plan : Plan}
    (h : checkErasure original plan = true) : erase plan = original := by
  exact of_decide_eq_true h

theorem checkErasure_complete {original : List GuestOp} {plan : Plan}
    (h : erase plan = original) : checkErasure original plan = true := by
  simp [checkErasure, h]

section Execution

variable {State Event : Type}

/- A guest operation may emit zero or more observations. Return values and
ordinary trap outcomes can be included in State or Event. Guest-step semantics
must itself enforce terminal states: this generic core invents no trap behavior. -/
abbrev GuestStep (State Event : Type) := GuestOp → State → List Event → State → Prop

inductive GuestExec (step : GuestStep State Event) :
    List GuestOp → State → List Event → State → Prop where
  | nil (s : State) : GuestExec step [] s [] s
  | cons {op rest s observations middle trace final}
      (head : step op s observations middle)
      (tail : GuestExec step rest middle trace final) :
      GuestExec step (op :: rest) s (observations ++ trace) final

inductive PlanExec (step : GuestStep State Event) :
    Plan → MeterState → State → List Event → MeterState → State → Prop where
  | nil (m : MeterState) (s : State) : PlanExec step [] m s [] m s
  | guest {op rest m s observations middle trace finalMeter final}
      (head : step op s observations middle)
      (tail : PlanExec step rest m middle trace finalMeter final) :
      PlanExec step (.guest op :: rest) m s (observations ++ trace) finalMeter final
  | meter {op rest m next s trace finalMeter final}
      (head : runMeter op m = some next)
      (tail : PlanExec step rest next s trace finalMeter final) :
      PlanExec step (.meter op :: rest) m s trace finalMeter final

theorem planExec_erases {step : GuestStep State Event}
    {plan m s trace finalMeter final}
    (execution : PlanExec step plan m s trace finalMeter final) :
    GuestExec step (erase plan) s trace final := by
  induction execution with
  | nil => exact GuestExec.nil _
  | guest head _ ih => exact GuestExec.cons head ih
  | meter _ _ ih => exact ih

/- This is the interface used by certificates produced from the C emission
plan: the checked original sequence replaces the abstract erased sequence. -/
theorem certified_execution {step : GuestStep State Event}
    {original plan m s trace finalMeter final}
    (certificate : checkErasure original plan = true)
    (execution : PlanExec step plan m s trace finalMeter final) :
    GuestExec step original s trace final := by
  rw [← checkErasure_sound certificate]
  exact planExec_erases execution

/- Unlike PlanExec, this relation ends at a failed meter checkpoint. It stops
before running the remainder of the path. Ordinary guest traps belong to the
guest relation, not this constructor. -/
inductive PlanExhausts (step : GuestStep State Event) :
    Plan → MeterState → State → List Event → State → Prop where
  | stop {op rest m s} (failure : runMeter op m = none) :
      PlanExhausts step (.meter op :: rest) m s [] s
  | guest {op rest m s observations middle trace final}
      (head : step op s observations middle)
      (tail : PlanExhausts step rest m middle trace final) :
      PlanExhausts step (.guest op :: rest) m s (observations ++ trace) final
  | meter {op rest m next s trace final}
      (head : runMeter op m = some next)
      (tail : PlanExhausts step rest next s trace final) :
      PlanExhausts step (.meter op :: rest) m s trace final

theorem exhaustion_prefix {step : GuestStep State Event}
    {plan m s trace final}
    (execution : PlanExhausts step plan m s trace final) :
    ∃ consumed suffix, erase plan = consumed ++ suffix ∧
      GuestExec step consumed s trace final := by
  induction execution with
  | stop => exact ⟨[], _, rfl, GuestExec.nil _⟩
  | @guest op rest m s observations middle trace final head tail ih =>
      obtain ⟨consumed, suffix, erased, guest⟩ := ih
      refine ⟨op :: consumed, suffix, ?_, GuestExec.cons head guest⟩
      simp only [erase, List.cons_append, erased]
  | meter _ _ ih => exact ih

theorem certified_exhaustion_prefix {step : GuestStep State Event}
    {original plan m s trace final}
    (certificate : checkErasure original plan = true)
    (execution : PlanExhausts step plan m s trace final) :
    ∃ consumed suffix, original = consumed ++ suffix ∧
      GuestExec step consumed s trace final := by
  rw [← checkErasure_sound certificate]
  exact exhaustion_prefix execution

end Execution

/-! ## Concrete control-flow and call-side lemmas -/

/- D counts original labels, including the function label. The inserted
exhaustion block is immediately inside that last label. -/
def mapDepth (D d : Nat) : Nat := if d = D - 1 then d + 1 else d

theorem mapDepth_inner {D d : Nat} (inner : d < D - 1) : mapDepth D d = d := by
  simp [mapDepth, Nat.ne_of_lt inner]

theorem mapDepth_function {D : Nat} (positive : 0 < D) :
    mapDepth D (D - 1) = D := by
  simp [mapDepth]
  omega

theorem mapDepth_valid {D d : Nat} (valid : d < D) : mapDepth D d < D + 1 := by
  unfold mapDepth
  split <;> omega

theorem mapDepth_injective {D a b : Nat} (ha : a < D) (hb : b < D)
    (same : mapDepth D a = mapDepth D b) : a = b := by
  unfold mapDepth at same
  split at same <;> split at same <;> omega

/- This is the actual label lookup property, not only arithmetic on indices.
The original innermost labels retain their order; the cold label is inserted
immediately before the outer function label. -/
theorem mapDepth_lookup {Label : Type} (inner : List Label)
    (cold functionLabel : Label) {d : Nat} (valid : d < inner.length + 1) :
    (inner ++ [cold, functionLabel])[mapDepth (inner.length + 1) d]? =
      (inner ++ [functionLabel])[d]? := by
  by_cases inside : d < inner.length
  · have remap : mapDepth (inner.length + 1) d = d :=
      mapDepth_inner (by omega)
    rw [remap, List.getElem?_append_left inside, List.getElem?_append_left inside]
  · have outermost : d = inner.length := by omega
    subst d
    simp [mapDepth]

/- A typed if owns exactly the call arguments; unrelated operands below them
survive either arm. This is a list-framing lemma, not a proof of Wasm typing. -/
def frameCall {Value : Type} (below arguments : List Value)
    (callee : List Value → List Value) : List Value := below ++ callee arguments

theorem typed_dispatch_frame {Value : Type} (below arguments : List Value)
    (fast fallback : List Value → List Value) (same : fast arguments = fallback arguments)
    (funded : Bool) :
    frameCall below arguments (if funded then fast else fallback) =
      frameCall below arguments fallback := by
  cases funded <;> simp [frameCall, same]

/- A clone table is an ordered list of original function indices. Clones are
appended after all originals. Their ordinal recovers their original identity. -/
def cloneIndex (originalCount ordinal : Nat) : Nat := originalCount + ordinal

def cloneOrigin (originalCount : Nat) (origins : List Nat) (index : Nat) : Option Nat :=
  if index < originalCount then some index else origins[index - originalCount]?

theorem cloneOrigin_original {originalCount index : Nat} (origins : List Nat)
    (valid : index < originalCount) : cloneOrigin originalCount origins index = some index := by
  simp [cloneOrigin, valid]

theorem cloneOrigin_appended (originalCount ordinal : Nat) (origins : List Nat) :
    cloneOrigin originalCount origins (cloneIndex originalCount ordinal) = origins[ordinal]? := by
  have notOriginal : ¬ originalCount + ordinal < originalCount := by omega
  simp [cloneOrigin, cloneIndex, notOriginal]

/- Proper prefixes of a fully funded positive-cost tree leave positive fuel.
The analysis/bridge must establish that each nested entry is such a prefix. -/
theorem fundedTree_prefix_positive {remaining total spent : Int}
    (funded : total ≤ remaining) (properPrefix : spent < total) :
    0 < remaining - spent := by
  omega

theorem fundedTree_debit_nonnegative {remaining total : Int}
    (funded : total ≤ remaining) : 0 ≤ remaining - total := by
  omega

/- U is the balance after the current header, before body cost B. H prepays
the next header. This exactly matches the adjusted fused-backedge predicate. -/
theorem prepaid_backedge {U B H : Int} :
    U - B - H ≥ -H ↔ U - B ≥ 0 := by
  omega

/- Semantic silence permits a finite negative tail; it does not prove that
any debit is followed immediately by an exhaustion check. -/
theorem debit_may_be_negative :
    runMeter (.debit 3) ⟨1, 1⟩ = some ⟨-2, 1⟩ := by
  decide

end WasmMeterProof
