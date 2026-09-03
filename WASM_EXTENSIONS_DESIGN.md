# WASM Extensions for Redis

> Items marked **OPEN** are undecided and need a call before implementation.

## Proof-of-concept decisions

The initial implementation is intentionally narrower than the complete design:

- Build with `BUILD_WASM=yes`. WAMR 2.4.5 is vendored in `deps/wamr` and built
  as a classic-interpreter-only static library. The runtime uses a 64 MiB
  Redis-owned pool, each module is capped at 128 64-KiB pages (8 MiB), load
  execution is capped at 10 million instructions, and calls at 100 million.
- A module must export `memory`, `redis_abi_version() -> i32` (returning `1`),
  and `redis_init() -> i32` (returning `0` on success). Registered function
  exports have the signature `() -> i32`, where zero means success. If a
  module exports `_initialize() -> void` (as TinyGo reactor modules do), Redis
  invokes it once before the ABI and registration exports.
- Packed input and command buffers are little-endian: a `u32` item count,
  followed by repeated `u32 byte_length` and raw byte payload pairs. Invocation
  input contains keys first, followed by arguments; `keys_count()` identifies
  the boundary.
- `parse_resp` writes 16-byte little-endian entries containing `u32 tag`,
  `u32 source_offset`, `u32 source_length`, and `u32 child_count`. Entries are
  depth-first. Scalar offsets and lengths select their payload; aggregate
  offsets and lengths select the complete encoded aggregate.
- Guest RESP is fully bounds-checked and validated. RESP3-only raw replies are
  rejected for RESP2 callers.
- Function flags, functional `FCALL_RO`, and the `blob_*`/`OBJ_WASM` data type
  are deferred. Consequently, the current flagless WASM functions are rejected
  by `FCALL_RO`.
- Function libraries retain their original binary payload through RDB and
  `FUNCTION DUMP`/`RESTORE`. A replica loading such a library must also have
  `BUILD_WASM=yes`; the ordinary command effects produced by an invocation do
  not require WASM to replay.
- A proof-of-concept TinyGo SDK and executable example live under
  `sdk/wasm/go`.

## 1. Integration

WASM is added as a second engine in the Redis Functions framework, alongside `LUA`. An extension is a WASM module loaded with `FUNCTION LOAD` using a `#!wasm` shebang followed by the module binary, and its functions are invoked with `FCALL` and `FCALL_RO`.

Building on Functions rather than a new command family means the following already exist and do not need to be designed or written:

- **ACL enforcement per command.** Every command a guest issues is checked against the calling user's permissions, and denials are recorded in the ACL log.
- **Cluster slot checks** on the keys a guest touches, including the cross-slot rule.
- **Pre-invocation gating:** OOM state, read-only replica, `-MISCONF` disk errors, stale replica, min-replicas-to-write.
- **The management surface:** `FUNCTION LIST`, `FUNCTION DELETE`, `FUNCTION FLUSH`, `FUNCTION STATS`, `FUNCTION DUMP` / `RESTORE`.
- **Library persistence and distribution:** libraries are written to RDB with their source payload and recompiled on load, and replicas receive them as part of a normal sync.
- **Recursion protection:** `EVAL`, `FCALL` and `FUNCTION LOAD` are already flagged `NOSCRIPT`, so a guest cannot re-enter the engine.
- **Observability:** the outer `FCALL` and every command the guest issues appear in `commandstats`, `errorstats`, `slowlog` and latency tracking.

At load the host instantiates the module once in a registration-only context and calls its exported `redis_init`, which declares each callable function via `register_function`. The host resolves those export names once at load, so `FCALL` dispatch is a direct call into the resolved export. WASM functions declare no script flags in v1 (there is no `no-writes` / `allow-oom` equivalent yet).

**OPEN:** whether the guest must export an ABI version that the host validates at load.

## 2. Bare-minimum host API

Imports live in the `redis` namespace. All parameters are `i32`. Buffers are `(ptr, len)` pairs into the guest's exported linear memory, named `memory`, and the host bounds-checks every access; an out-of-range pair traps rather than being clamped.

```
;; imports, namespace "redis"
log(level, ptr, len)

input_len() -> i32                       ;; size of the packed keys+args blob
input_read(dst, cap) -> i32
keys_count() -> i32

call(argv_ptr, argv_len) -> i32          ;; >=0 = staged reply length, -1 = rejected
reply_read(dst, cap, offset) -> i32
last_error_read(dst, cap) -> i32
parse_resp(src, src_len, out, out_cap) -> i32

reply_raw(ptr, len) -> i32               ;; complete RESP3 reply, validated by host
status_reply(ptr, len)
error_reply(ptr, len)

register_function(name, nlen, export, elen) -> i32   ;; load time only
blob_register(<type identity: OPEN>) -> i32          ;; load time only
blob_len(key, klen, type) -> i32
blob_read(key, klen, type, dst, cap, offset) -> i32
blob_write(key, klen, type, src, slen) -> i32

;; guest exports
memory                                   ;; exported linear memory
redis_init()                             ;; registration only
<one export per registered function>() -> i32
```

**Input.** The keys and arguments of the invocation are exposed as one packed, length-prefixed blob: the keys first, `keys_count()` of them, then the remaining arguments. The guest sizes it with `input_len()` and copies it with `input_read()`.

**Calling commands.** The guest packs an argv into its own memory and calls `call()`. A result `>= 0` means the command ran and its RESP3 reply is staged host-side, with the value being the reply length in bytes; the guest then copies the reply out with `reply_read()`, whole or in chunks. A result of `-1` means the host refused to run the command at all — unknown command, wrong arity, ACL denial, OOM, cluster violation, write on a read-only replica — and the reason is readable with `last_error_read()`. Errors produced *by* a command that did run, such as `WRONGTYPE`, are not `-1`; they arrive as a RESP error inside the staged reply. Every `*_read` import returns the number of bytes written, or the negated required size when the destination is too small, so one probe is enough to size a buffer.

The host never calls back into the guest while servicing an import, and never allocates inside guest memory. That is the reason for the stage-then-read pattern rather than having the host write the reply into a guest buffer of its own choosing.

**Parsing replies.** `parse_resp()` is a host-side tokenizer: it walks a staged RESP reply and writes a flat, depth-first array of entries — tag, offset into the source, length, and child count for aggregates — that an SDK can traverse with a cursor and no allocation. This keeps a RESP parser out of every per-language SDK, which is the answer to the RESP-parsing overhead the API's flat-argv/raw-reply shape implies.

**Producing a reply.** A guest builds a complete RESP3 reply in its own memory and hands it over with `reply_raw()`. The host validates the buffer before any of it reaches the client — framing, declared lengths, aggregate arity, nesting depth — because malformed RESP from a guest would desynchronize the client connection. `status_reply()` and `error_reply()` are shorthands for the two common single-line cases.

**Failures.** A guest that traps — out-of-bounds access, `unreachable`, stack exhaustion — or returns a non-zero result gets an error reply that names the function and the trap reason, plus a server log line.

## 3. Blob data type

An extension can store opaque linear buffers in the keyspace. A blob value is a new object type, `OBJ_WASM`, holding the type, the owning extension and the payload:

```
key -> robj(OBJ_WASM) -> { type, owner, payload }
```

Redis never interprets the payload. The guest serializes and deserializes its own structure; Redis stores, persists, replicates, digests and evicts bytes.

**Encapsulation.** Each blob is tagged with the id of the extension that created it, and only that extension can access it. Every `blob_`* call verifies that the calling extension is the owner and otherwise fails with a permission error. There is no sharing mechanism in v1, so adding one later means introducing an explicit grant rather than relaxing a check. Note the boundary this draws: extensions are isolated from each other, but an authorized client can still `DUMP` the key, which remains an ACL question.

**Access.** `blob_len()` returns the payload size, `blob_read()` copies out from a given offset, and `blob_write()` replaces the whole value, creating the key if it does not exist. Deletion, expiry and existence checks go through ordinary commands via `call()`, so there are no duplicate imports for them. A partial, offset-based write is a candidate follow-up; v1 rewrites the whole value.

Because the payload is opaque, everything Redis needs to do with the value is a generic byte operation — free it, copy it, size it, digest it, write it to RDB, read it back. No guest code runs on any of those paths, which means no WASM executes in the RDB-saving fork child, during AOF rewrite, during active defrag, or during eviction and lazy-free.

**OPEN — extension id.** What identifies the owning extension concretely. The library name from the shebang is the obvious candidate, since `FUNCTION LOAD` and RDB already persist it and it therefore survives a restart, but an assigned integer or a module hash are alternatives with different trade-offs on rename and recompile.

**OPEN — type identity.** Whether an extension registers named blob types (and if so, the name format and whether an encoding version is part of the identity) or gets a single implicit type. This decision fixes the signature of `blob_register` and the `type` parameter of the other blob imports, and it determines the type field in the RDB record below.

## 4. Persistence and replication

**RDB.** A blob is written as a self-describing record holding the type, the owner, the encoding version and the payload. Because all four are in the record, a server can load, re-save, `DUMP` and `RESTORE` such a key with the extension absent, so data durability does not depend on an operator keeping a library loaded. The cost is a new RDB record type and an `RDB_VERSION` bump, which means older servers cannot read RDBs containing blobs. The exact type and version fields follow from the type-identity decision above.

**Replication and AOF: effects.** A WASM invocation replicates its effects, exactly as a Lua script does. Each command the guest issues through `call()` propagates on its own, and Redis wraps the batch in `MULTI`/`EXEC` when a single invocation produced more than one operation; the `FCALL` itself is never propagated. The engine does not have to implement any of this: `scriptResetRun()` already marks the calling client `CLIENT_PREVENT_PROP`, and each write a script performs accumulates through `alsoPropagate()` inside `call()`, flushed by `propagatePendingCommands()`.

Two properties follow. Guests do not need to be deterministic, since nothing re-executes them. And replicas and AOF-loading servers never run WASM — they apply ordinary commands — so a server without the engine compiled in can still replicate from a master that uses it.

**OPEN — how blob writes propagate.** A `blob_write` is not a command, so effects replication has nothing to propagate on its behalf. This needs a decision before implementation.

**Other integration points.**

- **Keyspace notifications.** A blob write must fire a notification, so that clients watching a key see the change like any other write.
- **WATCH and client-side caching.** A blob write signals the key as modified, so watching clients are invalidated and tracking clients get an invalidation message.
- `DEBUG DIGEST`**.** The digest covers the payload bytes directly, so master and replica digests agree as long as blob writes reach the replica intact.
- `MEMORY USAGE` **and eviction.** The reported size is the payload allocation, and blobs are ordinary values for `maxmemory`, LRU and LFU purposes.
- `TYPE` **and** `OBJECT ENCODING`**.** Both must return something meaningful for `OBJ_WASM`. **OPEN:** the exact strings, which depend on the type-identity decision.
- **Expiry and lazy-free.** Blobs take TTLs through the ordinary expiry commands. A payload is a single allocation, so freeing is cheap and needs no special effort estimate.

## 5. Runtime

The discussion left three options: wasmer, WAMR, or our own implementation. **We propose integrating WAMR.**

WAMR is plain C and adds nothing to the build toolchain, whereas wasmer's C API has to be built through Rust. The decisive difference is memory: WAMR can cap a module instance's linear memory (`max_memory_pages`, passed through `wasm_runtime_instantiate_ex`) and can be told to take all of its own allocations from a buffer we hand it, so an extension cannot allocate outside the server's memory budget. wasmer's C API has no equivalent — `wasm_limits_t` constrains only memories the host creates or imports, and capping a module's own memory needs custom Rust `Tunables`. Both runtimes can bound how long a guest runs from C: WAMR through `wasm_runtime_set_instruction_count_limit`, wasmer through its metering middleware, which its C API does expose, on a surface upstream marks unstable. Licensing does not separate them (Apache-2.0 with LLVM exception against MIT). Writing our own runtime would mean owning WASM validation, spec conformance and sandbox security indefinitely, which is not worth doing while a C runtime fits.

Two consequences of choosing WAMR are worth stating up front. Its instruction metering works only in the classic interpreter — not the fast interpreter, Fast JIT or AOT — so v1 runs guests in the slowest mode in exchange for being able to bound them at all, and moving to a faster tier later means adding metering there ourselves, since `wasm_runtime_terminate` is disabled upstream for Fast JIT. And neither WAMR nor wasmer can resume a call once its budget is exhausted: both trap and unwind, so a guest that overruns is terminated rather than paused. Lua's behaviour, where a slow script keeps running while other clients receive `-BUSY` until it is killed, would need a local patch to the interpreter's dispatch loop.

## Appendix — Extended API (deferred)

Recorded for future discussion; not part of this phase.

Notable problem here is how do we handle kvobj/robj lifetimes - we'll need a new system for that, heavily complicating the redis code in the process IMO.

```
kvs_get(key) -> robj        kvs_set(key, val)          kvs_delete(key) -> bool
kvs_exists(key) -> bool     kvs_rename(old, new)       kvs_expire(key, ttl_ms)
kvs_ttl(key) -> int

robj_type(o) -> int         robj_dup(o) -> robj        robj_release(o)

string   robj_get_str, robj_get_int, robj_str_create, robj_str_set,
         robj_str_append, robj_str_setrange, robj_str_incrby, robj_str_incrbyfloat
list     robj_list_create, robj_list_get, robj_list_push, robj_list_pop,
         robj_list_set, robj_list_insert, robj_list_delete
hash     robj_hash_create, robj_hash_get, robj_hash_set, robj_hash_delete,
         robj_hash_exists, robj_hash_incrby, robj_hash_ttl, robj_hash_expire
set      robj_set_create, robj_set_add, robj_set_delete, robj_set_pop,
         robj_set_ismember, robj_set_rand
stream   robj_stream_create, robj_stream_add, robj_stream_delete,
         robj_stream_trim, robj_stream_len, robj_stream_last_id
```

