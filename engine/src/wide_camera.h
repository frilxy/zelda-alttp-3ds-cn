#ifndef ZELDA3_WIDE_CAMERA_H_
#define ZELDA3_WIDE_CAMERA_H_

int WideCamera_IsMapMenu(int module, int submodule);

// Dungeon-map state 0 still fades the room; states 7/8 restore it. Only
// states 1..6 contain the map picture (or its black preparation frames).
static inline int WideCamera_DungeonMapPicture(int module, int submodule, int state) {
  return module == 14 && submodule == 3 && state >= 1 && state <= 6;
}

int WideCamera_Unwrap16(int value, int reference);
int WideCamera_ClampToBounds(int logical_x, int left_bound,
                             int right_bound, int margin);
int WideCamera_FindDungeonTransitionEnd(int start_x, int direction,
                                        int target);
// From 'from' to 'to' at progress/256, rounded to the nearest pixel.
int WideCamera_Lerp(int from, int to, int progress);
// Towards 'to' by at most 'step'.
int WideCamera_Approach(int from, int to, int step);
// Follow a rule that moved from last_target to target while the camera
// moved by 'moved' pixels: take the part of that move the camera's own move
// explains (same direction, at most as far), then approach by 'rate'.
int WideCamera_Follow(int last, int last_target, int target, int moved,
                      int rate);

#endif  // ZELDA3_WIDE_CAMERA_H_
