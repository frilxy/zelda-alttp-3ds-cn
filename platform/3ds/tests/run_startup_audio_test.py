#!/usr/bin/env python3
"""Check setup intro NDSP ownership, bounded buffering and cleanup."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[3]
s=(root/'platform/3ds/source/platform_3ds.c').read_text()
def fn(name):
 m=re.search(re.escape(name)+r'[^;{]*\{',s);assert m,name
 end=m.end();depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[m.start():end]+'\n'
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
enum {NDSP_WBUF_FREE,NDSP_WBUF_QUEUED,NDSP_WBUF_PLAYING,NDSP_WBUF_DONE};
enum {NDSP_OUTPUT_STEREO,NDSP_INTERP_LINEAR,NDSP_FORMAT_MONO_PCM16,NDSP_FORMAT_STEREO_PCM16};
#define R_FAILED(x) ((x)<0)
typedef struct {const int16_t *data_pcm16;unsigned nsamples;bool looping;int status;} ndspWaveBuf;
static bool g_setup_audio_initialized;
static int16_t *g_setup_music_buffer,*g_setup_move_buffer;
static ndspWaveBuf g_setup_music_wavebuf,g_setup_move_wavebuf;
static const int16_t kSetupMusicSamples[]={1,2,3,4},kSetupMoveSamples[]={4,3,2,1};
enum {kSetupMusicSampleCount=4,kSetupMoveSampleCount=4,kSetupAudioRate=32728};
static int allocations,frees,exits,adds[3],clears[3],fail_at;
static bool dsp_alive;
static void *linearAlloc(size_t n){allocations++;return allocations==fail_at?NULL:malloc(n);}
static void linearFree(void*p){assert(!dsp_alive);frees++;free(p);}
static int ndspInit(void){dsp_alive=true;return 0;}
static void ndspExit(void){assert(clears[2]>0);dsp_alive=false;exits++;}
static void ndspSetCallback(void(*f)(void*),void*p){}
static void ndspChnWaveBufClear(int c){clears[c]++;}
static void ndspChnWaveBufAdd(int c,ndspWaveBuf *w){assert(dsp_alive);adds[c]++;w->status=NDSP_WBUF_QUEUED;}
static void ndspSetOutputMode(int m){assert(m==NDSP_OUTPUT_STEREO);}
static void ndspSetMasterVol(float v){}
static void ndspChnReset(int c){}
static void ndspChnSetInterp(int c,int v){}
static void ndspChnSetRate(int c,float v){if(c==2)assert(v==32040);}
static void ndspChnSetFormat(int c,int v){assert(v==(c==2?NDSP_FORMAT_STEREO_PCM16:NDSP_FORMAT_MONO_PCM16));}
static void ndspChnSetMix(int c,float *mix){}
static void DSP_FlushDataCache(void*p,size_t n){assert(n>0);}
static void gspWaitForVBlank(void){}
static void LogSetup(const char*s){}
'''
code+=s[s.index('enum { kSetupIntroBlocks='):s.index('static unsigned g_setup_intro_next;')+len('static unsigned g_setup_intro_next;')]
code+=s[s.index('enum {\n  kSetupMusicFadeInFrames'):s.index('static void SetupAudioSetMix(')]
for name in ['static void SetupAudioSetMix(','static void SetupAudioFrame(','static void SetupAudioStop(','static void SetupAudioInit(','static void SetupAudioStartMusic(','static void SetupAudioPlayIntro(','static void SetupAudioStopIntro(']:code+=fn(name)
code+=r'''
int main(void){
 int16_t pcm[kSetupIntroSamples*2];for(unsigned i=0;i<sizeof(pcm)/2;i++)pcm[i]=i;
 SetupAudioPlayIntro(pcm,kSetupIntroSamples,NULL);assert(!adds[2]);
 SetupAudioInit();assert(g_setup_audio_initialized&&allocations==3);
 SetupAudioStartMusic();assert(adds[0]==1&&g_setup_music_wavebuf.looping);
 for(int i=0;i<kSetupIntroBlocks;i++)SetupAudioPlayIntro(pcm,kSetupIntroSamples,NULL);
 assert(adds[2]==kSetupIntroBlocks);
 int16_t retained[kSetupIntroBlocks*kSetupIntroSamples*2];memcpy(retained,g_setup_intro_buffer,sizeof(retained));
 memset(pcm,0,sizeof(pcm));
 for(int i=0;i<20;i++)SetupAudioPlayIntro(pcm,kSetupIntroSamples,NULL);
 assert(adds[2]==kSetupIntroBlocks&&!memcmp(retained,g_setup_intro_buffer,sizeof(retained)));
 g_setup_intro_wavebuf[3].status=NDSP_WBUF_DONE;
 SetupAudioPlayIntro(pcm,kSetupIntroSamples,NULL);assert(adds[2]==kSetupIntroBlocks+1);
 assert(!memcmp(g_setup_intro_buffer+3*kSetupIntroSamples*2,pcm,sizeof(pcm)));
 SetupAudioStopIntro();assert(clears[2]==1&&!clears[0]&&adds[0]==1);
 SetupAudioStop();assert(frees==3&&exits==1&&!g_setup_audio_initialized);
 SetupAudioStop();assert(exits==1);
 for(int failed=1;failed<=3;failed++){
   fail_at=allocations+failed;int f=frees;
   SetupAudioInit();assert(!g_setup_audio_initialized&&frees==f+2);
   assert(!g_setup_intro_buffer&&!g_setup_music_buffer&&!g_setup_move_buffer);
 }
 fail_at=0;SetupAudioInit();SetupAudioStartMusic();assert(g_setup_audio_initialized&&adds[0]==2);
 SetupAudioStop();
 puts("PASS startup SFX: stereo channel 2, queued buffers never overwritten, bounded full ring, completed buffer reuse, music retained, partial allocation cleanup and reopen");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
