#!/usr/bin/env python3
# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Independently bind production-emitter receipts to original/output Wasm bytes.

This bounded parser/checker is trusted executable checking code, not a proved
Wasm validator or a proof of the C planner. Inputs must first pass a real Wasm
validator. Lean separately checks named-operation encodings in the normalized
certificate emitted here. No parser or placement code is imported from C.
"""
import argparse
import json
from pathlib import Path


class InvalidReceipt(ValueError):
    pass


def need(condition, message):
    if not condition:
        raise InvalidReceipt(message)


class Reader:
    def __init__(self, data, start=0, end=None):
        self.data = data
        self.pos = start
        self.end = len(data) if end is None else end

    def take(self, count):
        need(count >= 0 and self.pos + count <= self.end, "truncated Wasm input")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def byte(self):
        return self.take(1)[0]

    def u(self, bits=32):
        value = 0
        for i in range((bits + 6) // 7):
            byte = self.byte()
            value |= (byte & 127) << (7 * i)
            if not byte & 128:
                need(value < 1 << bits, "unsigned LEB overflow")
                return value
        raise InvalidReceipt("unterminated unsigned LEB")

    def s(self, bits):
        value = 0
        for i in range((bits + 6) // 7):
            byte = self.byte()
            value |= (byte & 127) << (7 * i)
            if not byte & 128:
                if byte & 64:
                    value -= 1 << (7 * (i + 1))
                need(-(1 << (bits - 1)) <= value < 1 << (bits - 1), "signed LEB overflow")
                return value
        raise InvalidReceipt("unterminated signed LEB")

    def name(self):
        return self.take(self.u()).decode("utf-8")

    def done(self):
        need(self.pos == self.end, "trailing bytes in decoded section")


def instruction(reader):
    start = reader.pos
    op = reader.byte()
    args = []
    if op in (0x02, 0x03, 0x04):
        args = [reader.s(33)]
    elif op in (0x0c, 0x0d, 0x10, 0x20, 0x21, 0x22, 0x23, 0x24, 0x3f, 0x40):
        args = [reader.u()]
    elif op == 0x0e:
        count = reader.u()
        need(count < reader.end - reader.pos, "invalid branch-table count")
        args = [reader.u() for _ in range(count + 1)]
    elif op == 0x11 or 0x28 <= op <= 0x3e:
        args = [reader.u(), reader.u()]
    elif op in (0x41, 0x42):
        args = [reader.s(32 if op == 0x41 else 64)]
    elif op in (0x43, 0x44):
        reader.take(4 if op == 0x43 else 8)
    elif op == 0xfc:
        sub = reader.u()
        need(sub <= 11, "unsupported bulk/numeric instruction")
        args = [sub]
        if sub in (8, 10):
            args += [reader.u(), reader.u()]
        elif sub in (9, 11):
            args += [reader.u()]
    else:
        need(op in (0, 1, 5, 11, 15, 26, 27) or 0x45 <= op <= 0xc4,
             f"unsupported instruction 0x{op:02x}")
    return {"offset": start, "end": reader.pos, "opcode": op, "args": args}


def limits(reader):
    flags = reader.u()
    need(flags in (0, 1), "unsupported memory/table limits")
    reader.u()
    if flags:
        reader.u()


def body(data, start, end):
    reader = Reader(data, start, end)
    groups = reader.u()
    count_bytes = reader.pos - start
    locals_count = 0
    for _ in range(groups):
        locals_count += reader.u()
        need(reader.byte() in (0x7f, 0x7e, 0x7d, 0x7c), "unsupported local type")
    local_end = reader.pos
    stack = [0]
    ops = []
    while reader.pos < end:
        item = instruction(reader)
        item["labels"] = tuple(stack)
        item["depth"] = len(stack)
        need(stack, "instruction after final function end")
        opcode = item["opcode"]
        if opcode in (0x0c, 0x0d, 0x0e):
            need(all(target < len(stack) for target in item["args"]), "invalid original branch target")
        if opcode in (2, 3, 4):
            stack.append(opcode)
        elif opcode == 5:
            need(stack[-1] == 4, "else outside if")
        elif opcode == 11:
            stack.pop()
        ops.append(item)
    need(not stack and ops and ops[-1]["opcode"] == 11, "unbalanced original function")
    return {"offset": start, "end": end, "data": data[start:end], "groups": groups, "countBytes": count_bytes,
            "locals": locals_count, "localEnd": local_end, "ops": ops,
            "byOffset": {item["offset"]: item for item in ops}}


def module(data):
    reader = Reader(data)
    need(reader.take(8) == b"\x00asm\x01\x00\x00\x00", "invalid Wasm header")
    result = {"data": data, "sections": [], "types": [], "functions": [],
              "importFunctions": [], "importGlobals": 0, "globals": [], "exports": [], "bodies": []}
    seen = set()
    while reader.pos < reader.end:
        ident = reader.byte()
        length = reader.u()
        start = reader.pos
        reader.take(length)
        need(ident <= 12 and (not ident or ident not in seen), "unsupported/duplicate section")
        seen.add(ident)
        result["sections"].append((ident, data[start:reader.pos]))
        section = Reader(data, start, reader.pos)
        if ident == 1:
            for _ in range(section.u()):
                need(section.byte() == 0x60, "unsupported function type")
                params = tuple(section.take(section.u()))
                returns = tuple(section.take(section.u()))
                result["types"].append((params, returns))
        elif ident == 2:
            for _ in range(section.u()):
                section.name(); section.name()
                kind = section.byte()
                if kind == 0:
                    result["importFunctions"].append(section.u())
                elif kind == 1:
                    section.byte(); limits(section)
                elif kind == 2:
                    limits(section)
                elif kind == 3:
                    section.take(2)
                    result["importGlobals"] += 1
                else:
                    raise InvalidReceipt("unsupported import kind")
        elif ident == 3:
            result["functions"] = [section.u() for _ in range(section.u())]
        elif ident == 6:
            for _ in range(section.u()):
                entry_start = section.pos
                valtype, mutable = section.byte(), section.byte()
                expression = []
                while True:
                    item = instruction(section)
                    expression.append((item["opcode"], item["args"]))
                    if item["opcode"] == 11:
                        break
                result["globals"].append({"raw": data[entry_start:section.pos], "type": valtype,
                                          "mutable": mutable, "expression": expression})
        elif ident == 7:
            for _ in range(section.u()):
                entry_start = section.pos
                name, kind, index = section.name(), section.byte(), section.u()
                result["exports"].append({"raw": data[entry_start:section.pos],
                                          "name": name, "kind": kind, "index": index})
        elif ident == 10:
            for _ in range(section.u()):
                size = section.u()
                function_start = section.pos
                section.take(size)
                result["bodies"].append(body(data, function_start, section.pos))
        else:
            section.pos = section.end
        section.done()
    need(len(result["functions"]) == len(result["bodies"]), "function/code counts differ")
    need(all(t < len(result["types"]) for t in result["functions"] + result["importFunctions"]),
         "unknown original function signature")
    return result


def fixed_functions(original):
    """Exact arbitrary-precision costs, using iterative DFS rather than C's queue."""
    imports = len(original["importFunctions"])
    count = len(original["functions"])
    children, costs, state = [], [None] * count, [0] * count
    referenced = set()
    for index, function in enumerate(original["bodies"]):
        calls = []
        linear = True
        for item in function["ops"]:
            opcode = item["opcode"]
            if opcode == 0x10:
                target = item["args"][0]
                need(target < imports + count, "invalid original direct call")
                if target < imports:
                    linear = False
                else:
                    calls.append(target - imports)
                    referenced.add(target - imports)
            elif opcode in (0, 2, 3, 4, 5, 12, 13, 14, 15, 17):
                linear = False
            elif opcode == 11 and item["end"] != function["end"]:
                linear = False
        children.append(calls)
        if not linear:
            state[index] = 2
    for root in range(count):
        if state[root]:
            continue
        state[root] = 1
        stack = [[root, 0, len(original["bodies"][root]["ops"])]]
        while stack:
            index, position, total = stack[-1]
            if position == len(children[index]):
                costs[index] = total if total <= 0xffffffff else None
                state[index] = 2
                stack.pop()
                continue
            child = children[index][position]
            if not state[child]:
                state[child] = 1
                stack.append([child, 0, len(original["bodies"][child]["ops"])])
            elif state[child] == 1 or costs[child] is None:
                state[index] = 2
                stack.pop()
            else:
                stack[-1][1] += 1
                stack[-1][2] += costs[child]
    clones = {}
    for tree in (False, True):
        for index in range(count):
            if index in referenced and costs[index] is not None and bool(children[index]) == tree:
                clones[imports + index] = imports + count + len(clones)
    return costs, children, clones


def receipt_bodies(records):
    result = []
    active = None
    for event in records:
        if event["event"] == "begin":
            need(active is None, "nested body receipt")
            active = {"begin": event, "ops": []}
        elif event["event"] == "op":
            need(active is not None, "operation outside body receipt")
            active["ops"].append(event)
        elif event["event"] == "end":
            need(active is not None, "end without body receipt")
            active["output"] = bytes(event["output"])
            result.append(active)
            active = None
        else:
            raise InvalidReceipt("unknown receipt event")
    need(active is None, "unterminated body receipt")
    return result


def check(original_bytes, output_bytes, records):
    original, output = module(original_bytes), module(output_bytes)
    modified = {3, 6, 7, 10}
    need([s for s in original["sections"] if s[0] not in modified] ==
         [s for s in output["sections"] if s[0] not in modified], "unrelated module sections changed")
    imports = len(original["importFunctions"])
    count = len(original["functions"])
    fuel_global = original["importGlobals"] + len(original["globals"])
    need(output["functions"][:count] == original["functions"], "original function signatures changed")
    need(len(output["globals"]) == len(original["globals"]) + 2, "private global count")
    need([g["raw"] for g in output["globals"][:-2]] == [g["raw"] for g in original["globals"]],
         "original global declarations changed")
    for value, valtype, initial in zip(output["globals"][-2:], (0x7e, 0x7f), ((1 << 63) - 1, 0)):
        need(value["type"] == valtype and value["mutable"] == 1 and
             value["expression"] == [(0x42 if valtype == 0x7e else 0x41, [initial]), (11, [])],
             "unexpected private global declaration/initializer")
    need(len(output["exports"]) == len(original["exports"]) + 2, "private export count")
    need([e["raw"] for e in output["exports"][:-2]] == [e["raw"] for e in original["exports"]],
         "original exports changed")
    for entry, name, index in zip(output["exports"][-2:],
                                  ("__redis_meter_fuel", "__redis_meter_exhausted"),
                                  (fuel_global, fuel_global + 1)):
        need((entry["name"], entry["kind"], entry["index"]) == (name, 3, index), "private export mismatch")
    costs, children, clones = fixed_functions(original)
    need(len(output["functions"]) == count + len(clones), "unexpected number of appended clones")
    bodies = receipt_bodies(records)
    need(len(bodies) == len(output["bodies"]), "missing/extra emitted body receipt")
    emitted_seen = set()
    normalized = []
    for record in bodies:
        context = record["begin"]
        source_index, emitted = context["original"], context["emitted"]
        need(imports <= source_index < imports + count, "receipt original function index")
        need(imports <= emitted < imports + len(output["bodies"]), "receipt emitted function index")
        need(emitted not in emitted_seen, "duplicate body receipt")
        emitted_seen.add(emitted)
        definition = source_index - imports
        source = original["bodies"][definition]
        actual = output["bodies"][emitted - imports]
        type_index = original["functions"][definition]
        clone_kind = context["cloneKind"]
        expected_kind = 0 if emitted == source_index else (2 if children[definition] else 1)
        need(clone_kind == expected_kind, "incorrect clone kind")
        if clone_kind:
            need(clones.get(source_index) == emitted, "clone ID does not map to this original function")
        else:
            need(emitted == source_index, "original function index changed")
        need(context["type"] == type_index == output["functions"][emitted - imports], "clone/original signature mismatch")
        expected_local = 0xffffffff if clone_kind else len(original["types"][type_index][0]) + source["locals"]
        need(context["global"] == fuel_global and context["local"] == expected_local, "private global/local index mismatch")
        need(context["offset"] == source["offset"] and context["length"] == len(source["data"]), "source body context mismatch")
        need(record["output"] == actual["data"], "body receipt differs from emitted module bytes")
        cursor, output_cursor = source["offset"], 0
        boundaries = {source["offset"], source["localEnd"], source["end"]}
        boundaries.update(item["offset"] for item in source["ops"])
        boundaries.update(item["end"] for item in source["ops"])
        ops = []
        for operation_index, event in enumerate(record["ops"]):
            kind, offset, length, args = event["kind"], event["sourceOffset"], event["sourceLength"], event["args"]
            need(0 <= kind <= 12 and isinstance(args, list), "unknown operation kind/arguments")
            need(source["offset"] <= offset <= source["end"] and length >= 0 and offset + length <= source["end"],
                 "operation source range outside original body")
            # Even a zero-length operation must belong to this exact position.
            # Its source anchor determines the enclosing control depth; accepting
            # an arbitrary anchor could turn an exhaustion branch into a branch
            # over guest instructions while retaining every source/output byte.
            need(offset == cursor, "operation source anchor does not match current source cursor")
            cursor += length
            produced = bytes(event["output"])
            need(event["outputOffset"] == output_cursor, "operation output spans do not partition emitted body")
            need(actual["data"][output_cursor:output_cursor + len(produced)] == produced,
                 "operation receipt differs from actual output bytes")
            output_cursor += len(produced)
            item = source["byOffset"].get(offset)
            if kind in (2, 4, 5, 6, 10, 11):
                need(length == 0, "private operation consumed original bytes")
            if clone_kind:
                need(kind in (0, 12), "meter operation inside unmetered clone")
            if kind == 0:
                need(length > 0 and not args and produced == original_bytes[offset:offset + length], "CopyOriginal changed source bytes")
                need(offset in boundaries and offset + length in boundaries,
                     "CopyOriginal split an instruction or locals-vector encoding")
                need(clone_kind or offset >= source["localEnd"], "original locals copied outside declaration operation")
                for inst in source["ops"]:
                    if offset <= inst["offset"] < offset + length:
                        if not clone_kind:
                            need(inst["opcode"] not in (12, 13, 14) and inst["end"] != source["end"],
                                 "branch/final end was copied instead of its named rewrite")
                        elif clone_kind == 2:
                            need(inst["opcode"] != 16, "tree clone retained an original direct target")
            elif kind == 1:
                need(not clone_kind and offset == source["offset"] and offset + length == source["localEnd"] and
                     args == [source["groups"], source["countBytes"]], "fuel-local declaration does not match original locals vector")
            elif kind == 3:
                need(len(args) == 4 and 0 <= args[0] <= 0xffffffff and args[1] in (-1, 0, 1)
                     and 0 <= args[3] <= 0xffffffff, "invalid charge arguments")
                need(item is not None, "charge anchor is not an original instruction")
                if args[1] == -1:
                    need(item["opcode"] == 12 and length == item["end"] - offset and
                         args[2] == item["args"][0] and item["labels"][-1 - args[2]] == 3,
                         "fused backedge charge does not match original loop branch")
                    need(operation_index + 1 < len(record["ops"]) and
                         record["ops"][operation_index + 1]["kind"] == 10,
                         "fused backedge charge is missing its immediate exhaustion jump")
                else:
                    need(length == 0 and args[2] == item["depth"] - 1 and args[3] == 0,
                         "private charge depth/source mismatch")
            elif kind == 6:
                need(item is not None and args == [item["depth"] - 1], "entry-check depth mismatch")
            elif kind == 7:
                need(item is not None and item["opcode"] in (12, 13, 14) and length == item["end"] - offset and
                     args == [item["opcode"], item["depth"]] + item["args"], "branch opcode/depth/targets mismatch")
            elif kind == 8:
                need(item is not None and item["opcode"] == 11 and item["depth"] == 1 and
                     offset + length == source["end"] and not args, "Return does not consume final original end")
            elif kind in (9, 12):
                need(item is not None and item["opcode"] == 16 and length == item["end"] - offset,
                     "clone operation does not consume an original direct call")
                target = item["args"][0]
                need(target in clones, "clone operation refers to ineligible callee")
                if kind == 9:
                    target_definition = target - imports
                    expected = [target, clones[target], original["functions"][target_definition],
                                costs[target_definition], int(bool(children[target_definition]))]
                    need(not clone_kind and args == expected, "CloneDispatch original/type/cost/kind/target mismatch")
                else:
                    need(clone_kind == 2 and args == [target, clones[target]], "CloneCall target mismatch")
            elif kind == 10:
                previous = ops[-1] if ops else None
                need(previous and previous["kind"] == 3 and previous["args"][1] == -1, "exhaustion jump without fused backedge")
                branch = source["byOffset"][source["offset"] + previous["sourceOffset"]]
                need(args == [branch["depth"] - 1], "exhaustion-jump depth mismatch")
            else:
                need(not args, "unexpected private-operation arguments")
            ops.append({"kind": kind, "sourceOffset": offset - source["offset"],
                        "sourceLength": length, "args": args, "outputOffset": event["outputOffset"],
                        "output": list(produced)})
        need(cursor == source["end"] and output_cursor == len(actual["data"]), "incomplete original/output partition")
        if not clone_kind:
            need([op["kind"] for op in ops[:4]] == [1, 5, 2, 6] and ops[-1]["kind"] == 11,
                 "missing original-body prologue/cold-exit structure")
            for structural in (1, 2, 8, 11):
                need(sum(op["kind"] == structural for op in ops) == 1,
                     "duplicate/missing declaration, wrapper, final return or cold exit")
        normalized.append({"input": list(source["data"]), "output": list(actual["data"]),
                           "fuelGlobal": fuel_global, "fuelLocal": expected_local, "ops": ops})
    need(emitted_seen == set(range(imports, imports + len(output["bodies"]))), "not every output function was audited")
    return {"bodies": normalized}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("injected", type=Path)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("certificate", type=Path)
    args = parser.parse_args()
    records = [json.loads(line) for line in args.receipt.read_text().splitlines() if line.strip()]
    certificate = check(args.original.read_bytes(), args.injected.read_bytes(), records)
    args.certificate.write_text(json.dumps(certificate, separators=(",", ":")) + "\n")
    print(f"PASS independent receipt check: {len(certificate['bodies'])} emitted bodies")


if __name__ == "__main__":
    main()
