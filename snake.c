#define _POSIX_C_SOURCE 200112L
#include "xdg-shell-client-protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

/* Shared memory support code */
static void randname(char *buf) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long r = ts.tv_nsec;
  for (int i = 0; i < 6; ++i) {
    buf[i] = 'A' + (r & 15) + (r & 16) * 2;
    r >>= 5;
  }
}

static int create_shm_file(void) {
  int retries = 100;
  do {
    char name[] = "/wl_shm-XXXXXX";
    randname(name + sizeof(name) - 7);
    --retries;
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      shm_unlink(name);
      return fd;
    }
  } while (retries > 0 && errno == EEXIST);
  return -1;
}

static int allocate_shm_file(size_t size) {
  int fd = create_shm_file();
  if (fd < 0)
    return -1;
  int ret;
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

#define SNAKE_INITIAL_LENGTH 32
#define SNAKE_HEIGHT 8
#define WINDOW_HEIGHT 500
#define WINDOW_WIDTH 500
#define SNAKE_COLOR 0xFF333333

struct coords {
  int x;
  int y;
  int index;
};

/* Wayland code */
struct client_state {
  /* Globals */
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_shm *wl_shm;
  struct wl_compositor *wl_compositor;
  struct xdg_wm_base *xdg_wm_base;
  struct wl_seat *wl_seat;
  /* Objects */
  struct wl_surface *wl_surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  struct wl_keyboard *wl_keyboard;
  /* State */
  float offset;
  uint32_t last_frame;
  struct xkb_state *xkb_state;
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  /* Game State */
  enum Direction { UP, RIGHT, DOWN, LEFT } direction;
  enum GameState { PROGRESS, OVER } game_state;
  short (*pixelmap)[WINDOW_WIDTH][WINDOW_HEIGHT];
  struct coords *start;
  struct coords *end;
};

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
  /* Sent by the compositor when it's no longer using this buffer */
  wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *draw_frame(struct client_state *state) {
  int stride = WINDOW_WIDTH * 4;
  int size = stride * WINDOW_HEIGHT;

  int fd = allocate_shm_file(size);
  if (fd == -1) {
    return NULL;
  }

  uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(
      pool, 0, WINDOW_WIDTH, WINDOW_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  if (state->direction == UP) {
    short *new_pixel = &(*state->pixelmap)[state->start->x][++state->start->y];
    if (*new_pixel > 0)
      state->game_state = OVER;
    else
      *new_pixel = ++state->start->index;
  }
  if (state->direction == RIGHT) {
    short *new_pixel = &(*state->pixelmap)[++state->start->x][state->start->y];
    if (*new_pixel > 0)
      state->game_state = OVER;
    else
      *new_pixel = ++state->start->index;
  }
  if (state->direction == DOWN) {
    short *new_pixel = &(*state->pixelmap)[state->start->x][--state->start->y];
    if (*new_pixel > 0)
      state->game_state = OVER;
    else
      *new_pixel = ++state->start->index;
  }
  if (state->direction == LEFT) {
    short *new_pixel = &(*state->pixelmap)[--state->start->x][state->start->y];
    if (*new_pixel > 0)
      state->game_state = OVER;
    else
      *new_pixel = ++state->start->index;
  }

  state->end->index++;
  (*state->pixelmap)[state->end->x][state->end->y] = 0;

  if ((*state->pixelmap)[state->end->x + 1][state->end->y] ==
      state->end->index) {
    state->end->x++;
  } else if ((*state->pixelmap)[state->end->x - 1][state->end->y] ==
             state->end->index) {
    state->end->x--;
  } else if ((*state->pixelmap)[state->end->x][state->end->y + 1] ==
             state->end->index) {
    state->end->y++;
  } else if ((*state->pixelmap)[state->end->x][state->end->y - 1] ==
             state->end->index) {
    state->end->y--;
  }

  for (int y = 0; y < WINDOW_HEIGHT; ++y) {
    for (int x = 0; x < WINDOW_WIDTH; ++x) {

      if (state->game_state == PROGRESS) {
        if ((*state->pixelmap)[x][y] > 0) {
          data[y * WINDOW_WIDTH + x] = SNAKE_COLOR;
        } else {
          data[y * WINDOW_WIDTH + x] = 0xFFEEEEEE;
        }
      } else if (state->game_state == OVER) {
        data[y * WINDOW_WIDTH + x] = 0xFFFF0000;
      }
    }
  }

  munmap(data, size);
  wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
  return buffer;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
  struct client_state *state = data;
  xdg_surface_ack_configure(xdg_surface, serial);

  struct wl_buffer *buffer = draw_frame(state);
  wl_surface_attach(state->wl_surface, buffer, 0, 0);
  wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                             uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static void wl_surface_frame_done(void *data, struct wl_callback *cb,
                                  uint32_t time) {
  /* Destroy this callback */
  wl_callback_destroy(cb);

  /* Request another frame */
  struct client_state *state = data;
  cb = wl_surface_frame(state->wl_surface);
  wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

  /* Update scroll amount at 24 pixels per second */
  if (state->last_frame != 0) {
    int elapsed = time - state->last_frame;
    state->offset += elapsed / 1000.0 * 24;
  }

  /* Submit a frame for this event */
  struct wl_buffer *buffer = draw_frame(state);
  wl_surface_attach(state->wl_surface, buffer, 0, 0);
  wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(state->wl_surface);

  state->last_frame = time;
}

static const struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                               uint32_t format, int32_t fd, uint32_t size) {
  struct client_state *client_state = data;
  assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

  char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  assert(map_shm != MAP_FAILED);

  struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_string(
      client_state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map_shm, size);
  close(fd);

  struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
  xkb_keymap_unref(client_state->xkb_keymap);
  xkb_state_unref(client_state->xkb_state);
  client_state->xkb_keymap = xkb_keymap;
  client_state->xkb_state = xkb_state;
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                              uint32_t serial, struct wl_surface *surface,
                              struct wl_array *keys) {
  struct client_state *client_state = data;
  fprintf(stderr, "keyboard enter; keys pressed are:\n");
  uint32_t *key;
  wl_array_for_each(key, keys) {
    char buf[128];
    xkb_keysym_t sym =
        xkb_state_key_get_one_sym(client_state->xkb_state, *key + 8);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    fprintf(stderr, "sym: %-12s (%d), ", buf, sym);
    xkb_state_key_get_utf8(client_state->xkb_state, *key + 8, buf, sizeof(buf));
    fprintf(stderr, "utf8: '%s'\n", buf);
  }
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                            uint32_t serial, uint32_t time, uint32_t key,
                            uint32_t state) {
  struct client_state *client_state = data;
  uint32_t keycode = key + 8;
  const char *action =
      state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release";

  if (strcmp(action, "release")) {
    if (keycode == 116 && client_state->direction != DOWN)
      client_state->direction = UP;
    if (keycode == 114 && client_state->direction != LEFT)
      client_state->direction = RIGHT;
    if (keycode == 111 && client_state->direction != UP)
      client_state->direction = DOWN;
    if (keycode == 113 && client_state->direction != RIGHT)
      client_state->direction = LEFT;
  }
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
                              uint32_t serial, struct wl_surface *surface) {
  fprintf(stderr, "keyboard leave\n");
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, uint32_t mods_depressed,
                                  uint32_t mods_latched, uint32_t mods_locked,
                                  uint32_t group) {
  struct client_state *client_state = data;
  xkb_state_update_mask(client_state->xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                    int32_t rate, int32_t delay) {
  /* Left as an exercise for the reader */
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

static void wl_seat_capabilities(void *data, struct wl_seat *wl_seat,
                                 uint32_t capabilities) {
  struct client_state *state = data;

  bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

  if (have_keyboard && state->wl_keyboard == NULL) {
    state->wl_keyboard = wl_seat_get_keyboard(state->wl_seat);
    wl_keyboard_add_listener(state->wl_keyboard, &wl_keyboard_listener, state);
  } else if (!have_keyboard && state->wl_keyboard != NULL) {
    wl_keyboard_release(state->wl_keyboard);
    state->wl_keyboard = NULL;
  }
}

static void wl_seat_name(void *data, struct wl_seat *wl_seat,
                         const char *name) {
  fprintf(stderr, "seat name: %s\n", name);
}

static const struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  struct client_state *state = (struct client_state *)data;
  if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);

  } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
    state->wl_compositor =
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);

  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    state->xdg_wm_base =
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);

  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    state->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 7);
    wl_seat_add_listener(state->wl_seat, &wl_seat_listener, state);
  }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
                                   uint32_t name) {
  /* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {

  int snake_starting_x = WINDOW_WIDTH / 2;
  int snake_starting_y = WINDOW_HEIGHT / 2;
  int snake_ending_x = snake_starting_x - SNAKE_INITIAL_LENGTH;

  struct coords start = {snake_starting_x, snake_starting_y,
                         SNAKE_INITIAL_LENGTH};

  struct coords end = {snake_starting_x - SNAKE_INITIAL_LENGTH,
                       snake_starting_y, 0};

  short pixelmap[WINDOW_WIDTH][WINDOW_HEIGHT] = {0};
  for (int index = SNAKE_INITIAL_LENGTH; snake_starting_x > snake_ending_x;
       snake_starting_x--) {
    pixelmap[snake_starting_x][snake_starting_y] = index--;
  }

  struct client_state state = {
      .start = &start,
      .end = &end,
      .pixelmap = &pixelmap,
      .direction = RIGHT,
      .game_state = PROGRESS,
  };

  state.wl_display = wl_display_connect(NULL);
  state.wl_registry = wl_display_get_registry(state.wl_display);
  state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
  wl_display_roundtrip(state.wl_display);

  state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
  state.xdg_surface =
      xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
  xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
  state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
  xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
  wl_surface_commit(state.wl_surface);

  struct wl_callback *cb = wl_surface_frame(state.wl_surface);
  wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);

  while (wl_display_dispatch(state.wl_display)) {
    /* This space deliberately left blank */
  }

  return 0;
}
