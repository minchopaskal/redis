WASM_CC ?= clang
WASM_CFLAGS ?= -Oz -Wall -Wextra -Werror
WASM_LDFLAGS ?=

all: ../gcra.wasm

../gcra.wasm: gcra.c ../../sdk/wasm/c/redis_wasm.h Makefile
	$(WASM_CC) $(WASM_CFLAGS) --target=wasm32-unknown-unknown -std=c11 -ffreestanding -fno-builtin -nostdlib -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=262144 -Wl,--max-memory=8388608 -Wl,--strip-all $(WASM_LDFLAGS) -o $@ $<
	chmod 644 $@

.DELETE_ON_ERROR:
.PHONY: all clean
clean:
	rm -f ../gcra.wasm
