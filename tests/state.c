#define main bar_main
#include "../main.c"
#undef main
#include <assert.h>

int main(void) {
  const char *ignored[] = {"title", "focus", "mark"};
  for (unsigned i = 0; i < G_N_ELEMENTS(ignored); i++) {
    char body[64];
    snprintf(body, sizeof(body), "{\"change\":\"%s\"}", ignored[i]);
    want_ws = want_lang = dirty = false;
    assert(on_event(NULL, 0x80000003u, body));
    assert(!want_ws && !want_lang && !dirty);
  }
  assert(on_event(NULL, 0x80000003u, "{\"change\":\"new\"}"));
  assert(want_ws);
  want_ws = false;
  assert(on_event(NULL, 0x80000015u, "{\"change\":\"xkb_layout\"}"));
  assert(want_lang && !want_ws);
  want_lang = false;
  assert(on_event(NULL, 0x80000015u, "{\"change\":\"libinput_config\"}"));
  assert(!want_lang);
  assert(on_event(NULL, 0x80000015u, "{\"change\":\"xkb_keymap\"}"));
  assert(want_lang);
  assert(on_event(NULL, 0x80000000u, "{\"change\":\"focus\"}"));
  assert(want_ws);
  assert(!on_event(NULL, SWAY_SUBSCRIBE, "{\"success\":false}"));

  subscribed = true;
  want_lang = false;
  want_ws = true;
  assert(schedule_query());
  assert(command_ipc.pending && command_ipc.expected == SWAY_GET_WORKSPACES);
  char *request = command_ipc.tx;
  for (int i = 0; i < 100; i++) {
    assert(on_event(NULL, 0x80000003u, "{\"change\":\"new\"}"));
    assert(schedule_query());
    assert(command_ipc.tx == request);
  }
  assert(want_ws); /* One pending follow-up, never one request per event. */
  ipc_close(&command_ipc);

  cJSON *wss = cJSON_Parse("[{\"num\":1,\"name\":\"1\",\"focused\":true},"
                           "{\"num\":2,\"name\":\"2\"}]");
  assert(parse_workspaces(wss));
  cJSON_Delete(wss);
  cJSON *tree =
      cJSON_Parse("{\"nodes\":[{\"type\":\"workspace\",\"name\":\"1\","
                  "\"nodes\":[{\"app_id\":\"native\"}],"
                  "\"floating_nodes\":[{\"nodes\":[{\"window_properties\":{"
                  "\"class\":\"x11\"}}]}]},"
                  "{\"type\":\"workspace\",\"name\":\"2\",\"nodes\":[]}]}");
  collect_workspaces(tree);
  assert(next_ws_count == 2 && next_workspaces[0].focused);
  assert(next_workspaces[0].app_count == 2);
  assert(!strcmp(next_workspaces[0].apps[0], "native"));
  assert(!strcmp(next_workspaces[0].apps[1], "x11"));
  assert(next_workspaces[1].app_count == 0);
  cJSON_Delete(tree);

  /* Visible working sets larger than the old FIFO retain every surface. */
  cairo_surface_t *saved[64];
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < 64; i++) {
      char id[128];
      snprintf(id, sizeof(id), "bar-missing-cache-test-%080d", i);
      cairo_surface_t *icon = icon_get(id);
      if (!pass)
        saved[i] = icon;
      else
        assert(icon == saved[i]);
    }
  }
  assert(g_hash_table_size(ram_cache) == 64);
  ++icon_generation;
  cairo_surface_t *keep;
  assert(ram_cache_get("bar-missing-cache-test-"
                       "0000000000000000000000000000000000000000000000000000000"
                       "0000000000000000000000000",
                       &keep));
  g_hash_table_foreach_remove(ram_cache, icon_unused, NULL);
  assert(g_hash_table_size(ram_cache) == 1);

  /* A busy slot is unavailable, even if a resize requests different geometry.
   */
  BarBuffer busy = {.busy = true, .width = 100, .height = 34};
  assert(!buffer_prepare(&busy, NULL, 200, 34, FONT));
  assert(busy.busy && busy.width == 100);
  g_hash_table_destroy(ram_cache);
  ram_cache = NULL;
  puts("Event filtering, workspace parsing, visible icon ownership, and "
       "busy-buffer checks passed.");
}
