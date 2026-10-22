;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
;; The verifier's expected fuel costs count the original executed opcodes,
;; including control delimiters reached by fallthrough, independently of the
;; injector. Branches skip the target block/function's end instruction.
(module
  (type $unary (func (param i32) (result i32)))
  (type $multi (func (param i32 i64) (result i32 i64)))
  (import "env" "observe" (func $observe (type $unary)))
  (import "env" "spend" (func $spend (type $unary)))
  (table 3 funcref)
  (elem (i32.const 0) $increment $recursive_indirect $cycle_indirect)
  (memory 1)
  (global $progress (export "progress") (mut i32) (i32.const 0))

  (func (export "empty"))
  (func (export "value") (result i32) i32.const 42)
  (func (export "linear") (param i32) (result i32)
    local.get 0 i32.const 3 i32.add i32.const 7 i32.xor)
  (func (export "if_else") (param i32) (result i32)
    local.get 0 if (result i32)
      i32.const 10 i32.const 2 i32.add
    else i32.const 7 end)
  (func (export "if_join") (param i32) (result i32)
    local.get 0 if nop nop end i32.const 9)
  (func (export "equal_if") (param i32) (result i32)
    local.get 0 if (result i32)
      i32.const 10 i32.const 2 i32.add
    else i32.const 5 i32.const 3 i32.mul end)
  (func (export "equal_if_skip_end") (param i32) (result i32)
    block $out (result i32)
      local.get 0 if (result i32) i32.const 17 else i32.const 19 end
      br $out
    end)
  (func (export "nested_if") (param i32) (result i32)
    local.get 0 if (result i32)
      i32.const 1 if (result i32) i32.const 11 else i32.const 12 end
    else i32.const 13 end)
  (func (export "branch_in_arm") (param i32) (result i32)
    block $out (result i32)
      local.get 0 if i32.const 17 br $out else nop nop end
      i32.const 19
    end)
  (func (export "call_in_arm") (param i32) (result i32)
    local.get 0 if (result i32)
      local.get 0 call $observe
    else local.get 0 call $observe end)
  (func (export "block_linear") (result i32)
    block nop block nop end nop end i32.const 33)
  (func (export "function_br") (result i32) i32.const 17 br 0)
  (func (export "function_br_if") (param i32) (result i32)
    i32.const 17 local.get 0 br_if 0 drop i32.const 19)
  (func (export "function_table") (param i32) (result i32)
    block (result i32) i32.const 23 local.get 0 br_table 0 1 end)
  (func (export "explicit_return") (result i32) i32.const 31 return)
  (func (export "multi") (type $multi) local.get 0 local.get 1)
  (func (export "multi_br") (type $multi) local.get 0 local.get 1 br 0)
  (func (export "multi_table") (param i32 i64 i32) (result i32 i64)
    block (result i32 i64) local.get 0 local.get 1 local.get 2 br_table 0 1 end)

  (func (export "counted") (param $n i32) (result i32) (local $i i32)
    block $out loop $again
      local.get $i local.get $n i32.ge_u br_if $out
      local.get $i i32.const 1 i32.add local.set $i br $again
    end end local.get $i)
  (func (export "counted_if") (param $n i32) (result i32) (local $i i32)
    loop $again
      local.get $i i32.const 1 i32.add local.set $i
      local.get $i local.get $n i32.lt_u br_if $again
    end local.get $i)
  (func (export "counted_table") (param $n i32) (result i32) (local $i i32)
    block $out loop $again
      local.get $i i32.const 1 i32.add local.set $i
      local.get $i local.get $n i32.ge_u br_table $again $out
    end end local.get $i)
  (func (export "typed_loop") (type $multi) (local $i i32) (local f64 i64 i32)
    local.get 0 local.get 1 loop $again (type $multi)
      local.get $i i32.const 1 i32.add local.set $i
      local.get $i i32.const 3 i32.lt_u br_if $again
    end)
  (func (export "typed_loop_br") (type $multi) (local $i i32) (local $x i32) (local $y i64)
    local.get 0 local.get 1 loop $again (type $multi)
      local.set $y local.tee $x local.get $y
      local.get $i i32.const 1 i32.add local.set $i
      local.get $i i32.const 3 i32.ge_u
      if local.get $x local.get $y return end
      br $again
    end)

  (func $increment (export "increment") (type $unary)
    local.get 0 i32.const 1 i32.add)
  (func (export "direct") (type $unary)
    local.get 0 call $increment i32.const 2 i32.mul)
  (func (export "indirect") (type $unary)
    local.get 0 i32.const 0 call_indirect (type $unary) i32.const 2 i32.mul)
  (func (export "twice") (type $unary)
    local.get 0 call $increment call $increment)
  (func (export "call_loop") (param $n i32) (result i32) (local $i i32)
    block $out loop $again
      local.get $i local.get $n i32.ge_u br_if $out
      local.get $i call $increment local.set $i br $again
    end end local.get $i)
  (func $recursive (export "recursive") (type $unary)
    local.get 0 if (result i32)
      local.get 0 i32.const 1 i32.sub call $recursive i32.const 1 i32.add
    else i32.const 0 end)
  (func $recursive_indirect (export "recursive_indirect") (type $unary)
    local.get 0 if (result i32)
      local.get 0 i32.const 1 i32.sub i32.const 1 call_indirect (type $unary)
      i32.const 1 i32.add
    else i32.const 0 end)
  (func (export "hosted") (type $unary)
    local.get 0 call $observe i32.const 1 i32.add)
  (func (export "host_spend") (type $unary)
    local.get 0 call $spend i32.const 1 i32.add)
  (func (export "host_after_branch") (type $unary)
    local.get 0 if nop nop else nop end
    i32.const 10 call $observe)

  (func (export "spin_br") loop $again br $again end)
  (func (export "spin_if") loop $again i32.const 1 br_if $again end)
  (func (export "spin_table") loop $again i32.const 0 br_table $again $again end)
  (func (export "spin_mixed_table")
    loop $again block $out i32.const 0 br_table $again $out end br $again end)
  (func (export "spin_nested")
    loop $outer loop $inner
      i32.const 1 if br $inner else br $outer end
    end end)
  (func (export "spin_values") (result i32)
    i32.const 123 loop $again (param i32) (result i32)
      i32.const 0 br_table $again $again
    end)
  (func (export "spin_values_br") (type $multi)
    local.get 0 local.get 1 loop $again (type $multi) br $again end)
  (func (export "spin_call") loop $again i32.const 1 call $increment drop br $again end)
  (func $cycle (export "cycle") call $cycle)
  (func $cycle_indirect (export "cycle_indirect") i32.const 2 call_indirect)
  (func (export "trap") unreachable)
  (func (export "memory_trap") i32.const -1 i32.load drop)
)
