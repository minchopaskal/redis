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
and forwards its validated RESP reply.

The sample also registers a `binary_tree` blob type. Its tree is a Go pointer
structure serialized as a pre-order binary stream with explicit nil markers:

```sh
src/redis-cli FCALL tree_create 1 my-tree 8 3 10 1 6 14 4 7 13
src/redis-cli TYPE my-tree
# wasm-blob
src/redis-cli FCALL tree_contains 1 my-tree 7
# 1
src/redis-cli FCALL tree_insert 1 my-tree 9
```

The blob SDK exposes `RegisterBlobType`, `BlobLen`, `BlobRead`, and `BlobWrite`.
Redis persists the module SHA-256 owner digest, type, and payload. Reads and
writes require the same module bytes and registered type; changing only the
user-controlled library name cannot claim another module's blobs.

The SDK targets `wasm-unknown`; the engine calls TinyGo's optional
`_initialize` export before `redis_init`.
