#include <assert.h>
#include <stdio.h>

#include "wide_camera.h"

static void TestWrappedCoordinates(void) {
  assert(WideCamera_IsMapMenu(14, 3));
  assert(WideCamera_IsMapMenu(14, 7));
  assert(WideCamera_IsMapMenu(14, 10));
  assert(!WideCamera_IsMapMenu(9, 7));
  assert(!WideCamera_IsMapMenu(14, 2));
  assert(WideCamera_Unwrap16(0xff00, 0x0000) == -256);
  assert(WideCamera_Unwrap16(0x0000, 0xff00) == 65536);
  assert(WideCamera_ClampToBounds(0, -256, 0, 72) == -72);
}

static void TestStableCamera(void) {
  assert(WideCamera_ClampToBounds(0, 0, 256, 72) == 72);
  assert(WideCamera_ClampToBounds(128, 0, 256, 72) == 128);
  assert(WideCamera_ClampToBounds(256, 0, 256, 72) == 184);
  assert(WideCamera_ClampToBounds(0, 0, 128, 72) == 0);
}

static void TestDungeonTransitionTargets(void) {
  assert(WideCamera_FindDungeonTransitionEnd(472, -1, 0) == 2);
  assert(WideCamera_FindDungeonTransitionEnd(780, -1, 0) == 514);
  assert(WideCamera_FindDungeonTransitionEnd(768, 1, 0) == 1024);
  assert(WideCamera_FindDungeonTransitionEnd(1024, 1, 256) == 1280);
}

static void TestLerp(void) {
  assert(WideCamera_Lerp(72, -72, 0) == 72);
  assert(WideCamera_Lerp(72, -72, 128) == 0);
  assert(WideCamera_Lerp(72, -72, 256) == -72);
  assert(WideCamera_Lerp(0, 16, 8) == 1);    // 0.5 rounds away from zero
  assert(WideCamera_Lerp(16, 0, 8) == 15);
  assert(WideCamera_Lerp(0, 16, 7) == 0);
}

static void TestApproach(void) {
  assert(WideCamera_Approach(16, 0, 1) == 15);
  assert(WideCamera_Approach(0, 72, 3) == 3);
  assert(WideCamera_Approach(70, 72, 3) == 72);
  assert(WideCamera_Approach(5, 5, 0) == 5);
}

static void TestFollow(void) {
  // Clamped at an edge: the rule moves with the camera, so does the view.
  assert(WideCamera_Follow(10, 10, 14, 4, 1) == 14);
  assert(WideCamera_Follow(10, 10, 6, -4, 1) == 6);
  // A gap already there stays, and closes by the rate.
  assert(WideCamera_Follow(20, 10, 14, 4, 1) == 23);
  // A jump the camera does not explain is closed by the rate only.
  assert(WideCamera_Follow(16, 16, 0, 2, 1) == 15);
  assert(WideCamera_Follow(16, 16, 0, -2, 1) == 13);
  assert(WideCamera_Follow(0, 0, 72, 0, 3) == 3);
}

int main(void) {
  TestWrappedCoordinates();
  TestStableCamera();
  TestDungeonTransitionTargets();
  TestLerp();
  TestApproach();
  TestFollow();
  puts("wide camera tests passed");
  return 0;
}
