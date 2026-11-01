#!/usr/bin/env python3
"""Exercise the real paused-menu DSP path without taking SDL channel/callback ownership."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'platform/3ds/source/platform_3ds.c').read_text();start=s.index('// SDL owns NDSP channel 0');end=s.index('static void PresentSetupConsole(void);',start)
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef struct {const int16_t *data_pcm16;unsigned nsamples;} ndspWaveBuf;
static const int16_t kSetupMoveSamples[]={1,-2,3,-4};
enum{kSetupMoveSampleCount=4,kSetupAudioRate=32000,NDSP_INTERP_LINEAR=1,NDSP_FORMAT_MONO_PCM16=1};
static const float kSetupMoveMix=.75f;
static bool g_setup_audio_initialized,fail_alloc;
static unsigned clear_count,add_count,reset_count,setup_count;
static void *linearAlloc(size_t n){return fail_alloc?NULL:malloc(n);}
static void linearFree(void*p){free(p);}
static void DSP_FlushDataCache(void*p,size_t n){assert(n==sizeof(kSetupMoveSamples));}
static void SetupAudioPlayMove(void){setup_count++;}
static void ndspChnWaveBufClear(int c){assert(c==23);clear_count++;}
static void ndspChnReset(int c){assert(c==23);reset_count++;}
static void ndspChnSetInterp(int c,int i){assert(c==23&&i==NDSP_INTERP_LINEAR);}
static void ndspChnSetRate(int c,int hz){assert(c==23&&hz==kSetupAudioRate);}
static void ndspChnSetFormat(int c,int f){assert(c==23&&f==NDSP_FORMAT_MONO_PCM16);}
static void SetupAudioSetMix(int c,float mix){assert(c==23&&mix==kSetupMoveMix);}
static void ndspChnWaveBufAdd(int c,ndspWaveBuf *w){assert(c==23&&w->nsamples==4&&!memcmp(w->data_pcm16,kSetupMoveSamples,sizeof(kSetupMoveSamples)));add_count++;}
'''+s[start:end]+r'''
int main(void){
 Platform3DS_PlayMenuMove();Platform3DS_SetMenuAudio(false);assert(!clear_count&&!add_count);
 fail_alloc=true;Platform3DS_SetMenuAudio(true);assert(!g_menu_audio_enabled&&!clear_count);
 fail_alloc=false;Platform3DS_SetMenuAudio(true);assert(g_menu_audio_enabled&&reset_count==1);
 Platform3DS_SetMenuAudio(true);assert(reset_count==1);
 for(int i=0;i<20;i++)Platform3DS_PlayMenuMove();assert(add_count==20);
 Platform3DS_SetMenuAudio(false);Platform3DS_PlayMenuMove();assert(add_count==20);
 Platform3DS_SetMenuAudio(true);assert(reset_count==2);Platform3DS_PlayMenuMove();assert(add_count==21);
 Platform3DS_SetMenuAudio(false);g_setup_audio_initialized=true;Platform3DS_PlayMenuMove();assert(setup_count==1&&add_count==21);
 puts("PASS paused-menu audio: copied PCM, channel 23 only, repeated moves, mute/close, reopen and allocation failure; SDL callback/channel untouched");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'audio.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined',str(p/'audio.c'),'-o',str(p/'audio')],check=True)
 subprocess.run([str(p/'audio')],check=True)
