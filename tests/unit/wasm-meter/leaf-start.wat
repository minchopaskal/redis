;; Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
(module
  (global $progress (export "progress") (mut i32) (i32.const 0))
  (func $start (export "start_again") i32.const 7 global.set $progress)
  (start $start)
  (func (export "call_start") call $start))
