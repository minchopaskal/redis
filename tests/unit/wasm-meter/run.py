#!/usr/bin/env python3
# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Compile and run the fuel injector against the existing stock FastJIT archive."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="retain compiled fixtures and verifier here")
    parser.add_argument("--wamr-library", type=Path, help="existing libiwasm.a (FastJIT enabled)")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    library = args.wamr_library or root / "deps/wamr/build-redis/libiwasm.a"
    if not library.is_file():
        parser.error("build Redis with BUILD_WASM=yes WASM_EXECUTION=fast-jit first")
    with tempfile.TemporaryDirectory(prefix="redis-wasm-meter-") as temporary:
        build = args.build_dir.resolve() if args.build_dir else Path(temporary)
        build.mkdir(parents=True, exist_ok=True)
        # Cross the 127/128 LEB boundary for both appended private indices.
        wide = "(module\n" + "(global i32 (i32.const 0))\n" * 128
        wide += '(func $wide (export "wide") (param i32 i64 i32) (result i64) (local '
        wide += "i32 " * 127 + "i64) local.get 1 local.set 130 local.get 130 global.get 127 drop)\n"
        wide += '(func (export "wide_call") (param i32 i64 i32) (result i64) local.get 0 local.get 1 local.get 2 call $wide))\n'
        (build / "wide-indices.wat").write_text(wide)
        # The first defined function and the final import straddle 127/128.
        wide_calls = '(module (type $unary (func (param i32) (result i32)))\n'
        wide_calls += '(import "env" "observe" (func (type $unary)))\n' * 128
        wide_calls += '(func $leaf (type $unary) local.get 0 i32.const 1 i32.add)\n'
        wide_calls += '(func (export "wide_guest") (type $unary) local.get 0 call $leaf)\n'
        wide_calls += '(func (export "wide_host") (type $unary) local.get 0 call 127))\n'
        (build / "wide-calls.wat").write_text(wide_calls)
        # Original direct target 126 is one-byte LEB; appended clone 128 is not.
        clone_indices = '(module\n' + '(func)\n' * 126
        clone_indices += '(func $leaf (param i32) (result i32) local.get 0 i32.const 1 i32.add)\n'
        clone_indices += '(func (export "clone_index") (param i32) (result i32) local.get 0 call $leaf))\n'
        (build / "clone-indices.wat").write_text(clone_indices)
        # Injected typed-if block types use signed LEB, unlike function indices.
        # Explicit duplicate types preserve these original type indices in WABT.
        tree_types = '(module\n'
        for i in range(129):
            signature = '(param i32 i64) (result i64 i32)' if i in (63, 64, 127, 128) else ''
            tree_types += f'(type $t{i} (func {signature}))\n'
        for i in (63, 64, 127, 128):
            tree_types += f'(func (export "tree_type_{i}") (type $t{i}) local.get 0 local.get 1 call $middle{i})\n'
            tree_types += f'(func $middle{i} (type $t{i}) local.get 0 local.get 1 call $leaf{i})\n'
            tree_types += f'(func $leaf{i} (type $t{i}) local.get 1 local.get 0)\n'
        (build / "tree-types.wat").write_text(tree_types + ')\n')
        # A long forward chain must be analyzed iteratively, not on the C stack.
        deep_tree = '(module (func (export "deep_tree") call $f0)\n'
        for i in range(4096):
            deep_tree += f'(func $f{i}' + (f' call $f{i + 1}' if i < 4095 else ' nop') + ')\n'
        (build / "deep-tree.wat").write_text(deep_tree + ')\n')
        # Reusing each child twice grows the dynamic cost beyond UINT32_MAX and
        # INT64_MAX with a tiny module. Analysis must not wrap into a cheap tree.
        expensive_tree = '(module (func (export "expensive_tree") call $f0)\n'
        for i in range(80):
            # Four root nops make its exact cost 5 * 2**79 + 1, wrapping to
            # one in both uint32 and uint64. A faulty sum would select the
            # astronomically expensive clone even with this test's 64 fuel.
            prefix = ' nop nop nop nop' if not i else ''
            expensive_tree += f'(func $f{i}' + prefix + (f' call $f{i + 1} call $f{i + 1}' if i < 79 else ' nop') + ')\n'
        (build / "expensive-tree.wat").write_text(expensive_tree + ')\n')
        # Appending a clone must not turn an invalid original function reference
        # into a valid one. WAMR must validate the original module first.
        invalid_refs = {
            "call": '(func call 0 call 2)',
            "export": '(func call 0) (export "invalid" (func 2))',
            "start": '(func call 0) (start 2)',
            "element": '(func call 0) (elem (i32.const 0) func 2)',
        }
        for kind, tail in invalid_refs.items():
            wat = build / f"invalid-{kind}.wat"
            wat.write_text('(module (type (func)) (table 1 funcref) (func) ' + tail + ')\n')
            subprocess.run(["wat2wasm", "--no-check", str(wat), "-o", str(wat.with_suffix(".wasm"))], check=True)

        (build / "reserved.wat").write_text('(module (func (export "__redis_meter_guest")))\n')
        # Header progress makes the exact backedge checkpoint observable even
        # when its prepaid signed fuel value is negative. Exercise SLEB widths.
        prefix_fixture = '(module (global $progress (export "progress") (mut i32) (i32.const 0))\n'
        for cost in [63, 64, 65, 127, 128, 129, 8191, 8192, 8193]:
            prefix_fixture += f'(func (export "prefix_{cost}") (param $n i32) (result i32) (local $i i32) block $out loop $L\n'
            prefix_fixture += 'nop ' * (cost - 8)
            prefix_fixture += 'global.get $progress i32.const 1 i32.add global.set $progress '
            prefix_fixture += 'local.get $i local.get $n i32.ge_u br_if $out\n'
            prefix_fixture += 'local.get $i i32.const 1 i32.and if nop else nop end '
            prefix_fixture += 'local.get $i i32.const 1 i32.add local.set $i br $L end end local.get $i)\n'
        prefix_fixture += ')\n'
        (build / "prefix-boundaries.wat").write_text(prefix_fixture)
        generated = ["wide-indices", "wide-calls", "clone-indices", "tree-types",
                     "deep-tree", "expensive-tree", "reserved", "prefix-boundaries"]
        for fixture in sorted(source.glob("*.wat")) + [build / f"{name}.wat" for name in generated]:
            subprocess.run(["wat2wasm", str(fixture), "-o", str(build / (fixture.stem + ".wasm"))], check=True)
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(root / "src"),
            "-I" + str(root / "deps/wamr/core/iwasm/include"),
            str(source / "verify.c"), str(root / "src/wasm_meter.c"),
            str(library.resolve()), "-lm", "-ldl", "-lpthread", "-lstdc++", "-o", str(build / "verify")]
        subprocess.run(command, check=True)
        # A missed backedge check must fail the test instead of hanging CI.
        subprocess.run([str(build / "verify"), str(build)], check=True, timeout=30)


if __name__ == "__main__":
    main()
