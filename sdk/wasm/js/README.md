# Redis WASM JavaScript SDK

WebAssembly cannot execute ordinary dynamic JavaScript directly, so this SDK
uses [AssemblyScript](https://www.assemblyscript.org/): a TypeScript/JavaScript
syntax and standard-library subset that compiles ahead of time to WebAssembly.

The build exports AssemblyScript's runtime initializer as `_initialize`.
Redis detects the void `redis_wasm_abi_version_0_1_0` marker without calling it,
then calls `_initialize` followed by `main(0, 0)` to register the functions.
The `main` result is ignored; registration failures trap and abort loading.

```sh
make -C sdk/wasm/js
(printf '#!wasm name=jsexample\n'; cat sdk/wasm/js/build/js-functions.wasm) |
  src/redis-cli -x FUNCTION LOAD
```

This example intentionally registers functions only:

```sh
src/redis-cli FCALL js_echo 0 hello
# hello
src/redis-cli FCALL js_incr 1 js-counter
# 1
```

The SDK wraps binary-safe FCALL input, function registration, Redis command
calls, staged replies, and final status/error/raw replies.
