TINYGO ?= tinygo
TINYGO_FLAGS ?= -target=wasm-unknown -scheduler=none -gc=conservative -panic=trap -no-debug
SAMPLE := example/redis-go-example.wasm

.PHONY: all sample clean

all: sample

sample: $(SAMPLE)

$(SAMPLE): example/main.go rediswasm/rediswasm.go go.mod
	$(TINYGO) build $(TINYGO_FLAGS) -o $@ ./example

clean:
	rm -f $(SAMPLE)
