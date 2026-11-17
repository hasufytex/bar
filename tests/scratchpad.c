#define main bar_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void snapshot(const char *wss, const char *tree)
{
	cJSON *json = cJSON_Parse(wss);
	assert(json && parse_workspaces(json));
	cJSON_Delete(json);
	json = cJSON_Parse(tree);
	assert(json);
	publish_workspaces(json);
	cJSON_Delete(json);
}

static Workspace *workspace(const char *name)
{
	for (int w = 0; w < ws_count; w++)
		if (!strcmp(workspaces[w].name, name))
			return &workspaces[w];
	return NULL;
}

static void click(int x, uint32_t button, uint32_t state)
{
	*pending_command = 0;
	pointer_x = x;
	ptr_button(NULL, NULL, 0, 0, button, state);
}

#define TWO_WS "[{\"num\":4,\"name\":\"4\"},{\"num\":5,\"name\":\"5\"}]"
#define ONE_WS "[{\"num\":5,\"name\":\"5\",\"focused\":true}]"
#define APP "\"app_id\":\"bar-scratchpad-test\""
#define HIDDEN_TREE                                                            \
	"{\"nodes\":[{\"type\":\"workspace\",\"name\":\"__i3_scratch\","       \
	"\"floating_nodes\":[{\"scratchpad_state\":\"fresh\",\"nodes\":["      \
	"{\"id\":101," APP                                                     \
	"},{\"id\":102,\"window_properties\":{\"class\":\"bar-"                \
	"scratchpad-test\"}}]}]},"                                             \
	"{\"type\":\"workspace\",\"name\":\"5\",\"nodes\":[]}]}"

int main(void)
{
	/* A visible scratchpad icon stays on its current workspace. */
	snapshot(TWO_WS,
		 "{\"nodes\":[{\"type\":\"workspace\",\"name\":\"4\","
		 "\"nodes\":[{\"id\":101," APP "}]},"
		 "{\"type\":\"workspace\",\"name\":\"5\",\"nodes\":[]}]}");
	assert(ws_count == 2 && !workspace("4")->scratchpad[0]);
	snapshot(TWO_WS,
		 "{\"nodes\":[{\"type\":\"workspace\",\"name\":\"4\","
		 "\"floating_nodes\":[{\"id\":101,\"scratchpad_state\":"
		 "\"fresh\"," APP "}]},"
		 "{\"type\":\"workspace\",\"name\":\"5\",\"nodes\":[]}]}");
	assert(workspace("4")->scratchpad[0] && !workspace("4")->hidden[0]);
	assert(!workspace("__i3_scratch"));

	/* Hidden windows move to S immediately. No former workspace is
	 * retained, including when Sway has destroyed the now-empty workspace.
	 * Nested splits and same-app native/X11 windows keep distinct IDs.
	 * Repeated snapshots and startup snapshots must behave identically,
	 * without duplicates. */
	for (int i = 0; i < 20; i++) {
		snapshot(ONE_WS, HIDDEN_TREE);
		assert(ws_count == 2 && workspaces[0].num == 5);
		assert(!workspace("4") && workspace("5")->app_count == 0);
		Workspace *scratch = workspace("__i3_scratch");
		assert(scratch && scratch->app_count == 2);
		assert(scratch->con_ids[0] == 101 &&
		       scratch->con_ids[1] == 102);
		assert(scratch->hidden[0] && scratch->hidden[1]);
		assert(scratch->scratchpad[0] && scratch->scratchpad[1]);
	}

	/* Hidden icons retain full brightness without depending on icon themes.
	 */
	cairo_surface_t *icon = cairo_image_surface_create(
	    CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
	cairo_t *paint = cairo_create(icon);
	cairo_set_source_rgb(paint, 1, 0, 0);
	cairo_paint(paint);
	cairo_destroy(paint);
	ram_cache_set("bar-scratchpad-test", icon);
	snapshot(ONE_WS, HIDDEN_TREE);
	cairo_surface_t *image =
	    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1024, BAR_HEIGHT);
	cairo_t *cr = cairo_create(image);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	draw_workspaces(cr, layout);
	assert(scratch_hit_count == 2 && ws_hit_count == 2);
	cairo_surface_flush(image);
	int x = scratch_hits[0].x0 + ICON_SIZE / 2;
	uint32_t pixel;
	memcpy(&pixel,
	       cairo_image_surface_get_data(image) +
		   BAR_HEIGHT / 2 * cairo_image_surface_get_stride(image) +
		   x * 4,
	       4);
	unsigned red = (pixel >> 16) & 255;
	assert(red == 255);
	for (int i = 0; i < 2; i++) {
		click(scratch_hits[i].x0 + 1, BTN_LEFT,
		      WL_POINTER_BUTTON_STATE_PRESSED);
		char expected[64];
		snprintf(expected, sizeof(expected),
			 "[con_id=%d] scratchpad show", 101 + i);
		assert(!strcmp(pending_command, expected));
	}
	click(x, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	assert(!*pending_command);
	click(x, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
	assert(!*pending_command);
	click(ws_hits[0].x0 + 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	assert(!strcmp(pending_command, "workspace number 5"));
	click(ws_hits[1].x0 + 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	assert(!*pending_command); /* S is not a destination workspace. */

	/* Showing on 5 moves both icons there and removes S. The next click
	 * hides the exact window; after the move event it appears in S again.
	 */
	snapshot(
	    ONE_WS,
	    "{\"nodes\":[{\"type\":\"workspace\",\"name\":\"5\","
	    "\"floating_nodes\":[{\"scratchpad_state\":\"fresh\",\"nodes\":["
	    "{\"id\":101," APP "},{\"id\":102," APP "}]}]}]}");
	assert(ws_count == 1 && !workspace("__i3_scratch"));
	assert(workspace("5")->app_count == 2 && workspace("5")->focused);
	assert(!workspace("5")->hidden[0] && !workspace("5")->hidden[1]);
	draw_workspaces(cr, layout);
	assert(scratch_hit_count == 2 && ws_hit_count == 1);
	for (int i = 0; i < 2; i++) {
		click(scratch_hits[i].x0 + 1, BTN_LEFT,
		      WL_POINTER_BUTTON_STATE_PRESSED);
		char expected[64];
		snprintf(expected, sizeof(expected),
			 "[con_id=%d] move scratchpad", 101 + i);
		assert(!strcmp(pending_command, expected));
	}
	snapshot(ONE_WS, HIDDEN_TREE);
	assert(workspace("5")->app_count == 0);
	assert(workspace("__i3_scratch")->app_count == 2);
	draw_workspaces(cr, layout);
	click(scratch_hits[0].x0 + 1, BTN_LEFT,
	      WL_POINTER_BUTTON_STATE_PRESSED);
	assert(!strcmp(pending_command, "[con_id=101] scratchpad show"));

	/* Restoring to normal tiling removes scratchpad click targets. Closing
	 * the remaining hidden window removes S without requiring a bar
	 * restart. */
	snapshot(ONE_WS, "{\"nodes\":[{\"type\":\"workspace\",\"name\":\"5\","
			 "\"nodes\":[{\"id\":101," APP "}]}]}");
	assert(ws_count == 1 && !workspace("5")->scratchpad[0]);
	draw_workspaces(cr, layout);
	assert(scratch_hit_count == 0);

	/* Display caps continue to bound both icon storage and click ranges. */
	cJSON *wss = cJSON_CreateArray(), *tree = cJSON_CreateObject();
	cJSON *nodes = cJSON_AddArrayToObject(tree, "nodes");
	for (int w = 0; w < MAX_WS; w++) {
		char name[16];
		snprintf(name, sizeof(name), "%d", w + 1);
		cJSON *meta = cJSON_CreateObject();
		cJSON_AddNumberToObject(meta, "num", w + 1);
		cJSON_AddStringToObject(meta, "name", name);
		cJSON_AddItemToArray(wss, meta);
		cJSON *ws = cJSON_CreateObject();
		cJSON_AddStringToObject(ws, "type", "workspace");
		cJSON_AddStringToObject(ws, "name", name);
		cJSON *apps = cJSON_AddArrayToObject(ws, "nodes");
		for (int a = 0; a < MAX_APPS + 1; a++) {
			cJSON *app = cJSON_CreateObject();
			cJSON_AddNumberToObject(app, "id",
						1000 + w * (MAX_APPS + 1) + a);
			cJSON_AddStringToObject(app, "app_id",
						"bar-scratchpad-test");
			cJSON_AddItemToArray(apps, app);
		}
		cJSON_AddItemToArray(nodes, ws);
	}
	assert(parse_workspaces(wss));
	publish_workspaces(tree);
	assert(ws_count == MAX_WS);
	for (int w = 0; w < ws_count; w++)
		assert(workspaces[w].app_count == MAX_APPS);
	draw_workspaces(cr, layout);
	assert(ws_hit_count == MAX_WS && scratch_hit_count == 0);
	cJSON_Delete(wss);
	cJSON_Delete(tree);

	/* Named workspaces still switch correctly, with escaped names. */
	snapshot("[{\"num\":-1,\"name\":\"dev\\\"box\"}]",
		 "{\"nodes\":[{\"type\":\"workspace\",\"name\":\"dev\\\"box\","
		 "\"nodes\":[{\"id\":101," APP "}]}]}");
	draw_workspaces(cr, layout);
	click(ws_hits[0].x0 + 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	assert(!strcmp(pending_command, "workspace \"dev\\\"box\""));
	snapshot(ONE_WS, "{\"nodes\":[]}");
	assert(ws_count == 1 && workspace("5")->app_count == 0);
	draw_workspaces(cr, layout);
	assert(scratch_hit_count == 0);

	/* Idle scheduling and title/focus/mark events never query the tree. */
	subscribed = true;
	want_ws = want_lang = dirty = false;
	*pending_command = 0;
	unsigned generation = icon_generation;
	const char *events[] = {"{\"change\":\"title\"}",
				"{\"change\":\"focus\"}",
				"{\"change\":\"mark\"}"};
	for (int i = 0; i < 10000; i++) {
		assert(on_event(NULL, 0x80000003u, events[i % 3]));
		assert(schedule_query());
		assert(!command_ipc.pending && !want_ws && !dirty);
	}
	assert(icon_generation == generation);
	assert(on_event(NULL, 0x80000003u, "{\"change\":\"move\"}"));
	assert(want_ws && schedule_query());
	assert(command_ipc.expected == SWAY_GET_WORKSPACES);
	ipc_close(&command_ipc);

	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(image);
	g_hash_table_destroy(ram_cache);
	puts("Scratchpad placement, S group lifecycle, full-brightness icons, "
	     "exact-window "
	     "toggles, and idle event filtering passed.");
}
