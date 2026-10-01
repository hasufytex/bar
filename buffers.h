#ifndef BAR_BUFFERS_H
#define BAR_BUFFERS_H
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

typedef struct {
  struct wl_buffer *buffer;
  void *data;
  size_t size;
  uint32_t width, height;
  bool busy;
  cairo_t *cr;
  PangoLayout *layout;
} BarBuffer;

bool buffer_prepare(BarBuffer *b, struct wl_shm *shm, uint32_t width,
                    uint32_t height, const char *font);
void buffer_destroy(BarBuffer *b);
#endif
