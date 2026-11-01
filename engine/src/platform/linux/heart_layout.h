#ifndef ZELDA3_HEART_LAYOUT_H_
#define ZELDA3_HEART_LAYOUT_H_

// Integer pixel layout shared by SDL and the retained Old 3DS heart layer.
// Maximize heart size, then fill every column before starting the next row.
typedef struct NativeHeartLayout {
  int size, columns, step, width, height;
} NativeHeartLayout;
static NativeHeartLayout NativeHearts_Layout(int count, int width, int height) {
  NativeHeartLayout best = {0};
  for (int columns = 1; columns <= count && columns <= 10; columns++) {
    int rows = (count + columns - 1) / columns;
    int size = (width - (columns - 1)) / columns;
    int vertical = (height - (rows - 1)) / rows;
    if (size > vertical) size = vertical;
    if (size > 16) size = 16;
    if (size > best.size) {
      best = (NativeHeartLayout){size, columns, size + 1,
        columns * size + columns - 1, rows * size + rows - 1};
    } else if (size > 0 && size == best.size && rows < (count + best.columns - 1) / best.columns) {
      best = (NativeHeartLayout){size, columns, size + 1,
        columns * size + columns - 1, rows * size + rows - 1};
    }
  }
  if (best.size > 0) {
    int columns = (width + 1) / (best.size + 1);
    if (columns > 10) columns = 10;
    if (columns > count) columns = count;
    int rows = (count + columns - 1) / columns;
    best = (NativeHeartLayout){best.size, columns, best.size + 1,
      columns * best.size + columns - 1, rows * best.size + rows - 1};
  }
  return best;
}
#endif
