#include "font.h"
#include "config.h"
#include <stdio.h>

void render_score(short (*buffer)[WINDOW_WIDTH][WINDOW_HEIGHT], short x,
                  short y, short score) {
  char score_text[6] = "Score ";
  char text_with_score[MAX_SCORE_DIGITS + 6];

  sprintf(text_with_score, "%s%d", score_text, score);
  render_font(buffer, x, y, text_with_score);
}

void render_font(short (*buffer)[WINDOW_WIDTH][WINDOW_HEIGHT], short x, short y,
                 char text[]) {

  for (int i = 0; text[i] != '\0'; i++) {
    const unsigned char *letter_bitmap = FONT_BITMAP[(int)text[i] - 32];
    for (int j = 0; j < 13; j++) {
      for (int k = 0; k < 8; k++) {

        if (letter_bitmap[j] & 1 << k) {
          (*buffer)[x + (8 - k) + i * 8 + i][y + 12 - j] = -2;
        } else {
          (*buffer)[x + (8 - k) + i * 8 + i][y + 12 - j] = 0;
        }
      }
    }
  }
}
