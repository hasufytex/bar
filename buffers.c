#define _GNU_SOURCE
#include "buffers.h"
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>

static void released(void *data, struct wl_buffer *buffer) {
  ((BarBuffer *)data)->busy = false;
}
static const struct wl_buffer_listener listener = {.release = released};

void buffer_destroy(BarBuffer *b) {
  if (b->layout)
    g_object_unref(b->layout);
  if (b->cr)
    cairo_destroy(b->cr);
  if (b->buffer)
    wl_buffer_destroy(b->buffer);
  if (b->data)
    munmap(b->data, b->size);
  *b = (BarBuffer){0};
}

bool buffer_prepare(BarBuffer *b, struct wl_shm *shm, uint32_t width,
                    uint32_t height, const char *font) {
  if (b->busy)
    return false;
  if (b->buffer && b->width == width && b->height == height)
    return true;
  if (!width || !height || width > INT_MAX / 4 ||
      height > INT_MAX / (width * 4))
    return false;
  buffer_destroy(b);
  int stride = width * 4;
  size_t size = (size_t)stride * height;
  int fd = memfd_create("bar", MFD_CLOEXEC);
  if (fd < 0)
    return false;
  if (ftruncate(fd, size) < 0) {
    close(fd);
    return false;
  }
  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return false;
  }
  *b =
      (BarBuffer){.data = data, .size = size, .width = width, .height = height};
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  close(fd);
  if (!pool)
    goto fail;
  b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                        WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  if (!b->buffer)
    goto fail;
  wl_buffer_add_listener(b->buffer, &listener, b);
  cairo_surface_t *cs = cairo_image_surface_create_for_data(
      data, CAIRO_FORMAT_ARGB32, width, height, stride);
  b->cr = cairo_create(cs);
  cairo_surface_destroy(cs);
  if (cairo_status(b->cr) != CAIRO_STATUS_SUCCESS)
    goto fail;
  b->layout = pango_cairo_create_layout(b->cr);
  PangoFontDescription *desc = pango_font_description_from_string(font);
  pango_layout_set_font_description(b->layout, desc);
  pango_font_description_free(desc);
  return true;
fail:
  buffer_destroy(b);
  return false;
}
