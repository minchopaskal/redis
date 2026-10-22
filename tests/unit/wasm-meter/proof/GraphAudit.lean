import Lean
import ControlFlow

/- A JSON-friendly wrapper for actual module-derived CFG certificates. This
file only decodes the certificate schema, not WebAssembly. check_cfg.py builds
these graphs after independently validating the byte-emission receipt. -/
namespace WasmMeterProof.GraphAudit
open Lean CFG

structure NodeRecord where
  kind : Nat
  op : List Nat
  successors : List Nat
  action : Nat
  amount : Nat
  deriving FromJson, ToJson, Repr

structure GraphRecord where
  source : List NodeRecord
  target : List NodeRecord
  origins : List Nat
  ranks : List Nat
  deriving FromJson, ToJson, Repr

structure Certificate where
  graphs : List GraphRecord
  deriving FromJson, ToJson, Repr

def decodeAction (kind amount : Nat) : Option MeterOp :=
  match kind with
  | 0 => some (.debit amount)
  | 1 => some .checkNonnegative
  | 2 => some .checkPositive
  | 3 => some .publish
  | 4 => some .reload
  | 5 => some (.checkPrepaid amount)
  | _ => none

def decodeSource (node : NodeRecord) : Option SourceNode :=
  match node.kind with
  | 0 => some (.guest node.op node.successors)
  | 3 => some .halt
  | _ => none

def decodeTarget (node : NodeRecord) : Option TargetNode :=
  match node.kind, node.successors with
  | 0, _ => some (.guest node.op node.successors)
  | 1, [next] => do
      let action ← decodeAction node.action node.amount
      pure (.meter action next)
  | 2, [fast, slow] => some (.guard node.amount fast slow)
  | 3, _ => some .halt
  | _, _ => none

def checkRecord (graph : GraphRecord) : Bool :=
  match graph.source.mapM decodeSource, graph.target.mapM decodeTarget with
  | some source, some target =>
      checkGraph source target graph.origins && checkProgress target graph.ranks
  | _, _ => false

theorem checked_record {graph : GraphRecord} {source target}
    (sourceDecoded : graph.source.mapM decodeSource = some source)
    (targetDecoded : graph.target.mapM decodeTarget = some target)
    (accepted : checkRecord graph = true) :
    checkGraph source target graph.origins = true ∧
      checkProgress target graph.ranks = true := by
  simpa [checkRecord, sourceDecoded, targetDecoded, Bool.and_eq_true] using accepted

end WasmMeterProof.GraphAudit
