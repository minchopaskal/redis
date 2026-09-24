# Redis WASM C SDK

`redis_wasm.h` is a freestanding, header-only C wrapper for the Redis WASM
Functions ABI. It requires Clang with the `wasm32-unknown-unknown` target and
does not depend on WASI or a guest libc.

For direct Redis commands, call
`redis_wasm_create_command(name, export_name, arity, numkeys)` during startup.
It also registers an FCALL function. Arity includes the command name (negative
means minimum); the first `numkeys` arguments are keys. See `extensions/gcra`
for a rate-limiter example using this API.

Export a `() -> void` function using `REDIS_WASM_EXPORT(REDIS_WASM_ABI_MARKER)`;
the marker's name declares the ABI version, and Redis never calls it. The example
registers its type and functions in `_initialize() -> void`, trapping on errors.
Redis calls optional `main(i32, i32) -> i32` after `_initialize`, or falls back to
`_start() -> void` only if `_initialize` is absent. The `main` result is ignored.

```sh
make -C sdk/wasm/c
(printf '#!wasm name=cexample\n'; cat sdk/wasm/c/example/c-list.wasm) |
  src/redis-cli -x FUNCTION LOAD
```

The example implements a custom doubly-linked-list blob. Its binary format
stores a header plus, for every node, previous/next indexes and a binary-safe
value.

```sh
src/redis-cli FCALL c_list_create 1 c-list first second third
src/redis-cli FCALL c_list_len 1 c-list
# 3
src/redis-cli FCALL c_list_push 1 c-list fourth
src/redis-cli FCALL c_list_len 1 c-list
# 4
src/redis-cli TYPE c-list
# wasm-blob
```
