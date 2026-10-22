;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
(module
  (global $progress (export "progress") (mut i32) (i32.const 0))
  (start $start)
  (func $start (export "tree_start") call $middle)
  (func $middle call $leaf)
  (func $leaf i32.const 7 global.set $progress)
  (func (export "call_tree_start") call $start)
)
