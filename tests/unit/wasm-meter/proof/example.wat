;; A small actual-byte certificate exercises cloned direct calls, a guarded
;; nested call tree, a branch to the function label, and ordinary returns.
(module
  (func $leaf (param i32) (result i32)
    local.get 0 i32.const 1 i32.add)
  (func $middle (param i32) (result i32)
    local.get 0 call $leaf)
  (func (export "answer") (param i32) (result i32)
    block (result i32)
      local.get 0 i32.eqz
      if
        i32.const 7 br 2
      end
      local.get 0 call $middle
    end))
