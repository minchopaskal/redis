;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
;; Directly called linear leaves are eligible for caller-paid accounting.
;; Their original exported/table definitions must retain entry metering.
(module
  (type $unary (func (param i32) (result i32)))
  (type $pair (func (param i32 i64) (result i64 i32)))
  (import "env" "observe" (func $observe (type $unary)))
  (memory (export "memory") 1)
  (global $progress (export "progress") (mut i32) (i32.const 0))
  (table 1 funcref)
  (elem (i32.const 0) $read)
  (func $read (export "read") (type $unary) local.get 0 i32.load)
  (func (export "read_direct") (type $unary) local.get 0 call $read)
  (func (export "read_table") (type $unary)
    local.get 0 i32.const 0 call_indirect (type $unary))
  (func $write (export "write") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.store local.get 1)
  (func (export "write_direct") (param i32 i32) (result i32)
    local.get 0 local.get 1 call $write)
  (func $effect_div (export "effect_div") (type $unary)
    global.get $progress i32.const 1 i32.add global.set $progress
    i32.const 0 local.get 0 i32.store
    i32.const 120 local.get 0 i32.div_u)
  (func (export "effect_direct") (type $unary) local.get 0 call $effect_div)
  (func (export "effect_twice") (type $unary)
    local.get 0 call $effect_div call $effect_div)
  (func (export "effect_host") (type $unary)
    local.get 0 call $effect_div call $observe)
  ;; A returned leaf cost must survive the next structured boundary, including
  ;; function-label exits that bypass both the if and function end opcodes.
  (func (export "effect_if") (type $unary) (local i32)
    i32.const 6 call $effect_div local.set 1
    local.get 0 if (result i32) i32.const 17 br 1 else i32.const 19 end)
  (func (export "effect_br_if") (type $unary)
    i32.const 6 call $effect_div local.get 0 br_if 0 drop i32.const 19)
  ;; Each header and leaf contributes one progress increment. The first
  ;; backedge costs 20; only a funded backedge can run the next header effect.
  (func (export "effect_backedge")
    loop $again
      global.get $progress i32.const 1 i32.add global.set $progress
      i32.const 6 call $effect_div drop br $again
    end)
  ;; The block keeps this callee in the ordinary metered-call path.
  (func $ordinary_effect (type $unary)
    block global.get $progress i32.const 100 i32.add global.set $progress end
    local.get 0)
  (func (export "effect_guest") (type $unary)
    local.get 0 call $effect_div call $ordinary_effect)
  (func $pair (export "pair") (type $pair) (local i64 i32)
    local.get 1 local.set 2
    local.get 0 i32.const 3 i32.add local.set 3
    local.get 2 local.get 3)
  (func (export "pair_direct") (type $pair)
    local.get 0 local.get 1 call $pair)
  (func $fill (type $unary)
    i32.const 0 i32.const 7 local.get 0 memory.fill local.get 0)
  (func (export "fill_direct") (type $unary) local.get 0 call $fill)
)
