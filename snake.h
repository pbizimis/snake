#include "config.h"

struct coords {
  int x;
  int y;
  int index;
};

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
  struct xkb_state *xkb_state;
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  /* Game State */
  enum Direction { UP, RIGHT, DOWN, LEFT } direction;
  enum GameState { PROGRESS, OVER } game_state;
  short pixelmap[WINDOW_WIDTH][WINDOW_HEIGHT];
  struct coords start;
  struct coords end;
  struct coords target;
  short score;

  // TEMP
  struct window_state *window_state;
};

struct client_state *init_wayland();
void TEMP_update(struct client_state *);
void TEMP_render(struct client_state *, int *);
void TEMP_handle_events(struct client_state *);
