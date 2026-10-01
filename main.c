/* bar - minimal sway status bar prototype
 *
 * left : every workspace: number + icons of apps on it (focused number = white)
 * right: ipv4   ram avail   CPU%   date   time (1s tick)
 *
 * deps: wayland-client, cairo, pango, pangocairo, cjson, librsvg-2.0
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input-event-codes.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <cjson/cJSON.h>
#include <gio/gdesktopappinfo.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>

#include "buffers.h"
#include "ipc.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define BAR_HEIGHT 34
#define ICON_SIZE 22
#define MAX_APPS 32
#define FONT "Liberation Mono 14"

/* ---------------- wayland globals ---------------- */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static BarBuffer buffers[2];
static struct wl_callback *frame;
static bool dirty = true, running = true;
static uint32_t bar_height = BAR_HEIGHT;

static uint32_t bar_width = 0;
static bool configured = false;

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t ver) {
  if (!strcmp(iface, wl_compositor_interface.name))
    compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                  ver < 4 ? ver : 4);
  else if (!strcmp(iface, wl_shm_interface.name))
    shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
    layer_shell =
        wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
  else if (!strcmp(iface, wl_seat_interface.name))
    seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  if (w > 0)
    bar_width = w;
  if (!bar_width)
    bar_width = 1920;
  if (h > 0)
    bar_height = h;
  configured = true;
  dirty = true;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
  running = false;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* ---------------- appearance ----------------
 * Match foot's Liberation Mono:size=14 and default colors directly. */

typedef struct {
  double r, g, b;
} Color;

static const Color bar_bg = {36 / 255.0, 36 / 255.0, 36 / 255.0};
static const Color bar_fg = {1.0, 1.0, 1.0};
static const Color ws_inactive_fg = {136 / 255.0, 136 / 255.0, 136 / 255.0};

/* ---------------- icon cache ---------------- */

typedef struct {
  cairo_surface_t *surface;
  unsigned generation;
} IconCacheEntry;

static GHashTable *ram_cache;
static unsigned icon_generation;

static void icon_entry_free(void *data) {
  IconCacheEntry *entry = data;
  cairo_surface_destroy(entry->surface);
  free(entry);
}

static bool ram_cache_get(const char *app_id, cairo_surface_t **out) {
  IconCacheEntry *entry =
      ram_cache ? g_hash_table_lookup(ram_cache, app_id) : NULL;
  if (!entry)
    return false;
  entry->generation = icon_generation;
  *out = entry->surface;
  return true;
}

static void ram_cache_set(const char *app_id, cairo_surface_t *s) {
  if (!ram_cache)
    ram_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, icon_entry_free);
  IconCacheEntry *entry = g_new0(IconCacheEntry, 1);
  entry->surface = s;
  entry->generation = icon_generation;
  g_hash_table_replace(ram_cache, g_strdup(app_id), entry);
}

static gboolean icon_unused(void *key, void *value, void *data) {
  return ((IconCacheEntry *)value)->generation != icon_generation;
}

/* Resolve the desktop ID first, then StartupWMClass. GIO handles XDG data
 * directories, user overrides, and the Desktop Entry group. The fallback
 * catalog is loaded once; restart the bar after installing desktop entries. */
static void desktop_icon_name(const char *app_id, char *out, size_t out_len) {
  static GList *apps;
  static bool catalog_loaded;
  char *desktop_id = g_strconcat(app_id, ".desktop", NULL);
  GDesktopAppInfo *app = g_desktop_app_info_new(desktop_id);
  g_free(desktop_id);

  if (!app) {
    if (!catalog_loaded) {
      apps = g_app_info_get_all();
      catalog_loaded = true;
    }
    for (GList *it = apps; it; it = it->next) {
      if (!G_IS_DESKTOP_APP_INFO(it->data))
        continue;
      const char *wm_class = g_desktop_app_info_get_startup_wm_class(it->data);
      if (wm_class && !strcmp(wm_class, app_id)) {
        app = g_object_ref(it->data);
        break;
      }
    }
  }

  char *icon = app ? g_desktop_app_info_get_string(app, "Icon") : NULL;
  snprintf(out, out_len, "%s", icon && *icon ? icon : app_id);
  g_free(icon);
  if (app)
    g_object_unref(app);
}

static cairo_surface_t *try_png(const char *path) {
  if (access(path, F_OK) != 0)
    return NULL;
  cairo_surface_t *s = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return NULL;
  }
  int width = cairo_image_surface_get_width(s);
  int height = cairo_image_surface_get_height(s);
  if (width <= 0 || height <= 0) {
    cairo_surface_destroy(s);
    return NULL;
  }
  cairo_surface_t *scaled =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
  cairo_t *cr = cairo_create(scaled);
  double scale = (double)ICON_SIZE / (width > height ? width : height);
  cairo_translate(cr, (ICON_SIZE - width * scale) / 2,
                  (ICON_SIZE - height * scale) / 2);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, s, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
  return scaled;
}

static cairo_surface_t *try_svg(const char *path) {
  if (access(path, F_OK) != 0)
    return NULL;
  RsvgHandle *h = rsvg_handle_new_from_file(path, NULL);
  if (!h)
    return NULL;
  cairo_surface_t *s =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
  cairo_t *cr = cairo_create(s);
  RsvgRectangle vp = {0, 0, ICON_SIZE, ICON_SIZE};
  gboolean rendered = rsvg_handle_render_document(h, cr, &vp, NULL);
  cairo_destroy(cr);
  g_object_unref(h);
  if (!rendered || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return NULL;
  }
  return s;
}

static cairo_surface_t *icon_lookup(const char *app_id) {
  char icon[256], path[512];
  desktop_icon_name(app_id, icon, sizeof(icon));

  /* absolute path in Icon= */
  if (icon[0] == '/') {
    cairo_surface_t *s = try_png(icon);
    return s ? s : try_svg(icon);
  }

  const char *dirs[] = {
      "/usr/share/icons/hicolor/48x48/apps/%s.png",
      "/usr/share/icons/hicolor/64x64/apps/%s.png",
      "/usr/share/icons/hicolor/32x32/apps/%s.png",
      "/usr/share/icons/hicolor/128x128/apps/%s.png",
      "/usr/share/icons/hicolor/256x256/apps/%s.png",
      "/usr/share/icons/hicolor/512x512/apps/%s.png", /* e.g. zed ships only
                                                         this */
      "/usr/share/pixmaps/%s.png",
  };
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    snprintf(path, sizeof(path), dirs[i], icon);
    cairo_surface_t *s = try_png(path);
    if (s)
      return s;
  }

  const char *svg_dirs[] = {
      "/usr/share/icons/hicolor/scalable/apps/%s.svg",
      "/usr/share/pixmaps/%s.svg",
  };
  for (size_t i = 0; i < sizeof(svg_dirs) / sizeof(svg_dirs[0]); i++) {
    snprintf(path, sizeof(path), svg_dirs[i], icon);
    cairo_surface_t *s = try_svg(path);
    if (s)
      return s;
  }
  return NULL;
}

/* A built-in window glyph keeps apps visible even without installed icons. */
static cairo_surface_t *generic_app_icon(void) {
  cairo_surface_t *s =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
  cairo_set_line_width(cr, 1.5);
  cairo_rectangle(cr, 3, 4, ICON_SIZE - 6, ICON_SIZE - 8);
  cairo_move_to(cr, 3, 8);
  cairo_line_to(cr, ICON_SIZE - 3, 8);
  cairo_stroke(cr);
  cairo_destroy(cr);
  return s;
}

static cairo_surface_t *icon_get(const char *app_id) {
  cairo_surface_t *s;
  if (ram_cache_get(app_id, &s))
    return s;
  s = icon_lookup(app_id);
  if (!s) {
    static cairo_surface_t *fallback;
    if (!fallback)
      fallback = generic_app_icon();
    s = cairo_surface_reference(fallback);
  }
  ram_cache_set(app_id, s);
  return s;
}

/* ---------------- sway ipc ---------------- */

#define SWAY_RUN_COMMAND 0
#define SWAY_GET_WORKSPACES 1
#define SWAY_SUBSCRIBE 2
#define SWAY_GET_TREE 4
#define SWAY_GET_INPUTS 100
#define SWAY_EVT_INPUT 21 /* event type field with the high bit masked off */

static Ipc command_ipc = {.fd = -1}, event_ipc = {.fd = -1};
static bool want_ws = true, want_lang = true, subscribed;
static char pending_command[64];

static int sway_connect(void) {
  const char *sock = getenv("SWAYSOCK");
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (!sock || strlen(sock) >= sizeof(addr.sun_path)) {
    fprintf(stderr, "SWAYSOCK missing or too long\n");
    return -1;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  memcpy(addr.sun_path, sock, strlen(sock) + 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 &&
      errno != EINPROGRESS) {
    perror("sway connect");
    close(fd);
    return -1;
  }
  return fd;
}

/* ---------------- bar state ---------------- */

#define MAX_WS 16

typedef struct {
  int num;
  char name[128];
  bool focused;
  bool urgent;
  char apps[MAX_APPS][64];
  cairo_surface_t *icons[MAX_APPS];
  int app_count;
} Workspace;

static Workspace workspaces[MAX_WS];
static int ws_count = 0;

static void collect_apps(cJSON *node, Workspace *ws) {
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

  const char *kids[] = {"nodes", "floating_nodes"};
  for (int k = 0; k < 2; k++) {
    cJSON *arr = cJSON_GetObjectItem(node, kids[k]);
    cJSON *child;
    cJSON_ArrayForEach(child, arr) collect_apps(child, ws);
  }
}

static Workspace next_workspaces[MAX_WS];
static int next_ws_count;

static bool parse_workspaces(cJSON *wss) {
  if (!cJSON_IsArray(wss))
    return false;
  next_ws_count = 0;
  memset(next_workspaces, 0, sizeof(next_workspaces));
  cJSON *ws;
  cJSON_ArrayForEach(ws, wss) {
    if (next_ws_count >= MAX_WS)
      break;
    Workspace *w = &next_workspaces[next_ws_count++];
    cJSON *num = cJSON_GetObjectItem(ws, "num");
    cJSON *name = cJSON_GetObjectItem(ws, "name");
    w->num = cJSON_IsNumber(num) ? num->valueint : -1;
    snprintf(w->name, sizeof(w->name), "%s",
             cJSON_IsString(name) ? name->valuestring : "");
    w->focused = cJSON_IsTrue(cJSON_GetObjectItem(ws, "focused"));
    w->urgent = cJSON_IsTrue(cJSON_GetObjectItem(ws, "urgent"));
  }
  return true;
}

/* Walk the tree once; workspace subtrees are consumed by collect_apps. */
static void collect_workspaces(cJSON *node) {
  cJSON *type = cJSON_GetObjectItem(node, "type");
  cJSON *name = cJSON_GetObjectItem(node, "name");
  if (cJSON_IsString(type) && !strcmp(type->valuestring, "workspace")) {
    for (int i = 0; cJSON_IsString(name) && i < next_ws_count; i++)
      if (!strcmp(name->valuestring, next_workspaces[i].name)) {
        collect_apps(node, &next_workspaces[i]);
        break;
      }
    return;
  }
  cJSON *child;
  cJSON_ArrayForEach(child, cJSON_GetObjectItem(node, "nodes"))
      collect_workspaces(child);
  cJSON_ArrayForEach(child, cJSON_GetObjectItem(node, "floating_nodes"))
      collect_workspaces(child);
}

static void publish_workspaces(cJSON *tree) {
  collect_workspaces(tree);
  ++icon_generation;
  for (int w = 0; w < next_ws_count; w++)
    for (int a = 0; a < next_workspaces[w].app_count; a++)
      next_workspaces[w].icons[a] = icon_get(next_workspaces[w].apps[a]);
  memcpy(workspaces, next_workspaces, sizeof(workspaces));
  ws_count = next_ws_count;
  if (ram_cache)
    g_hash_table_foreach_remove(ram_cache, icon_unused, NULL);
  dirty = true;
}

/* ---------------- input: clickable workspaces ----------------
 * draw() records each workspace's x-range; a left click hit-tests against them
 * and switches workspace via sway IPC. */

typedef struct {
  int x0, x1, num;
} WsHit;
static WsHit ws_hits[MAX_WS];
static int ws_hit_count = 0;
static double pointer_x = -1;

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
  pointer_x = wl_fixed_to_double(sx);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {
  pointer_x = -1;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy) {
  pointer_x = wl_fixed_to_double(sx);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
  if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
    return;
  for (int i = 0; i < ws_hit_count; i++) {
    if (pointer_x >= ws_hits[i].x0 && pointer_x < ws_hits[i].x1) {
      snprintf(pending_command, sizeof(pending_command), "workspace number %d",
               ws_hits[i].num);
      break;
    }
  }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
    pointer = wl_seat_get_pointer(s);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
    wl_pointer_destroy(pointer);
    pointer = NULL;
    pointer_x = -1;
  }
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

/* ---------------- right side stats ---------------- */

/* IPv4 is cached and refreshed every ten seconds — getifaddrs() is a netlink
 * dump, too heavy to run every second for a value that rarely changes */
static char ip_str[64];

static void refresh_ipv4(void) {
  struct ifaddrs *ifs, *ifa;
  snprintf(ip_str, sizeof(ip_str), "down");
  if (getifaddrs(&ifs) < 0)
    return;
  for (ifa = ifs; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr)
      continue;
    if (ifa->ifa_addr->sa_family != AF_INET)
      continue;
    if (ifa->ifa_flags & IFF_LOOPBACK)
      continue;
    if (!(ifa->ifa_flags & IFF_RUNNING))
      continue;
    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    inet_ntop(AF_INET, &sa->sin_addr, ip_str, sizeof(ip_str));
    break;
  }
  freeifaddrs(ifs);
}

static void get_ram(char *out, size_t len) {
  static int fd = -1;
  if (fd < 0)
    fd = open("/proc/meminfo", O_RDONLY);
  /* MemAvailable is on the 3rd line, well within 2k */
  char buf[2048];
  ssize_t n = fd < 0 ? -1 : pread(fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) {
    snprintf(out, len, "?");
    return;
  }
  buf[n] = 0;
  const char *p = strstr(buf, "MemAvailable:");
  long avail = p ? strtol(p + 13, NULL, 10) : 0;
  snprintf(out, len, "%.1fG", avail / 1048576.0);
}

/* active keyboard layout via sway GET_INPUTS: "English (US)" -> EN,
 * "Bulgarian (phonetic)" -> BG; unknown languages get their first two
 * letters uppercased. Cached: only queried at startup and on input events. */
static char lang_str[8] = "??";

static void refresh_lang(cJSON *inputs) {
  char *out = lang_str;
  size_t len = sizeof(lang_str);
  snprintf(out, len, "??");

  cJSON *dev;
  cJSON_ArrayForEach(dev, inputs) {
    cJSON *type = cJSON_GetObjectItem(dev, "type");
    cJSON *layout = cJSON_GetObjectItem(dev, "xkb_active_layout_name");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "keyboard"))
      continue;
    if (!cJSON_IsString(layout))
      continue;
    const char *name = layout->valuestring;
    if (!strncmp(name, "English", 7))
      snprintf(out, len, "EN");
    else if (!strncmp(name, "Bulgarian", 9))
      snprintf(out, len, "BG");
    else if (name[0] && name[1])
      snprintf(out, len, "%c%c", toupper((unsigned char)name[0]),
               toupper((unsigned char)name[1]));
    break;
  }
  dirty = true;
}

static int get_cpu_pct(void) {
  static long prev_idle = 0, prev_total = 0;
  static int fd = -1;
  if (fd < 0)
    fd = open("/proc/stat", O_RDONLY);
  char buf[256]; /* only the aggregate "cpu " first line is needed */
  ssize_t rn = fd < 0 ? -1 : pread(fd, buf, sizeof(buf) - 1, 0);
  if (rn <= 0)
    return -1;
  buf[rn] = 0;
  long u, n, s, i, w, irq, sirq, st;
  if (sscanf(buf, "cpu %ld %ld %ld %ld %ld %ld %ld %ld", &u, &n, &s, &i, &w,
             &irq, &sirq, &st) != 8)
    return -1;

  long idle = i + w;
  long total = u + n + s + i + w + irq + sirq + st;
  long d_total = total - prev_total;
  long d_idle = idle - prev_idle;
  prev_total = total;
  prev_idle = idle;

  if (d_total <= 0)
    return 0;
  return (int)(100 * (d_total - d_idle) / d_total);
}

static char ram[32], cpubuf[16] = "CPU ?", datebuf[16], timebuf[16];
static void sample_status(bool prime) {
  get_ram(ram, sizeof(ram));
  int cpu = get_cpu_pct();
  if (prime || cpu < 0)
    snprintf(cpubuf, sizeof(cpubuf), "CPU ?");
  else
    snprintf(cpubuf, sizeof(cpubuf), "CPU %d%%", cpu);
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);
  dirty = true;
}

static bool on_event(void *data, uint32_t type, const char *body) {
  cJSON *event = cJSON_Parse(body);
  if (!event)
    return false;
  if (type == SWAY_SUBSCRIBE) {
    subscribed = cJSON_IsTrue(cJSON_GetObjectItem(event, "success"));
    cJSON_Delete(event);
    return subscribed;
  }
  if (!(type & 0x80000000u)) {
    cJSON_Delete(event);
    return false;
  }
  cJSON *change = cJSON_GetObjectItem(event, "change");
  const char *what = cJSON_IsString(change) ? change->valuestring : "";
  type &= 0x7fffffffu;
  if (type == SWAY_EVT_INPUT) {
    if (!strcmp(what, "xkb_layout") || !strcmp(what, "added") ||
        !strcmp(what, "removed") || !strcmp(what, "xkb_keymap"))
      want_lang = true;
  } else if (type == 3) { /* window */
    if (strcmp(what, "title") && strcmp(what, "focus") && strcmp(what, "mark"))
      want_ws = true;
  } else if (type == 0) { /* workspace */
    want_ws = true;
  }
  cJSON_Delete(event);
  return true;
}

static bool on_reply(void *data, uint32_t type, const char *body) {
  cJSON *reply = cJSON_Parse(body);
  if (!reply)
    return false;
  bool ok = true;
  switch (type) {
  case SWAY_GET_WORKSPACES:
    ok = parse_workspaces(reply) &&
         ipc_request(&command_ipc, SWAY_GET_TREE, NULL);
    break;
  case SWAY_GET_TREE:
    ok = cJSON_IsObject(reply);
    if (ok)
      publish_workspaces(reply);
    break;
  case SWAY_GET_INPUTS:
    ok = cJSON_IsArray(reply);
    if (ok)
      refresh_lang(reply);
    break;
  case SWAY_RUN_COMMAND: {
    ok = cJSON_IsArray(reply);
    cJSON *result;
    cJSON_ArrayForEach(result, reply) if (!cJSON_IsTrue(cJSON_GetObjectItem(
                                              result, "success")))
        fprintf(stderr, "workspace command failed\n");
    break;
  }
  default:
    ok = false;
  }
  cJSON_Delete(reply);
  return ok;
}

static bool schedule_query(void) {
  if (!subscribed || command_ipc.pending)
    return true;
  if (*pending_command) {
    bool ok = ipc_request(&command_ipc, SWAY_RUN_COMMAND, pending_command);
    *pending_command = 0;
    return ok;
  }
  if (want_lang) {
    want_lang = false;
    return ipc_request(&command_ipc, SWAY_GET_INPUTS, NULL);
  }
  if (want_ws) {
    want_ws = false;
    return ipc_request(&command_ipc, SWAY_GET_WORKSPACES, NULL);
  }
  return true;
}

/* ---------------- drawing ---------------- */

#define MODULE_GAP                                                             \
  48 /* px between right-side modules (no separators, like eww) */

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t time) {
  wl_callback_destroy(callback);
  frame = NULL;
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static bool draw(void) {
  if (!configured || !dirty || frame)
    return true;
  BarBuffer *target = NULL;
  for (int i = 0; i < 2; i++)
    if (!buffers[i].busy) {
      target = &buffers[i];
      break;
    }
  if (!target)
    return true;
  if (!buffer_prepare(target, shm, bar_width, bar_height, FONT))
    return false;
  cairo_t *cr = target->cr;
  PangoLayout *layout = target->layout;

  /* Replace the previous frame with the terminal background. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgb(cr, bar_bg.r, bar_bg.g, bar_bg.b);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  int tw, th;

  /* ---- left: workspace numbers + icons; only the active number is white.
   * Padding 2px 10px, icon spacing 3, gap 6. ---- */
  int x = 10;
  ws_hit_count = 0;
  for (int w = 0; w < ws_count; w++) {
    Workspace *ws = &workspaces[w];
    char wsbuf[16];
    snprintf(wsbuf, sizeof(wsbuf), "%d:", ws->num);
    pango_layout_set_text(layout, wsbuf, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_surface_t **icons = ws->icons;
    int n_icons = ws->app_count;
    int content_w = tw + n_icons * (ICON_SIZE + 3);

    ws_hits[ws_hit_count++] = (WsHit){
        .x0 = x,
        .x1 = x + content_w + 20,
        .num = ws->num,
    };

    const Color *numc = ws->focused ? &bar_fg : &ws_inactive_fg;
    cairo_set_source_rgb(cr, numc->r, numc->g, numc->b);
    cairo_move_to(cr, x + 10, ((int)bar_height - th) / 2.0);
    pango_cairo_show_layout(cr, layout);

    int ix = x + 10 + tw + 3;
    for (int a = 0; a < n_icons; a++) {
      cairo_save(cr);
      cairo_set_source_surface(cr, icons[a], ix,
                               ((int)bar_height - ICON_SIZE) / 2.0);
      cairo_paint(cr);
      cairo_restore(cr);
      ix += ICON_SIZE + 3;
    }

    x += content_w + 20 + 6;
  }

  /* ---- right: modules drawn right-to-left with a fixed pixel gap ----
   * ip_str and lang_str are cached globals, refreshed outside draw() */
  const char *modules[] = {cpubuf, ram, ip_str, datebuf, timebuf, lang_str};
  int n_modules = sizeof(modules) / sizeof(modules[0]);

  cairo_set_source_rgb(cr, bar_fg.r, bar_fg.g, bar_fg.b);
  int rx = bar_width - 10;
  for (int m = n_modules - 1; m >= 0; m--) {
    pango_layout_set_text(layout, modules[m], -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    rx -= tw;
    cairo_move_to(cr, rx, ((int)bar_height - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    rx -= MODULE_GAP;
  }

  cairo_surface_flush(cairo_get_target(cr));
  wl_surface_attach(surface, target->buffer, 0, 0);
  if (wl_surface_get_version(surface) >= 4)
    wl_surface_damage_buffer(surface, 0, 0, bar_width, bar_height);
  else
    wl_surface_damage(surface, 0, 0, bar_width, bar_height);
  frame = wl_surface_frame(surface);
  wl_callback_add_listener(frame, &frame_listener, NULL);
  target->busy = true;
  wl_surface_commit(surface);
  dirty = false;
  return true;
}

/* ---------------- main ---------------- */

int main(void) {
  /* wayland setup */
  display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "no wayland display\n");
    return 1;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if (wl_display_roundtrip(display) < 0) {
    fprintf(stderr, "Wayland registry discovery failed\n");
    wl_display_disconnect(display);
    return 1;
  }

  if (!compositor || !shm || !layer_shell) {
    fprintf(stderr,
            "missing wayland globals (is wlr layer shell supported?)\n");
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

  command_ipc.fd = sway_connect();
  event_ipc.fd = sway_connect();
  if (command_ipc.fd < 0 || event_ipc.fd < 0 ||
      !ipc_request(&event_ipc, SWAY_SUBSCRIBE,
                   "[\"window\",\"workspace\",\"input\"]"))
    return 1;

  int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  struct itimerspec ts = {
      .it_interval = {.tv_sec = 1},
      .it_value = {.tv_sec = 1},
  };
  if (timer_fd < 0 || timerfd_settime(timer_fd, 0, &ts, NULL) < 0) {
    perror("timerfd");
    return 1;
  }
  sample_status(true);
  refresh_ipv4();
  int64_t next_ip = monotonic_ms() + 10000;
  int result = 0;
  while (running) {
    if (wl_display_dispatch_pending(display) < 0 || !schedule_query() ||
        !draw())
      goto failed;
    if (!running)
      break;
    while (wl_display_prepare_read(display) != 0)
      if (wl_display_dispatch_pending(display) < 0)
        goto failed;
    int flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      goto failed;
    }
    struct pollfd fds[] = {
        {.fd = wl_display_get_fd(display),
         .events = POLLIN | (flushed < 0 ? POLLOUT : 0)},
        {.fd = event_ipc.fd,
         .events = POLLIN | (event_ipc.tx_size ? POLLOUT : 0)},
        {.fd = command_ipc.fd,
         .events = POLLIN | (command_ipc.tx_size ? POLLOUT : 0)},
        {.fd = timer_fd, .events = POLLIN},
    };
    int ready = poll(fds, 4, -1);
    if (ready < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR)
        continue;
      goto failed;
    }
    if (fds[0].revents & POLLIN) {
      if (wl_display_read_events(display) < 0)
        goto failed;
    } else {
      wl_display_cancel_read(display);
    }
    for (int i = 0; i < 4; i++)
      if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
        goto failed;
    if (wl_display_dispatch_pending(display) < 0)
      goto failed;
    if (!running)
      break;
    if ((fds[1].revents & POLLOUT) && !ipc_write(&event_ipc))
      goto failed;
    if ((fds[2].revents & POLLOUT) && !ipc_write(&command_ipc))
      goto failed;
    if ((fds[1].revents & POLLIN) && !ipc_read(&event_ipc, on_event, NULL))
      goto failed;
    if ((fds[2].revents & POLLIN) && !ipc_read(&command_ipc, on_reply, NULL))
      goto failed;
    if (fds[3].revents & POLLIN) {
      uint64_t ticks;
      if (read(timer_fd, &ticks, sizeof(ticks)) == sizeof(ticks)) {
        sample_status(false);
        if (monotonic_ms() >= next_ip) {
          refresh_ipv4();
          next_ip = monotonic_ms() + 10000;
        }
      }
    }
    if (ipc_expired(&command_ipc) || ipc_expired(&event_ipc)) {
      fprintf(stderr, "Sway IPC request timed out\n");
      goto failed;
    }
  }
  goto cleanup;
failed:
  fprintf(stderr,
          "bar: display/IPC disconnected or invalid response/resource\n");
  result = 1;
cleanup:
  ipc_close(&command_ipc);
  ipc_close(&event_ipc);
  close(timer_fd);
  if (frame)
    wl_callback_destroy(frame);
  for (int i = 0; i < 2; i++)
    buffer_destroy(&buffers[i]);
  if (ram_cache)
    g_hash_table_destroy(ram_cache);
  if (pointer)
    wl_pointer_destroy(pointer);
  if (seat)
    wl_seat_destroy(seat);
  zwlr_layer_surface_v1_destroy(layer_surface);
  wl_surface_destroy(surface);
  zwlr_layer_shell_v1_destroy(layer_shell);
  wl_shm_destroy(shm);
  wl_compositor_destroy(compositor);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return result;
}
