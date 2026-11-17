# bar

A small Sway status bar written in C using Wayland layer shell and Cairo/Pango.
It shows workspaces and app icons on the left, with CPU usage, available RAM,
IPv4 address, date, time, and keyboard layout on the right.

## Requirements

- GCC, GNU Make, `pkg-config`, and `wayland-scanner`.
- Development libraries for these pkg-config modules:
  `wayland-client cairo pango pangocairo libcjson librsvg-2.0 gio-unix-2.0`.
- The xdg-shell protocol XML from `wayland-protocols`. The layer-shell XML
  from `wlr-protocols` is vendored in `protocols/`.
- Sway and the Liberation Mono font for running the bar.

## Building

```sh
make
```

The executable and generated Wayland bindings are written to `build/`.
The default protocol locations are:

```text
WLR_XML=protocols/wlr-layer-shell-unstable-v1.xml
XDG_XML=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
```

`protocols/wlr-layer-shell-unstable-v1.xml` is copied unchanged from
[wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols)
(version 5, MIT license).

Override either path when necessary:

```sh
make WLR_XML=/path/to/wlr-layer-shell-unstable-v1.xml XDG_XML=/path/to/xdg-shell.xml
```

`make clean` removes the executable and generated bindings.

## Running

Run inside a Sway session with `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, and
`SWAYSOCK` set:

```sh
./build/bar
```

To install, run `make install`. This copies the executable to `~/.local/bin/bar`;
use `PREFIX=/another/prefix` to change the destination. Building alone does not
replace the installed binary. Restart the bar after installing an update.

For automatic startup, add this to the Sway configuration:

```text
exec ~/.local/bin/bar
```

## Workspaces and appearance

Click a workspace label or an ordinary app icon to switch to that workspace.
Each window has its own icon; the focused workspace label is white and other
labels are gray. Scratchpad windows follow their current placement:

| Window state | Icon location | Clicking the icon |
| --- | --- | --- |
| Shown on a workspace | That workspace | Hides the window in the scratchpad |
| Hidden in the scratchpad | `S:` | Shows the window on the current workspace |

`S:` appears only when hidden windows have displayable app IDs or X11 classes.
The bar installs no keyboard bindings or scratchpad window limits.

Appearance is compiled into `src/main.c`: Liberation Mono 14, a 34-pixel height,
22-pixel icons, background `#000000`, and foreground `#ffffff`. Available RAM
is shown in GiB, such as `3.2G`. The first keyboard supplies the layout:
English is `EN`, Bulgarian is `BG`, and other names use their first two letters.

App icons resolve from desktop entries using the Wayland app ID or X11 class,
with `StartupWMClass` and case/separator matching as fallbacks. PNG/SVG lookup
uses XDG icon directories, `~/.icons`, theme inheritance, and pixmaps. The theme
comes from `gtk-icon-theme-name` in `~/.config/gtk-3.0/settings.ini` (or under
`XDG_CONFIG_HOME`), with hicolor, Adwaita, and AdwaitaLegacy fallbacks. Missing
icons use a built-in glyph. Restart after changing desktop entries, icons, or
the theme setting.

Workspace updates follow Sway events. CPU, memory, and the clock update once
per second; IPv4 refreshes every ten seconds. The bar exits on compositor or
IPC loss. It creates one bottom bar on the compositor-selected output.

The display is capped at 16 groups and 32 icons per group; `S:` consumes one
group. Excess groups/icons are omitted. Crowded content can overlap the status
modules, and output scaling is not handled explicitly.

## Checks and source

```sh
make check
make check-integration
```

`make check` covers IPC, workspace/event/buffer state, scratchpad clicks and
rendering, and icon lookup/loading/cache behavior. Individual suites are
available as `check-ipc`, `check-state`, and `check-scratchpad`.

The integration check also requires Python 3. It starts an isolated headless
Sway compositor to check startup, buffer release/reuse, resize, and disconnect
handling. It does not exercise scratchpad clicks against a live compositor.
Socket restrictions can prevent IPC and integration checks from running.

`src/main.c` owns state, scheduling, icons, and rendering; `src/ipc.c` handles
socket transport; `src/buffers.c` owns Wayland/Cairo drawing buffers. Tests live
in `tests/`. C sources and headers follow the repository's `.clang-format`.
