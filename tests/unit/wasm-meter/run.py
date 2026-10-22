#!/usr/bin/env python3
# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Compile and run the fuel injector against the existing stock FastJIT archive."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from comparison_fixtures import generate as comparison_fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="retain compiled fixtures and verifier here")
    parser.add_argument("--wamr-library", type=Path, help="existing libiwasm.a (FastJIT enabled)")
    parser.add_argument("--baseline-source", type=Path,
                        help="compare actual emitted bytes/rejections with this frozen wasm_meter.c")
    parser.add_argument("--compare-only", action="store_true",
                        help="run byte comparisons without linking or executing WAMR (requires --baseline-source)")
    args = parser.parse_args()
    if args.compare_only and not args.baseline_source:
        parser.error("--compare-only requires --baseline-source")
    if args.baseline_source and not args.baseline_source.is_file():
        parser.error("--baseline-source must name an existing wasm_meter.c")
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    library = args.wamr_library or root / "deps/wamr/build-redis/libiwasm.a"
    if not args.compare_only and not library.is_file():
        parser.error("build Redis with BUILD_WASM=yes WASM_EXECUTION=fast-jit first")
    with tempfile.TemporaryDirectory(prefix="redis-wasm-meter-") as temporary:
        build = args.build_dir.resolve() if args.build_dir else Path(temporary)
        build.mkdir(parents=True, exist_ok=True)
        wasm_fixtures = []
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
        # A single body must cross the 1,024-operation plan streaming boundary.
        # The deep-chain fixture alone only exercises many very small plans.
        streaming = '(module (func $leaf (param i32) (result i32) local.get 0 i32.const 1 i32.add)\n'
        streaming += '(func (export "chunked") (param i32) (result i32)\n'
        streaming += 'local.get 0 call $leaf local.set 0\n' * 2048
        (build / "streaming-plan.wat").write_text(streaming + 'local.get 0))\n')
        # WABT emits canonical LEBs, so build this small valid module explicitly.
        # Noncanonical encodings appear in the function type index, locals-vector
        # count, local.get index and br-to-function target. Validation is external.
        noncanonical = b'\x00asm\x01\x00\x00\x00\x01\x06\x01\x60\x01\x7f\x01\x7f'
        noncanonical += b'\x03\x03\x01\x80\x00'
        exported = b'\x01\x0cnoncanonical\x00\x80\x00'
        noncanonical += b'\x07' + bytes([len(exported)]) + exported
        function = b'\x80\x00\x20\x80\x00\x0c\x80\x00\x0b'
        code = b'\x01' + bytes([len(function)]) + function
        noncanonical += b'\x0a' + bytes([len(code)]) + code
        noncanonical_path = build / "noncanonical-leb.wasm"
        noncanonical_path.write_bytes(noncanonical)
        subprocess.run(["wasm-validate", str(noncanonical_path)], check=True)
        wasm_fixtures.append(noncanonical_path)
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
            wasm_fixtures.append(wat.with_suffix(".wasm"))

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
                     "deep-tree", "expensive-tree", "streaming-plan", "reserved", "prefix-boundaries"]
        fixtures = sorted(source.glob("*.wat")) + [build / f"{name}.wat" for name in generated]
        if args.baseline_source:
            fixtures += comparison_fixtures(build)
        for fixture in fixtures:
            wasm = build / (fixture.stem + ".wasm")
            subprocess.run(["wat2wasm", str(fixture), "-o", str(wasm)], check=True)
            wasm_fixtures.append(wasm)
        compiler = shlex.split(os.environ.get("CC", "cc")) + [
            "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(root / "src"),
            "-I" + str(root / "deps/wamr/core/iwasm/include")]
        current_object = build / "wasm_meter.o"
        subprocess.run(compiler + ["-c", str(root / "src/wasm_meter.c"),
                                  "-o", str(current_object)], check=True)
        if args.baseline_source:
            baseline_object = build / "wasm_meter_baseline.o"
            subprocess.run(compiler + ["-DwasmMeterInject=wasmMeterInjectBaseline", "-c",
                                      str(args.baseline_source.resolve()), "-o", str(baseline_object)], check=True)
            subprocess.run(compiler + [str(source / "compare.c"), str(current_object),
                                      str(baseline_object), "-o", str(build / "compare")], check=True)
            comparison = subprocess.run([str(build / "compare")] + [str(p) for p in sorted(wasm_fixtures)],
                                        text=True, capture_output=True, timeout=30)
            (build / "comparison.log").write_text(comparison.stdout + comparison.stderr)
            print(comparison.stdout, end="", flush=True)
            if comparison.returncode:
                print(comparison.stderr, end="", flush=True)
            comparison.check_returncode()
        if args.compare_only:
            return

        libraries = [str(library.resolve()), "-lm", "-ldl", "-lpthread", "-lstdc++"]
        subprocess.run(compiler + [str(source / "verify.c"), str(current_object)] +
                       libraries + ["-o", str(build / "verify")], check=True)
        # A missed backedge check must fail the test instead of hanging CI.
        subprocess.run([str(build / "verify"), str(build)], check=True, timeout=30)
        subprocess.run(compiler + [str(source / "differential.c"), str(current_object)] +
                       libraries + ["-o", str(build / "differential")], check=True)
        differential = subprocess.run([str(build / "differential"), str(build)],
                                      text=True, capture_output=True, timeout=30)
        (build / "differential.log").write_text(differential.stdout + differential.stderr)
        if differential.returncode:
            print(differential.stdout + differential.stderr, end="", flush=True)
        else:
            print(differential.stdout.strip().splitlines()[-1], flush=True)
        differential.check_returncode()


if __name__ == "__main__":
    main()
