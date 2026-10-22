import GraphAudit

def main (args : List String) : IO UInt32 := do
  let [path] := args | do
    IO.eprintln "usage: graph-audit certificate.json"
    return 2
  let text ← IO.FS.readFile path
  let json ← match Lean.Json.parse text with
    | .ok value => pure value
    | .error error => IO.eprintln error; return 1
  let certificate ← match (Lean.fromJson? json : Except String WasmMeterProof.GraphAudit.Certificate) with
    | .ok value => pure value
    | .error error => IO.eprintln error; return 1
  let mut count := 0
  for graph in certificate.graphs do
    if !WasmMeterProof.GraphAudit.checkRecord graph then
      IO.eprintln s!"FAIL CFG {count}: operation/successor projection or private progress"
      return 1
    count := count + 1
  IO.println s!"PASS Lean CFG/progress reference check: {count} graphs"
  return 0
