# Each source directory provides a Makefile that writes its .wasm into this dir.
EXTENSIONS := $(patsubst %/Makefile,%,$(wildcard */Makefile))
CLEAN_TARGETS := $(addsuffix .clean,$(EXTENSIONS))

.PHONY: all clean $(EXTENSIONS) $(CLEAN_TARGETS)
all: $(EXTENSIONS)

$(EXTENSIONS):
	$(MAKE) -C $@

clean: $(CLEAN_TARGETS)

$(CLEAN_TARGETS):
	$(MAKE) -C $(patsubst %.clean,%,$@) clean
