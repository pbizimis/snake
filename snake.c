#define _POSIX_C_SOURCE 200112L
#include "snake.h"
#include "config.h"
#include "font.h"
#include "glue_code/xdg-shell-client-protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

long get_nanoseconds() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_nsec;
}
/* Shared memory support code */
static void randname(char *buf) {
  long r = get_nanoseconds();
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

struct wayland_state {
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
  struct xkb_state *xkb_state;
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
};

struct buffer {
  struct wl_buffer *wl_buffer;
  uint32_t *shm_data;
  bool busy;
};

struct window_state {
  struct wl_surface *wl_surface;
  struct buffer buffers[2];
};

struct input_state {};

struct game_state {};

/* Wayland code */

static void buffer_release(void *data, struct wl_buffer *buffer) {
  struct buffer *released_buffer = data;

  released_buffer->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {buffer_release};

int random_coordinate(int max_number) { return rand() % max_number + 1; }

bool set_cube_to(short pixelmap[WINDOW_WIDTH][WINDOW_HEIGHT], short value,
                 short x, short y) {

  struct coords track_changed_coords[(TARGET_OFFSET + 1) * (TARGET_OFFSET + 1)];
  short count = 0;

  short cube_size = TARGET_OFFSET / 2;
  for (int i = -cube_size; i < cube_size + 1; i++) {
    for (int j = -cube_size; j < cube_size + 1; j++) {

      if (pixelmap[x + i][y + j] > 0) {
        for (int k = 0; k < count; k++) {
          pixelmap[track_changed_coords[k].x][track_changed_coords[k].y] = 0;
        }

        return false;
      } else {
        pixelmap[x + i][y + j] = value;
        track_changed_coords[count++] = (struct coords){x + i, y + j, value};
      }
    }
  }
  return true;
}

void generate_target(struct client_state *state) {

  if (state->target.x > -1 && state->target.y > -1) {
    set_cube_to(state->pixelmap, 0, state->target.x, state->target.y);
  }

  bool success = false;
  int random_x = random_coordinate(WINDOW_WIDTH);
  int random_y = random_coordinate(WINDOW_HEIGHT);

  while (!success) {
    random_x = random_coordinate(WINDOW_WIDTH);
    random_y = random_coordinate(WINDOW_HEIGHT);

    int minimum_offset = FRAME_OFFSET + TARGET_OFFSET / 2;

    // check that target is not inside frame
    if (random_x < minimum_offset) {
      random_x = minimum_offset;
    } else if (random_x >= WINDOW_WIDTH - minimum_offset) {
      random_x = WINDOW_WIDTH - (minimum_offset + 1);
    }
    if (random_y < minimum_offset) {
      random_y = minimum_offset;
    } else if (random_y >= WINDOW_HEIGHT - minimum_offset) {
      random_y = WINDOW_HEIGHT - (minimum_offset + 1);
    }

    success = set_cube_to(state->pixelmap, -1, random_x, random_y);
  }

  state->target.x = random_x;
  state->target.y = random_y;
}

void move_start_pixel(struct client_state *state, short x, short y,
                      short *pause_deletion_duration) {
  short *new_pixel = &(state->pixelmap[x][y]);
  if (*new_pixel > 0 || x < FRAME_OFFSET || y < FRAME_OFFSET ||
      x >= WINDOW_WIDTH - FRAME_OFFSET || y >= WINDOW_HEIGHT - FRAME_OFFSET)
    state->game_state = OVER;
  else if (*new_pixel < 0) {
    *pause_deletion_duration = 100;
    generate_target(state);
    *new_pixel = ++state->start.index;
    state->score++;
  } else
    *new_pixel = ++state->start.index;
}

void update_game_state(struct client_state *state) {
  static short pause_deletion_duration;

  if (state->direction == UP) {

    move_start_pixel(state, state->start.x, ++state->start.y,
                     &pause_deletion_duration);
  }
  if (state->direction == RIGHT) {
    move_start_pixel(state, ++state->start.x, state->start.y,
                     &pause_deletion_duration);
  }
  if (state->direction == DOWN) {
    move_start_pixel(state, state->start.x, --state->start.y,
                     &pause_deletion_duration);
  }
  if (state->direction == LEFT) {
    move_start_pixel(state, --state->start.x, state->start.y,
                     &pause_deletion_duration);
  }

  if (pause_deletion_duration > 0) {
    pause_deletion_duration--;
  } else {
    state->end.index++;
    state->pixelmap[state->end.x][state->end.y] = 0;

    if (state->pixelmap[state->end.x + 1][state->end.y] == state->end.index) {
      state->end.x++;
    } else if (state->pixelmap[state->end.x - 1][state->end.y] ==
               state->end.index) {
      state->end.x--;
    } else if (state->pixelmap[state->end.x][state->end.y + 1] ==
               state->end.index) {
      state->end.y++;
    } else if (state->pixelmap[state->end.x][state->end.y - 1] ==
               state->end.index) {
      state->end.y--;
    }
  }
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
  struct client_state *state = data;
  xdg_surface_ack_configure(xdg_surface, serial);
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
  /* This space deliberately left blank */
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
  /* This space deliberately left blank */
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
  /* This space deliberately left blank */
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

struct client_state *setup_OLD_wayland() {

  struct client_state *wayland_state;
  wayland_state = malloc(sizeof(*wayland_state));

  wayland_state->wl_display = wl_display_connect(NULL);
  wayland_state->wl_registry =
      wl_display_get_registry(wayland_state->wl_display);
  wayland_state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl_registry_add_listener(wayland_state->wl_registry, &wl_registry_listener,
                           wayland_state);
  wl_display_roundtrip(wayland_state->wl_display);

  wayland_state->wl_surface =
      wl_compositor_create_surface(wayland_state->wl_compositor);
  wayland_state->xdg_surface = xdg_wm_base_get_xdg_surface(
      wayland_state->xdg_wm_base, wayland_state->wl_surface);
  xdg_surface_add_listener(wayland_state->xdg_surface, &xdg_surface_listener,
                           wayland_state);
  wayland_state->xdg_toplevel =
      xdg_surface_get_toplevel(wayland_state->xdg_surface);
  xdg_toplevel_set_title(wayland_state->xdg_toplevel, "Example client");
  wl_surface_commit(wayland_state->wl_surface);

  struct wl_callback *cb = wl_surface_frame(wayland_state->wl_surface);
  wl_callback_add_listener(cb, &wl_surface_frame_listener, wayland_state);

  return wayland_state;
}

struct wayland_state *setup_wayland() {

  struct wayland_state *wayland_state;
  wayland_state = malloc(sizeof(*wayland_state));

  wayland_state->wl_display = wl_display_connect(NULL);
  wayland_state->wl_registry =
      wl_display_get_registry(wayland_state->wl_display);
  wayland_state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl_registry_add_listener(wayland_state->wl_registry, &wl_registry_listener,
                           wayland_state);
  wl_display_roundtrip(wayland_state->wl_display);

  wayland_state->wl_surface =
      wl_compositor_create_surface(wayland_state->wl_compositor);
  wayland_state->xdg_surface = xdg_wm_base_get_xdg_surface(
      wayland_state->xdg_wm_base, wayland_state->wl_surface);
  xdg_surface_add_listener(wayland_state->xdg_surface, &xdg_surface_listener,
                           wayland_state);
  wayland_state->xdg_toplevel =
      xdg_surface_get_toplevel(wayland_state->xdg_surface);
  xdg_toplevel_set_title(wayland_state->xdg_toplevel, "Example client");
  wl_surface_commit(wayland_state->wl_surface);

  struct wl_callback *cb = wl_surface_frame(wayland_state->wl_surface);
  wl_callback_add_listener(cb, &wl_surface_frame_listener, wayland_state);

  return wayland_state;
}

void update_game() {}

struct buffer *find_free_buffer(struct window_state *window_state) {

  if (!window_state->buffers[0].busy)
    return &window_state->buffers[0];
  if (!window_state->buffers[1].busy)
    return &window_state->buffers[1];
  else
    return NULL;
}

void render_frame(struct wl_surface *wl_surface, struct buffer *buffer) {
  wl_surface_attach(wl_surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage_buffer(wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(wl_surface);
  buffer->busy = 1;
}

void fill_frame(uint32_t *data, struct client_state *state) {

  for (int y = 0; y < WINDOW_HEIGHT; ++y) {
    for (int x = 0; x < WINDOW_WIDTH; ++x) {

      if (state->game_state == PROGRESS) {
        if (state->pixelmap[x][y] > 0) {
          data[y * WINDOW_WIDTH + x] = SNAKE_COLOR;
        } else if (state->pixelmap[x][y] < 0) {
          if (state->pixelmap[x][y] == -2) {
            data[y * WINDOW_WIDTH + x] = 0xFF00FF00;
          } else {
            // background
            data[y * WINDOW_WIDTH + x] = 0xFFFFC0CB;
          }

        } else {
          // put this outside of the draw frame (it is fixed)
          if (x < FRAME_OFFSET || y < FRAME_OFFSET ||
              x >= WINDOW_WIDTH - FRAME_OFFSET ||
              y >= WINDOW_HEIGHT - FRAME_OFFSET) {
            data[y * WINDOW_WIDTH + x] = 0xFF333333;
          } else {
            data[y * WINDOW_WIDTH + x] = 0xFFEEEEEE;
          }
        }
      } else if (state->game_state == OVER) {
        data[y * WINDOW_WIDTH + x] = 0xFFFF0000;
      }
    }
  }
}

void setup_game() {}

struct window_state *setup_window_state(struct client_state *wayland_state) {
  struct window_state *window_state;
  window_state = malloc(sizeof(*window_state));

  // Calculate shared memory pool size
  int buffer_amount = 2;
  int stride = WINDOW_WIDTH * 4;
  int shm_pool_size = stride * WINDOW_HEIGHT * buffer_amount;

  int fd = allocate_shm_file(shm_pool_size);
  if (fd == -1) {
    fprintf(stderr, "SHM allocation failed");
  }

  uint8_t *pool_data =
      mmap(NULL, shm_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pool_data == MAP_FAILED) {
    fprintf(stderr, "MMAP failed");
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool =
      wl_shm_create_pool(wayland_state->wl_shm, fd, shm_pool_size);

  int buffer_offset = stride * WINDOW_HEIGHT;

  struct wl_buffer *buffer1 = wl_shm_pool_create_buffer(
      pool, 0, WINDOW_WIDTH, WINDOW_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
  uint32_t *shm_data_buffer1 = (uint32_t *)&pool_data[0];

  struct wl_buffer *buffer2 =
      wl_shm_pool_create_buffer(pool, buffer_offset, WINDOW_WIDTH,
                                WINDOW_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
  uint32_t *shm_data_buffer2 = (uint32_t *)&pool_data[buffer_offset];

  window_state->buffers[0].wl_buffer = buffer1;
  window_state->buffers[0].shm_data = shm_data_buffer1;
  window_state->buffers[0].busy = false;
  window_state->buffers[1].wl_buffer = buffer2;
  window_state->buffers[1].shm_data = shm_data_buffer2;
  window_state->buffers[1].busy = false;

  wl_buffer_add_listener(window_state->buffers[0].wl_buffer, &buffer_listener,
                         &window_state->buffers[0]);
  wl_buffer_add_listener(window_state->buffers[1].wl_buffer, &buffer_listener,
                         &window_state->buffers[1]);
  wl_shm_pool_destroy(pool);
  close(fd);

  return window_state;
}

void TEMP_update(struct client_state *state) { update_game_state(state); }

void TEMP_render(struct client_state *state, int *fps_counter) {

  struct buffer *free_buffer = find_free_buffer(state->window_state);
  if (free_buffer != NULL) {
    (*fps_counter)++;
    render_score(state->pixelmap, WINDOW_WIDTH - 100, 2, state->score);
    fill_frame(free_buffer->shm_data, state);
    render_frame(state->wl_surface, free_buffer);
  }
}

struct client_state *init_wayland() {

  struct client_state *state = setup_OLD_wayland();

  for (int x = 0; x < WINDOW_WIDTH; x++) {
    for (int y = 0; y < WINDOW_HEIGHT; y++) {
      state->pixelmap[x][y] = 0;
    }
  }

  state->direction = RIGHT;
  state->game_state = PROGRESS;
  state->score = 0;

  int snake_starting_x = WINDOW_WIDTH / 2;
  int snake_starting_y = WINDOW_HEIGHT / 2;
  int snake_ending_x = snake_starting_x - SNAKE_INITIAL_LENGTH;

  state->start.x = snake_starting_x;
  state->start.y = snake_starting_y;
  state->start.index = SNAKE_INITIAL_LENGTH;

  state->end.x = snake_starting_x - SNAKE_INITIAL_LENGTH;
  state->end.y = snake_starting_y;
  state->end.index = 0;

  state->target.x = -1;
  state->target.y = -1;
  state->target.index = -1;

  for (int index = SNAKE_INITIAL_LENGTH; snake_starting_x > snake_ending_x;
       snake_starting_x--) {
    state->pixelmap[snake_starting_x][snake_starting_y] = index--;
  }

  srand(get_nanoseconds());

  generate_target(state);

  // struct wayland_state *wayland_state = setup_wayland();
  struct window_state *window_state = setup_window_state(state);

  state->window_state = window_state;

  return state;
}
