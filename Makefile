# Arch: install wlr-protocols (AUR) or adjust WLR_XML to wherever the xml lives
WLR_XML ?= /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml
XDG_XML ?= /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

PREFIX ?= $(HOME)/.local

CC      = gcc
PACKAGES = wayland-client cairo pango pangocairo libcjson librsvg-2.0 gio-unix-2.0
CFLAGS  = -O2 -Wall $(shell pkg-config --cflags $(PACKAGES))
LIBS    = $(shell pkg-config --libs $(PACKAGES))

PROTO_SRC = wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c
PROTO_HDR = wlr-layer-shell-unstable-v1-client-protocol.h xdg-shell-client-protocol.h

SOURCES = main.c ipc.c buffers.c
HEADERS = ipc.h buffers.h

bar: $(SOURCES) $(HEADERS) $(PROTO_SRC) $(PROTO_HDR)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(PROTO_SRC) $(LIBS)

wlr-layer-shell-unstable-v1-client-protocol.h: $(wildcard $(WLR_XML))
	test -f "$(WLR_XML)" || { echo "Set WLR_XML to the layer-shell XML path" >&2; exit 1; }
	wayland-scanner client-header $(WLR_XML) $@

wlr-layer-shell-unstable-v1-protocol.c: $(wildcard $(WLR_XML))
	test -f "$(WLR_XML)" || { echo "Set WLR_XML to the layer-shell XML path" >&2; exit 1; }
	wayland-scanner private-code $(WLR_XML) $@

# layer shell references xdg_popup, so xdg-shell glue must be linked too
xdg-shell-client-protocol.h: $(wildcard $(XDG_XML))
	wayland-scanner client-header $(XDG_XML) $@

xdg-shell-protocol.c: $(wildcard $(XDG_XML))
	wayland-scanner private-code $(XDG_XML) $@

# Copies (not symlinks): rebuilding the repo must not touch the live binary
# until the next explicit `make install`.
install: bar
	install -Dm755 bar $(PREFIX)/bin/bar

check: check-ipc check-state $(PROTO_SRC) $(PROTO_HDR)
	@set -eu; test_bin=$$(mktemp /tmp/bar-icons-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -I. -o "$$test_bin" tests/icons.c ipc.c buffers.c $(PROTO_SRC) $(LIBS); \
	"$$test_bin"; \
	if [ -n "$(CHECK_APP)" ]; then "$$test_bin" $(CHECK_APP); fi

check-ipc:
	@set -eu; test_bin=$$(mktemp /tmp/bar-ipc-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -I. -o "$$test_bin" tests/ipc.c ipc.c; \
	"$$test_bin"

check-state: $(PROTO_SRC) $(PROTO_HDR)
	@set -eu; test_bin=$$(mktemp /tmp/bar-state-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -I. -o "$$test_bin" tests/state.c ipc.c buffers.c $(PROTO_SRC) $(LIBS); \
	"$$test_bin"

check-integration: bar
	python3 tests/integration.py

clean:
	rm -f bar $(PROTO_SRC) $(PROTO_HDR)

.PHONY: install check check-ipc check-state check-integration clean
