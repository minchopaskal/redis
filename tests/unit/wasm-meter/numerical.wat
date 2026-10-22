;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
;; Original-versus-injected numerical, memory and host-observation fixtures.
(module
  (type $unary (func (param i32) (result i32)))
  (type $mixed (func (param f32 f64 i32) (result f64 i32 f32)))
  (import "env" "observe" (func $observe (type $unary)))
  (memory (export "memory") 1 3)
  (global $progress (export "progress") (mut i32) (i32.const 0))
  (table 3 funcref)
  (elem (i32.const 0) $unary $wrong_type)
  (func $unary (type $unary) local.get 0 i32.const 3 i32.add)
  (func $wrong_type (param i64) (result i64) local.get 0)
  (func (export "indirect_errors") (param i32) (result i32)
    i32.const 17 global.set $progress
    i32.const 7 local.get 0 call_indirect (type $unary))
  (func (export "grow") (param i32) (result i32)
    local.get 0 memory.grow)
  (func (export "memory_effect_then_trap") (param i32) (result i32)
    i32.const 0 i32.const 99 i32.store
    i32.const 11 call $observe drop
    local.get 0 i32.const 65536 i32.add i32.load)
  (func $leaf (type $mixed)
    local.get 1 f64.const 0.5 f64.add
    local.get 2 i32.const 17 i32.xor
    local.get 0 f32.const -2 f32.mul)
  (func $middle (type $mixed)
    local.get 0 local.get 1 local.get 2 call $leaf)
  (func (export "mixed_tree") (type $mixed)
    local.get 0 local.get 1 local.get 2 call $middle)
  (func (export "mixed_branch") (type $mixed)
    local.get 0 local.get 1 local.get 2 call $middle br 0)
  (func (export "trunc") (param f64) (result i32)
    i32.const 1 global.set $progress local.get 0 i32.trunc_f64_s)
  (func (export "signed_div") (param i32) (result i32)
    i32.const 1 global.set $progress
    i32.const -2147483648 local.get 0 i32.div_s)
  (func (export "host_sequence") (param i32) (result i32)
    local.get 0 global.set $progress
    i32.const 0 local.get 0 i32.store
    local.get 0 call $observe drop
    local.get 0 i32.const 1 i32.add global.set $progress
    i32.const 0 local.get 0 i32.const 10 i32.add i32.store
    local.get 0 i32.const 1 i32.add call $observe)
)
