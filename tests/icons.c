#define main bar_main
#include "../main.c"
#undef main
#include <assert.h>
#include <glib/gstdio.h>

static void entry(const char *dir, const char *id, const char *fields) {
  char *path = g_build_filename(dir, "applications", id, NULL);
  char *contents = g_strconcat(
      "[Desktop Entry]\nType=Application\nName=Icon test\nExec=/bin/true\n",
      fields, NULL);
  assert(g_file_set_contents(path, contents, -1, NULL));
  g_free(contents);
  g_free(path);
}

static void expect_icon(const char *app_id, const char *expected) {
  char icon[512];
  desktop_icon_name(app_id, icon, sizeof(icon));
  assert(!strcmp(icon, expected));
}

int main(int argc, char **argv) {
  /* Optional smoke check against the installed desktop entries and icons. */
  if (argc == 3) {
    expect_icon(argv[1], argv[2]);
    cairo_surface_t *s = icon_lookup(argv[1]);
    assert(s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);
    puts("Installed app icon resolved and loaded.");
    return 0;
  }

  char *tmp = g_dir_make_tmp("bar-icons-XXXXXX", NULL);
  assert(tmp);
  char *user = g_build_filename(tmp, "user", NULL);
  char *system = g_build_filename(tmp, "system", NULL);
  char *user_apps = g_build_filename(user, "applications", NULL);
  char *system_apps = g_build_filename(system, "applications", NULL);
  assert(g_mkdir_with_parents(user_apps, 0700) == 0);
  assert(g_mkdir_with_parents(system_apps, 0700) == 0);
  g_setenv("XDG_DATA_HOME", user, TRUE);
  g_setenv("XDG_DATA_DIRS", system, TRUE);

  entry(system, "direct.desktop", "Icon=system-icon\n");
  entry(user, "direct.desktop", "Icon=user-icon\n");
  entry(system, "class-match.desktop",
        "StartupWMClass=direct\nIcon=wrong-icon\n");
  entry(system, "browser-package.desktop",
        "StartupWMClass=browser\nIcon=browser-logo\n");
  entry(system, "action-only.desktop",
        "Actions=New;\n[Desktop Action New]\nName=New\nIcon=wrong-action\n");
  entry(system, "broken.desktop", "Icon=/nonexistent/bar-test.png\n");

  char *png = g_build_filename(tmp, "icon.png", NULL);
  cairo_surface_t *source = generic_app_icon();
  assert(cairo_surface_write_to_png(source, png) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(source);
  char *fields = g_strdup_printf("Icon=%s\n", png);
  entry(system, "png.desktop", fields);
  g_free(fields);

  char *svg = g_build_filename(tmp, "icon.svg", NULL);
  assert(g_file_set_contents(
      svg,
      "<svg xmlns='http://www.w3.org/2000/svg' width='22' height='22'>"
      "<rect width='22' height='22' fill='red'/></svg>",
      -1, NULL));
  fields = g_strdup_printf("Icon=%s\n", svg);
  entry(system, "svg.desktop", fields);
  g_free(fields);

  expect_icon("direct", "user-icon");
  expect_icon("browser", "browser-logo");
  expect_icon("action-only", "action-only");
  expect_icon("unknown-bar-test", "unknown-bar-test");
  const char *formats[] = {"png", "svg"};
  for (size_t i = 0; i < G_N_ELEMENTS(formats); i++) {
    cairo_surface_t *s = icon_lookup(formats[i]);
    assert(s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS);
    assert(cairo_image_surface_get_width(s) == ICON_SIZE);
    cairo_surface_destroy(s);
  }
  assert(icon_lookup("broken") == NULL);
  cairo_surface_t *fallback = icon_get("broken");
  assert(fallback && cairo_surface_status(fallback) == CAIRO_STATUS_SUCCESS);
  assert(icon_get("broken") == fallback);
  cairo_surface_flush(fallback);
  unsigned char *pixels = cairo_image_surface_get_data(fallback);
  bool visible = false;
  for (int i = 0; i < cairo_image_surface_get_stride(fallback) * ICON_SIZE; i++)
    visible |= pixels[i] != 0;
  assert(visible);

  /* An external reference must survive removal of an inactive cache entry. */
  cairo_surface_reference(fallback);
  ++icon_generation;
  for (int i = 0; i < 64; i++) {
    char id[64];
    snprintf(id, sizeof(id), "unknown-bar-test-%d", i);
    assert(icon_get(id));
  }
  g_hash_table_foreach_remove(ram_cache, icon_unused, NULL);
  assert(g_hash_table_size(ram_cache) == 64);
  assert(cairo_image_surface_get_width(fallback) == ICON_SIZE);
  cairo_surface_destroy(fallback);

  /* PNG rasterization preserves aspect ratio and caches target-sized pixels. */
  source = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 80, 40);
  cairo_t *cr = cairo_create(source);
  cairo_set_source_rgb(cr, 1, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  assert(cairo_surface_write_to_png(source, png) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(source);
  source = try_png(png);
  assert(source && cairo_image_surface_get_width(source) == ICON_SIZE);
  assert(cairo_image_surface_get_height(source) == ICON_SIZE);
  cairo_surface_flush(source);
  uint32_t *row = (uint32_t *)cairo_image_surface_get_data(source);
  assert(row[ICON_SIZE / 2] == 0); /* Transparent letterbox above image. */
  row = (uint32_t *)(cairo_image_surface_get_data(source) +
                     (ICON_SIZE / 2) * cairo_image_surface_get_stride(source));
  assert(row[ICON_SIZE / 2] == 0xffff0000u);
  cairo_surface_destroy(source);

  const char *dirs[] = {user_apps, system_apps};
  for (size_t i = 0; i < G_N_ELEMENTS(dirs); i++) {
    GDir *dir = g_dir_open(dirs[i], 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
      char *path = g_build_filename(dirs[i], name, NULL);
      g_remove(path);
      g_free(path);
    }
    g_dir_close(dir);
    g_rmdir(dirs[i]);
  }
  g_remove(png);
  g_remove(svg);
  g_rmdir(user);
  g_rmdir(system);
  g_rmdir(tmp);
  g_free(png);
  g_free(svg);
  g_free(user_apps);
  g_free(system_apps);
  g_free(user);
  g_free(system);
  g_free(tmp);
  puts("Icon resolution, image loading, fallback, and cache checks passed.");
  return 0;
}
