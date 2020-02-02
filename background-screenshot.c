#include "background-screenshot.h"
#include "log.h"
#include <assert.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <wayland-client-protocol.h>

struct format {
  enum wl_shm_format wl_format;
  bool is_bgr;
};

static struct {
  struct wl_buffer *wl_buffer;
  void *data;
  enum wl_shm_format format;
  int width, height, stride;
  bool y_invert;
} buffer;
bool buffer_copy_done = false;

// wl_shm_format describes little-endian formats, libpng uses big-endian
// formats (so Wayland's ABGR is libpng's RGBA).
/*static const struct format formats[] = {
    {WL_SHM_FORMAT_XRGB8888, true},
    {WL_SHM_FORMAT_ARGB8888, true},
    {WL_SHM_FORMAT_XBGR8888, false},
    {WL_SHM_FORMAT_ABGR8888, false},
};*/

static struct wl_shm *shm = NULL;

static struct wl_buffer *create_shm_buffer(enum wl_shm_format fmt, int width,
                                           int height, int stride,
                                           void **data_out) {
  int size = stride * height;

  const char shm_name[] = "/wlroots-screencopy";
  int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    fprintf(stderr, "shm_open failed\n");
    return NULL;
  }
  shm_unlink(shm_name);

  int ret;
  while ((ret = ftruncate(fd, size)) == EINTR) {
    // No-op
  }
  if (ret < 0) {
    close(fd);
    fprintf(stderr, "ftruncate failed\n");
    return NULL;
  }

  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    fprintf(stderr, "mmap failed: %m\n");
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  close(fd);
  struct wl_buffer *buffer =
      wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
  wl_shm_pool_destroy(pool);

  *data_out = data;
  return buffer;
}

static void frame_handle_buffer(void *data,
                                struct zwlr_screencopy_frame_v1 *frame,
                                uint32_t format, uint32_t width,
                                uint32_t height, uint32_t stride) {
  buffer.format = format;
  buffer.width = width;
  buffer.height = height;
  buffer.stride = stride;
  buffer.wl_buffer =
      create_shm_buffer(format, width, height, stride, &buffer.data);
  if (buffer.wl_buffer == NULL) {
    fprintf(stderr, "failed to create buffer\n");
    exit(EXIT_FAILURE);
  }

  zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
}

static void frame_handle_flags(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t flags) {
  buffer.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec) {
  buffer_copy_done = true;
}

static void frame_handle_failed(void *data,
                                struct zwlr_screencopy_frame_v1 *frame) {
  fprintf(stderr, "failed to copy frame\n");
  exit(EXIT_FAILURE);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
};

#define IMAGE_XY(x, y, width) ((x) + (y) * (width))

uint32_t sample_image(uint8_t *im, int x, int y, size_t width,
                             size_t height) {
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x >= (int)width)
    x = width;
  if (y >= (int)width)
    y = width;
  return ((uint32_t *)im)[x + y * width];
}

void write_image(uint8_t *im, int x, int y, size_t width, size_t height,
                        uint32_t data) {
  if (x < 0 || y < 0 || x >= (int)width || y >= (int)width)
    return;
  ((uint32_t *)im)[x + y * width] = data;
}

void write_image_fast(uint8_t *im, int x, int y, size_t width, size_t height,
                        uint32_t data) {
  ((uint32_t *)im)[x + y * width] = data;
}

const int radius = 16;
const int radius_log2 = 5;

// O(n+k) fast box blur, n = screen resolution, k = blur radius
#pragma GCC optimize("O3", "Ofast")
void fast_blur_V(uint8_t *target, uint8_t *src, size_t width, size_t height) {
  for (int i = 0; i < (int)width; i++) {
    uint32_t r = 0, g = 0, b = 0;
    for (int j = 0; j < (int)height + radius; j++) {
      size_t idx = IMAGE_XY(i, j >= (int)height ? (int)height - 1 : j, width)
                   << 2;

      r += src[idx + 2];
      g += src[idx + 1];
      b += src[idx];

      if (j >= radius << 1) {
        size_t idx = IMAGE_XY(i, j - (radius << 1), width) << 2;
        r -= src[idx + 2];
        g -= src[idx + 1];
        b -= src[idx];
      }

      if (j >= radius) {
        uint32_t _r = r >> radius_log2;
        uint32_t _g = g >> radius_log2;
        uint32_t _b = b >> radius_log2;
        ((uint32_t *)target)[IMAGE_XY(i, j - radius, width)] =
            0xFF000000 | (_r << 16) | (_g << 8) | _b;
      }
    }
  }
}

#pragma GCC optimize("O3", "Ofast")
void fast_blur_H(uint8_t *target, uint8_t *src, size_t width, size_t height) {
  for (int j = 0; j < (int)height; j++) {
    uint32_t r = 0, g = 0, b = 0;
    for (int i = 0; i < (int)width + radius; i++) {
      size_t idx = IMAGE_XY(i >= (int)width ? (int)width - 1 : i, j, width) << 2;

      r += src[idx + 2];
      g += src[idx + 1];
      b += src[idx];

      if (i >= radius << 1) {
        size_t idx = IMAGE_XY(i - (radius << 1), j, width) << 2;
        r -= src[idx + 2];
        g -= src[idx + 1];
        b -= src[idx];
      }

      if (i >= radius) {
        uint32_t _r = r >> radius_log2;
        uint32_t _g = g >> radius_log2;
        uint32_t _b = b >> radius_log2;
        ((uint32_t *)target)[IMAGE_XY(i - radius, j, width)] =
            0xFF000000 | (_r << 16) | (_g << 8) | _b;
      }
    }
  }
}

void fast_copy_flip(uint8_t *target, uint8_t *src, size_t width,
                    size_t height) {
  for (size_t i = 0; i < height; i++) {
    memcpy(target + (height - i - 1) * width * 4, src + i * width * 4, width * 4);
  }
}

cairo_surface_t *load_background_screenshot(struct swaylock_state *state, struct swaylock_surface *surface) {
  struct zwlr_screencopy_frame_v1 *frame =
      zwlr_screencopy_manager_v1_capture_output(state->screencopy_manager, 0,
                                                surface->output);
  zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);

  shm = state->shm;

  while (!buffer_copy_done && wl_display_dispatch(state->display) != -1) {
    // This space is intentionally left blank
  }

  buffer_copy_done = false;

  uint8_t *interim = malloc(buffer.width * buffer.height * 4);
  uint8_t *data = malloc(buffer.width * buffer.height * 4);

  fast_copy_flip(data, buffer.data, buffer.width, buffer.height);

  struct timespec start, end;

  clock_gettime(CLOCK_MONOTONIC, &start);
  fast_blur_V(interim, data, buffer.width, buffer.height);
  fast_blur_V(data, interim, buffer.width, buffer.height);
  fast_blur_V(interim, data, buffer.width, buffer.height);
  fast_blur_H(data, interim, buffer.width, buffer.height);
  fast_blur_H(interim, data, buffer.width, buffer.height);
  fast_blur_H(data, interim, buffer.width, buffer.height);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double time_taken;
  time_taken = (end.tv_sec - start.tv_sec) * 1e9;
  time_taken = (time_taken + (end.tv_nsec - start.tv_nsec)) * 1e-9;

  swaylock_log(LOG_DEBUG, "Blurring time: %f s", time_taken);

  free(interim);

  cairo_surface_t *image = cairo_image_surface_create_for_data(
      data, CAIRO_FORMAT_ARGB32, buffer.width, buffer.height, buffer.stride);

  munmap(buffer.data, buffer.stride * buffer.height);
  wl_buffer_destroy(buffer.wl_buffer);
  zwlr_screencopy_frame_v1_destroy(frame);

  return image;
}
