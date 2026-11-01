#!/usr/bin/env python3
"""The redraw a tap asks for has to reach the screen without being scheduled.

The panel is drawn by a worker one priority below the game thread on the same
core, so it runs on what the game leaves over. In WIDE the PPU renders 400x240
instead of 256x224, and a frame that overruns the refresh leaves nothing at
all: the game thread does not block at a vblank it has already passed, and a
strictly lower priority thread on that core is then never scheduled. The report
was exact about what that looks like. The tab changes, nothing appears, and the
panel finally shows the tab that was pressed when something unrelated lets the
worker run, such as going back to ORIGINAL or walking through a door.

Lifting the worker above the game is not the answer: the iris between screens
is drawn by the game thread, and bottom_worker_priority has always kept panel
work at or below it for that reason. So the tap's redraw is drawn on the game
thread itself.

This compiles the real SecondScreenSDL_BeginFrame out of second_screen_sdl.c
and drives it with a worker that never runs, which is the case the report is.
What it checks is that the tap still reaches the screen, in the frame after the
tap, with no thread priority touched.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
UI = Path(os.environ.get('SECOND_SCREEN_SOURCE',
                         ROOT / 'engine/src/platform/linux/second_screen_sdl.c'))

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int s32;
static void *ss_old_display_pixels;
static bool ss_is_new_3ds, ss_scene_redraw_pending, ss_worker_interactive;
static bool ss_touch_redraw_pending, ss_worker_door_transition;
static bool ss_worker_busy, ss_worker_sidebar_patch, ss_worker_map_patch;
static bool ss_worker_running, ss_enabled = true, ss_frame_ready;
static bool ss_present_retry, art_ready = true, update_mode;
static bool ss_touch_sync_redraw;
typedef struct { unsigned revision; } UpdateStatus;
static void Updater_GetStatus(UpdateStatus *s) { s->revision = 0; }
static int ss_worker_thread = 1;
static int ss_worker_interactive_priority = 0x2f, ss_worker_scene_priority = 0x30;
static int ss_worker_idle_priority = 0x31;
static int ss_gpu_source_buffer = -1;
static int ss_front_buffer = 0, ss_worker_buffer, ss_win = 1;
static int ss_worker_start, ss_worker_done, ss_worker_logic_frames;
static int priority = 0x31, tab, module = 9, area;
static int load_result, load_flash_until;
static uint32_t ss_redraw_requests;
static uint64_t ss_touch_request_ticks, ss_worker_touch_request_ticks;
static uint8_t live[128];
enum { TAB_MAP, TAB_ITEMS, TAB_GEAR, TAB_SETTINGS, MODE_GAME,
       kBottomRedrawFull = 1, kBottomRedrawHud = 2, kBottomRedrawMap = 4 };

// The worker in this model never runs: that is the frame the report describes.
// Its job is only ever collected when the test says so.
static int signals, waits, done_ready, drawn, drawn_into, drawn_tab;
static int threadGetHandle(int t) { return t; }
static void svcSetThreadPriority(int t, int p) { (void)t; priority = p; }
static uint64_t svcGetSystemTick(void) { return 100; }
static void SS_ReadSram(uint8_t *p, unsigned n) { memcpy(p, live, n); }
static int SS_GetModule(void) { return module; }
static int SS_GetArea(void) { return area; }
static int SS_GetDungeon(void) { return 0; }
static bool SS_IsIndoors(void) { return module == 7; }
static int SS_GetEquippedSlot(void) { return 0; }
static bool SS_GetMirrorPortal(int *p) { (void)p; return false; }
static int SS_GetLinkX(void) { return 0; }
static int SS_GetLinkY(void) { return 0; }
static uint32_t SDL_GetTicks(void) { return 1000; }
static int mode_for_module(int m) { (void)m; return MODE_GAME; }
static void request_bottom_redraw(unsigned r) { ss_redraw_requests |= r; }
static bool Platform3DS_IsSystemClosing(void) { return false; }
static int SS_TakeLoadDumpStateResult(void) { return -1; }
static bool ensure_window(void) { return true; }
static bool ensure_second_screen_worker(void) { return true; }
static bool bottom_needs_periodic_redraw(void) { return true; }
static bool can_patch_bottom_map(void) { return !ss_is_new_3ds; }
static bool can_patch_bottom_sidebar(void) { return !ss_is_new_3ds; }
static void LightEvent_Wait(int *e) { (void)e; waits++; done_ready = 0; }
static bool LightEvent_TryWait(int *e) { (void)e; int d = done_ready; done_ready = 0; return d; }
static void LightEvent_Signal(int *e) { if (e == &ss_worker_start) signals++; else done_ready = 1; }
static void draw_bottom_map_patch(void) {}
static void draw_bottom_sidebar_patch(void) {}
static void draw_second_screen(int n) {
  (void)n;
  // A panel drawn from the game thread must never outrank the game.
  assert(priority >= ss_worker_scene_priority);
  drawn++;
  drawn_into = ss_worker_buffer;
  drawn_tab = tab;
}

STATE_UNDER_TEST
FUNCTIONS_UNDER_TEST

int main(void) {
  live[0x6c] = 24; live[0x6d] = 24;
  request_bottom_redraw_on_state_change();
  ss_redraw_requests = 0;

  // New 3DS in WIDE, the frame the report is about: the worker was given a job
  // and the game never leaves it any time, so nothing it draws will ever land.
  ss_is_new_3ds = true;
  ss_worker_busy = true;
  ss_frame_ready = false;
  ss_front_buffer = 0;
  ss_worker_buffer = 1;
  priority = ss_worker_idle_priority;
  int before_priority = priority, before_signals = signals;

  // Frames pass. Without the tap, the panel stays where it is: this is the
  // starvation itself, and it is not what this change is claiming to fix.
  for (int i = 0; i < 8; i++) SecondScreenSDL_BeginFrame(1);
  assert(!drawn && !ss_frame_ready);

  // The tap. Its redraw is drawn in the very next frame, into the back buffer,
  // and published, with no thread priority touched and no worker job started.
  tab = TAB_GEAR;
  ss_touch_sync_redraw = true;
  ss_redraw_requests = kBottomRedrawFull;
  done_ready = 1;  // the starved job is collected by the wait, not before it
  SecondScreenSDL_BeginFrame(1);
  assert(drawn == 1);
  assert(drawn_tab == TAB_GEAR);
  assert(ss_frame_ready);
  assert(ss_front_buffer == drawn_into);
  assert(!ss_touch_sync_redraw);
  assert(!ss_worker_busy);
  assert(priority == before_priority);
  assert(signals == before_signals);
  assert(ss_redraw_requests == 0);

  // It is not a state the port can stay in: the next frames go back to the
  // worker, and one tap draws one panel.
  int after = drawn;
  for (int i = 0; i < 4; i++) SecondScreenSDL_BeginFrame(1);
  assert(drawn == after);

  // A tap while the worker is idle needs no wait at all.
  ss_worker_busy = false;
  ss_frame_ready = false;
  int before_waits = waits;
  tab = TAB_MAP;
  ss_touch_sync_redraw = true;
  SecondScreenSDL_BeginFrame(1);
  assert(drawn == after + 1 && drawn_tab == TAB_MAP && ss_frame_ready);
  assert(waits == before_waits);
  assert(ss_front_buffer == drawn_into);

  // The back buffer alternates, so a tap never draws over the image on screen.
  int first = ss_front_buffer;
  ss_frame_ready = false;
  ss_touch_sync_redraw = true;
  SecondScreenSDL_BeginFrame(1);
  assert(ss_front_buffer != first && drawn_into != first);

  // Between screens the game thread is drawing the iris. The panel is drawn on
  // that same thread now, so it can never outrank it, which draw_second_screen
  // asserts on every call. What matters here is that it still terminates.
  module = 15;
  ss_frame_ready = false;
  ss_touch_sync_redraw = true;
  SecondScreenSDL_BeginFrame(1);
  assert(!ss_touch_sync_redraw && ss_frame_ready);
  module = 9;

  // Old 3DS is untouched: it answers a tap the way it was tuned to, through
  // the worker and the touch bookkeeping, and never takes this path.
  ss_is_new_3ds = false;
  ss_worker_busy = false;
  ss_frame_ready = false;
  int old_drawn = drawn;
  ss_redraw_requests = kBottomRedrawFull;
  ss_touch_redraw_pending = true;
  ss_touch_request_ticks = 25;
  SecondScreenSDL_BeginFrame(1);
  assert(drawn == old_drawn);        // the worker draws it, not this thread
  assert(signals > before_signals);  // and it was given the job

  printf("PASS touch redraw: the tap's panel is drawn by the game thread in the "
         "next frame and published, %d draws in all, no priority changed, no job "
         "started, back buffer alternates, terminates between screens, Old 3DS "
         "unchanged\n", drawn);
  return 0;
}
'''

WANTED = [
    'static void prioritize_bottom_scene(void)',
    'static void prioritize_bottom_touch(void)',
    'static s32 bottom_worker_priority(void)',
    'static void prioritize_bottom_hud(void)',
    'static void request_bottom_redraw_on_state_change(void)',
    'void SecondScreenSDL_BeginFrame(',
]


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[brace] == '{') - (text[brace] == '}')
        brace += 1
    return text[start:brace] + '\n'


def main():
    source = UI.read_text().replace('\r', '')

    # The flag has to be set where the tap is handled, or the frame below is
    # never asked for anything. handle_tap is full of panel geometry and is not
    # compiled here, so the two places it sets it are read instead.
    tap = source[source.index('static void handle_tap(float x, float y) {'):]
    tap = tap[:tap.index('\nbool SecondScreenSDL_HandleEvent')]
    assert tap.count('ss_touch_sync_redraw = true;') == 2, (
        'handle_tap no longer asks the game thread to draw the redraw a tap '
        'owes, on both the navigating tap and the tap on the live tab')
    assert 'ss_is_new_3ds' in tap, (
        'the game-thread redraw has to stay New 3DS only: Old 3DS answers a '
        'tap through the worker and its own touch bookkeeping')

    state = source[source.index('typedef struct BottomCriticalState'):]
    state = state[:state.index('static void request_bottom_redraw_on_state_change')]
    body = '\n'.join(function(source, signature) for signature in WANTED)

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path / 'test.c').write_text(HARNESS
                                     .replace('STATE_UNDER_TEST', state)
                                     .replace('FUNCTIONS_UNDER_TEST', body))
        subprocess.run(
            ['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Wno-unused-function',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             str(path / 'test.c'), '-o', str(path / 'test')],
            check=True)
        subprocess.run([str(path / 'test')], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
