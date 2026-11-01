#!/usr/bin/env python3
"""Exercise production bottom-worker shutdown ordering and restart state."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'engine/src/platform/linux/second_screen_sdl.c').read_text()
a=s.index('void SecondScreenSDL_Shutdown(void)');b=s.index('{',a)+1;depth=1
while depth:
    depth+=(s[b]=='{')-(s[b]=='}');b+=1
function=s[a:b]
special={'ss_worker_thread','ss_present_pixels','ss_old_display_pixels','ss_worker_hearts','ss_display_hearts','ss_win','ss_r'}
names=sorted(set(re.findall(r'\bss_\w+\b',function))-special)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __3DS__ 1
#define R_FAILED(r) ((r)<0)
#define Platform3DS_LogRuntime(...) ((void)0)
typedef int Result;
static void *ss_worker_thread,*ss_present_pixels[2],*ss_old_display_pixels,*ss_win,*ss_r;
static char ss_worker_hearts[2],ss_display_hearts;
static bool has_last_outdoor,hud_pref_applied;
static int view_floor_offset,view_floor_touched_at,tab,joined,had_worker,cleared;
enum {TAB_MAP=1,kBottomRedrawFull=2};
'''
code+='static int '+','.join(names)+';\n'
code+=r'''
static void NativeTouch_Shutdown(void){}
static void LightEvent_Signal(int *e){}
static Result threadJoin(void *t,uint64_t timeout) {
  assert(tab==7 && !cleared);assert(!ss_worker_running);joined=1;return 0;
}
static void threadFree(void *t){assert(joined);}
static void linearFree(void *p){assert(!had_worker||joined);free(p);}
static void leave_settings_submenu(void){assert(!had_worker||joined);cleared=1;}
static void SS_ResetRomCaches(void){assert(cleared);}
static void destroy_textures(void){assert(!had_worker||joined);}
static void SDL_DestroyRenderer(void *p){}
static void SDL_DestroyWindow(void *p){}
'''+function+r'''
int main(void) {
  for(int worker=0;worker<2;worker++) for(int window=0;window<2;window++) {
    joined=cleared=0;had_worker=worker;tab=7;ss_worker_thread=worker?(void*)1:NULL;
    ss_win=ss_r=window?(void*)2:NULL;ss_worker_running=1;
    ss_worker_busy=ss_frame_ready=1;ss_front_buffer=ss_gpu_source_buffer=1;
    ss_present_pixels[0]=malloc(32);ss_present_pixels[1]=malloc(32);
    ss_old_display_pixels=malloc(32);
    SecondScreenSDL_Shutdown();
    assert(!ss_worker_thread&&!ss_worker_busy&&!ss_frame_ready);
    assert(ss_front_buffer==-1&&ss_gpu_source_buffer==-1);
    assert(ss_redraw_requests==kBottomRedrawFull&&tab==TAB_MAP);
    assert(!ss_present_pixels[0]&&!ss_present_pixels[1]&&!ss_old_display_pixels);
    assert(!ss_win&&!ss_r);
  }
  puts("PASS production ROM shutdown: join pending draw before menu/cache/buffer mutation, reset retained GPU source, worker/window present and absent");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-rom-shutdown-') as directory:
    p=Path(directory);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
