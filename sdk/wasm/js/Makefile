NPM ?= npm
SAMPLE := build/js-functions.wasm

.PHONY: all sample clean

all: sample

sample: $(SAMPLE)

node_modules/.bin/asc: package.json package-lock.json
	$(NPM) ci

$(SAMPLE): assembly/sdk.ts assembly/example.ts node_modules/.bin/asc
	$(NPM) run build

clean:
	rm -rf build
