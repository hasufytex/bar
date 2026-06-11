/* bar - minimal sway status bar prototype
 *
 * left : every workspace: number + icons of apps on it (focused = accent pill)
 * right: ipv4   ram avail   CPU%   date   time (1s tick)
 *
 * deps: wayland-client, cairo, pango, pangocairo, cjson
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <cjson/cJSON.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define BAR_HEIGHT    34
#define ICON_SIZE     22
#define MAX_APPS      32
#define RAM_CACHE_MAX 20
#define FONT          "JetBrainsMono Nerd Font 14" /* follows eww.scss */

/* ---------------- wayland globals ---------------- */

static struct wl_display            *display;
static struct wl_compositor         *compositor;
static struct wl_shm                *shm;
static struct zwlr_layer_shell_v1   *layer_shell;
static struct wl_seat               *seat;
static struct wl_pointer            *pointer;
static struct wl_surface            *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_buffer             *buffer;
static void                         *shm_data;

static uint32_t bar_width = 0;
static bool     configured = false;

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t ver)
{
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h)
{
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0) bar_width = w;
    configured = true;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    exit(0);
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed    = layer_closed,
};

/* ---------------- theme ----------------
 * Colors come from ~/.config/bar/theme.conf, rendered from theme.conf.in by
 * toggle_theme.sh (the single source of truth for OS theming). The file is
 * re-read whenever its mtime changes, so a theme toggle restyles the running
 * bar on the next redraw. Fallbacks below are catppuccin mocha. */

typedef struct { double r, g, b; } Color;

static Color  th_bg, th_fg, th_ws_num, th_focused_bg, th_focused_fg, th_urgent_bg;
static double th_bg_alpha = 1.0;
static char   theme_path[512];

static void parse_hex(const char *s, Color *c)
{
    unsigned r, g, b;
    if (s[0] == '#') s++;
    if (sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3) return;
    c->r = r / 255.0; c->g = g / 255.0; c->b = b / 255.0;
}

static void load_theme(void)
{
    parse_hex("#1e1e2e", &th_bg);
    parse_hex("#cdd6f4", &th_fg);
    parse_hex("#7f849c", &th_ws_num);
    parse_hex("#89b4fa", &th_focused_bg);
    parse_hex("#1e1e2e", &th_focused_fg);
    parse_hex("#f38ba8", &th_urgent_bg);
    th_bg_alpha = 1.0;

    FILE *f = fopen(theme_path, "r");
    if (!f) return;
    char line[128], key[32], val[64];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31[a-z_]=%63s", key, val) != 2) continue;
        if      (!strcmp(key, "bg"))         parse_hex(val, &th_bg);
        else if (!strcmp(key, "bg_alpha"))   th_bg_alpha = atof(val);
        else if (!strcmp(key, "fg"))         parse_hex(val, &th_fg);
        else if (!strcmp(key, "ws_num_fg"))  parse_hex(val, &th_ws_num);
        else if (!strcmp(key, "focused_bg")) parse_hex(val, &th_focused_bg);
        else if (!strcmp(key, "focused_fg")) parse_hex(val, &th_focused_fg);
        else if (!strcmp(key, "urgent_bg"))  parse_hex(val, &th_urgent_bg);
    }
    fclose(f);
}

static void maybe_reload_theme(void)
{
    static time_t last_mtime;
    struct stat st;
    if (stat(theme_path, &st) == 0 && st.st_mtime != last_mtime) {
        last_mtime = st.st_mtime;
        load_theme();
    }
}

/* ---------------- icon cache ---------------- */

typedef struct {
    char app_id[64];
    cairo_surface_t *surface; /* NULL allowed: caches "not found" too */
} IconCacheEntry;

static IconCacheEntry ram_cache[RAM_CACHE_MAX];
static int ram_cache_count = 0;

static bool ram_cache_get(const char *app_id, cairo_surface_t **out)
{
    for (int i = 0; i < ram_cache_count; i++) {
        if (!strcmp(ram_cache[i].app_id, app_id)) {
            *out = ram_cache[i].surface;
            return true;
        }
    }
    return false;
}

static void ram_cache_set(const char *app_id, cairo_surface_t *s)
{
    if (ram_cache_count < RAM_CACHE_MAX) {
        snprintf(ram_cache[ram_cache_count].app_id, 64, "%s", app_id);
        ram_cache[ram_cache_count].surface = s;
        ram_cache_count++;
        return;
    }
    /* evict oldest (FIFO) */
    if (ram_cache[0].surface)
        cairo_surface_destroy(ram_cache[0].surface);
    memmove(&ram_cache[0], &ram_cache[1],
            sizeof(IconCacheEntry) * (RAM_CACHE_MAX - 1));
    snprintf(ram_cache[RAM_CACHE_MAX - 1].app_id, 64, "%s", app_id);
    ram_cache[RAM_CACHE_MAX - 1].surface = s;
}

/* read Icon= from the app's .desktop file, fall back to app_id itself */
static void desktop_icon_name(const char *app_id, char *out, size_t out_len)
{
    char path[512];
    const char *home = getenv("HOME");
    const char *fmts[] = {
        "%s/.local/share/applications/%s.desktop",
        "/usr/share/applications/%s.desktop",
        "/usr/local/share/applications/%s.desktop",
    };

    snprintf(out, out_len, "%s", app_id); /* fallback */

    for (int i = 0; i < 3; i++) {
        if (i == 0) {
            if (!home) continue;
            snprintf(path, sizeof(path), fmts[0], home, app_id);
        } else {
            snprintf(path, sizeof(path), fmts[i], app_id);
        }
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "Icon=", 5)) {
                line[strcspn(line, "\n")] = 0;
                snprintf(out, out_len, "%s", line + 5);
                fclose(f);
                return;
            }
        }
        fclose(f);
    }
}

static cairo_surface_t *try_png(const char *path)
{
    if (access(path, F_OK) != 0) return NULL;
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

static cairo_surface_t *icon_lookup(const char *app_id)
{
    char icon[256], path[512];
    desktop_icon_name(app_id, icon, sizeof(icon));

    /* absolute path in Icon= */
    if (icon[0] == '/')
        return try_png(icon);

    const char *dirs[] = {
        "/usr/share/icons/hicolor/48x48/apps/%s.png",
        "/usr/share/icons/hicolor/64x64/apps/%s.png",
        "/usr/share/icons/hicolor/32x32/apps/%s.png",
        "/usr/share/icons/hicolor/128x128/apps/%s.png",
        "/usr/share/pixmaps/%s.png",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), dirs[i], icon);
        cairo_surface_t *s = try_png(path);
        if (s) return s;
    }
    return NULL;
}

static cairo_surface_t *icon_get(const char *app_id)
{
    cairo_surface_t *s;
    if (ram_cache_get(app_id, &s))
        return s;
    s = icon_lookup(app_id);
    ram_cache_set(app_id, s); /* cache misses too, avoids re-walking dirs */
    return s;
}

/* ---------------- sway ipc ---------------- */

#define SWAY_RUN_COMMAND    0
#define SWAY_GET_WORKSPACES 1
#define SWAY_SUBSCRIBE      2
#define SWAY_GET_TREE       4
#define SWAY_GET_INPUTS     100

static int sway_cmd_fd = -1; /* query/command socket, shared with input */

static int sway_connect(void)
{
    const char *sock = getenv("SWAYSOCK");
    if (!sock) {
        fprintf(stderr, "SWAYSOCK not set\n");
        exit(1);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("sway connect");
        exit(1);
    }
    return fd;
}

static void write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) { perror("write"); exit(1); }
        p += n; len -= n;
    }
}

static void read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) { perror("read"); exit(1); }
        p += n; len -= n;
    }
}

static void sway_send(int fd, uint32_t type, const char *payload)
{
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    char hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    memcpy(hdr + 6, &len, 4);
    memcpy(hdr + 10, &type, 4);
    write_all(fd, hdr, 14);
    if (len) write_all(fd, payload, len);
}

/* caller frees */
static char *sway_recv(int fd)
{
    char hdr[14];
    uint32_t len;
    read_all(fd, hdr, 14);
    memcpy(&len, hdr + 6, 4);
    char *buf = malloc(len + 1);
    read_all(fd, buf, len);
    buf[len] = 0;
    return buf;
}

/* ---------------- bar state ---------------- */

#define MAX_WS 16

typedef struct {
    int  num;
    char name[128];
    bool focused;
    bool urgent;
    char apps[MAX_APPS][64];
    int  app_count;
} Workspace;

static Workspace workspaces[MAX_WS];
static int ws_count = 0;

static void collect_apps(cJSON *node, Workspace *ws)
{
    cJSON *app_id = cJSON_GetObjectItem(node, "app_id");
    if (cJSON_IsString(app_id) && ws->app_count < MAX_APPS)
        snprintf(ws->apps[ws->app_count++], 64, "%s", app_id->valuestring);

    /* xwayland windows have no app_id; use the X11 class instead */
    if (!cJSON_IsString(app_id)) {
        cJSON *props = cJSON_GetObjectItem(node, "window_properties");
        cJSON *cls = props ? cJSON_GetObjectItem(props, "class") : NULL;
        if (cJSON_IsString(cls) && ws->app_count < MAX_APPS)
            snprintf(ws->apps[ws->app_count++], 64, "%s", cls->valuestring);
    }

    const char *kids[] = { "nodes", "floating_nodes" };
    for (int k = 0; k < 2; k++) {
        cJSON *arr = cJSON_GetObjectItem(node, kids[k]);
        cJSON *child;
        cJSON_ArrayForEach(child, arr)
            collect_apps(child, ws);
    }
}

static cJSON *find_workspace(cJSON *node, const char *name)
{
    cJSON *type = cJSON_GetObjectItem(node, "type");
    cJSON *nm   = cJSON_GetObjectItem(node, "name");
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "workspace") &&
        cJSON_IsString(nm) && !strcmp(nm->valuestring, name))
        return node;

    cJSON *arr = cJSON_GetObjectItem(node, "nodes");
    cJSON *child;
    cJSON_ArrayForEach(child, arr) {
        cJSON *found = find_workspace(child, name);
        if (found) return found;
    }
    return NULL;
}

static void update_workspace_state(int cmd_fd)
{
    ws_count = 0;

    /* all workspaces */
    sway_send(cmd_fd, SWAY_GET_WORKSPACES, NULL);
    char *reply = sway_recv(cmd_fd);
    cJSON *wss = cJSON_Parse(reply);
    free(reply);
    if (!wss) return;

    cJSON *ws;
    cJSON_ArrayForEach(ws, wss) {
        if (ws_count >= MAX_WS) break;
        Workspace *w = &workspaces[ws_count];
        cJSON *num     = cJSON_GetObjectItem(ws, "num");
        cJSON *name    = cJSON_GetObjectItem(ws, "name");
        cJSON *focused = cJSON_GetObjectItem(ws, "focused");
        cJSON *urgent  = cJSON_GetObjectItem(ws, "urgent");
        w->num = cJSON_IsNumber(num) ? num->valueint : -1;
        snprintf(w->name, sizeof(w->name), "%s",
                 cJSON_IsString(name) ? name->valuestring : "");
        w->focused = cJSON_IsTrue(focused);
        w->urgent  = cJSON_IsTrue(urgent);
        w->app_count = 0;
        ws_count++;
    }
    cJSON_Delete(wss);
    if (ws_count == 0) return;

    /* apps on each workspace, one tree query */
    sway_send(cmd_fd, SWAY_GET_TREE, NULL);
    reply = sway_recv(cmd_fd);
    cJSON *tree = cJSON_Parse(reply);
    free(reply);
    if (!tree) return;

    for (int i = 0; i < ws_count; i++) {
        cJSON *wsnode = find_workspace(tree, workspaces[i].name);
        if (wsnode)
            collect_apps(wsnode, &workspaces[i]);
    }
    cJSON_Delete(tree);
}

/* ---------------- input: clickable workspaces ----------------
 * draw() records each pill's x-range; a left click hit-tests against them
 * and switches workspace via sway IPC. */

typedef struct { int x0, x1, num; } WsHit;
static WsHit  ws_hits[MAX_WS];
static int    ws_hit_count = 0;
static double pointer_x = -1;

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy)
{
    pointer_x = wl_fixed_to_double(sx);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s)
{
    pointer_x = -1;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy)
{
    pointer_x = wl_fixed_to_double(sx);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state)
{
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    for (int i = 0; i < ws_hit_count; i++) {
        if (pointer_x >= ws_hits[i].x0 && pointer_x < ws_hits[i].x1) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "workspace number %d", ws_hits[i].num);
            sway_send(sway_cmd_fd, SWAY_RUN_COMMAND, cmd);
            free(sway_recv(sway_cmd_fd));
            break;
        }
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter  = ptr_enter,
    .leave  = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis   = ptr_axis,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

/* ---------------- right side stats ---------------- */

static void get_ipv4(char *out, size_t len)
{
    struct ifaddrs *ifs, *ifa;
    snprintf(out, len, "down");
    if (getifaddrs(&ifs) < 0) return;
    for (ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_RUNNING)) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, out, len);
        break;
    }
    freeifaddrs(ifs);
}

static void get_ram(char *out, size_t len)
{
    long avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { snprintf(out, len, "?"); return; }
    char line[128];
    while (fgets(line, sizeof(line), f))
        sscanf(line, "MemAvailable: %ld kB", &avail);
    fclose(f);
    snprintf(out, len, "%.1fG", avail / 1048576.0);
}

/* active keyboard layout via sway GET_INPUTS: "English (US)" -> EN,
 * "Bulgarian (phonetic)" -> BG; unknown languages get their first two
 * letters uppercased */
static void get_lang(char *out, size_t len)
{
    snprintf(out, len, "??");

    sway_send(sway_cmd_fd, SWAY_GET_INPUTS, NULL);
    char *reply = sway_recv(sway_cmd_fd);
    cJSON *inputs = cJSON_Parse(reply);
    free(reply);
    if (!inputs) return;

    cJSON *dev;
    cJSON_ArrayForEach(dev, inputs) {
        cJSON *type   = cJSON_GetObjectItem(dev, "type");
        cJSON *layout = cJSON_GetObjectItem(dev, "xkb_active_layout_name");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "keyboard"))
            continue;
        if (!cJSON_IsString(layout))
            continue;
        const char *name = layout->valuestring;
        if      (!strncmp(name, "English", 7))   snprintf(out, len, "EN");
        else if (!strncmp(name, "Bulgarian", 9)) snprintf(out, len, "BG");
        else if (name[0] && name[1])
            snprintf(out, len, "%c%c", toupper(name[0]), toupper(name[1]));
        break;
    }
    cJSON_Delete(inputs);
}

static int get_cpu_pct(void)
{
    static long prev_idle = 0, prev_total = 0;
    long u, n, s, i, w, irq, sirq, st;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
               &u, &n, &s, &i, &w, &irq, &sirq, &st) != 8) {
        fclose(f);
        return -1;
    }
    fclose(f);

    long idle = i + w;
    long total = u + n + s + i + w + irq + sirq + st;
    long d_total = total - prev_total;
    long d_idle  = idle  - prev_idle;
    prev_total = total;
    prev_idle  = idle;

    if (d_total <= 0) return 0;
    return (int)(100 * (d_total - d_idle) / d_total);
}

/* ---------------- drawing ---------------- */

#define MODULE_GAP 48 /* px between right-side modules (no separators, like eww) */

static void draw(void)
{
    if (!configured || !shm_data) return;

    maybe_reload_theme();

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        shm_data, CAIRO_FORMAT_ARGB32, bar_width, BAR_HEIGHT, bar_width * 4);
    cairo_t *cr = cairo_create(cs);

    /* background: SOURCE so the translucent base replaces the old frame
     * instead of blending with it */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, th_bg.r, th_bg.g, th_bg.b, th_bg_alpha);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(FONT);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int tw, th;

    /* ---- left: every workspace as an eww-style pill: number + icons.
     * Focused gets an accent background, urgent a red one (like eww.scss).
     * Geometry mirrors eww: padding 2px 10px, icon spacing 3, gap 6. ---- */
    int x = 10;
    ws_hit_count = 0;
    for (int w = 0; w < ws_count; w++) {
        Workspace *ws = &workspaces[w];
        char wsbuf[16];
        snprintf(wsbuf, sizeof(wsbuf), "%d:", ws->num);
        pango_layout_set_text(layout, wsbuf, -1);
        pango_layout_get_pixel_size(layout, &tw, &th);

        cairo_surface_t *icons[MAX_APPS];
        int n_icons = 0;
        for (int a = 0; a < ws->app_count; a++) {
            cairo_surface_t *icon = icon_get(ws->apps[a]);
            if (icon && cairo_image_surface_get_width(icon) > 0)
                icons[n_icons++] = icon;
        }
        int content_w = tw + n_icons * (ICON_SIZE + 3);

        ws_hits[ws_hit_count++] = (WsHit){
            .x0 = x, .x1 = x + content_w + 20, .num = ws->num,
        };

        if (ws->focused || ws->urgent) {
            Color *bg = ws->urgent ? &th_urgent_bg : &th_focused_bg;
            cairo_set_source_rgb(cr, bg->r, bg->g, bg->b);
            cairo_rectangle(cr, x, 4, content_w + 20, BAR_HEIGHT - 8);
            cairo_fill(cr);
        }

        Color *numc = (ws->focused || ws->urgent) ? &th_focused_fg : &th_ws_num;
        cairo_set_source_rgb(cr, numc->r, numc->g, numc->b);
        cairo_move_to(cr, x + 10, (BAR_HEIGHT - th) / 2.0);
        pango_cairo_show_layout(cr, layout);

        int ix = x + 10 + tw + 3;
        for (int a = 0; a < n_icons; a++) {
            int iw = cairo_image_surface_get_width(icons[a]);
            double scale = (double)ICON_SIZE / iw;
            cairo_save(cr);
            cairo_translate(cr, ix, (BAR_HEIGHT - ICON_SIZE) / 2.0);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, icons[a], 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            ix += ICON_SIZE + 3;
        }

        x += content_w + 20 + 6;
    }

    /* ---- right: modules drawn right-to-left with a fixed pixel gap ---- */
    char ip[64], ram[32], cpubuf[16], datebuf[16], timebuf[16], lang[8];
    get_ipv4(ip, sizeof(ip));
    get_ram(ram, sizeof(ram));
    get_lang(lang, sizeof(lang));
    int cpu = get_cpu_pct();
    snprintf(cpubuf, sizeof(cpubuf), "CPU %d%%", cpu < 0 ? 0 : cpu);

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", tm);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

    const char *modules[] = { cpubuf, ram, ip, datebuf, timebuf, lang };
    int n_modules = sizeof(modules) / sizeof(modules[0]);

    cairo_set_source_rgb(cr, th_fg.r, th_fg.g, th_fg.b);
    int rx = bar_width - 10;
    for (int m = n_modules - 1; m >= 0; m--) {
        pango_layout_set_text(layout, modules[m], -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        rx -= tw;
        cairo_move_to(cr, rx, (BAR_HEIGHT - th) / 2.0);
        pango_cairo_show_layout(cr, layout);
        rx -= MODULE_GAP;
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, bar_width, BAR_HEIGHT);
    wl_surface_commit(surface);
    wl_display_flush(display);
}

/* ---------------- main ---------------- */

int main(void)
{
    const char *home = getenv("HOME");
    snprintf(theme_path, sizeof(theme_path), "%s/.config/bar/theme.conf",
             home ? home : "");
    load_theme();

    /* wayland setup */
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "no wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "missing wayland globals (is wlr layer shell supported?)\n");
        return 1;
    }

    if (seat)
        wl_seat_add_listener(seat, &seat_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, BAR_HEIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, BAR_HEIGHT);
    wl_surface_commit(surface);

    while (!configured)
        wl_display_dispatch(display);

    if (bar_width == 0) bar_width = 1920;

    /* shm buffer */
    int stride = bar_width * 4;
    int size = stride * BAR_HEIGHT;
    int fd = memfd_create("bar", 0);
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); return 1; }
    shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    buffer = wl_shm_pool_create_buffer(pool, 0, bar_width, BAR_HEIGHT,
                                       stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* sway: one socket for queries/commands, one subscribed to events */
    int cmd_fd = sway_cmd_fd = sway_connect();
    int evt_fd = sway_connect();
    sway_send(evt_fd, SWAY_SUBSCRIBE, "[\"window\",\"workspace\",\"input\"]");
    free(sway_recv(evt_fd)); /* {"success": true} */

    /* 1s timer */
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec ts = {
        .it_interval = { .tv_sec = 1 },
        .it_value    = { .tv_sec = 1 },
    };
    timerfd_settime(timer_fd, 0, &ts, NULL);

    get_cpu_pct(); /* prime the cpu delta */
    update_workspace_state(cmd_fd);
    draw();

    struct pollfd fds[] = {
        { .fd = wl_display_get_fd(display), .events = POLLIN },
        { .fd = evt_fd,                     .events = POLLIN },
        { .fd = timer_fd,                   .events = POLLIN },
    };

    for (;;) {
        wl_display_flush(display);
        if (poll(fds, 3, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(display) < 0)
                break;
        }

        if (fds[1].revents & POLLIN) {
            free(sway_recv(evt_fd));        /* drain event, content doesn't matter */
            update_workspace_state(cmd_fd); /* just re-query, simplest + robust */
            draw();
        }

        if (fds[2].revents & POLLIN) {
            uint64_t ticks;
            if (read(timer_fd, &ticks, 8) != 8) {}
            draw();
        }
    }
    return 0;
}
