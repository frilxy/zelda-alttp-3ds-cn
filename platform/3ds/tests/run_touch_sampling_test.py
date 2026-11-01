#!/usr/bin/env python3
"""A tap must survive a frame longer than the tap.

SDL's 3DS backend reads the touchscreen once per SDL_PumpEvents and reports
only the not-touching -> touching edge, so a contact that opens and closes
between two pumps is never seen. WIDE renders 400x240 instead of 256x224 and
moves the pumps far enough apart that ordinary taps land in that gap.

This compiles the real native_touch.h against a model of the 3DS HID timeline
and of SDL's own edge detector, and checks that every contact produces exactly
one tap at its own coordinates however the frame is cut up around it.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HEADER = Path(os.environ.get('NATIVE_TOUCH_SOURCE',
                             ROOT / 'platform/3ds/source/native_touch.h'))
PANEL = ROOT / 'engine/src/platform/linux/second_screen_sdl.c'
LOOP = Path(os.environ.get('MAIN_SOURCE', ROOT / 'engine/src/main.c'))

SKIP_PREFIXES = ('#pragma once', '#include <3ds.h>', '#include <SDL.h>',
                 '#include <stdbool.h>', '#include <math.h>')

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint32_t u32;
typedef uint16_t u16;

#define SDLCALL
#define SDL_FINGERDOWN   0x700u
#define SDL_FINGERUP     0x701u
#define SDL_FINGERMOTION 0x702u
#define KEY_TOUCH        (1u << 20)

typedef struct { u16 px, py; } touchPosition;

typedef struct { Uint32 type, timestamp; } SDL_CommonEvent;
typedef struct {
  Uint32 type, timestamp, windowID;
  long long touchId, fingerId;
  float x, y, dx, dy, pressure;
} SDL_TouchFingerEvent;
typedef struct {
  Uint32 type, timestamp, windowID;
  Sint32 code;
  void *data1, *data2;
} SDL_UserEvent;
typedef union {
  Uint32 type;
  SDL_CommonEvent common;
  SDL_TouchFingerEvent tfinger;
  SDL_UserEvent user;
  unsigned char padding[64];
} SDL_Event;

#define SDL_zero(x) memset(&(x), 0, sizeof(x))
typedef int (*SDL_EventFilter)(void *userdata, SDL_Event *event);

/* --- the console's HID timeline -------------------------------------- */
/* The hardware keeps the last coordinates after a release, which is why the
   port reads KEY_TOUCH rather than trusting the position alone. */
static bool hw_pressed;
static u16 hw_x, hw_y;
static u32 latched_keys;
static touchPosition latched_touch;
static int scan_count;

static void hidScanInput(void) {
  scan_count++;
  latched_keys = hw_pressed ? KEY_TOUCH : 0;
  if (hw_pressed) { latched_touch.px = hw_x; latched_touch.py = hw_y; }
}
static u32 hidKeysHeld(void) { return latched_keys; }
static void hidTouchRead(touchPosition *p) { *p = latched_touch; }

/* --- just enough SDL -------------------------------------------------- */
static SDL_Event queue[64];
static int queue_len;
static SDL_EventFilter host_filter;
static void *host_filter_data;
static Uint32 host_ticks;

static Uint32 SDL_RegisterEvents(int n) { (void)n; return 0x8000u; }
static Uint32 SDL_GetTicks(void) { return host_ticks; }
static void SDL_GetEventFilter(SDL_EventFilter *f, void **d) {
  *f = host_filter; *d = host_filter_data;
}
static void SDL_SetEventFilter(SDL_EventFilter f, void *d) {
  host_filter = f; host_filter_data = d;
}
static int SDL_PushEvent(SDL_Event *event) {
  SDL_Event queued = *event;
  if (host_filter && !host_filter(host_filter_data, &queued)) return 0;
  assert(queue_len < (int)(sizeof(queue) / sizeof(queue[0])));
  queue[queue_len++] = queued;
  return 1;
}
static void SDL_FlushEvent(Uint32 type) {
  int kept = 0;
  for (int i = 0; i < queue_len; i++)
    if (queue[i].type != type) queue[kept++] = queue[i];
  queue_len = kept;
}

HEADER_UNDER_TEST

/* --- SDL's own once-per-pump edge detector (SDL_n3dstouch.c) ---------- */
static bool sdl_was_pressed;
static void sdl_pump(void) {
  hidScanInput();
  bool pressed = (hidKeysHeld() & KEY_TOUCH) != 0;
  if (pressed != sdl_was_pressed) {
    touchPosition touch;
    hidTouchRead(&touch);
    sdl_was_pressed = pressed;
    SDL_Event event;
    SDL_zero(event);
    event.type = pressed ? SDL_FINGERDOWN : SDL_FINGERUP;
    event.tfinger.type = event.type;
    event.tfinger.touchId = 0;
    event.tfinger.x = touch.px * (1.0f / 320.0f);
    event.tfinger.y = touch.py * (1.0f / 240.0f);
    SDL_PushEvent(&event);
  }
}

/* --- helpers ---------------------------------------------------------- */
static void contact(int x, int y) { hw_pressed = true; hw_x = (u16)x; hw_y = (u16)y; }
static void release(void) { hw_pressed = false; }

static int taps(float *x, float *y) {
  int found = 0;
  for (int i = 0; i < queue_len; i++) {
    float tx, ty;
    if (NativeTouch_Decode(&queue[i], &tx, &ty)) {
      if (found == 0) { *x = tx; *y = ty; }
      found++;
    }
  }
  return found;
}
static void drain(void) { queue_len = 0; }

static void expect_one(int x, int y, const char *what) {
  float tx = -1, ty = -1;
  int n = taps(&tx, &ty);
  if (n != 1 || (int)tx != x || (int)ty != y) {
    printf("FAIL %s: %d tap(s), first at (%d,%d), wanted 1 at (%d,%d)\n",
           what, n, (int)tx, (int)ty, x, y);
    fflush(stdout);
    assert(0);
  }
  drain();
}
static void expect_none(const char *what) {
  float tx, ty;
  int n = taps(&tx, &ty);
  if (n != 0) { printf("FAIL %s: %d tap(s), wanted none\n", what, n);
                fflush(stdout); assert(0); }
  drain();
}

int main(void) {
  assert(NativeTouch_Init());

  /* One turn of the game loop: the point held from inside the previous frame
     is handed over first, then SDL pumps, then the queue is drained. */
/* The real loop hands over first, then pumps, then reads once itself, so the
     contact state is refreshed every turn exactly as SDL's own is. */
#define FRAME_TOP() do { NativeTouch_Flush(); sdl_pump(); NativeTouch_Sample(); } while (0)
  /* The same turn with the in-frame reading taken out entirely. */
#define SDL_ONLY_TOP() do { NativeTouch_Flush(); sdl_pump(); } while (0)

  /* The property that matters most: with the in-frame reading never running at
     all, every contact SDL sees still becomes exactly one tap. This is what
     the port did before the reading existed, and it has to keep doing it, so
     that a reading that fails on hardware can only cost the taps it was added
     to rescue. */
  for (int i = 0; i < 16; i++) {
    contact(20 + i, 30 + i);
    SDL_ONLY_TOP();
    expect_one(20 + i, 30 + i, "SDL alone, no in-frame reading");
    release();
    SDL_ONLY_TOP();
    expect_none("SDL alone, release");
  }

  /* A contact that opens and closes inside one long frame: SDL never sees it,
     the in-frame reading holds it, the next frame top delivers it. */
  FRAME_TOP();
  contact(64, 100);
  NativeTouch_Sample();
  release();
  FRAME_TOP();
  expect_one(64, 100, "tap entirely inside one frame");

  /* Seen by both: one tap, not two. */
  contact(200, 30);
  NativeTouch_Sample();
  FRAME_TOP();
  expect_one(200, 30, "contact seen in frame then by SDL");
  NativeTouch_Sample();
  FRAME_TOP();
  expect_none("still held is not a second tap");
  release();
  FRAME_TOP();
  expect_none("release is not a tap");

  /* Seen by SDL first: the in-frame reading must stay quiet. */
  contact(111, 222);
  FRAME_TOP();
  expect_one(111, 222, "contact seen by SDL first");
  for (int i = 0; i < 4; i++) NativeTouch_Sample();
  FRAME_TOP();
  expect_none("in-frame reading adds nothing to an SDL contact");
  release();
  FRAME_TOP();
  drain();

  /* Back-to-back contacts, each caught in frame. */
  contact(10, 200);
  NativeTouch_Sample();
  release();
  FRAME_TOP();
  expect_one(10, 200, "first of two contacts");
  contact(300, 5);
  NativeTouch_Sample();
  release();
  FRAME_TOP();
  expect_one(300, 5, "second of two contacts");

  /* Stale hardware coordinates after a release must never become a tap. */
  release();
  for (int i = 0; i < 8; i++) { FRAME_TOP(); NativeTouch_Sample(); }
  expect_none("stale coordinates after release");

  /* Held contact across many frames: one tap however it is read. */
  contact(8, 231);
  for (int i = 0; i < 20; i++) { FRAME_TOP(); NativeTouch_Sample(); }
  expect_one(8, 231, "held contact");
  release();
  FRAME_TOP();
  drain();

  /* Every corner survives untouched, by either path: no viewport rescaling,
     no rounding, which is what the private event exists for. */
  static const int corners[4][2] = {{0, 0}, {319, 0}, {0, 239}, {319, 239}};
  for (int i = 0; i < 4; i++) {
    contact(corners[i][0], corners[i][1]);
    NativeTouch_Sample();
    release();
    FRAME_TOP();
    expect_one(corners[i][0], corners[i][1], "corner read in frame");
    contact(corners[i][0], corners[i][1]);
    FRAME_TOP();
    expect_one(corners[i][0], corners[i][1], "corner read by SDL");
    release();
    FRAME_TOP();
    drain();
  }

  /* A run of taps under a frame long enough to hide each one from SDL. */
  unsigned before_sdl = native_touch_sdl_taps, before_frame = native_touch_sampled_taps;
  int delivered = 0;
  for (int i = 0; i < 64; i++) {
    FRAME_TOP();
    contact(1 + i * 4, 2 + i * 3);
    NativeTouch_Sample();
    release();
    FRAME_TOP();
    float tx, ty;
    int n = taps(&tx, &ty);
    assert(n == 1 && (int)tx == 1 + i * 4 && (int)ty == 2 + i * 3);
    delivered += n;
    drain();
  }
  assert(delivered == 64);
  assert(native_touch_sdl_taps == before_sdl);
  assert(native_touch_sampled_taps == before_frame + 64);

  /* The same run with only SDL reading, which is what the port does without
     this change: none of them arrive. */
  int without = 0;
  for (int i = 0; i < 64; i++) {
    SDL_ONLY_TOP();
    contact(1 + i * 4, 2 + i * 3);
    release();
    SDL_ONLY_TOP();
    float tx, ty;
    without += taps(&tx, &ty);
    drain();
  }
  assert(without == 0);

  int scans = scan_count;
  unsigned sdl_total = native_touch_sdl_taps, frame_total = native_touch_sampled_taps;
  NativeTouch_Shutdown();
  assert(!NativeTouch_Decode(&queue[0], &(float){0}, &(float){0}));

  printf("PASS touch sampling: %u taps from SDL alone with no in-frame reading, "
         "%u rescued from inside long frames, one tap per contact when both see "
         "it, releases from either path, stale coordinates ignored, 4 corners "
         "exact both ways, 64/64 short taps delivered where SDL alone delivers "
         "0/64 (%d HID scans)\n", sdl_total, frame_total, scans);
  return 0;
}
'''


def main():
    header = [line for line in HEADER.read_text().splitlines()
              if not line.startswith(SKIP_PREFIXES)]
    assert any('NativeTouch_Sample' in line for line in header), (
        'native_touch.h has no NativeTouch_Sample: the frame can only read the '
        'touchscreen where SDL pumps it')

    # Compiling this header against a model proves what the two functions do
    # together. It cannot prove that the loop calls both of them, and a call
    # that is not there is invisible to every compiler.
    #
    # It has to be checked because the two are not independent. NativeTouch_
    # Sample latches the contact, and the filter then drops SDL's copy of that
    # same contact as already reported, on the strength of the point the sample
    # is holding. NativeTouch_Flush is what hands that point back. A sample
    # without a flush therefore does not make a tap late, it destroys it: the
    # only reader left is the one whose events are being thrown away. The port
    # says so in its own diagnostics, as in_frame=0 beside pending=1.
    loop = LOOP.read_text()
    assert 'SecondScreenSDL_SampleTouch();' in loop, (
        'main.c no longer reads the touchscreen from inside the frame')
    flush = loop.find('SecondScreenSDL_FlushTouch();')
    assert flush >= 0, (
        'main.c reads the touchscreen from inside the frame and never hands '
        'the point back, so every contact the frame sees before SDL does is '
        'dropped by the filter and reaches nothing')
    drain = loop.find('while(SDL_PollEvent(&event))')
    assert drain >= 0, 'main.c no longer drains SDL events in a loop'
    assert flush < drain, (
        'the flush has to run before the events are drained, or the point it '
        'pushes waits for the next drain a whole frame later')

    # Root Settings keeps five destinations and the home control in its header.
    # The production UI fixture exercises the requested common Developer-sized
    # rows, their bounds and gaps; do not pin the superseded five-way height.
    panel = PANEL.read_text()
    body = panel[panel.index('static void draw_settings(RectFS r) {'):]
    body = body[:body.index('\nstatic void draw_cinema_settings_overlay')]
    assert 'language_chip_r' in body, 'the startup-home control left the header'
    assert 'for (int i = 0; i < 5; i++)' in body, 'root Settings must have five destinations'
    source = HARNESS.replace('HEADER_UNDER_TEST', '\n'.join(header))

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(source)
        subprocess.run(
            ['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Wno-unused-function',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             str(path / 'test.c'), '-o', str(path / 'test'), '-lm'],
            check=True)
        subprocess.run([str(path / 'test')], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
