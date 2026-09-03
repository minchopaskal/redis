WASM_CC ?= clang
CFLAGS ?= -Oz
WASM_CFLAGS := --target=wasm32-unknown-unknown -std=c11 -ffreestanding \
	-fno-builtin -fvisibility=hidden
WASM_LDFLAGS := -nostdlib -Wl,--no-entry -Wl,--export-memory \
	-Wl,--initial-memory=262144 -Wl,--max-memory=8388608 \
	-Wl,--strip-all
SAMPLE := example/c-list.wasm

.PHONY: all sample clean

all: sample

sample: $(SAMPLE)

$(SAMPLE): example/list.c redis_wasm.h
	$(WASM_CC) $(CFLAGS) $(WASM_CFLAGS) $(WASM_LDFLAGS) -o $@ $<
	chmod 644 $@

clean:
	rm -f $(SAMPLE)
