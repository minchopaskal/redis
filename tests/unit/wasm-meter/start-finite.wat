;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
(module
  (global $progress (export "progress") (mut i32) (i32.const 0))
  (func $start i32.const 7 global.set $progress)
  (start $start)
  (func (export "initialize") i32.const 11 global.set $progress)
  (func (export "ok") (result i32) i32.const 42))
