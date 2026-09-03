# Redis WASM JavaScript SDK

WebAssembly cannot execute ordinary dynamic JavaScript directly, so this SDK
uses [AssemblyScript](https://www.assemblyscript.org/): a TypeScript/JavaScript
syntax and standard-library subset that compiles ahead of time to WebAssembly.

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
