# WASM extensions

Extensions are compiled guests loaded by Redis Functions.

Build Redis with `make BUILD_WASM=yes`. Build the C GCRA rate limiter with
`make -C extensions/gcra WASM_CC=clang` (Clang must include the wasm32 linker).

Configure one or more raw WASM files using absolute paths in redis.conf:

```conf
loadextension /absolute/path/to/extensions/gcra/gcra.wasm
```

Redis registers these after loading RDB/AOF and before accepting clients.
It also validates the files before startup ACL loading so named command ACL
rules can resolve extension commands. That initial library context is discarded
before RDB/AOF recovery; only the final reconciled libraries execute requests.
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

## Direct commands

The C SDK's `redis_wasm_create_command(name, export_name, arity, numkeys)` opts
into a direct Redis command and registers the function for FCALL as well.
Arity follows Redis conventions: positive is exact, negative is a minimum,
and both include the command name. `numkeys` declares a fixed number of leading
key arguments; the remaining arguments become the function's arguments.
`redis_wasm_register_function` retains its existing FCALL-only behavior.

The direct command uses the same function executor, script restrictions, host
imports, and effect replication. Its own command ACL and key checks run before
execution. `COMMAND INFO` and `COMMAND GETKEYS` expose the declared metadata.
Current commands are conservatively classified as write/deny-oom, matching the
flagless WASM Functions ABI. Registration fails on built-in/module name conflicts.

Commands become callable only after successful library loading, are removed by
DELETE/FLUSH, and are recreated by replacement, restore, replication and restart.
The server retains small immutable command descriptors and ACL identities after
unload so queued transactions and I/O cannot retain dangling pointers. Names are
reserved, and arity/key counts cannot change until restart. This PoC bounds the
registry to 256 names of up to 128 bytes per process, including failed registration
attempts that passed signature validation. A queued call whose function has been
removed reports an error. Re-registering the same name/signature calls the current
function, as FCALL does.

Configure `loadextension` on restart when persisted ACL rules explicitly name an
extension command: its command identity must exist before ACL files are loaded.
