# Redis WASM Rust SDK

The `redis-wasm` crate is a `no_std`, allocation-free wrapper around function
input, registration, blob I/O, and replies. Build it with Rust's
`wasm32-unknown-unknown` target:

```sh
rustup target add wasm32-unknown-unknown
make -C sdk/wasm/rust
(printf '#!wasm name=rustexample\n'; \
  cat sdk/wasm/rust/example/rust-hashmap.wasm) |
  src/redis-cli -x FUNCTION LOAD
```

The example implements a fixed-capacity, open-addressed hash map with
multiplicative hashing and linear probing. Its slot array is serialized into a
`rust_hashmap` blob.

```sh
src/redis-cli FCALL rust_map_create 1 rust-map
src/redis-cli FCALL rust_map_set 1 rust-map 10 100
src/redis-cli FCALL rust_map_set 1 rust-map 26 260
src/redis-cli FCALL rust_map_get 1 rust-map 26
# 260
src/redis-cli FCALL rust_map_len 1 rust-map
# 2
```
