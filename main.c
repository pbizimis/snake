#define _POSIX_C_SOURCE 199309L
#include "snake.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define TICKS_PER_SECOND 300
#define SKIP_TICKS 1000 / TICKS_PER_SECOND
#define MAX_FRAMESKIP 5

long long get_tick_count() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int get_second_from_tick_count(long long tick_count) {
  return (tick_count / (int)pow(10, 3)) % 10;
}

// void display_game_new(float interpolation) {}

void start_game_loop() {

  struct client_state *state = init_wayland();

  long long next_game_tick = get_tick_count();
  printf("first tick: %lld\n", next_game_tick);

  int loops;
  float interpolation;
  int fps = 0;
  int fps_timer = get_second_from_tick_count(next_game_tick);

  bool game_is_running = true;

  TEMP_update(state);
  TEMP_render(state, &fps);

  while (game_is_running) {
    loops = 0;


    while (get_tick_count() > next_game_tick && loops < MAX_FRAMESKIP) {
      TEMP_update(state);

      next_game_tick += SKIP_TICKS;
      loops++;
    }

    // FPS COUNTER
    int new_fps_timer = get_second_from_tick_count(get_tick_count());
    if (fps_timer < new_fps_timer || (fps_timer == 9 && new_fps_timer == 0)) {
      printf("FPS: %d\n", fps);
      fps = 0;
      fps_timer = new_fps_timer;
    }

    interpolation = (get_tick_count() + (float)SKIP_TICKS - next_game_tick) /
                    (float)SKIP_TICKS;

    // display_game(interpolation);
    TEMP_render(state, &fps);
  }
}

int main() {

    start_game_loop();

}
