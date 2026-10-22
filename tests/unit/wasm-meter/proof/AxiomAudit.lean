import ControlFlow
import Audit
import Calls

/- Keep these dependencies visible in build output. The intended policy is no
unproved project axioms, sorryAx, native evaluation trust, or unsafe proof code.
Lean's foundational axioms, if used by library lemmas, are reported explicitly. -/
#print axioms WasmMeterProof.checkErasure_sound
#print axioms WasmMeterProof.certified_execution
#print axioms WasmMeterProof.certified_exhaustion_prefix
#print axioms WasmMeterProof.mapDepth_lookup
#print axioms WasmMeterProof.mapDepth_injective
#print axioms WasmMeterProof.typed_dispatch_frame
#print axioms WasmMeterProof.cloneOrigin_appended
#print axioms WasmMeterProof.fundedTree_prefix_positive
#print axioms WasmMeterProof.prepaid_backedge
#print axioms WasmMeterProof.CFG.checked_execution
#print axioms WasmMeterProof.CFG.checked_halt
#print axioms WasmMeterProof.CFG.checked_exhaustion_prefix
#print axioms WasmMeterProof.CFG.private_chain_bounded
#print axioms WasmMeterProof.CFG.no_private_cycle
#print axioms WasmMeterProof.Audit.checked_body_is_reference_encoding
#print axioms WasmMeterProof.Audit.checked_body_recovers_original
#print axioms WasmMeterProof.Calls.module_preserves_execution
#print axioms WasmMeterProof.Calls.entry_preserves_execution
