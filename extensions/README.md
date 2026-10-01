# WASM extensions

Extensions are compiled guests loaded by Redis Functions.

Build Redis and its extensions with `make -j BUILD_WASM=yes`. Extension binaries
are generated from source and are not checked in. WASM-enabled builds require
CMake for WAMR and a wasm32-capable Clang with its matching `wasm-ld` linker.
Non-WASM builds do not compile extensions or require the WASM toolchain.

## Extension autoload

WASM-enabled Redis automatically loads all top-level `.wasm` files from
`./extensions` at startup, in bytewise filename order independent of locale.
GCRA compiles to `extensions/gcra.wasm`, with its source under `extensions/gcra/`.
The loader has no GCRA-specific logic and does not scan subdirectories.
No guest byte array or binary is embedded in redis-server.

From the repository root:

```sh
make -j BUILD_WASM=yes
./src/redis-server
```

Subsequent builds recompile extensions when their sources change. To build only
the extensions, run `make -C extensions`. The build discovers immediate
subdirectories containing a `Makefile`; each writes its binaries directly into
`extensions/`. `make clean` removes those generated binaries.

Override `WASM_CC` to select Clang, and `WASM_CFLAGS` / `WASM_LDFLAGS` to customize
guest compiler and linker flags. These are separate from Redis's native flags.
For example, `make -j BUILD_WASM=yes WASM_CC=/path/to/clang`.

The startup-only `extension-dir` setting accepts a relative or absolute directory,
or `""` to disable automatic loading. Relative paths resolve against Redis's
working directory (`dir`, which defaults to the directory where Redis starts).
An empty directory is valid. Missing or unreadable
directories and invalid extensions abort startup. The `.wasm` suffix is
case-sensitive; other files and subdirectories are skipped. Symlinks to regular
files are supported; broken links and other non-regular `.wasm` entries fail.
Directory entries load before explicit `loadextension` directives.
Sentinel skips autoload. Non-WASM builds default to an empty directory setting
and reject nonempty values. Install extension files in an administrator-owned
directory; replacing the bytes changes both the code and its blob owner hash.

## Additional extensions

Configure one or more additional raw WASM files using absolute paths in redis.conf:

```conf
loadextension /absolute/path/to/extensions/gcra.wasm
```

Redis registers these after loading RDB/AOF and before accepting clients.
It also validates the files before startup ACL loading so named command ACL
rules can resolve extension commands. That initial library context is discarded
before RDB/AOF recovery; only the final reconciled libraries execute requests.
Library names are `wasm_<SHA-256 of module bytes>`. Identical persisted libraries
and duplicate directives are idempotent. Missing/invalid files or conflicting
function names fail startup. `CONFIG REWRITE` retains the directives; runtime
`CONFIG SET loadextension` is not supported. `CONFIG REWRITE` also retains
`extension-dir`, which cannot be changed at runtime. Builds without WASM and
Sentinel reject `loadextension`.

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

Keep the extension in the autoload directory, or configure `loadextension` on
restart when persisted ACL rules explicitly name an extension command: its
command identity must exist before ACL files are loaded.
