# GCRA rate limiter

This freestanding C extension ports the algorithm from `src/gcra.c`. The
algorithm runs inside WAMR, including weighted requests and burst calculation.
It registers the Redis function `GCRA` and the blob type `gcra_v1`.

```sh
make -j BUILD_WASM=yes
./src/redis-server
# In another terminal:
redis-cli GCRA rate:user:123 10 5 1
redis-cli GCRA rate:user:123 10 5 1 TOKENS 3
```

The WASM-enabled Redis build compiles this source into `extensions/gcra.wasm`.
It requires a wasm32-capable Clang and matching `wasm-ld`; use `WASM_CC` to select
the compiler. `make -C extensions` builds only the extensions. Set
`extension-dir ""` to disable autoload; explicit `loadextension` remains
supported. GCRA is an ordinary entry in the generic autoload directory, not a
special case in the loader. See
[extension autoload](../README.md#extension-autoload).

Arguments are `max_burst tokens_per_period period [TOKENS count]` after the
single key. Burst capacity is `max_burst + 1`; cost defaults to one. Period is
decimal seconds, greater than zero and below 1e12, with up to six fractional
digits. Scientific notation is not accepted. Integers are nonnegative decimal
int64 values, with positive rate/cost and checked arithmetic. Inputs, including
the key, must fit the example's 8 KiB buffer.

The reply follows the native command: `[limited, capacity, remaining,
retry_after_seconds, reset_after_seconds]`. A limited flag of zero allows the
request. Retry is -1 on success or when the cost exceeds total capacity.
Rejected requests leave state unchanged.

Time comes from Redis `TIME`, rounded down to milliseconds like the native
command. The emission interval uses rounded microseconds. State consists of a
four-byte version marker and a little-endian int64 theoretical arrival time.
The blob expires at that time, rounded up to milliseconds to avoid early refill.
The state and absolute expiry are written together through `RESTORE ABSTTL`;
replicas and AOF replay stored results without re-running the clock-dependent
algorithm. Execution is atomic under Redis's existing Functions mechanism.

ACLs need GCRA, TIME, EXISTS, TYPE and RESTORE on the intended keys. The direct
command declares its first argument as a key for ACL and cluster checks.
`FCALL GCRA 1 key ...` remains supported and requires FCALL permission instead
of GCRA permission. Neither entry point supports read-only invocation.
Ordinary Redis key administration can still delete or
replace state. Module SHA-256 ownership protects blob reads from other module
code; upgrading the binary changes ownership and requires deliberate state
migration or expiry. Native GCRA keys are a different object type and cannot
be reused directly.

Run `./runtest --single unit/gcra-wasm --clients 1` with a WASM-enabled build.

`gcraInitialize()`, exported as `_initialize() -> void`, calls
`redis_wasm_create_command("GCRA", "GCRA", -5, 1)` and traps on registration errors.
The void `redis_wasm_abi_version_0_1_0` export declares the ABI; Redis never calls it.
This declares at least five command arguments (including GCRA), with one
leading key, and registers the same callback for FCALL. Loading fails if a
built-in or module command already owns the name GCRA.
