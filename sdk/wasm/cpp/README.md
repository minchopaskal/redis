# Redis WASM C++ SDK

`redis_wasm.hpp` adds allocation-free C++17 views and a `FlatBlob<T>` template
on top of the C ABI. `T` must be trivially copyable. Because blob ownership is
the exact module hash, a binary with a different C++ ABI or structure layout
cannot claim the stored value.

The example exports the void `redis_wasm_abi_version_0_1_0` marker and registers
its type and functions in `_initialize`, trapping on registration errors. The
marker is never called. See the C SDK README for the startup contract.

```sh
make -C sdk/wasm/cpp
(printf '#!wasm name=cppexample\n'; \
  cat sdk/wasm/cpp/example/cpp-flat-record.wasm) |
  src/redis-cli -x FUNCTION LOAD
```

The example instantiates `FlatRecord<int32_t>` and stores its fixed-layout
bytes:

```sh
src/redis-cli FCALL cpp_record_set 1 record 42 10 20 30 7
src/redis-cli FCALL cpp_record_sum 1 record
# 60
src/redis-cli TYPE record
# wasm-blob
```
