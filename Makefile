# Arch: install wlr-protocols (AUR) or adjust WLR_XML to wherever the xml lives
WLR_XML ?= /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml
XDG_XML ?= /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

PREFIX ?= $(HOME)/.local

CC      = gcc
CFLAGS  = -O2 -Wall $(shell pkg-config --cflags wayland-client cairo pango pangocairo libcjson)
LIBS    = $(shell pkg-config --libs   wayland-client cairo pango pangocairo libcjson)

PROTO_SRC = wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c
PROTO_HDR = wlr-layer-shell-unstable-v1-client-protocol.h xdg-shell-client-protocol.h

bar: main.c $(PROTO_SRC) $(PROTO_HDR)
	$(CC) $(CFLAGS) -o $@ main.c $(PROTO_SRC) $(LIBS)

wlr-layer-shell-unstable-v1-client-protocol.h: $(WLR_XML)
	wayland-scanner client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: $(WLR_XML)
	wayland-scanner private-code $< $@

# layer shell references xdg_popup, so xdg-shell glue must be linked too
xdg-shell-client-protocol.h: $(XDG_XML)
	wayland-scanner client-header $< $@

xdg-shell-protocol.c: $(XDG_XML)
	wayland-scanner private-code $< $@

# Copies (not symlinks): rebuilding the repo must not touch the live binary
# until the next explicit `make install`.
install: bar
	install -Dm755 bar $(PREFIX)/bin/bar
	install -Dm644 theme.conf.in $(HOME)/.local/share/bar/theme.conf.in

clean:
	rm -f bar $(PROTO_SRC) $(PROTO_HDR)

.PHONY: install clean
