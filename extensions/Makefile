# Guests are checked in; rebuilding them requires a wasm32-capable Clang.
EXTENSIONS = gcra

.PHONY: all clean $(EXTENSIONS)
all: $(EXTENSIONS)

$(EXTENSIONS):
	$(MAKE) -C $@

clean:
	$(foreach extension,$(EXTENSIONS),$(MAKE) -C $(extension) clean;)
