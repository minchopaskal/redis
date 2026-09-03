(module
  (import "redis" "input_len" (func $input_len (result i32)))
  (import "redis" "input_read" (func $input_read (param i32 i32) (result i32)))
  (import "redis" "keys_count" (func $keys_count (result i32)))
  (import "redis" "call" (func $call (param i32 i32) (result i32)))
  (import "redis" "reply_read" (func $reply_read (param i32 i32 i32) (result i32)))
  (import "redis" "reply_raw" (func $reply_raw (param i32 i32) (result i32)))
  (import "redis" "status_reply" (func $status_reply (param i32 i32)))
  (import "redis" "error_reply" (func $error_reply (param i32 i32)))
  (import "redis" "register_function" (func $register_function (param i32 i32 i32 i32) (result i32)))

  (memory (export "memory") 1 128)

  ;; Function and export names.
  (data (i32.const 0) "hellohello_fn")
  (data (i32.const 32) "rawraw_fn")
  (data (i32.const 64) "set_valueset_fn")
  (data (i32.const 96) "inputinput_fn")
  (data (i32.const 128) "failurefailure_fn")
  (data (i32.const 160) "traptrap_fn")
  (data (i32.const 192) "bad_returnbad_return_fn")
  (data (i32.const 240) "resp3resp3_fn")
  (data (i32.const 600) "spinspin_fn")
  (data (i32.const 630) "bad_ptrbad_ptr_fn")

  ;; Replies and a packed ["SET", "wasm-key", "wasm-value"] command.
  (data (i32.const 300) "hello from wasm")
  (data (i32.const 340) "$5\0d\0ahello\0d\0a")
  (data (i32.const 380) "wasm failure")
  (data (i32.const 420) "_\0d\0a")
  (data (i32.const 460)
    "\03\00\00\00\03\00\00\00SET\08\00\00\00wasm-key\0a\00\00\00wasm-value")

  (func (export "redis_abi_version") (result i32)
    i32.const 1)

  (func (export "redis_init") (result i32)
    i32.const 0 i32.const 5 i32.const 5 i32.const 8 call $register_function drop
    i32.const 32 i32.const 3 i32.const 35 i32.const 6 call $register_function drop
    i32.const 64 i32.const 9 i32.const 73 i32.const 6 call $register_function drop
    i32.const 96 i32.const 5 i32.const 101 i32.const 8 call $register_function drop
    i32.const 128 i32.const 7 i32.const 135 i32.const 10 call $register_function drop
    i32.const 160 i32.const 4 i32.const 164 i32.const 7 call $register_function drop
    i32.const 192 i32.const 10 i32.const 202 i32.const 13 call $register_function drop
    i32.const 240 i32.const 5 i32.const 245 i32.const 8 call $register_function drop
    i32.const 600 i32.const 4 i32.const 604 i32.const 7 call $register_function drop
    i32.const 630 i32.const 7 i32.const 637 i32.const 10 call $register_function drop
    i32.const 0)

  (func (export "hello_fn") (result i32)
    i32.const 300 i32.const 15 call $status_reply
    i32.const 0)

  (func (export "raw_fn") (result i32)
    i32.const 340 i32.const 11 call $reply_raw drop
    i32.const 0)

  (func (export "set_fn") (result i32)
    i32.const 460 i32.const 37 call $call drop
    i32.const 300 i32.const 15 call $status_reply
    i32.const 0)

  (func (export "input_fn") (result i32)
    call $keys_count i32.const 1 i32.ne
    if (result i32)
      i32.const 1
    else
      call $input_len i32.const 14 i32.ne
      if (result i32)
        i32.const 2
      else
        i32.const 1024 i32.const 14 call $input_read drop
        i32.const 300 i32.const 15 call $status_reply
        i32.const 0
      end
    end)

  (func (export "failure_fn") (result i32)
    i32.const 380 i32.const 12 call $error_reply
    i32.const 0)

  (func (export "trap_fn") (result i32)
    unreachable)

  (func (export "bad_return_fn") (result i32)
    i32.const 7)

  (func (export "resp3_fn") (result i32)
    i32.const 420 i32.const 3 call $reply_raw drop
    i32.const 0)

  (func (export "spin_fn") (result i32)
    loop $spin
      br $spin
    end
    unreachable)

  (func (export "bad_ptr_fn") (result i32)
    i32.const 999999 i32.const 8 call $status_reply
    i32.const 0)
)
