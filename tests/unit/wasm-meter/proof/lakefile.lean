import Lake
open Lake DSL

package wasmMeterProof where
  version := v!"0.1.0"

@[default_target]
lean_lib Metering

@[default_target]
lean_lib ControlFlow

@[default_target]
lean_lib AxiomAudit

@[default_target]
lean_lib Audit

lean_exe audit where
  root := `Audit

@[default_target]
lean_lib GraphAudit

lean_exe graphAudit where
  root := `GraphAuditMain

@[default_target]
lean_lib CheckerChecks

@[default_target]
lean_lib Calls
