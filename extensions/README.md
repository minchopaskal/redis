# WASM extensions

Extensions are compiled guests loaded by Redis Functions.

Build Redis with `make BUILD_WASM=yes`. Build the C GCRA rate limiter with
`make -C extensions/gcra WASM_CC=clang` (Clang must include the wasm32 linker).

Configure one or more raw WASM files using absolute paths in redis.conf:

```conf
loadextension /absolute/path/to/extensions/gcra/gcra.wasm
```

Redis registers these after loading RDB/AOF and before accepting clients.
Library names are `wasm_<SHA-256 of module bytes>`. Identical persisted libraries
and duplicate directives are idempotent. Missing/invalid files or conflicting
function names fail startup. `CONFIG REWRITE` retains the directives; runtime
`CONFIG SET loadextension` is not supported. Builds without WASM and Sentinel
reject the directive.

Keep the configuration and binaries on every restart. Preloading itself is not
appended to an existing AOF; AOF rewrites and RDB saves include the library.
Replication's full synchronization includes currently loaded libraries. A
replica's local preloads are replaced by the primary's libraries during sync.
`FUNCTION FLUSH`/`DELETE` can remove a preload until the next restart.

See [GCRA](gcra/README.md) for invocation and data compatibility.
