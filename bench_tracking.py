#!/usr/bin/env python3
"""Correctness + statistical bench for server.tracking_pending_keys.

Exercises the exact path the list -> vec change targets: a RESP3 client
with CLIENT TRACKING on writes (from inside an EXEC, i.e. while a
command is executing) to keys it has previously read. Each such write
gets queued in server.tracking_pending_keys and drained as an RESP3
invalidation push when the command returns.

Requires: redis-py >= 5 (uses _RESP3Parser.set_invalidation_push_handler).

A/B usage:
    # one server built from baseline (list), one from the PR branch (vec):
    ./src/redis-server --port 6398 ...  # baseline binary
    ./src/redis-server --port 6399 ...  # candidate (PR) binary
    python3 bench_tracking.py --baseline-port 6398 --port 6399 --trials 5

Single-binary usage:
    python3 bench_tracking.py --port 6399
"""

import argparse
import random
import statistics
import sys
import time
from dataclasses import dataclass

import redis

# redis-py's pure-Python RESP3 parser recurses once per array element,
# so an EXEC reply for K queued SETs needs ~K stack frames. Default
# limit is 1000; raise so we can bench K up to a few thousand.
sys.setrecursionlimit(50000)


# ---------- Tracking client ----------

class TrackingClient:
    """A single-connection RESP3 redis client with CLIENT TRACKING on and
    a counter of invalidation push messages received.

    All commands are sent directly on the underlying Connection rather
    than through Redis.pipeline(). This is deliberate: redis-py's
    pipeline grabs a *different* connection from the pool (and ignores
    single_connection_client=True for pipelines), which means pipelined
    GETs would not be tracked for OUR connection, and our MULTI/EXEC
    writes would not match the tracking-during-executing-command path
    we're trying to exercise.

    Batching is done by writing multiple commands to the socket before
    reading any replies, which is what redis-py's pipeline does anyway —
    we just keep it on the same connection."""

    def __init__(self, host: str, port: int):
        self.r = redis.Redis(
            host=host, port=port, protocol=3,
            single_connection_client=True, decode_responses=False,
        )
        # Force the connection so _parser exists.
        self.r.ping()
        self.invalidations = 0

        def on_invalidation(response):
            # response is [b'invalidate', [b'k1', b'k2', ...]] or
            # [b'invalidate', None] for the NULL-sentinel "all keys" case.
            keys = response[1]
            self.invalidations += len(keys) if isinstance(keys, list) else 1

        self.conn = self.r.connection
        self.conn._parser.set_invalidation_push_handler(on_invalidation)
        self.r.execute_command("CLIENT", "TRACKING", "on")

    def flushdb(self):
        self.r.flushdb()

    def _exec_batch(self, commands):
        """Send a batch of commands on the tracking connection, then read
        N replies. Each command is a tuple/list of args."""
        for cmd in commands:
            self.conn.send_command(*cmd)
        replies = []
        for _ in commands:
            replies.append(self.conn.read_response())
        return replies

    def populate_and_track(self, n_keys: int) -> list:
        """SET then GET n_keys on the tracking connection. After this
        the server is tracking every one of them for this connection."""
        keys = [f"trkkey:{i}".encode() for i in range(n_keys)]
        BATCH = 1000
        for i in range(0, n_keys, BATCH):
            chunk = keys[i:i + BATCH]
            self._exec_batch([("SET", k, b"v0") for k in chunk])
        for i in range(0, n_keys, BATCH):
            chunk = keys[i:i + BATCH]
            self._exec_batch([("GET", k) for k in chunk])
        return keys

    def multi_exec_writes(self, sample):
        """MULTI + len(sample) SETs + EXEC, all over the wire as one batch,
        on the tracking connection (so writes-during-EXEC properly route
        through trackingInvalidateKey for THIS client)."""
        cmds = [("MULTI",)]
        for k in sample:
            cmds.append(("SET", k, b"v"))
        cmds.append(("EXEC",))
        self._exec_batch(cmds)

    def drain_pushes(self) -> int:
        """Force a roundtrip so the parser definitely reads any pending
        invalidation pushes. Returns the number absorbed during this drain."""
        before = self.invalidations
        self.r.ping()
        return self.invalidations - before

    def used_memory(self) -> int:
        return self.r.info("memory")["used_memory"]

    def close(self):
        try:
            self.r.close()
        except Exception:
            pass


# ---------- Correctness gate ----------
#
# trackingInvalidateKey() removes a key from the tracking table once it
# emits the invalidation, so re-writing the SAME key produces NO further
# invalidations. For a correct count, each EXEC must write a disjoint
# window of keys. We do that by pre-tracking writes*iters keys and
# rotating through them.

def correctness_one(host: str, port: int, writes: int, iters: int, label: str):
    c = TrackingClient(host, port)
    try:
        c.flushdb()
        keys = c.populate_and_track(writes * iters)
        c.drain_pushes()
        before = c.invalidations
        for i in range(iters):
            window = keys[i * writes:(i + 1) * writes]
            c.multi_exec_writes(window)
        c.drain_pushes()
        delivered = c.invalidations - before
        expected = writes * iters
        if delivered != expected:
            raise SystemExit(
                f"FAIL [{label}]: expected {expected} invalidations "
                f"({writes} writes/EXEC * {iters} iters, disjoint), "
                f"got {delivered}"
            )
        print(f"  OK  [{label:>9}]: writes/EXEC={writes:>3} iters={iters:>4} "
              f"invalidations={delivered}")
    finally:
        c.close()


def correctness_null_sentinel(host: str, port: int):
    """FLUSHDB invalidates everything as a single NULL-sentinel push."""
    c = TrackingClient(host, port)
    try:
        c.flushdb()
        c.populate_and_track(8)
        c.drain_pushes()
        before = c.invalidations
        c.r.flushdb()
        c.drain_pushes()
        delivered = c.invalidations - before
        if delivered == 0:
            raise SystemExit("FAIL [flushdb]: no invalidation push received "
                             "after FLUSHDB on tracked keys")
        print(f"  OK  [flushdb ]: FLUSHDB produced {delivered} invalidation push(es) "
              f"(NULL-sentinel path)")
    finally:
        c.close()


def correctness_gate(host: str, port: int):
    print(f"\n=== Correctness gate against {host}:{port} ===")
    correctness_one(host, port, writes=1,  iters=200, label="tiny")
    correctness_one(host, port, writes=16, iters=200, label="boundary")
    correctness_one(host, port, writes=64, iters=200, label="overflow")
    correctness_null_sentinel(host, port)


# ---------- Perf bench ----------

@dataclass
class Trial:
    elapsed_s: float
    execs_per_sec: float
    invalidations_per_sec: float
    bytes_per_tracked_write: float


def bench_one_trial(host: str, port: int, writes: int, iters: int) -> Trial:
    c = TrackingClient(host, port)
    try:
        c.flushdb()
        keys = c.populate_and_track(writes * iters)

        # Warmup: amortize allocator, branch predictors, and any first-call
        # JIT inside redis-py.
        warmup = min(20, iters // 10)
        for i in range(warmup):
            c.multi_exec_writes(keys[i * writes:(i + 1) * writes])
        c.drain_pushes()

        mem_before = c.used_memory()
        inv_before = c.invalidations

        t0 = time.perf_counter()
        for i in range(warmup, iters):
            c.multi_exec_writes(keys[i * writes:(i + 1) * writes])
        c.drain_pushes()
        elapsed = time.perf_counter() - t0

        mem_after = c.used_memory()
        delivered = c.invalidations - inv_before
        timed_iters = iters - warmup
        tracked_writes = timed_iters * writes

        return Trial(
            elapsed_s=elapsed,
            execs_per_sec=timed_iters / elapsed,
            invalidations_per_sec=delivered / elapsed,
            bytes_per_tracked_write=(mem_after - mem_before) / max(tracked_writes, 1),
        )
    finally:
        c.close()


def stat(values):
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "stddev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def run_workload(host: str, port: int, label: str, writes: int, iters: int,
                 trials: int) -> dict:
    print(f"\n--- {label}: writes/EXEC={writes} iters={iters} trials={trials} "
          f"target={host}:{port} ---")
    runs = []
    for t in range(trials):
        s = bench_one_trial(host, port, writes, iters)
        runs.append(s)
        print(f"  trial {t+1:>2}: {s.execs_per_sec:>10,.1f} EXEC/s,  "
              f"{s.invalidations_per_sec:>10,.1f} inv/s,  "
              f"used_memory delta = {s.bytes_per_tracked_write:>6.1f} B / tracked write")
    return {
        "label": label, "writes": writes, "iters": iters,
        "execs_per_sec": stat([r.execs_per_sec for r in runs]),
        "invalidations_per_sec": stat([r.invalidations_per_sec for r in runs]),
        "bytes_per_tracked_write": stat([r.bytes_per_tracked_write for r in runs]),
    }


def print_workload(w):
    e = w["execs_per_sec"]; i = w["invalidations_per_sec"]; b = w["bytes_per_tracked_write"]
    print(f"\n  [{w['label']}]")
    print(f"    EXEC/s     : median {e['median']:>12,.1f}  "
          f"min {e['min']:>12,.1f}  max {e['max']:>12,.1f}  stddev {e['stddev']:>10,.1f}")
    print(f"    inv/s      : median {i['median']:>12,.1f}  "
          f"min {i['min']:>12,.1f}  max {i['max']:>12,.1f}  stddev {i['stddev']:>10,.1f}")
    print(f"    B/track-wr : median {b['median']:>12.2f}  "
          f"min {b['min']:>12.2f}  max {b['max']:>12.2f}  stddev {b['stddev']:>10.2f}")


def fmt_pct(a: float, b: float) -> str:
    if b == 0:
        return "  n/a  "
    delta = (a - b) / b * 100.0
    sign = "+" if delta >= 0 else ""
    return f"{sign}{delta:6.2f}%"


def print_ab(c, b):
    print(f"\n  [{c['label']}]")
    for key, pretty, dir_note in [
        ("execs_per_sec",          "EXEC/s    ", " (higher is better)"),
        ("invalidations_per_sec",  "inv/s     ", " (higher is better)"),
        ("bytes_per_tracked_write","B/track-wr", " (lower is better)"),
    ]:
        cv = c[key]["median"]; bv = b[key]["median"]
        print(f"    {pretty}  baseline median {bv:>12,.2f}  "
              f"candidate median {cv:>12,.2f}  delta {fmt_pct(cv, bv)}{dir_note}")


# ---------- main ----------

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6379,
                   help="Candidate (PR) server port.")
    p.add_argument("--baseline-port", type=int, default=0,
                   help="If non-zero, also run against this baseline port and print A/B.")
    p.add_argument("--trials", type=int, default=5,
                   help="Independent trials per workload (default 5).")
    p.add_argument("--iters", type=int, default=2000,
                   help="MULTI/EXEC iterations per trial (default 2000).")
    p.add_argument("--writes-list", default="1,16,64,256",
                   help="Comma-separated list of 'writes-per-EXEC' sizes to "
                        "bench. Each size N needs N*iters keys pre-tracked "
                        "per trial; tune --iters down for very large N.")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--skip-correctness", action="store_true",
                   help="Skip the correctness gate (perf only).")
    args = p.parse_args()
    random.seed(args.seed)

    if not args.skip_correctness:
        correctness_gate(args.host, args.port)
        if args.baseline_port:
            correctness_gate(args.host, args.baseline_port)

    # Workload sizes are user-tunable via --writes-list.
    sizes = [int(s) for s in args.writes_list.split(",")]
    workloads = [(f"w{n}", n) for n in sizes]

    cand_results, base_results = [], []
    for label, writes in workloads:
        cand_results.append(run_workload(args.host, args.port, label,
                                         writes, args.iters, args.trials))
        if args.baseline_port:
            base_results.append(run_workload(args.host, args.baseline_port,
                                             f"{label}/baseline",
                                             writes, args.iters, args.trials))

    print(f"\n=== Candidate ({args.host}:{args.port}) summary ===")
    for w in cand_results:
        print_workload(w)

    if args.baseline_port:
        print(f"\n=== Baseline ({args.host}:{args.baseline_port}) summary ===")
        for w in base_results:
            print_workload(w)
        print(f"\n=== A/B (candidate vs baseline) ===")
        for c, b in zip(cand_results, base_results):
            print_ab(c, b)


if __name__ == "__main__":
    main()
