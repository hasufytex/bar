# Layer shell XML is vendored from wlr-protocols (not packaged on every distro).
WLR_XML ?= protocols/wlr-layer-shell-unstable-v1.xml
XDG_XML ?= /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

PREFIX ?= $(HOME)/.local

CC      = gcc
PACKAGES = wayland-client cairo pango pangocairo libcjson librsvg-2.0 gio-unix-2.0
CFLAGS  = -O2 -Wall $(shell pkg-config --cflags $(PACKAGES))
LIBS    = $(shell pkg-config --libs $(PACKAGES))

CPPFLAGS += -Isrc -Ibuild
TARGET = build/bar
PROTO_SRC = build/wlr-layer-shell-unstable-v1-protocol.c build/xdg-shell-protocol.c
PROTO_HDR = build/wlr-layer-shell-unstable-v1-client-protocol.h build/xdg-shell-client-protocol.h

SOURCES = src/main.c src/ipc.c src/buffers.c
HEADERS = src/ipc.h src/buffers.h

all: $(TARGET)

bar: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) $(PROTO_SRC) $(PROTO_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(PROTO_SRC) $(LIBS)

build:
	mkdir -p $@

build/wlr-layer-shell-unstable-v1-client-protocol.h: $(wildcard $(WLR_XML)) | build
	test -f "$(WLR_XML)" || { echo "Set WLR_XML to the layer-shell XML path" >&2; exit 1; }
	wayland-scanner client-header $(WLR_XML) $@

build/wlr-layer-shell-unstable-v1-protocol.c: $(wildcard $(WLR_XML)) | build
	test -f "$(WLR_XML)" || { echo "Set WLR_XML to the layer-shell XML path" >&2; exit 1; }
	wayland-scanner private-code $(WLR_XML) $@

# layer shell references xdg_popup, so xdg-shell glue must be linked too
build/xdg-shell-client-protocol.h: $(wildcard $(XDG_XML)) | build
	test -f "$(XDG_XML)" || { echo "Set XDG_XML to the xdg-shell XML path" >&2; exit 1; }
	wayland-scanner client-header $(XDG_XML) $@

build/xdg-shell-protocol.c: $(wildcard $(XDG_XML)) | build
	test -f "$(XDG_XML)" || { echo "Set XDG_XML to the xdg-shell XML path" >&2; exit 1; }
	wayland-scanner private-code $(XDG_XML) $@

# Copies (not symlinks): rebuilding the repo must not touch the live binary
# until the next explicit `make install`.
install: $(TARGET)
	install -Dm755 $(TARGET) $(PREFIX)/bin/bar

check: check-ipc check-state check-scratchpad $(PROTO_SRC) $(PROTO_HDR)
	@set -eu; test_bin=$$(mktemp /tmp/bar-icons-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$$test_bin" tests/icons.c src/ipc.c src/buffers.c $(PROTO_SRC) $(LIBS); \
	"$$test_bin"; \
	if [ -n "$(CHECK_APP)" ]; then "$$test_bin" $(CHECK_APP); fi

check-ipc:
	@set -eu; test_bin=$$(mktemp /tmp/bar-ipc-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$$test_bin" tests/ipc.c src/ipc.c; \
	"$$test_bin"

check-state: $(PROTO_SRC) $(PROTO_HDR)
	@set -eu; test_bin=$$(mktemp /tmp/bar-state-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$$test_bin" tests/state.c src/ipc.c src/buffers.c $(PROTO_SRC) $(LIBS); \
	"$$test_bin"

check-scratchpad: $(PROTO_SRC) $(PROTO_HDR)
	@set -eu; test_bin=$$(mktemp /tmp/bar-scratchpad-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$$test_bin" tests/scratchpad.c src/ipc.c src/buffers.c $(PROTO_SRC) $(LIBS); \
	"$$test_bin"

check-integration: $(TARGET)
	python3 tests/integration.py

clean:
	rm -f $(TARGET) $(PROTO_SRC) $(PROTO_HDR)

.PHONY: all bar install check check-ipc check-state check-scratchpad check-integration clean
