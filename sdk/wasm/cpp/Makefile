WASM_CXX ?= clang++
CXXFLAGS ?= -Oz
WASM_CXXFLAGS := --target=wasm32-unknown-unknown -std=c++17 -ffreestanding \
	-fno-builtin -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fvisibility=hidden
WASM_LDFLAGS := -nostdlib -Wl,--no-entry -Wl,--export-memory \
	-Wl,--initial-memory=262144 -Wl,--max-memory=8388608 \
	-Wl,--strip-all
SAMPLE := example/cpp-flat-record.wasm

.PHONY: all sample clean

all: sample

sample: $(SAMPLE)

$(SAMPLE): example/flat_record.cpp redis_wasm.hpp ../c/redis_wasm.h
	$(WASM_CXX) $(CXXFLAGS) $(WASM_CXXFLAGS) $(WASM_LDFLAGS) -o $@ $<
	chmod 644 $@

clean:
	rm -f $(SAMPLE)
