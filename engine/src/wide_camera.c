#include "wide_camera.h"

#include <stdint.h>

int WideCamera_IsMapMenu(int module, int submodule) {
  // Dungeon map, world map, and the flute's destination-selection map.
  return module == 14 && (submodule == 3 || submodule == 7 || submodule == 10);
}

int WideCamera_Unwrap16(int value, int reference) {
  return reference + (int16_t)((uint16_t)value - (uint16_t)reference);
}

int WideCamera_ClampToBounds(int logical_x, int left_bound,
                             int right_bound, int margin) {
  if (margin <= 0 || right_bound - left_bound < margin * 2)
    return logical_x;

  int minimum = left_bound + margin;
  int maximum = right_bound - margin;
  if (logical_x < minimum)
    return minimum;
  if (logical_x > maximum)
    return maximum;
  return logical_x;
}

int WideCamera_FindDungeonTransitionEnd(int start_x, int direction,
                                        int target) {
  if (direction != -1 && direction != 1)
    return start_x;

  uint16_t current = (uint16_t)start_x;
  target &= 0x1fc;
  for (int i = 0; i < 256; i++) {
    current = (uint16_t)((current + direction * 2) & ~1);
    if ((current & 0x1fc) == target)
      return start_x + (int16_t)(current - (uint16_t)start_x);
  }
  return start_x + direction * 256;
}

int WideCamera_Lerp(int from, int to, int progress) {
  int d = (to - from) * progress;
  return from + (d >= 0 ? (d + 128) >> 8 : -((-d + 128) >> 8));
}

int WideCamera_Approach(int from, int to, int step) {
  return to > from + step ? from + step : to < from - step ? from - step : to;
}

int WideCamera_Follow(int last, int last_target, int target, int moved,
                      int rate) {
  int change = target - last_target;
  int low = moved < 0 ? moved : 0, high = moved > 0 ? moved : 0;
  int follow = last + (change < low ? low : change > high ? high : change);
  return WideCamera_Approach(follow, target, rate);
}
