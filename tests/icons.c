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

static void fixture(const char *path, const char *contents) {
  char *dir = g_path_get_dirname(path);
  assert(g_mkdir_with_parents(dir, 0700) == 0);
  assert(g_file_set_contents(path, contents, -1, NULL));
  g_free(dir);
}

static void remove_tree(const char *path) {
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
      char *child = g_build_filename(path, name, NULL);
      remove_tree(child);
      g_free(child);
    }
    g_dir_close(dir);
    assert(g_rmdir(path) == 0);
  } else {
    assert(g_remove(path) == 0);
  }
}

int main(int argc, char **argv) {
  /* Optional smoke check against the installed desktop entries and icons. */
  if (argc > 1) {
    assert(argc % 2 == 1);
    for (int i = 1; i < argc; i += 2) {
      expect_icon(argv[i], argv[i + 1]);
      cairo_surface_t *s = icon_lookup(argv[i]);
      assert(s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS);
      cairo_surface_destroy(s);
      printf("Installed app icon resolved and loaded: %s\n", argv[i]);
    }
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
  char *config = g_build_filename(tmp, "config", NULL);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  char *settings = g_build_filename(config, "gtk-3.0", "settings.ini", NULL);
  fixture(settings, "[Settings]\ngtk-icon-theme-name=TestTheme\n");

  entry(system, "direct.desktop", "Icon=system-icon\n");
  entry(user, "direct.desktop", "Icon=user-icon\n");
  entry(system, "class-match.desktop",
        "StartupWMClass=direct\nIcon=wrong-icon\n");
  entry(system, "browser-package.desktop",
        "StartupWMClass=browser\nIcon=browser-logo\n");
  entry(system, "action-only.desktop",
        "Actions=New;\n[Desktop Action New]\nName=New\nIcon=wrong-action\n");
  entry(system, "broken.desktop", "Icon=/nonexistent/bar-test.png\n");
  entry(user, "unityhub.desktop", "Icon=unity-test-logo\n");
  entry(system, "firefox-bin.desktop",
        "StartupWMClass=firefox\nIcon=firefox-test-logo\n");
  entry(system, "thunderbird-bin.desktop",
        "StartupWMClass=thunderbird\nIcon=thunderbird-test-logo\n");
  entry(system, "collision-a.desktop", "Icon=wrong-a\n");
  entry(system, "collision_a.desktop", "Icon=wrong-b\n");
  entry(system, "inherited.desktop", "Icon=inherited-test-logo\n");
  entry(system, "pixmap.desktop", "Icon=pixmap-test-logo\n");
  entry(system, "override.desktop", "Icon=override-test-logo\n");

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

  const char *red_svg =
      "<svg xmlns='http://www.w3.org/2000/svg' width='22' height='22'>"
      "<rect width='22' height='22' fill='red'/></svg>";
  char *path = g_build_filename(user, "icons", "hicolor", "256x256", "apps",
                                "unity-test-logo.svg", NULL);
  fixture(path, red_svg);
  g_free(path);
  path = g_build_filename(system, "icons", "TestTheme", "index.theme", NULL);
  fixture(path, "[Icon Theme]\nName=Test\nDirectories=large/apps,custom/categories\n"
                "Inherits=TestParent\n[large/apps]\nSize=128\nType=Fixed\n"
                "[custom/categories]\nSize=22\nType=Fixed\n");
  g_free(path);
  path = g_build_filename(system, "icons", "TestParent", "index.theme", NULL);
  fixture(path, "[Icon Theme]\nName=Parent\nDirectories=custom/apps\n"
                "Inherits=TestTheme\n[custom/apps]\nSize=22\nType=Fixed\n");
  g_free(path);
  path = g_build_filename(user, "icons", "TestParent", "custom", "apps",
                          "inherited-test-logo.svg", NULL);
  fixture(path, red_svg);
  g_free(path);
  path = g_build_filename(user, "icons", "TestTheme", "custom", "categories",
                          "override-test-logo.svg", NULL);
  fixture(path, red_svg);
  g_free(path);
  const char *blue_svg =
      "<svg xmlns='http://www.w3.org/2000/svg' width='22' height='22'>"
      "<rect width='22' height='22' fill='blue'/></svg>";
  path = g_build_filename(system, "icons", "TestTheme", "custom", "categories",
                          "override-test-logo.svg", NULL);
  fixture(path, blue_svg);
  g_free(path);
  path = g_build_filename(user, "icons", "TestTheme", "large", "apps",
                          "override-test-logo.svg", NULL);
  fixture(path, blue_svg);
  g_free(path);
  path = g_build_filename(system, "pixmaps", "pixmap-test-logo.svg", NULL);
  fixture(path, red_svg);
  g_free(path);

  expect_icon("direct", "user-icon");
  expect_icon("direct.desktop", "user-icon");
  expect_icon("browser", "browser-logo");
  expect_icon("unity-hub", "unity-test-logo");
  expect_icon("Unity-Hub", "unity-test-logo");
  expect_icon("Firefox", "firefox-test-logo");
  expect_icon("Thunderbird", "thunderbird-test-logo");
  expect_icon("FIREFOX-BIN", "firefox-test-logo");
  expect_icon("collision a", "collision a"); /* Ambiguous relaxed match. */
  expect_icon("action-only", "action-only");
  expect_icon("unknown-bar-test", "unknown-bar-test");
  const char *formats[] = {"png", "svg", "unity-hub", "inherited", "pixmap"};
  for (size_t i = 0; i < G_N_ELEMENTS(formats); i++) {
    cairo_surface_t *s = icon_lookup(formats[i]);
    assert(s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS);
    assert(cairo_image_surface_get_width(s) == ICON_SIZE);
    cairo_surface_destroy(s);
  }
  source = icon_lookup("override");
  assert(source);
  cairo_surface_flush(source);
  uint32_t *center = (uint32_t *)(cairo_image_surface_get_data(source) +
                      (ICON_SIZE / 2) * cairo_image_surface_get_stride(source));
  assert(center[ICON_SIZE / 2] == 0xffff0000u); /* Closest size, user override. */
  cairo_surface_destroy(source);
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

  remove_tree(tmp);
  g_free(settings);
  g_free(config);
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
