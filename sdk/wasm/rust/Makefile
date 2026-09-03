CARGO ?= cargo
SAMPLE := example/rust-hashmap.wasm
BUILD_OUTPUT := target/wasm32-unknown-unknown/release/redis_wasm_rust_hashmap.wasm

.PHONY: all sample clean

all: sample

sample: $(SAMPLE)

$(SAMPLE): redis-wasm/src/lib.rs example/src/lib.rs \
		redis-wasm/Cargo.toml example/Cargo.toml Cargo.toml .cargo/config.toml
	$(CARGO) build --release -p redis-wasm-rust-hashmap
	cp $(BUILD_OUTPUT) $@
	chmod 644 $@

clean:
	$(CARGO) clean
	rm -f $(SAMPLE)
