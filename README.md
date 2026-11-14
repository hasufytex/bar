# bar

A small status bar for Sway, written in C using Wayland layer shell and
Cairo/Pango, without a widget toolkit. It creates one bottom bar on the output
chosen by the compositor.

## Workspaces and scratchpad

The left side shows workspace numbers (or names for unnumbered workspaces) and
one app icon per window. Multiple windows from the same app have separate icons.
Click a workspace label or an ordinary window icon to switch to that workspace.
The focused workspace label is white; other labels are gray.

Scratchpad icons follow the window's current placement:

| Window state | Icon location | Clicking the icon |
| --- | --- | --- |
| Shown on a workspace | That workspace | Hides the window in the scratchpad |
| Hidden in the scratchpad | `S:` | Shows the window on the current workspace |

`S:` appears when hidden scratchpad windows have displayable app IDs or X11
classes and disappears when none remain. Its label does not switch workspaces.
Icons stay at full brightness. The bar remembers no former workspace or "home."

The bar installs no keyboard bindings and enforces no one-window scratchpad
limit. Those policies belong in the Sway configuration or a separate helper.

## Status and appearance

The right side shows aggregate CPU usage, available RAM in GiB, the first running
non-loopback IPv4 address (or `down`), local date and time, and keyboard layout.
Available RAM is displayed as a value such as `3.2G`, without a `MEM` label.
The first keyboard in Sway's input list supplies the layout: English is `EN`,
Bulgarian is `BG`, and other layout names use their first two letters.

The bar uses Liberation Mono 14, a 34-pixel height, 22-pixel icons, background
`#242424`, and foreground `#ffffff`. Appearance is compiled into `main.c`;
`theme.conf.in` is an unused template, not a runtime configuration file.
Install the Liberation Mono font to reproduce the intended appearance.

## App icons

Wayland windows use their app ID; XWayland windows fall back to their X11 class.
Desktop entries resolve through GIO's XDG lookup, then `StartupWMClass` and
relaxed case/separator matching (for example, `unity-hub` and
`unityhub.desktop`). Ambiguous fallback matches are skipped. The entry's
`Icon=` value supplies the icon name or absolute path.

PNG/SVG lookup searches user and system XDG icon directories, `~/.icons`, theme
inheritance, and pixmaps. The theme comes from `gtk-icon-theme-name` in
`$XDG_CONFIG_HOME/gtk-3.0/settings.ini` (normally
`~/.config/gtk-3.0/settings.ini`), with hicolor, Adwaita, and AdwaitaLegacy
fallbacks. Missing icons use a built-in window glyph.

Desktop entries and icon surfaces are cached. Restart the bar after changing
desktop entries, installed icons, or the icon-theme setting. GTK is not a runtime
toolkit dependency.

## Build

Build tools: GCC, GNU Make, `pkg-config`, and `wayland-scanner`. The Makefile
requires development headers and libraries for these pkg-config modules:

```text
wayland-client cairo pango pangocairo libcjson librsvg-2.0 gio-unix-2.0
```

A fresh checkout also requires protocol XML files from `wayland-protocols` and
`wlr-protocols`. Generated protocol files are ignored by Git. Defaults are:

```text
WLR_XML=/usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml
XDG_XML=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
```

```sh
make
```

Override either XML location when necessary:

```sh
make WLR_XML=/path/to/wlr-layer-shell-unstable-v1.xml XDG_XML=/path/to/xdg-shell.xml
```

Existing generated files can be reused without the layer-shell XML. `make clean`
removes the binary and generated files, so rebuilding afterward requires both
protocol sources.

## Install and run

```sh
make install
~/.local/bin/bar
```

Run inside a Sway session with `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, and
`SWAYSOCK` set. Installation copies the binary to `~/.local/bin/bar` by default;
use `make install PREFIX=/another/prefix` to change that destination. Building
alone never replaces the installed binary. Restart an already running bar after
installing an update.

For automatic startup, add this to the Sway configuration:

```text
exec ~/.local/bin/bar
```

## Scheduling and limits

Workspace and scratchpad membership refresh from Sway events, using the existing
workspace-list and tree queries. Window title, focus, and mark events skip those
queries. Event bursts coalesce into an in-flight refresh plus a pending follow-up.
Scratchpad support adds no polling, timers, or helper processes to the bar.

The status timer samples CPU/memory and updates the clock once per second;
IPv4 refreshes every ten seconds. Rendering reuses cached icon surfaces.
Nonblocking framed IPC bounds reads and uses a three-second request deadline.
Two release-tracked drawing buffers and frame callbacks handle resize and
coalesce presentation. The bar exits on compositor/IPC loss rather than
reconnecting automatically.

The display is capped at 16 groups and 32 window icons per group. `S:` consumes
one group; if 16 regular workspaces already fill the model, `S:` is omitted.
Excess icons are omitted without limiting the windows held by Sway. Workspace
names and app IDs also use fixed-size buffers (127 and 63 bytes respectively).
There is no overflow indicator or shared width budget, so crowded workspace
content can overlap the status modules. Output scaling is not handled explicitly.

## Checks and source layout

```sh
make check
```

This runs icon lookup/loading/cache checks, workspace/event/buffer state checks,
scratchpad placement/rendering/click checks, and socket-pair IPC checks. The
state and scratchpad suites can also run separately:

```sh
make check-state check-scratchpad
```

`make check-integration` additionally requires Sway and Python 3. It starts an
isolated headless compositor and checks startup, buffer release/reuse, four
resizes, and disconnect handling. It does not exercise scratchpad clicks against
a live compositor. Socket restrictions can prevent the IPC or integration
checks from running.

`main.c` owns state, scheduling, icons, and rendering; `ipc.c` handles socket
transport; `buffers.c` owns the Wayland/Cairo drawing buffers.
[ARCHITECTURE.md](ARCHITECTURE.md) preserves the original optimization review
and identifies which work has since been implemented.
