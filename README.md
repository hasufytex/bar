# bar

Minimal status bar for Sway. Raw Wayland (wlr-layer-shell) + Cairo/Pango, no toolkit.

Workspaces with app icons on the left (clickable), `CPU MEM IP DATE TIME LANG` on the right.

App icons resolve through desktop filenames, then `StartupWMClass`, using GIO's
XDG desktop-entry lookup. The matched entry supplies `Icon=`; missing icons use
a built-in window glyph. Restart the bar after installing or changing desktop
entries, since the catalog and icons are cached.

## Dependencies

```
pacman -S wayland cairo pango cjson librsvg glib2
yay -S wlr-protocols
```

## Compile

```
make
```

Run icon, workspace/event, and nonblocking IPC checks with `make check`.

If the layer-shell XML lives elsewhere: `make WLR_XML=/path/to/wlr-layer-shell-unstable-v1.xml`

Existing generated protocol files can be reused without the XML; a clean build
still requires it. `make check-integration` additionally needs Sway and Python 3
and runs a separate headless compositor to check buffer release, resize, and
disconnect behavior without changing the current desktop.

The bar samples CPU/memory once per second and refreshes IP every ten seconds.
Window title/focus/mark events do not trigger workspace-tree queries. Sway IPC
uses nonblocking framed messages with bounded reads and a three-second request
deadline. Two release-tracked drawing buffers handle resize safely; frame
callbacks coalesce rendering. Icons are rasterized once at display size and
retained for the current workspace snapshot, including more than 20 app IDs.

`main.c` owns state and scheduling, `ipc.c` handles socket transport, and
`buffers.c` owns Wayland/Cairo drawing buffers.

## Install

```
make install
```

Uses Liberation Mono 14 and foot’s default background (`#242424`) and foreground (`#ffffff`). The active workspace number is white; inactive numbers are gray, with no workspace backgrounds or theme file.

Copies the binary to `~/.local/bin/bar`.
Rebuilding never touches the installed binary until the next `make install`.
