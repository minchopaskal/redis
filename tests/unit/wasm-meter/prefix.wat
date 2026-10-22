;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
;; Equal linear arms allow loop-header prepayment. The memory/division cases
;; distinguish preserving the previous checkpoint from prematurely trapping
;; when the local includes a prepaid, not-yet-executed next header.
(module
  (memory 1)
  (global $progress (export "progress") (mut i32) (i32.const 0))
  (func (export "prefix_simple") (param $n i32) (result i32) (local $i i32)
    block $out loop $L
      local.get $i local.get $n i32.ge_u br_if $out
      local.get $i i32.const 1 i32.and if nop else nop end local.get $i i32.const 1 i32.add local.set $i br $L
    end end local.get $i)
  (func (export "prefix_memory") (param $n i32) (result i32) (local $i i32)
    block $out loop $L
      local.get $i i32.const 65536 i32.mul i32.load drop local.get $i local.get $n i32.ge_u br_if $out
      local.get $i i32.const 1 i32.and if nop else nop end global.get $progress i32.const 1 i32.add global.set $progress local.get $i i32.const 1 i32.add local.set $i br $L
    end end local.get $i)
  (func (export "prefix_division") (param $n i32) (result i32) (local $i i32)
    block $out loop $L
      i32.const 10 local.get $i i32.const 1 i32.xor i32.div_u drop local.get $i local.get $n i32.ge_u br_if $out
      local.get $i i32.const 1 i32.and if nop else nop end global.get $progress i32.const 1 i32.add global.set $progress local.get $i i32.const 1 i32.add local.set $i br $L
    end end local.get $i)
  (func (export "prefix_typed") (param $n i32) (result i32) (local $i i32)
    i64.const 123 block $out (param i64) (result i64)
      loop $L (param i64) (result i64)
        local.get $i local.get $n i32.ge_u br_if $out
        local.get $i i32.const 1 i32.and if nop else nop end
        i64.const 1 i64.add local.get $i i32.const 1 i32.add local.set $i br $L
      end
    end i32.wrap_i64)
)
