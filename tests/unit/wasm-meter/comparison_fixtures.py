# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Small deterministic, valid Wasm corpus for byte-preserving C refactors.

These modules are checked by wat2wasm and compared as injector inputs, not run:
some intentionally contain cycles. This is a varied finite corpus, not fuzzing
or a proof about arbitrary Wasm.
"""
import random


def generate(build):
    randomizer = random.Random(0x52454449)
    constants = [0, 1, -1, 63, 64, 127, 128, 8191, 8192, 2147483647, -2147483648]
    paths = []
    for case in range(24):
        constant = randomizer.choice(constants)
        arithmetic = randomizer.choice(["i32.add", "i32.sub", "i32.xor", "i32.mul"])
        functions = [
            f'(func $leaf (type $unary) local.get 0 i32.const {constant} {arithmetic})',
            '(func $tree (export "tree") (type $unary) local.get 0 call $leaf call $leaf)',
            '(func $diamond (type $unary) local.get 0 if (result i32) '
            'local.get 0 call $tree else local.get 0 call $leaf end)',
            '(func $equal (type $unary) local.get 0 if (result i32) '
            f'i32.const {constant} i32.const 3 i32.add else i32.const 9 i32.const 5 i32.sub end)',
            '(func $loop (type $unary) (local i32) block $out loop $again '
            'local.get 1 local.get 0 i32.ge_u br_if $out '
            'local.get 1 call $tree local.set 1 br $again end end local.get 1)',
            '(func $host (type $unary) local.get 0 call $observe)',
            '(func $indirect (type $unary) local.get 0 i32.const 1 call_indirect (type $unary))',
            '(func $cycle_a (type $unary) local.get 0 call $cycle_b)',
            '(func $cycle_b (type $unary) local.get 0 call $cycle_a)',
            '(func $exit (type $unary) i32.const 17 local.get 0 br_if 0 drop i32.const 19)',
            '(func $fill (type $unary) i32.const 0 i32.const 7 local.get 0 memory.fill local.get 0)',
            '(func $trap (type $unary) local.get 0 if unreachable end local.get 0)',
            '(func $pair (type $pair) local.get 1 local.get 0)',
            '(func $pair_middle (type $pair) local.get 0 local.get 1 call $pair)',
            '(func $pair_outer (export "pair") (type $pair) local.get 0 local.get 1 call $pair_middle)',
        ]
        randomizer.shuffle(functions)
        text = '(module (type $unary (func (param i32) (result i32)))\n'
        text += '(type $pair (func (param i32 i64) (result i64 i32)))\n'
        text += '(import "env" "observe" (func $observe (type $unary)))\n'
        text += '(memory 1) (table 3 funcref) (elem (i32.const 0) $leaf $diamond $observe)\n'
        text += '\n'.join(functions) + ')\n'
        path = build / f"compare-generated-{case:02}.wat"
        path.write_text(text)
        paths.append(path)
    return paths
