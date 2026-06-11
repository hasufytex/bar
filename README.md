# bar

Minimal status bar for Sway. Raw Wayland (wlr-layer-shell) + Cairo/Pango, no toolkit.

Workspaces with app icons on the left (clickable), `CPU MEM IP DATE TIME LANG` on the right.

## Dependencies

```
pacman -S wayland cairo pango cjson
yay -S wlr-protocols
```

## Compile

```
make
```

If the layer-shell XML lives elsewhere: `make WLR_XML=/path/to/wlr-layer-shell-unstable-v1.xml`

## Install

```
make install
```

Copies the binary to `~/.local/bin/bar` and the theme template to `~/.local/share/bar/`.
Rebuilding never touches the installed binary until the next `make install`.
