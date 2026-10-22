import Lean
import Metering

/-!
An independent executable encoder for the production C emission-plan vocabulary.
The input is a receipt from the real emitter, bound to the actual original and
output module by check_receipt.py. This file checks every function-body byte and
the ordered coverage of original bytes. It does not parse complete Wasm modules
or claim that byte erasure alone proves control-flow semantics.
-/
namespace WasmMeterProof.Audit
open Lean

structure Op where
  kind : Nat
  sourceOffset : Nat
  sourceLength : Nat
  args : List Int
  outputOffset : Nat
  output : List Nat
  deriving FromJson, ToJson, Repr

structure Body where
  input : List Nat
  output : List Nat
  fuelGlobal : Nat
  fuelLocal : Nat
  ops : List Op
  deriving FromJson, ToJson, Repr

structure Certificate where
  bodies : List Body
  deriving FromJson, ToJson, Repr

/- Bounded encoders handle the C API's u32/u64 and signed i64 constants without
partial functions, native_decide, or an external arithmetic package. -/
def ulebAux : Nat → Nat → Bool → List Nat
  | 0, _, _ => []
  | n + 1, value, signedPositive =>
    let byte := value % 128
    let rest := value / 128
    if rest != 0 || (signedPositive && byte >= 64) then
      (byte + 128) :: ulebAux n rest signedPositive
    else [byte]

def uleb (value : Nat) := ulebAux 10 value false
def positiveLeb (value : Nat) := ulebAux 10 value true

def signedLebAux : Nat → Int → List Nat
  | 0, _ => []
  | n + 1, value =>
    let byte := (value % 128).toNat
    let rest := value / 128
    if (rest == 0 && byte < 64) || (rest == -1 && byte >= 64) then [byte]
    else (byte + 128) :: signedLebAux n rest

def signedLeb (value : Int) := signedLebAux 10 value

def readUlebAux : Nat → Nat → Nat → List Nat → Option (Nat × List Nat)
  | 0, _, _, _ => none
  | _ + 1, _, _, [] => none
  | n + 1, scale, accumulated, byte :: rest =>
    if byte > 255 then none else
    let value := accumulated + (byte % 128) * scale
    if byte < 128 then some (value, rest)
    else readUlebAux n (scale * 128) value rest

def readUleb (bytes : List Nat) := readUlebAux 5 1 0 bytes

def readVector : Nat → List Nat → Option (List Nat × List Nat)
  | 0, bytes => some ([], bytes)
  | n + 1, bytes => do
    let (head, rest) ← readUleb bytes
    let (tail, final) ← readVector n rest
    pure (head :: tail, final)

def readBranch (bytes : List Nat) : Option (Nat × List Nat) := do
  let opcode :: tail := bytes | none
  if opcode == 14 then
    let (count, rest) ← readUleb tail
    if count >= rest.length then none else do
      let (targets, final) ← readVector (count + 1) rest
      if final.isEmpty then pure (opcode, targets) else none
  else if opcode == 12 || opcode == 13 then
    let (target, final) ← readUleb tail
    if final.isEmpty then pure (opcode, [target]) else none
  else none

def readCall (bytes : List Nat) : Option Nat := do
  let 16 :: tail := bytes | none
  let (target, final) ← readUleb tail
  if final.isEmpty then some target else none

def natArg (value : Int) : Option Nat :=
  if value < 0 || value > 4294967295 then none else some value.toNat

def publish (b : Body) : List Nat := [32] ++ uleb b.fuelLocal ++ [36] ++ uleb b.fuelGlobal
def reload (b : Body) : List Nat := [35] ++ uleb b.fuelGlobal ++ [33] ++ uleb b.fuelLocal
def entry (b : Body) (depth : Nat) : List Nat :=
  [32] ++ uleb b.fuelLocal ++ [66, 0, 87, 13] ++ uleb depth

def charge (b : Body) (cost : Nat) (check : Int) (depth prepaid : Nat) : List Nat :=
  [32] ++ uleb b.fuelLocal ++ [66] ++ positiveLeb cost ++
    [125, if check == 0 then 33 else 34] ++ uleb b.fuelLocal ++
    (if check == 0 then [] else
      [66] ++ signedLeb (-Int.ofNat prepaid) ++
      [if check > 0 then 83 else 89, 13] ++ uleb depth)

/- The tags correspond to wasmMeterPlanKind. Every original-byte replacement
checks its source shape, instead of trusting arbitrary supplied output bytes. -/
def encode (b : Body) (op : Op) (source : List Nat) : Option (List Nat) := do
  match op.kind, op.args with
  | 0, [] => pure source
  | 1, [groups, countBytes] =>
    let groups ← natArg groups
    let countBytes ← natArg countBytes
    let (actualGroups, rest) ← readUleb source
    if actualGroups != groups || source.length - rest.length != countBytes then none
    else pure (uleb (groups + 1) ++ rest ++ [1, 126])
  | 2, [] => if source.isEmpty then pure [2, 64] else none
  | 3, [cost, check, depth, prepaid] =>
    let cost ← natArg cost
    let depth ← natArg depth
    let prepaid ← natArg prepaid
    if check != 0 && check != 1 && check != -1 then none else
    if check == -1 then
      if readBranch source == some (12, [depth]) then pure (charge b cost check depth prepaid)
      else none
    else if source.isEmpty then pure (charge b cost check depth prepaid) else none
  | 4, [] => if source.isEmpty then pure (publish b) else none
  | 5, [] => if source.isEmpty then pure (reload b) else none
  | 6, [depth] =>
    let depth ← natArg depth
    if source.isEmpty then pure (entry b depth) else none
  | 7, opcode :: depth :: rawTargets =>
    let opcode ← natArg opcode
    let depth ← natArg depth
    let targets ← rawTargets.mapM natArg
    if depth == 0 || targets.isEmpty || targets.any (· >= depth) ||
        readBranch source != some (opcode, targets) then none
    else pure ([opcode] ++ (if opcode == 14 then uleb (targets.length - 1) else []) ++
      (targets.flatMap fun target => uleb (mapDepth depth target)))
  | 8, [] => if source == [11] then pure [15] else none
  | 9, [original, clone, typeIndex, cost, tree] =>
    let original ← natArg original
    let clone ← natArg clone
    let typeIndex ← natArg typeIndex
    let cost ← natArg cost
    if readCall source != some original || (tree != 0 && tree != 1) then none else
    let fast := [16] ++ uleb clone ++ charge b cost 0 0 0
    if tree == 0 then pure fast
    else pure ([32] ++ uleb b.fuelLocal ++ [66] ++ positiveLeb cost ++ [89, 4] ++
      positiveLeb typeIndex ++ fast ++ [5] ++ publish b ++ source ++ reload b ++ [11])
  | 10, [depth] =>
    let depth ← natArg depth
    if source.isEmpty then pure ([12] ++ uleb depth) else none
  | 11, [] =>
    if source.isEmpty then
      pure ([11, 66, 0, 36] ++ uleb b.fuelGlobal ++ [65, 1, 36] ++
        uleb (b.fuelGlobal + 1) ++ [0, 11])
    else none
  | 12, [original, clone] =>
    let original ← natArg original
    let clone ← natArg clone
    if readCall source == some original then pure ([16] ++ uleb clone) else none
  | _, _ => none

def assemble (b : Body) : List Op → Nat → Nat → Option (List Nat)
  | [], sourceCursor, outputCursor =>
    if sourceCursor == b.input.length && outputCursor == b.output.length then some [] else none
  | op :: rest, sourceCursor, outputCursor => do
    if op.outputOffset != outputCursor then none else do
      if op.sourceOffset != sourceCursor then none else do
        if sourceCursor + op.sourceLength > b.input.length then none else do
          let source := (b.input.drop sourceCursor).take op.sourceLength
          let encoded ← encode b op source
          if encoded != op.output then none else do
            let tail ← assemble b rest (sourceCursor + op.sourceLength)
              (outputCursor + encoded.length)
            pure (encoded ++ tail)

def recoveredSource (body : Body) : List Nat :=
  body.ops.flatMap fun op => (body.input.drop op.sourceOffset).take op.sourceLength

def checkBody (body : Body) : Bool :=
  body.input.all (· < 256) && body.output.all (· < 256) &&
    decide (assemble body body.ops 0 0 = some body.output) &&
    decide (recoveredSource body = body.input)

/- A successful certificate checks actual bytes against the reference emitter,
not against an "expected" byte array supplied by the C emitter itself. -/
theorem checked_body_is_reference_encoding {body : Body}
    (accepted : checkBody body = true) :
    assemble body body.ops 0 0 = some body.output := by
  simp only [checkBody, Bool.and_eq_true] at accepted
  exact of_decide_eq_true accepted.1.2

theorem checked_body_recovers_original {body : Body}
    (accepted : checkBody body = true) : recoveredSource body = body.input := by
  simp only [checkBody, Bool.and_eq_true] at accepted
  exact of_decide_eq_true accepted.2

def checkCertificate (certificate : Certificate) : Bool :=
  certificate.bodies.all checkBody

theorem checked_certificate_body {certificate : Certificate} {body : Body}
    (accepted : checkCertificate certificate = true)
    (member : body ∈ certificate.bodies) : checkBody body = true := by
  exact (List.all_eq_true.mp accepted) body member

/- Concrete LEB edge cases are checked by the kernel as ordinary reduction. -/
example : positiveLeb 64 = [192, 0] := by decide
example : positiveLeb 128 = [128, 1] := by decide
example : signedLeb (-64) = [64] := by decide
example : signedLeb (-65) = [191, 127] := by decide

/- Private operations consume no source bytes, but their placement still matters:
their source anchor determines which structured labels a checkpoint can target.
These encoder-fragment tests do not claim that a lone block is a valid module. -/
private def anchorFragment (anchor : Nat) : Body :=
  { input := []
    output := [2, 64]
    fuelGlobal := 0
    fuelLocal := 0
    ops := [{
      kind := 2
      sourceOffset := anchor
      sourceLength := 0
      args := []
      outputOffset := 0
      output := [2, 64] }] }

example : checkBody (anchorFragment 0) = true := by decide
example : checkBody (anchorFragment 1) = false := by decide

end WasmMeterProof.Audit

def main (args : List String) : IO UInt32 := do
  let [path] := args | do
    IO.eprintln "usage: lean --run Audit.lean certificate.json"
    return 2
  let text ← IO.FS.readFile path
  let json ← match Lean.Json.parse text with
    | .ok value => pure value
    | .error error => IO.eprintln error; return 1
  let certificate ← match (Lean.fromJson? json : Except String WasmMeterProof.Audit.Certificate) with
    | .ok value => pure value
    | .error error => IO.eprintln error; return 1
  let mut count := 0
  for body in certificate.bodies do
    if !WasmMeterProof.Audit.checkBody body then
      IO.eprintln s!"FAIL body {count}: reference encoding/source coverage mismatch"
      return 1
    count := count + 1
  IO.println s!"PASS Lean reference encoding: {count} function bodies"
  return 0
