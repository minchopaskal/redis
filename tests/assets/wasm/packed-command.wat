;; Build with: wat2wasm tests/assets/wasm/packed-command.wat -o tests/assets/wasm/packed-command.wasm
(module
  (import "redis" "input_read" (func $input_read (param i32 i32) (result i32)))
  (import "redis" "call" (func $call (param i32 i32) (result i32)))
  (import "redis" "reply_read" (func $reply_read (param i32 i32 i32) (result i32)))
  (import "redis" "reply_raw" (func $reply_raw (param i32 i32) (result i32)))
  (import "redis" "register_function" (func $register (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1 128)
  (data (i32.const 0) "packed_call")
  (data (i32.const 32) "run")

  (func (export "redis_wasm_abi_version_0_1_0"))
  (func (export "_initialize")
    i32.const 0 i32.const 11 i32.const 32 i32.const 3 call $register
    if unreachable end)

  ;; Pass the first FCALL argument verbatim to redis.call, including malformed
  ;; count/length fields that the normal SDK would never produce.
  (func (export "run") (result i32)
    (local $reply_len i32)
    i32.const 256 i32.const 4096 call $input_read
    i32.const 8 i32.lt_s
    if i32.const 1 return end
    i32.const 264 i32.const 260 i32.load call $call
    i32.const 0 i32.lt_s
    if i32.const 1 return end
    i32.const 4352 i32.const 4096 i32.const 0 call $reply_read
    local.tee $reply_len i32.const 0 i32.lt_s
    if i32.const 1 return end
    i32.const 4352 local.get $reply_len call $reply_raw)
)
