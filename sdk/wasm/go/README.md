# Redis WASM Go SDK (PoC)

This package is the TinyGo guest SDK for the experimental Redis WASM Functions
engine. It provides binary-safe function input, Redis command calls, function
registration, RESP tokenization, and reply helpers.

Requirements:

- Redis built from the repository root with `make BUILD_WASM=yes`
- TinyGo 0.41 or newer

Build the included sample:

```sh
make -C sdk/wasm/go
```

Load it into a running Redis server:

```sh
(printf '#!wasm name=goexample\n'; \
  cat sdk/wasm/go/example/redis-go-example.wasm) |
  src/redis-cli -x FUNCTION LOAD
```

The sample registers two functions:

```sh
src/redis-cli FCALL go_set 1 my-key my-value
src/redis-cli FCALL go_get 1 my-key
```

`go_set` invokes Redis `SET` through the host ABI, while `go_get` invokes `GET`
and forwards its validated RESP reply. The SDK targets `wasm-unknown`; the
engine calls TinyGo's optional `_initialize` export before `redis_init`.

The PoC does not implement `OBJ_WASM` or blob imports. Go functions currently
store data through ordinary Redis commands and data types.
