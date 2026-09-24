WASM_CC ?= clang
CFLAGS ?= -Oz -Wall -Wextra -Werror

all: ../gcra.wasm

../gcra.wasm: gcra.c ../../sdk/wasm/c/redis_wasm.h
	$(WASM_CC) $(CFLAGS) --target=wasm32-unknown-unknown -std=c11 -ffreestanding -fno-builtin -nostdlib -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=262144 -Wl,--max-memory=8388608 -Wl,--strip-all -o $@ $<
	chmod 644 $@

.PHONY: all clean
clean:
	rm -f ../gcra.wasm
