PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBTAM_PREFIX ?= $(HOME)/.local
BUILD ?= build
VERSION := $(shell cat VERSION)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
            -D_FILE_OFFSET_BITS=64 -Iinclude -I$(LIBTAM_PREFIX)/include
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wvla
# The document source, layout and renderers are libtam-render (libtam 0.13.0),
# linked before libtam.
LIBTAM ?= $(LIBTAM_PREFIX)/lib/libtam-render.a $(LIBTAM_PREFIX)/lib/libtam.a
OBJECTS = $(addprefix $(BUILD)/,main.o watch.o pager.o)

all: $(BUILD)/tam-view

libtam-check:
	@printf '#include <tam.h>\n#if TAM_VERSION_MAJOR == 0 && (TAM_VERSION_MINOR < 14 || (TAM_VERSION_MINOR == 14 && TAM_VERSION_PATCH < 1))\n#error libtam 0.14.1 or later required\n#endif\n#include <tam_render.h>\n' | $(CC) $(CPPFLAGS) -x c -fsyntax-only - || { echo 'tam-view requires libtam 0.14.1 or later, with libtam-render' >&2; exit 1; }

$(OBJECTS): | libtam-check

$(BUILD)/tam-view: $(OBJECTS) $(LIBTAM)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBTAM)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/main.o $(BUILD)/pager.o: CPPFLAGS += -DVIEW_VERSION='"$(VERSION)"'
$(BUILD)/main.o $(BUILD)/pager.o: VERSION

$(BUILD)/test-pty: CPPFLAGS += -DVIEW_VERSION='"$(VERSION)"'
$(BUILD)/test-pty: tests/test-pty.c VERSION | libtam-check
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

# The renderer's own tests are in libtam (tests/test_render*.c).
test: $(BUILD)/tam-view $(BUILD)/test-pty
	$(BUILD)/test-pty $(BUILD)/tam-view
	sh tests/cli.sh $(BUILD)/tam-view $(VERSION)

sanitize:
	$(MAKE) BUILD=build/sanitize \
	  CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -fno-pie' \
	  LDFLAGS='-fsanitize=address,undefined -no-pie' test

install: $(BUILD)/tam-view
	install -Dm755 $(BUILD)/tam-view $(DESTDIR)$(BINDIR)/tam-view

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tam-view

clean:
	rm -rf build

.PHONY: all test sanitize install uninstall clean libtam-check

-include $(OBJECTS:.o=.d)
