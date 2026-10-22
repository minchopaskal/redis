#!/usr/bin/env python3
# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Turn a small real emitter certificate into a Lean kernel-checkable theorem.

This generator is untrusted: Lean checks the generated literal and proof. The
independent receipt checker must first bind the certificate to the actual module
files. No native_decide, external oracle, or hand-authored expected bytes are used.
"""
import argparse
import json
from pathlib import Path


def numbers(values):
    if any(type(value) is not int for value in values):
        raise ValueError("certificate arrays must contain integers")
    return "[" + ", ".join(str(value) for value in values) + "]"


def operation(op):
    fields = []
    for key in ("kind", "sourceOffset", "sourceLength", "outputOffset"):
        value = op[key]
        if type(value) is not int or value < 0:
            raise ValueError(f"invalid {key}")
        fields.append(f"{key} := {value}")
    fields.extend(f"{key} := {numbers(op[key])}" for key in ("args", "output"))
    return "{ " + ", ".join(fields) + " }"


def body_literal(body):
    fields = [f"{key} := {numbers(body[key])}" for key in ("input", "output")]
    for key in ("fuelGlobal", "fuelLocal"):
        value = body[key]
        if type(value) is not int or value < 0:
            raise ValueError(f"invalid {key}")
        fields.append(f"{key} := {value}")
    fields.append("ops := [\n    " + ",\n    ".join(map(operation, body["ops"])) + "]")
    return "{ " + ",\n  ".join(fields) + " }"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("certificate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--max-bytes", type=int, default=12000)
    args = parser.parse_args()
    certificate = json.loads(args.certificate.read_text())
    bodies = certificate["bodies"]
    size = sum(len(body["input"]) + len(body["output"]) for body in bodies)
    if not bodies or size > args.max_bytes:
        parser.error(f"use a small nonempty certificate (got {size} body bytes)")
    literal = ",\n".join(map(body_literal, bodies))
    args.output.write_text(
        "import Audit\n\n"
        "-- Generated from actual production-emitter receipts; do not edit.\n"
        "set_option maxRecDepth 100000\n"
        "set_option maxHeartbeats 8000000\n"
        "open WasmMeterProof.Audit\n\n"
        f"def actualCertificate : Certificate := {{ bodies := [\n{literal}\n] }}\n\n"
        "theorem actual_bytes_checked : checkCertificate actualCertificate = true := by\n"
        "  decide\n\n"
        "theorem actual_body_reference_encoding (body : Body)\n"
        "    (member : body ∈ actualCertificate.bodies) :\n"
        "    assemble body body.ops 0 0 = some body.output := by\n"
        "  exact checked_body_is_reference_encoding\n"
        "    (checked_certificate_body actual_bytes_checked member)\n\n"
        "theorem actual_body_original_recovered (body : Body)\n"
        "    (member : body ∈ actualCertificate.bodies) :\n"
        "    recoveredSource body = body.input := by\n"
        "  exact checked_body_recovers_original\n"
        "    (checked_certificate_body actual_bytes_checked member)\n\n"
        "#print axioms actual_bytes_checked\n"
        "#print axioms actual_body_original_recovered\n"
    )
    print(f"Generated kernel proof for {len(bodies)} actual bodies ({size} bytes)")


if __name__ == "__main__":
    main()
