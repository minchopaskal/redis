# Redis WASM C SDK

`redis_wasm.h` is a freestanding, header-only C wrapper for the Redis WASM
Functions ABI. It requires Clang with the `wasm32-unknown-unknown` target and
does not depend on WASI or a guest libc.

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
