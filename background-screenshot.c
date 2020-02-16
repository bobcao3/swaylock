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

#define USE_PTHREAD

#ifdef USE_PTHREAD
#include <pthread.h>
#include <sys/sysinfo.h>
#include <semaphore.h>
#endif

struct format {
  enum wl_shm_format wl_format;
  bool is_bgr;
};

struct screenshot_state {
  struct {
    struct wl_buffer *wl_buffer;
    void *data;
    enum wl_shm_format format;
    int width, height, stride;
    bool y_invert;
  } buffer;

  struct wl_shm* shm;
  bool buffer_copy_done;
};

// wl_shm_format describes little-endian formats, libpng uses big-endian
// formats (so Wayland's ABGR is libpng's RGBA).
/*static const struct format formats[] = {
    {WL_SHM_FORMAT_XRGB8888, true},
    {WL_SHM_FORMAT_ARGB8888, true},
    {WL_SHM_FORMAT_XBGR8888, false},
    {WL_SHM_FORMAT_ABGR8888, false},
};*/

static struct wl_buffer *create_shm_buffer(enum wl_shm_format fmt, int width,
                                           int height, int stride,
                                           struct wl_shm* shm,
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
  struct screenshot_state *screenshot_state = (struct screenshot_state *)data;

  screenshot_state->buffer.format = format;
  screenshot_state->buffer.width = width;
  screenshot_state->buffer.height = height;
  screenshot_state->buffer.stride = stride;
  screenshot_state->buffer.wl_buffer =
      create_shm_buffer(format, width, height, stride, screenshot_state->shm,
                        &screenshot_state->buffer.data);
  
  if (screenshot_state->buffer.wl_buffer == NULL) {
    fprintf(stderr, "failed to create buffer\n");
    exit(EXIT_FAILURE);
  }

  zwlr_screencopy_frame_v1_copy(frame, screenshot_state->buffer.wl_buffer);
}

static void frame_handle_flags(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t flags) {
  struct screenshot_state *screenshot_state = (struct screenshot_state *)data;

  screenshot_state->buffer.y_invert =
      flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec) {
  struct screenshot_state *screenshot_state = (struct screenshot_state *)data;

  screenshot_state->buffer_copy_done = true;
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

// O(n+k) fast box blur, n = screen resolution, k = blur radius
void fast_blur_V(uint8_t *target, uint8_t *src, size_t width, size_t height, int start, int end, int radius, int radius_log2) {
  for (int i = start; i < (int)end; i++) {
    size_t line_start = IMAGE_XY(i, 0, width) << 2;
    uint32_t r = ((uint32_t)src[line_start + 2]) << radius_log2,
             g = ((uint32_t)src[line_start + 1]) << radius_log2,
             b = ((uint32_t)src[line_start]) << radius_log2;
    for (int j = 0; j < (int)height + radius; j++) {
      size_t idx = IMAGE_XY(i, j >= (int)height ? (int)height - 1 : j, width)
                   << 2;

      r += src[idx + 2];
      g += src[idx + 1];
      b += src[idx];

      if (j >= radius) {
        size_t idx = IMAGE_XY(i, j >= (radius << 1) ? j - (radius << 1) : 0, width) << 2;
        r -= src[idx + 2];
        g -= src[idx + 1];
        b -= src[idx];

        uint32_t _r = r >> radius_log2 >> 1;
        uint32_t _g = g >> radius_log2 >> 1;
        uint32_t _b = b >> radius_log2 >> 1;
        ((uint32_t *)target)[IMAGE_XY(i, j - radius, width)] =
            0xFF000000 | (_r << 16) | (_g << 8) | _b;
      }
    }
  }
}

void fast_blur_H(uint8_t *target, uint8_t *src, size_t width, size_t height,
                 int start, int end, int radius, int radius_log2) {
  for (int j = start; j < end; j++) {
    size_t line_start = IMAGE_XY(0, j, width) << 2;
    uint32_t r = ((uint32_t)src[line_start + 2]) << radius_log2,
             g = ((uint32_t)src[line_start + 1]) << radius_log2,
             b = ((uint32_t)src[line_start]) << radius_log2;
    for (int i = 0; i < (int)width + radius; i++) {
      size_t idx = IMAGE_XY(i >= (int)width ? (int)width - 1 : i, j, width) << 2;

      r += src[idx + 2];
      g += src[idx + 1];
      b += src[idx];

      if (i >= radius) {
        size_t idx = IMAGE_XY(i > (radius << 1) ? i - (radius << 1) : 0, j, width) << 2;
        r -= src[idx + 2];
        g -= src[idx + 1];
        b -= src[idx];

        uint32_t _r = r >> radius_log2 >> 1;
        uint32_t _g = g >> radius_log2 >> 1;
        uint32_t _b = b >> radius_log2 >> 1;
        ((uint32_t *)target)[IMAGE_XY(i - radius, j, width)] =
            0xFF000000 | (_r << 16) | (_g << 8) | _b;
      }
    }
  }
}

#ifdef USE_PTHREAD
struct pthread_blur_args {
  uint8_t *interim;
  uint8_t *data;
  size_t width;
  size_t height;
  int start;
  int end;
  int radius, radius_log2;

  bool job_finished;
  sem_t Vfinished;
};

void* pthread_blur(void* args) {
  struct pthread_blur_args *a = args;
  fast_blur_V(a->interim, a->data, a->width, a->height, a->start, a->end,
              a->radius, a->radius_log2);
  fast_blur_V(a->data, a->interim, a->width, a->height, a->start, a->end,
              a->radius, a->radius_log2);
  fast_blur_V(a->interim, a->data, a->width, a->height, a->start, a->end,
              a->radius, a->radius_log2);

  a->job_finished = true;
  sem_wait(&a->Vfinished);

  fast_blur_H(a->data, a->interim, a->width, a->height, a->start, a->end,
              a->radius, a->radius_log2);
  fast_blur_H(a->interim, a->data, a->width, a->height, a->start, a->end,
              a->radius, a->radius_log2);
  fast_blur_H(a->data, a->interim, a->width, a->height, a->start, a->end,
              a->radius, a->radius_log2);

  pthread_exit(0);
}
#endif

void fast_copy_flip(uint8_t *target, uint8_t *src, size_t width,
                    size_t height) {
  for (size_t i = 0; i < height; i++) {
    memcpy(target + (height - i - 1) * width * 4, src + i * width * 4, width * 4);
  }
}

cairo_surface_t *load_background_screenshot(struct swaylock_state *state, struct swaylock_surface *surface) {
  struct screenshot_state screenshot_state;
  screenshot_state.buffer_copy_done = false;
  screenshot_state.shm = state->shm;

  struct zwlr_screencopy_frame_v1 *frame =
      zwlr_screencopy_manager_v1_capture_output(state->screencopy_manager, 0,
                                                surface->output);
  zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener,
                                        &screenshot_state);

  while (!screenshot_state.buffer_copy_done &&
         wl_display_dispatch(state->display) != -1) {
    // This space is intentionally left blank
  }

  screenshot_state.buffer_copy_done = false;

  size_t width = screenshot_state.buffer.width;
  size_t height = screenshot_state.buffer.height;

  uint8_t *interim = malloc(width * height * 4);
  uint8_t *data = malloc(width * height * 4);

  fast_copy_flip(data, screenshot_state.buffer.data, width, height);

  struct timespec start, end;

  clock_gettime(CLOCK_MONOTONIC, &start);

  int radius = 32 * surface->scale;
  int radius_log2 = (int)log2(radius);

  swaylock_log(LOG_DEBUG, "Blur radius: %d", radius);

#ifdef USE_PTHREAD
  int num_procs = get_nprocs();

  // Spawn num_procs - 1 threads while the main thread blurs the last portion
  pthread_t workers[num_procs - 1];
  struct pthread_blur_args args[num_procs - 1];

  for (int i = 0; i < num_procs - 1; i++) {
    args[i].data = data;
    args[i].interim = interim;
    args[i].width = width;
    args[i].height = height;
    args[i].start = width * i / num_procs;
    args[i].end = width * (i + 1) / num_procs;
    args[i].job_finished = false;
    args[i].radius = radius;
    args[i].radius_log2 = radius_log2;

    sem_init(&args[i].Vfinished, 0, 0);

    if (pthread_create(&workers[i], NULL, pthread_blur, &args[i])) {
      swaylock_log(LOG_ERROR, "Failed to create thread");
    }
  }

  // This redues spin wait because the main thread is probably going to finish
  // the work in the similar amount of time.
  fast_blur_V(interim, data, width, height, width * (num_procs - 1) / num_procs,
              width, radius, radius_log2);
  fast_blur_V(data, interim, width, height, width * (num_procs - 1) / num_procs,
              width, radius, radius_log2);
  fast_blur_V(interim, data, width, height, width * (num_procs - 1) / num_procs,
              width, radius, radius_log2);

  bool v_finished = false;
  while (!v_finished) {
    v_finished = true;
    for (int i = 0; i < num_procs - 1; i++) {
      v_finished = v_finished && args[i].job_finished;
    }
  }

  for (int i = 0; i < num_procs - 1; i++) {
    args[i].start = height * i / num_procs;
    args[i].end = height * (i + 1) / num_procs;

    sem_post(&args[i].Vfinished);
  }

  fast_blur_H(data, interim, width, height,
              height * (num_procs - 1) / num_procs, height, radius, radius_log2);
  fast_blur_H(interim, data, width, height,
              height * (num_procs - 1) / num_procs, height, radius, radius_log2);
  fast_blur_H(data, interim, width, height,
              height * (num_procs - 1) / num_procs, height, radius, radius_log2);

  for (int i = 0; i < num_procs - 1; i++) {
    pthread_join(workers[i], NULL);
  }
#else
  fast_blur_V(interim, data, width, height, 0, width, radius, radius_log2, diameter_log2);
  fast_blur_V(data, interim, width, height, 0, width, radius, radius_log2,
              diameter_log2);
  fast_blur_V(interim, data, width, height, 0, width, radius, radius_log2,
              diameter_log2);
  fast_blur_H(data, interim, width, height, 0, height, radius, radius_log2,
              diameter_log2);
  fast_blur_H(interim, data, width, height, 0, height, radius, radius_log2,
              diameter_log2);
  fast_blur_H(data, interim, width, height, 0, height, radius, radius_log2,
              diameter_log2);
#endif

  clock_gettime(CLOCK_MONOTONIC, &end);

  double time_taken;
  time_taken = (end.tv_sec - start.tv_sec) * 1e9;
  time_taken = (time_taken + (end.tv_nsec - start.tv_nsec)) * 1e-9;

  swaylock_log(LOG_DEBUG, "Blurring time of %ld x %ld: %f s", width, height, time_taken);

  free(interim);

  cairo_surface_t *image = cairo_image_surface_create_for_data(
      data, CAIRO_FORMAT_ARGB32, width, height,
      screenshot_state.buffer.stride);

  munmap(screenshot_state.buffer.data,
         screenshot_state.buffer.stride * height);
  wl_buffer_destroy(screenshot_state.buffer.wl_buffer);
  zwlr_screencopy_frame_v1_destroy(frame);

  return image;
}
