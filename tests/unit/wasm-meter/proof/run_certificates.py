#!/usr/bin/env python3
# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Check real emitter receipts, independent module binding and Lean encodings.

Use run.py --build-dir DIR first to materialize the WAT fixtures. This runner
accepts only inputs passing wasm-validate, excluding explicit rejection fixtures.
An executable Lean check of each JSON receipt is finite checking evidence; it is
not the same as a kernel proof term for every receipt or a C/Wasm refinement proof.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

from check_receipt import InvalidReceipt, check, module


def execute(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed: {shlex.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result


def rejection_mutations(original, injected, records):
    source_event = next(event for event in records if event["event"] == "op" and
                        event["kind"] == 0 and event["sourceLength"])
    altered = bytearray(original)
    altered[source_event["sourceOffset"]] ^= 1
    yield "source-byte", bytes(altered), injected, copy.deepcopy(records)

    mutated = copy.deepcopy(records)
    branch = next(event for event in mutated if event["event"] == "op" and event["kind"] == 7)
    branch["args"][1] += 1
    yield "branch-depth", original, injected, mutated

    mutated = copy.deepcopy(records)
    next(event for event in mutated if event["event"] == "begin")["global"] += 1
    yield "private-global-index", original, injected, mutated

    mutated = copy.deepcopy(records)
    next(event for event in mutated if event["event"] == "begin")["local"] += 1
    yield "private-local-index", original, injected, mutated

    mutated = copy.deepcopy(records)
    dispatch = next(event for event in mutated if event["event"] == "op" and event["kind"] == 9)
    dispatch["args"][1] += 1
    yield "clone-index", original, injected, mutated

    mutated = copy.deepcopy(records)
    last_begin = max(i for i, event in enumerate(mutated) if event["event"] == "begin")
    yield "missing-body-receipt", original, injected, mutated[:last_begin]

    mutated = copy.deepcopy(records)
    event = next(event for event in mutated if event["event"] == "op" and event["output"])
    event["output"][0] ^= 1
    yield "receipt-output-byte", original, injected, mutated

    # Preserve all original/output bytes and their partitions while splitting
    # local.get's opcode from its immediate. The independent instruction-boundary
    # rule must reject this weaker receipt, even though simple byte erasure passes.
    mutated = copy.deepcopy(records)
    position = next(i for i, event in enumerate(mutated) if event["event"] == "op" and
                    event["kind"] == 0 and event["sourceLength"] >= 2 and
                    original[event["sourceOffset"]] in (0x20, 0x41))
    first, second = copy.deepcopy(mutated[position]), copy.deepcopy(mutated[position])
    first["sourceLength"], first["output"] = 1, first["output"][:1]
    second["sourceOffset"] += 1
    second["sourceLength"] -= 1
    second["outputOffset"] += 1
    second["output"] = second["output"][1:]
    mutated[position:position + 1] = [first, second]
    yield "split-instruction-copy", original, injected, mutated

    for kind, name in ((2, "duplicate-exhaustion-wrapper"), (11, "duplicate-cold-exit")):
        mutated = copy.deepcopy(records)
        position = next(i for i, event in enumerate(mutated) if event["event"] == "op" and event["kind"] == kind)
        extra = copy.deepcopy(mutated[position])
        if kind == 2:
            position += 2  # after the existing four-operation prologue
        extra["outputOffset"] = mutated[position]["outputOffset"]
        extra["output"] = []
        mutated.insert(position, extra)
        yield name, original, injected, mutated

    mutated = copy.deepcopy(records)
    position = next(i for i, event in enumerate(mutated) if event["event"] == "op" and event["kind"] == 10)
    del mutated[position]
    yield "missing-fused-exhaustion-jump", original, injected, mutated

    # The Python binding checker intentionally does not reimplement all charge
    # placement arithmetic. Lean's independent encoder must reject this change.
    mutated = copy.deepcopy(records)
    event = next(event for event in mutated if event["event"] == "op" and event["kind"] == 3)
    event["args"][0] += 1
    yield "charge-encoding", original, injected, mutated


def private_anchor_mutation(original, injected, records):
    """Forge a valid output branch which skips an original unreachable.

    The forged checked debit uses an outer instruction as its private source
    anchor. Its encoded depth then exits the guest if/block instead of the
    exhaustion wrapper. Byte encoding alone accepts this consistent forgery;
    binding zero-length anchors to the current source cursor must reject it.
    """
    source = output = None
    context = None
    for index, event in enumerate(records):
        if event["event"] == "begin":
            context = event
        if (event["event"] != "op" or event["kind"] != 3 or event["args"][1] != 1
                or not 0 < event["args"][2] < 64):
            continue
        if source is None:
            source, output = module(original), module(injected)
        imports = len(source["importFunctions"])
        body = source["bodies"][context["original"] - imports]
        instruction = body["byOffset"].get(event["sourceOffset"])
        if instruction is None or instruction["opcode"] != 0:
            continue
        outer = next(instruction for instruction in body["ops"] if instruction["depth"] == 1)
        mutated = copy.deepcopy(records)
        charge = mutated[index]
        assert charge["output"][-2:] == [13, charge["args"][2]]
        charge["sourceOffset"], charge["args"][2], charge["output"][-1] = outer["offset"], 0, 0
        offset = charge["outputOffset"] + len(charge["output"]) - 1
        end = next(item for item in mutated[index:] if item["event"] == "end")
        end["output"][offset] = 0
        actual = output["bodies"][context["emitted"] - imports]
        altered = bytearray(injected)
        altered[actual["offset"] + offset] = 0
        return "private-check-anchor-and-depth", original, bytes(altered), mutated
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--receipt-executable", type=Path,
                        help="reuse a previously compiled production receipt harness")
    parser.add_argument("--audit-executable", type=Path,
                        help="compiled Lean Audit executable (default: proof/.lake/build/bin/audit)")
    parser.add_argument("--lean-root", type=Path,
                        help="Lean toolchain directory containing bin/lake and bin/lean")
    parser.add_argument("--only", nargs="+", help="fixture stems to check")
    parser.add_argument("--skip-lean", action="store_true",
                        help="development-only Python binding checks; no reference-encoding result")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parents[3]
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source_names = ("wasm_meter.c", "wasm_meter.h", "wasm_meter_body.h", "wasm_meter_plan.h")
    source_hashes = {name: hashlib.sha256((root / "src" / name).read_bytes()).hexdigest() for name in source_names}
    receipt = args.receipt_executable.resolve() if args.receipt_executable else build / "receipt"
    if not args.receipt_executable:
        compiler = shlex.split(os.environ.get("CC", "cc"))
        execute(compiler + ["-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(root / "src"),
                            str(source / "receipt.c"), "-o", str(receipt)])
    audit = args.audit_executable.resolve() if args.audit_executable else source / ".lake/build/bin/audit"
    lean_env = os.environ.copy()
    lake = None
    if not args.skip_lean:
        if args.lean_root:
            lean_env["PATH"] = str(args.lean_root.resolve() / "bin") + os.pathsep + lean_env.get("PATH", "")
        lake = shutil.which("lake", path=lean_env.get("PATH"))
        if not lake:
            parser.error("Lean lake not found; use --lean-root TOOLCHAIN_DIRECTORY")
        built = execute([lake, "build", "audit"], cwd=source, env=lean_env, timeout=120)
        (build / "lean-build.log").write_text(built.stdout + built.stderr)
    if not args.skip_lean and not audit.is_file():
        parser.error("build the Lean audit target first: lake build audit")
    fixtures = sorted(args.fixture_dir.resolve().glob("*.wasm"))
    if args.only:
        selected = set(args.only)
        fixtures = [path for path in fixtures if path.stem in selected]
        if {path.stem for path in fixtures} != selected:
            parser.error("one or more --only fixtures were not found")
    fixtures = [path for path in fixtures if not path.stem.startswith("invalid-") and path.stem != "reserved"]
    if not fixtures:
        parser.error("no valid fixtures selected")
    if not args.skip_lean:
        example = build / "kernel-example.wasm"
        execute(["wat2wasm", str(source / "example.wat"), "-o", str(example)])
        fixtures.append(example)
    rows, mutation_case, anchor_case = [], None, None
    for fixture in fixtures:
        execute(["wasm-validate", str(fixture)])
        injected_path = build / (fixture.stem + ".injected.wasm")
        receipt_path = build / (fixture.stem + ".receipt.jsonl")
        certificate_path = build / (fixture.stem + ".certificate.json")
        production = execute([str(receipt), str(fixture), str(injected_path)], timeout=60)
        receipt_path.write_text(production.stdout)
        execute(["wasm-validate", str(injected_path)])
        original, injected = fixture.read_bytes(), injected_path.read_bytes()
        records = [json.loads(line) for line in production.stdout.splitlines() if line.strip()]
        certificate = check(original, injected, records)
        certificate_path.write_text(json.dumps(certificate, separators=(",", ":")) + "\n")
        if not args.skip_lean:
            result = execute([str(audit), str(certificate_path)], timeout=120)
            (build / (fixture.stem + ".lean.log")).write_text(result.stdout + result.stderr)
        rows.append({"fixture": fixture.stem, "bodies": len(certificate["bodies"]),
                     "original_sha256": hashlib.sha256(original).hexdigest(),
                     "injected_sha256": hashlib.sha256(injected).hexdigest(),
                     "certificate_sha256": hashlib.sha256(certificate_path.read_bytes()).hexdigest()})
        if fixture.stem == "call-trees":
            mutation_case = original, injected, records
        if anchor_case is None:
            anchor_case = private_anchor_mutation(original, injected, records)
        print(f"PASS certificate {fixture.stem}: {len(certificate['bodies'])} bodies", flush=True)
    mutations = []
    mutation_inputs = list(rejection_mutations(*mutation_case)) if mutation_case else []
    if anchor_case:
        mutation_inputs.append(anchor_case)
    if mutation_inputs:
        for name, original, injected, records in mutation_inputs:
            if name == "private-check-anchor-and-depth":
                # Ensure rejection detects the control correspondence defect,
                # rather than merely malformed Wasm or inconsistent receipts.
                path = build / ("mutation-" + name + ".wasm")
                path.write_bytes(injected)
                execute(["wasm-validate", str(path)])
                (build / ("mutation-" + name + ".receipt.jsonl")).write_text(
                    "".join(json.dumps(record) + "\n" for record in records))
            caught_by = None
            try:
                certificate = check(original, injected, records)
            except (InvalidReceipt, ValueError, KeyError, IndexError) as error:
                caught_by = "independent Python module/receipt binding"
                diagnostic = str(error)
            else:
                if args.skip_lean:
                    if name == "charge-encoding":
                        continue
                    raise RuntimeError(f"mutation was not detected: {name}")
                path = build / ("mutation-" + name + ".certificate.json")
                path.write_text(json.dumps(certificate, separators=(",", ":")) + "\n")
                result = subprocess.run([str(audit), str(path)], capture_output=True, text=True, timeout=30)
                if result.returncode:
                    caught_by = "Lean reference encoder"
                    diagnostic = result.stdout + result.stderr
            if not caught_by:
                raise RuntimeError(f"mutation was not detected: {name}")
            mutations.append({"mutation": name, "caught_by": caught_by, "diagnostic": diagnostic})
            print(f"PASS rejection mutation {name}: {caught_by}", flush=True)
    kernel_certificates = []
    if not args.skip_lean:
        for stem, theorem_file in (("kernel-example", "KernelExample.lean"), ("tree-types", "KernelTreeTypes.lean")):
            certificate_path = build / (stem + ".certificate.json")
            if not certificate_path.exists():
                continue
            theorem_path = build / theorem_file
            execute([sys.executable, str(source / "kernel_receipt.py"), str(certificate_path), str(theorem_path)])
            checked = execute([lake, "env", "lean", str(theorem_path)], cwd=source, env=lean_env, timeout=120)
            (build / (theorem_file + ".log")).write_text(checked.stdout + checked.stderr)
            kernel_certificates.append({"fixture": stem, "theorem_file": str(theorem_path),
                                        "theorem_sha256": hashlib.sha256(theorem_path.read_bytes()).hexdigest()})
            print(f"PASS kernel reduction of actual-byte certificate: {stem}", flush=True)
    final_hashes = {name: hashlib.sha256((root / "src" / name).read_bytes()).hexdigest() for name in source_names}
    if not args.receipt_executable and source_hashes != final_hashes:
        raise RuntimeError("injector sources changed while compiling/checking receipts; rerun after source freeze")
    manifest = {"fixtures": rows, "mutations": mutations, "lean_reference_encoding_checked": not args.skip_lean,
                "bodies": sum(row["bodies"] for row in rows),
                "claim": "Finite production receipt/module binding and independent encoding checks, not full Wasm semantics or C verification.",
                "kernel_certificates": kernel_certificates,
                "receipt_compiled_by_this_run": not bool(args.receipt_executable),
                "receipt_executable_sha256": hashlib.sha256(receipt.read_bytes()).hexdigest(),
                "source_sha256": source_hashes if not args.receipt_executable else None,
                "workspace_source_sha256_at_check": source_hashes,
                "checker_sha256": hashlib.sha256((source / "check_receipt.py").read_bytes()).hexdigest()}
    (build / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"PASS {len(rows)} module certificates, {manifest['bodies']} bodies, {len(mutations)} detected mutations; "
          f"Lean reference encoding {'checked' if not args.skip_lean else 'SKIPPED'}.")


if __name__ == "__main__":
    main()
