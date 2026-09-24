;; Build with: wat2wasm tests/assets/wasm/commands.wat -o tests/assets/wasm/commands.wasm
(module
  (import "redis" "create_command" (func $create (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "redis" "status_reply" (func $status (param i32 i32)))
  (memory (export "memory") 1 128)
  (data (i32.const 0) "wasm_zero")
  (data (i32.const 32) "wasm_two")
  (data (i32.const 64) "run")
  (data (i32.const 80) "OK")
  (func (export "redis_abi_version") (result i32) i32.const 1)
  (func (export "redis_init") (result i32)
    i32.const 0 i32.const 9 i32.const 64 i32.const 3 i32.const 1 i32.const 0
    call $create
    if
      i32.const 1
      return
    end
    i32.const 32 i32.const 8 i32.const 64 i32.const 3 i32.const -3 i32.const 2
    call $create)
  (func (export "run") (result i32)
    i32.const 80 i32.const 2 call $status
    i32.const 0)
)
