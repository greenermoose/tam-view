PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBTAM_PREFIX ?= $(HOME)/.local
BUILD ?= build
VERSION := $(shell cat VERSION)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
            -D_FILE_OFFSET_BITS=64 -Iinclude -I$(LIBTAM_PREFIX)/include
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wvla
LIBTAM ?= $(LIBTAM_PREFIX)/lib/libtam.a
OBJECTS = $(addprefix $(BUILD)/,main.o source.o layout.o pager.o)

all: $(BUILD)/tam-view

$(BUILD)/tam-view: $(OBJECTS) $(LIBTAM)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBTAM)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/main.o: CPPFLAGS += -DVIEW_VERSION='"$(VERSION)"'
$(BUILD)/main.o: VERSION

$(BUILD)/test-layout: tests/test-layout.c $(BUILD)/layout.o $(BUILD)/source.o $(LIBTAM)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/test-pty: tests/test-pty.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: $(BUILD)/tam-view $(BUILD)/test-layout $(BUILD)/test-pty
	$(BUILD)/test-layout
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

.PHONY: all test sanitize install uninstall clean

-include $(OBJECTS:.o=.d)
