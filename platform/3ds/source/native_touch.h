#pragma once
#include <3ds.h>
#include <SDL.h>
#include <stdbool.h>
#include <math.h>

// SDL renderer event watches rewrite FINGER coordinates using their viewport.
// Preserve the native 320x240 point before those watches run, in a private
// queued event they cannot reinterpret. No polling thread or allocation.
static Uint32 native_touch_event;
static SDL_EventFilter native_touch_previous_filter;
static void *native_touch_previous_data;

// SDL's 3DS backend reads the touchscreen once per SDL_PumpEvents and reports
// only the not-touching -> touching edge, so the contact has to still be down
// at the instant the frame happens to pump. A contact that opens and closes
// between two pumps is never observed at all, which is what a long WIDE frame
// makes routine.
//
// NativeTouch_Sample reads the same HID state from inside the frame and holds
// what it finds; NativeTouch_Flush hands it to SDL at the top of the next loop.
// SDL's own edge keeps producing taps exactly as before: this only adds the
// contacts SDL was going to miss. If the sample never fires at all, for any
// reason, touch still works the way it does without it.
//
// One flag decides which of the two reported a contact. Whichever sees it
// first sets it, and the other finds it set and stays quiet, so a contact both
// of them see is still one tap.
static bool native_touch_pressed;
static bool native_touch_pending;
static int native_touch_pending_code;
static unsigned native_touch_sdl_taps;
static unsigned native_touch_sampled_taps;

static int SDLCALL NativeTouch_Filter(void *unused, SDL_Event *event) {
  (void)unused;
  SDL_Event original = *event;
  if (native_touch_previous_filter &&
      !native_touch_previous_filter(native_touch_previous_data, event)) return 0;
  if (original.tfinger.touchId != 0) return 1;
  if (original.type == SDL_FINGERUP) {
    native_touch_pressed = false;
    return 1;
  }
  if (original.type != SDL_FINGERDOWN) return 1;
  float nx = original.tfinger.x, ny = original.tfinger.y;
  if (!isfinite(nx) || !isfinite(ny) || nx < 0 || nx > 1 || ny < 0 || ny > 1) return 0;
  int x = (int)lroundf(nx * 320), y = (int)lroundf(ny * 240);
  if (x > 319) x = 319;
  if (y > 239) y = 239;
  // Already reported from inside the frame: drop SDL's copy and leave the
  // queued point alone, it is the one with the untransformed coordinates.
  if (native_touch_pressed) return 0;
  native_touch_pressed = true;
  native_touch_sdl_taps++;
  SDL_zero(*event);
  event->user.type = native_touch_event;
  event->user.timestamp = original.tfinger.timestamp;
  event->user.code = (y << 16) | x;
  return 1;
}

// Called from inside the frame, wherever a few microseconds are free. Costs one
// HID shared-memory read. hidKeysDown()/hidKeysUp() edges are consumed by the
// scan, but on this port the game reads hidKeysHeld() through
// Platform3DS_ReadInput, which scans for itself before every logic step, and no
// SDL joystick is opened on 3DS.
static void NativeTouch_Sample(void) {
  if (!native_touch_event) return;
  hidScanInput();
  bool pressed = (hidKeysHeld() & KEY_TOUCH) != 0;
  bool began = pressed && !native_touch_pressed;
  native_touch_pressed = pressed;
  if (!began) return;
  // Coordinates stay populated after a release, so they are only read once
  // KEY_TOUCH has said this sample is a contact.
  touchPosition touch;
  hidTouchRead(&touch);
  if (touch.px >= 320 || touch.py >= 240) return;
  native_touch_pending_code = (int)(((Uint32)touch.py << 16) | (Uint32)touch.px);
  native_touch_pending = true;
}

// Called once per loop, immediately before the events are drained, so a point
// held from inside the previous frame is picked up by that same drain.
static void NativeTouch_Flush(void) {
  if (!native_touch_event || !native_touch_pending) return;
  native_touch_pending = false;
  native_touch_sampled_taps++;
  SDL_Event event;
  SDL_zero(event);
  event.user.type = native_touch_event;
  event.user.timestamp = SDL_GetTicks();
  event.user.code = native_touch_pending_code;
  SDL_PushEvent(&event);
}

static bool NativeTouch_Init(void) {
  if (native_touch_event) return true;
  Uint32 type = SDL_RegisterEvents(1);
  if (type == (Uint32)-1) return false;
  native_touch_event = type;
  native_touch_pressed = native_touch_pending = false;
  native_touch_sdl_taps = native_touch_sampled_taps = 0;
  SDL_GetEventFilter(&native_touch_previous_filter, &native_touch_previous_data);
  SDL_SetEventFilter(NativeTouch_Filter, NULL);
  return true;
}
static bool NativeTouch_Decode(const SDL_Event *event, float *x, float *y) {
  if (!native_touch_event || event->type != native_touch_event) return false;
  *x = (Uint32)event->user.code & 0xffff;
  *y = (Uint32)event->user.code >> 16;
  return *x < 320 && *y < 240;
}
static void NativeTouch_Shutdown(void) {
  if (!native_touch_event) return;
  SDL_EventFilter current; void *data;
  SDL_GetEventFilter(&current, &data);
  if (current == NativeTouch_Filter)
    SDL_SetEventFilter(native_touch_previous_filter, native_touch_previous_data);
  SDL_FlushEvent(native_touch_event);
  native_touch_event = 0;
  native_touch_pressed = native_touch_pending = false;
  native_touch_previous_filter = NULL;
  native_touch_previous_data = NULL;
}
