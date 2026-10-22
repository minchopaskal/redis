#!/usr/bin/env python3
# Copyright (c) 2026 Redis Ltd. Licensed under RSALv2, SSPLv1, or AGPLv3.
"""Build actual no-call CFG certificates from validated production receipts.

This is a trusted Wasm-to-model adapter, not a formal Wasm decoder. It supports
structured branches/loops and original operations, but excludes every body with
a direct or indirect call. Call/clone semantics are not assumed away. Named
emission macros are checked against actual output bytes by the receipt checker
and the Lean reference encoder, both invoked before any CFG check.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import check_receipt as receipt


def node(kind, op=(), successors=(), action=0, amount=0):
    return dict(kind=kind, op=list(op), successors=list(successors), action=action, amount=amount)


def source_graph(parsed):
    """Successor positions: if/br_if [taken, fallthrough], br_table in vector order."""
    ops, data = parsed['ops'], parsed['data']
    count = len(ops)
    stack, ends, alternatives, owner, labels = [-1], {}, {}, {}, []
    for index, op in enumerate(ops):
        labels.append(tuple(stack))
        code = op['opcode']
        if code in (2, 3, 4):
            stack.append(index)
        elif code == 5:
            receipt.need(stack[-1] >= 0 and ops[stack[-1]]['opcode'] == 4, 'else owner')
            alternatives[stack[-1]] = index
            owner[index] = stack[-1]
        elif code == 11:
            start = stack.pop()
            ends[start] = index
    receipt.need(not stack and ends[-1] == count - 1, 'source CFG is not a complete function')

    def destination(index, depth):
        target = labels[index][-1 - depth]
        if target == -1:
            return count
        return target + 1 if ops[target]['opcode'] == 3 else ends[target] + 1

    source = []
    for index, op in enumerate(ops):
        code = op['opcode']
        if code == 4:
            successors = [index + 1, alternatives[index] + 1 if index in alternatives else ends[index] + 1]
        elif code == 5:
            successors = [ends[owner[index]] + 1]
        elif code == 12:
            successors = [destination(index, op['args'][0])]
        elif code == 13:
            successors = [destination(index, op['args'][0]), index + 1]
        elif code == 14:
            successors = [destination(index, depth) for depth in op['args']]
        elif code in (0, 15):
            # The guest semantics gives unreachable its ordinary trap outcome;
            # return and the final end give the function's terminal outcome.
            successors = [count]
        else:
            successors = [index + 1]
        source.append(node(0, data[op['offset']:op['end']], successors))
    source.append(node(3))
    return source


def build_graph(body):
    data = bytes(body['input'])
    parsed = receipt.body(data, 0, len(data))
    if any(op['opcode'] in (16, 17) for op in parsed['ops']):
        return None, 'contains direct or indirect calls'
    source = source_graph(parsed)
    target, origins, positions = [], [], []
    emitted = 0

    def guest(index):
        nonlocal emitted
        receipt.need(index == emitted, 'target guest operations do not retain original order')
        positions.append(len(target))
        target.append(node(0, source[index]['op']))
        origins.append(index)
        emitted += 1

    def meter(action, amount=0):
        target.append(node(1, action=action, amount=amount))
        origins.append(emitted)

    by_offset = {op['offset']: index for index, op in enumerate(parsed['ops'])}
    for operation in body['ops']:
        kind, args = operation['kind'], operation['args']
        start, length = operation['sourceOffset'], operation['sourceLength']
        if kind == 0:
            end = start + length
            for index, original in enumerate(parsed['ops']):
                if start <= original['offset'] < end:
                    receipt.need(original['end'] <= end, 'copy cuts an original instruction')
                    guest(index)
        elif kind == 3:
            cost, check, _depth, prepaid = args
            meter(0, cost)
            if check == 1:
                meter(1)
            elif check == -1:
                meter(5, prepaid)
                guest(by_offset[start])
            else:
                receipt.need(check == 0, 'invalid charge check')
        elif kind == 4:
            meter(3)
        elif kind == 5:
            meter(4)
        elif kind == 6:
            meter(2)
        elif kind in (7, 8):
            guest(by_offset[start])
        elif kind in (1, 2, 10, 11):
            # Local declaration and wrapper are administrative. The cold
            # handler is represented by failure of the checkpoint, not by an
            # original guest trap. Its exact bytes/context are checked outside.
            pass
        else:
            raise receipt.InvalidReceipt('unexpected call macro in no-call CFG')
    receipt.need(emitted == len(parsed['ops']), 'missing source operation in target CFG')
    halt = len(target)
    target.append(node(3))
    origins.append(emitted)

    # Jump to the actual position immediately after the preceding source
    # delimiter, including any private prefix there. Loop branches target the
    # position after the loop opcode; block branches skip the original end.
    entry = [0] + [position + 1 for position in positions[:-1]] + [halt]
    for index, position in enumerate(positions):
        target[position]['successors'] = [entry[next_pc] for next_pc in source[index]['successors']]
    for index, item in enumerate(target):
        if item['kind'] == 1:
            item['successors'] = [index + 1]

    ranks = [0] * len(target)
    for index in range(len(target) - 1, -1, -1):
        if target[index]['kind'] == 1:
            ranks[index] = ranks[index + 1] + 1
    return dict(source=source, target=target, origins=origins, ranks=ranks), None


def lean_list(values):
    return '[' + ','.join(map(str, values)) + ']'


def lean_node(item):
    return ('⟨' + str(item['kind']) + ',' + lean_list(item['op']) + ',' +
            lean_list(item['successors']) + ',' + str(item['action']) + ',' +
            str(item['amount']) + '⟩')


def lean_graph(graph):
    source = '[' + ','.join(map(lean_node, graph['source'])) + ']'
    target = '[' + ','.join(map(lean_node, graph['target'])) + ']'
    return '⟨' + source + ',' + target + ',' + lean_list(graph['origins']) + ',' + lean_list(graph['ranks']) + '⟩'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('injected', type=Path)
    parser.add_argument('receipt', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--lake', default='lake')
    parser.add_argument('--validator', default='wasm-validate')
    parser.add_argument('--kernel', action='store_true', help='also kernel-check generated decide proofs')
    parser.add_argument('--max-kernel-ops', type=int, default=256,
                        help='larger graphs retain executable checks and are explicitly excluded from kernel reduction')
    args = parser.parse_args()
    runs = []
    for path in (args.original, args.injected):
        command = [args.validator, str(path.resolve())]
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        runs.append(dict(command=command, returncode=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
        if result.returncode:
            print(result.stdout, end='')
            print(result.stderr, end='')
            raise SystemExit(result.returncode)
    records = [json.loads(line) for line in args.receipt.read_text().splitlines() if line.strip()]
    normalized = receipt.check(args.original.read_bytes(), args.injected.read_bytes(), records)
    contexts = [record['begin'] for record in receipt.receipt_bodies(records)]
    args.output.mkdir(parents=True, exist_ok=True)
    byte_certificate_path = args.output / 'bytes.json'
    byte_certificate_path.write_text(json.dumps(normalized, separators=(',', ':')) + '\n')
    graphs, covered, excluded, kernel = [], [], [], []
    for body_index, (body, context) in enumerate(zip(normalized['bodies'], contexts)):
        graph, reason = build_graph(body)
        identity = dict(body=body_index, original=context['original'], emitted=context['emitted'],
                        cloneKind=context['cloneKind'])
        if reason:
            excluded.append(dict(identity, reason=reason))
            continue
        graph_index = len(graphs)
        graphs.append(graph)
        entry = dict(identity, graph=graph_index, originalOperations=len(graph['source']) - 1,
                     targetNodes=len(graph['target']))
        if entry['originalOperations'] <= args.max_kernel_ops:
            kernel.append(graph_index)
            entry['kernelSelected'] = True
        else:
            entry['kernelSelected'] = False
            entry['kernelExclusion'] = 'exceeds explicit kernel reduction size bound'
        covered.append(entry)
    certificate_path = args.output / 'graphs.json'
    certificate_path.write_text(json.dumps(dict(graphs=graphs), separators=(',', ':')) + '\n')
    lines = ['import GraphAudit', 'set_option maxRecDepth 100000', 'set_option maxHeartbeats 0',
             'namespace ActualModuleCFG', 'open WasmMeterProof.GraphAudit']
    for index in kernel:
        lines.append(f'def graph_{index} : GraphRecord := {lean_graph(graphs[index])}')
        lines.append(f'theorem graph_{index}_checked : checkRecord graph_{index} = true := by decide')
    if kernel:
        lines.append(f'#print axioms graph_{kernel[0]}_checked')
    lines.append('end ActualModuleCFG')
    lean_path = args.output / 'ActualModuleCFG.lean'
    lean_path.write_text('\n'.join(lines) + '\n')
    proof_dir = Path(__file__).resolve().parent
    commands = [[str(proof_dir / '.lake/build/bin/audit'), str(byte_certificate_path.resolve())],
                [str(proof_dir / '.lake/build/bin/graphAudit'), str(certificate_path.resolve())]]
    if args.kernel:
        commands.append([args.lake, 'env', 'lean', str(lean_path.resolve())])
    for command in commands:
        result = subprocess.run(command, cwd=proof_dir, capture_output=True, text=True, timeout=300)
        runs.append(dict(command=command, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr))
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            break
    report = dict(scope='Actual no-call function CFGs; Wasm decoding/macro semantics remain trusted',
                  files={str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in (args.original, args.injected, args.receipt, byte_certificate_path,
                                      certificate_path, lean_path)},
                  checkerSources={path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in [Path(__file__).resolve(), proof_dir / 'check_receipt.py',
                                               *sorted(proof_dir.glob('*.lean'))]},
                  covered=covered, excluded=excluded, kernelRequested=args.kernel,
                  kernelSelected=len(kernel), commands=runs)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'CFG coverage: {len(covered)} bodies; {len(excluded)} call-containing bodies excluded; '
          f'{len(kernel)} selected for kernel reduction')
    if any(run['returncode'] for run in runs):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
