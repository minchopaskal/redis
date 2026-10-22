import Metering

/-!
# Checked control-flow simulation

Unlike a fixed execution-path erasure, this model chooses successors during
execution. Guest semantics returns the successor position; private guards
choose an edge using fuel. The executable checker verifies each target node's
operation and projected successor positions. The simulation theorem quantifies
over every finite target execution accepted by those operational rules.

The model does not decode Wasm or construct its control-flow graph. A bridge
must justify its graph, operation semantics, and clone/call-state projection.
Native resource failures remain outside this abstract model.
-/

namespace WasmMeterProof.CFG

inductive SourceNode where
  | guest (op : GuestOp) (successors : List Nat)
  | halt
  deriving DecidableEq, Repr

inductive TargetNode where
  | guest (op : GuestOp) (successors : List Nat)
  | meter (action : MeterOp) (next : Nat)
  | guard (minimum fast slow : Nat)
  | halt
  deriving DecidableEq, Repr

def projectSuccessors (origins : List Nat) : List Nat → Option (List Nat)
  | [] => some []
  | next :: rest =>
      match origins[next]?, projectSuccessors origins rest with
      | some original, some tail => some (original :: tail)
      | _, _ => none

/- All entries, including private-only nodes, must project to a valid original
node. A target guest's successor positions retain their source meaning. -/
def checkNode (source : List SourceNode) (origins : List Nat)
    (pc : Nat) (node : TargetNode) : Bool :=
  match origins[pc]? with
  | none => false
  | some original =>
      match node with
      | .guest op successors =>
          match projectSuccessors origins successors with
          | none => false
          | some mapped => decide (source[original]? = some (.guest op mapped))
      | .meter _ next =>
          decide (original < source.length ∧ origins[next]? = some original)
      | .guard _ fast slow =>
          decide (original < source.length ∧
            origins[fast]? = some original ∧ origins[slow]? = some original)
      | .halt => decide (source[original]? = some .halt)

def checkGraph (source : List SourceNode) (target : List TargetNode)
    (origins : List Nat) : Bool :=
  decide (origins.length = target.length) &&
  (List.range target.length).all fun pc =>
    match target[pc]? with
    | none => false
    | some node => checkNode source origins pc node

theorem checked_node {source target origins pc node}
    (certificate : checkGraph source target origins = true)
    (fetch : target[pc]? = some node) : checkNode source origins pc node = true := by
  have bound : pc < target.length := by
    by_cases h : pc < target.length
    · exact h
    · have missing : target[pc]? = none := List.getElem?_eq_none (by omega)
      simp [missing] at fetch
  simp only [checkGraph, Bool.and_eq_true] at certificate
  have checked := List.all_eq_true.mp certificate.2 pc (List.mem_range.mpr bound)
  simpa [fetch] using checked

theorem projectSuccessors_get {origins successors mapped : List Nat} {index next : Nat}
    (projection : projectSuccessors origins successors = some mapped)
    (fetch : successors[index]? = some next) :
    ∃ original, origins[next]? = some original ∧ mapped[index]? = some original := by
  induction successors generalizing mapped index with
  | nil => simp at fetch
  | cons head rest ih =>
      cases headOrigin : origins[head]? with
      | none => simp [projectSuccessors, headOrigin] at projection
      | some original =>
          cases restOrigin : projectSuccessors origins rest with
          | none => simp [projectSuccessors, headOrigin, restOrigin] at projection
          | some mappedRest =>
              simp [projectSuccessors, headOrigin, restOrigin] at projection
              subst mapped
              cases index with
              | zero =>
                  simp at fetch
                  subst next
                  exact ⟨original, headOrigin, rfl⟩
              | succ index =>
                  simp only [List.getElem?_cons_succ] at fetch ⊢
                  exact ih restOrigin fetch

/-! ## Private-step progress

The simulation checker alone would allow an infinite private-only cycle.
This second executable checker rules it out using a natural-number rank which
strictly decreases on every possible private edge. Guest edges may increase
rank: original loops and recursion must remain possible.
-/

def privateSuccessors : TargetNode → List Nat
  | .guest _ _ => []
  | .meter _ next => [next]
  | .guard _ fast slow => [fast, slow]
  | .halt => []

def checkProgress (target : List TargetNode) (ranks : List Nat) : Bool :=
  decide (ranks.length = target.length) &&
  (List.range target.length).all fun pc =>
    match target[pc]? with
    | none => false
    | some node => (privateSuccessors node).all fun next =>
        match ranks[pc]?, ranks[next]? with
        | some currentRank, some nextRank => decide (nextRank < currentRank)
        | _, _ => false

def PrivateEdge (target : List TargetNode) (pc next : Nat) : Prop :=
  ∃ node, target[pc]? = some node ∧ next ∈ privateSuccessors node

theorem checked_private_decrease {target ranks pc next}
    (certificate : checkProgress target ranks = true)
    (edge : PrivateEdge target pc next) :
    ∃ currentRank nextRank, ranks[pc]? = some currentRank ∧
      ranks[next]? = some nextRank ∧ nextRank < currentRank := by
  obtain ⟨node, fetch, member⟩ := edge
  have bound : pc < target.length := by
    by_cases h : pc < target.length
    · exact h
    · have missing : target[pc]? = none := List.getElem?_eq_none (by omega)
      simp [missing] at fetch
  simp only [checkProgress, Bool.and_eq_true] at certificate
  have nodeChecked := List.all_eq_true.mp certificate.2 pc (List.mem_range.mpr bound)
  simp only [fetch] at nodeChecked
  have edgeChecked := List.all_eq_true.mp nodeChecked next member
  cases fromRank : ranks[pc]? with
  | none => simp [fromRank] at edgeChecked
  | some currentRank =>
      cases toRank : ranks[next]? with
      | none => simp [fromRank, toRank] at edgeChecked
      | some nextRank =>
          simp only [fromRank, toRank] at edgeChecked
          exact ⟨currentRank, nextRank, rfl, rfl, of_decide_eq_true edgeChecked⟩

inductive PrivateChain (target : List TargetNode) : Nat → Nat → Nat → Prop where
  | nil (pc : Nat) : PrivateChain target pc 0 pc
  | cons {pc next count final}
      (edge : PrivateEdge target pc next)
      (tail : PrivateChain target next count final) :
      PrivateChain target pc (count + 1) final

theorem private_chain_bounded {target ranks pc count final initialRank}
    (certificate : checkProgress target ranks = true)
    (chain : PrivateChain target pc count final)
    (initial : ranks[pc]? = some initialRank) :
    ∃ finalRank, ranks[final]? = some finalRank ∧ count + finalRank ≤ initialRank := by
  induction chain generalizing initialRank with
  | nil => exact ⟨initialRank, initial, by omega⟩
  | cons edge tail ih =>
      obtain ⟨currentRank, nextRank, current, next, decrease⟩ :=
        checked_private_decrease certificate edge
      have equal : currentRank = initialRank := by simpa [initial] using current.symm
      obtain ⟨finalRank, finalLookup, bound⟩ := ih next
      exact ⟨finalRank, finalLookup, by omega⟩

theorem no_private_cycle {target ranks pc count initialRank}
    (certificate : checkProgress target ranks = true)
    (initial : ranks[pc]? = some initialRank)
    (nonempty : 0 < count) : ¬ PrivateChain target pc count pc := by
  intro chain
  obtain ⟨finalRank, finalLookup, bound⟩ := private_chain_bounded certificate chain initial
  have equal : finalRank = initialRank := by simpa [initial] using finalLookup.symm
  omega

section Execution

variable {State Event : Type}

/- The final Nat is the selected successor position. It permits arbitrary
conditional control, including a br_table-like choice, without predicting the
choice in the certificate. Calls may be modeled atomically only when their
callee semantics and state projection are independently justified. -/
abbrev Semantics (State Event : Type) :=
  GuestOp → State → List Event → State → Nat → Prop

inductive SourceRun (source : List SourceNode) (step : Semantics State Event) :
    Nat → State → List Event → Nat → State → Prop where
  | nil (pc : Nat) (s : State) : SourceRun source step pc s [] pc s
  | guest {pc op successors s observations middle choice next trace finalPC final}
      (fetch : source[pc]? = some (.guest op successors))
      (action : step op s observations middle choice)
      (edge : successors[choice]? = some next)
      (tail : SourceRun source step next middle trace finalPC final) :
      SourceRun source step pc s (observations ++ trace) finalPC final

inductive TargetRun (target : List TargetNode) (step : Semantics State Event) :
    Nat → MeterState → State → List Event → Nat → MeterState → State → Prop where
  | nil (pc : Nat) (m : MeterState) (s : State) :
      TargetRun target step pc m s [] pc m s
  | guest {pc op successors m nextMeter s observations middle choice next trace finalPC finalMeter final}
      (fetch : target[pc]? = some (.guest op successors))
      (action : step op s observations middle choice)
      (edge : successors[choice]? = some next)
      (tail : TargetRun target step next nextMeter middle trace finalPC finalMeter final) :
      TargetRun target step pc m s (observations ++ trace) finalPC finalMeter final
  | meter {pc action next m nextMeter s trace finalPC finalMeter final}
      (fetch : target[pc]? = some (.meter action next))
      (actionStep : runMeter action m = some nextMeter)
      (tail : TargetRun target step next nextMeter s trace finalPC finalMeter final) :
      TargetRun target step pc m s trace finalPC finalMeter final
  | guard {pc minimum fast slow m s trace finalPC finalMeter final}
      (fetch : target[pc]? = some (.guard minimum fast slow))
      (tail : TargetRun target step
        (if Int.ofNat minimum ≤ m.cachedFuel then fast else slow)
        m s trace finalPC finalMeter final) :
      TargetRun target step pc m s trace finalPC finalMeter final

/- Private state after a guest operation is deliberately unconstrained: nested
metered calls or host integrations may update shared fuel. This theorem proves
guest behavior preservation, not authority over fuel or a total fuel bound. -/
theorem checked_execution {source target origins} {step : Semantics State Event}
    (certificate : checkGraph source target origins = true)
    {pc m s trace finalPC finalMeter final}
    (execution : TargetRun target step pc m s trace finalPC finalMeter final)
    {originalPC} (entry : origins[pc]? = some originalPC) :
    ∃ originalFinal, origins[finalPC]? = some originalFinal ∧
      SourceRun source step originalPC s trace originalFinal final := by
  induction execution generalizing originalPC with
  | nil => exact ⟨originalPC, entry, SourceRun.nil _ _⟩
  | @guest pc op successors m nextMeter s observations middle choice next trace finalPC finalMeter final
      fetch action edge tail ih =>
      have checked := checked_node certificate fetch
      simp only [checkNode, entry] at checked
      cases projected : projectSuccessors origins successors with
      | none => simp [projected] at checked
      | some mapped =>
          simp only [projected] at checked
          have sourceFetch : source[originalPC]? = some (.guest op mapped) := of_decide_eq_true checked
          obtain ⟨originalNext, nextOrigin, sourceEdge⟩ := projectSuccessors_get projected edge
          obtain ⟨originalFinal, finalOrigin, run⟩ := ih nextOrigin
          exact ⟨originalFinal, finalOrigin, SourceRun.guest sourceFetch action sourceEdge run⟩
  | @meter pc action next m nextMeter s trace finalPC finalMeter final fetch actionStep tail ih =>
      have checked := checked_node certificate fetch
      simp only [checkNode, entry] at checked
      have aligned : originalPC < source.length ∧ origins[next]? = some originalPC :=
        of_decide_eq_true checked
      exact ih aligned.2
  | @guard pc minimum fast slow m s trace finalPC finalMeter final fetch tail ih =>
      have checked := checked_node certificate fetch
      simp only [checkNode, entry] at checked
      have aligned : originalPC < source.length ∧
          origins[fast]? = some originalPC ∧ origins[slow]? = some originalPC :=
        of_decide_eq_true checked
      apply ih
      split <;> simp_all

theorem checked_halt {source target origins} {step : Semantics State Event}
    (certificate : checkGraph source target origins = true)
    {pc m s trace finalPC finalMeter final}
    (execution : TargetRun target step pc m s trace finalPC finalMeter final)
    {originalPC} (entry : origins[pc]? = some originalPC)
    (halted : target[finalPC]? = some .halt) :
    ∃ originalFinal, SourceRun source step originalPC s trace originalFinal final ∧
      source[originalFinal]? = some .halt := by
  obtain ⟨originalFinal, finalOrigin, run⟩ := checked_execution certificate execution entry
  have checked := checked_node certificate halted
  simp only [checkNode, finalOrigin] at checked
  exact ⟨originalFinal, run, of_decide_eq_true checked⟩

/- A failed checkpoint adds no guest effect. The run before it already maps to
an original execution prefix with exactly the same state and observations. -/
theorem checked_exhaustion_prefix {source target origins} {step : Semantics State Event}
    (certificate : checkGraph source target origins = true)
    {pc m s trace finalPC finalMeter final action next}
    (execution : TargetRun target step pc m s trace finalPC finalMeter final)
    {originalPC} (entry : origins[pc]? = some originalPC)
    (_checkpoint : target[finalPC]? = some (.meter action next))
    (_exhausted : runMeter action finalMeter = none) :
    ∃ originalFinal, origins[finalPC]? = some originalFinal ∧
      SourceRun source step originalPC s trace originalFinal final := by
  exact checked_execution certificate execution entry

end Execution
end WasmMeterProof.CFG
