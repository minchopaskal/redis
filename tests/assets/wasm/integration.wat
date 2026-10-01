;; Build with: wat2wasm tests/assets/wasm/integration.wat -o tests/assets/wasm/integration.wasm
;; Tests redirect export indices to the alternate functions below. All export
;; names, kinds and indices fit in one byte; do not enable debug names.
(module
  (import "redis" "register_function" (func $register (param i32 i32 i32 i32) (result i32)))
  (import "redis" "create_command" (func $command (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "redis" "status_reply" (func $status (param i32 i32)))
  (memory (export "memory") 1 128)
  (global $stage (mut i32) (i32.const 0))
  (data (i32.const 0) "abi_init")
  (data (i32.const 16) "abi_main")
  (data (i32.const 32) "abi_start")
  (data (i32.const 48) "run")
  (data (i32.const 64) "OK")

  ;; Even a trapping marker must load: Redis must never call it.
  (func (export "redis_wasm_abi_version_0_1_0") unreachable)
  ;; Obsolete exports are deliberately poisonous and must be ignored.
  (func (export "redis_abi_version") (result i32) unreachable)
  (func (export "redis_init") (result i32) unreachable)

  (func $initialize (export "_initialize")
    global.get $stage
    if unreachable end
    i32.const 1 global.set $stage
    i32.const 0 i32.const 8 i32.const 48 i32.const 3 i32.const 1 i32.const 0
    call $command
    if unreachable end)

  (func $main (export "main") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.or
    if unreachable end
    global.get $stage i32.const 1 i32.ne
    if unreachable end
    i32.const 2 global.set $stage
    i32.const 16 i32.const 8 i32.const 48 i32.const 3 call $register
    if unreachable end
    ;; The return value is unused, not a success/failure code.
    i32.const 42)

  (func (export "_start")
    global.get $stage
    if unreachable end
    i32.const 3 global.set $stage
    i32.const 32 i32.const 9 i32.const 48 i32.const 3 call $register
    if unreachable end)

  (func (export "run") (result i32)
    i32.const 64 i32.const 2 call $status
    i32.const 0)

  (func (export "void_param") (param i32))
  (func (export "i64_result") (result i64) i64.const 0)
  (func (export "main_param") (param i64 i32) (result i32) i32.const 0)
  (func (export "main_result") (param i32 i32) (result i64) i64.const 0)
  (func (export "trap_void") unreachable)
  (func (export "trap_main") (param i32 i32) (result i32) unreachable)
  (func $spin (export "spin_void")
    loop $forever br $forever end)
  (func (export "spin_main") (param i32 i32) (result i32)
    call $spin i32.const 0)
)
