import GraphAudit

namespace WasmMeterProof.CheckerChecks
open CFG

/- These are kernel-reduced negative cases for certificate validation, not
evidence that a complete Wasm program has the modeled semantics. -/

-- Extra origin entries must not make an out-of-range target edge appear valid.
example : checkGraph [.guest [1] [0]] [.guest [1] [1]] [0, 0] = false := by decide
example : checkGraph [.guest [1] [0]] [.guest [1] [1]] [0] = false := by decide

-- An original operation, successor order and successor multiplicity all matter.
example : checkGraph [.guest [1] [1], .halt]
    [.guest [2] [1], .halt] [0, 1] = false := by decide
example : checkGraph [.guest [4] [1, 2], .halt, .halt]
    [.guest [4] [2, 1], .halt, .halt] [0, 1, 2] = false := by decide
example : checkGraph [.guest [14] [1, 1], .halt]
    [.guest [14] [1], .halt] [0, 1] = false := by decide

-- Silent self-cycles can simulate finite prefixes but must fail progress.
example : checkGraph [.halt] [.meter .publish 0] [0] = true := by decide
example : checkProgress [.meter .publish 0] [1] = false := by decide
example : checkProgress [.meter .publish 1, .halt] [1, 0, 99] = false := by decide
example : checkProgress [.meter .publish 1, .halt] [1, 0] = true := by decide

-- Both branches of a private fuel decision must preserve the current origin.
example : checkGraph [.halt, .halt]
    [.guard 7 1 2, .halt, .halt] [0, 0, 1] = false := by decide
example : checkGraph [.halt]
    [.guard 7 1 2, .halt, .halt] [0, 0, 0] = true := by decide
example : checkProgress [.guard 7 1 2, .halt, .halt] [1, 0, 0] = true := by decide

end WasmMeterProof.CheckerChecks
