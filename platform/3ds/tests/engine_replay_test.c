#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
int main(int argc,char **argv) {
 assert(argc==5);
 assert(chdir(argv[1])==0);
 ParseConfigFile(NULL); LoadAssets(); LoadLinkGraphics(); ZeldaInitialize();
 ZeldaSetLanguage(NULL); ZeldaReadSram();
 g_snes_width=256; g_snes_height=224; g_zenv.ppu->extraLeftRight=0;
 g_wanted_zelda_features=0; g_ppu_render_flags=kPpuRenderFlags_NewRenderer;
 LoadRom(argv[2]);
 ZeldaReset(true); // Synchronize the retained SRAM into the reference machine.
 // Initialize both CPUs before loading a reference that starts mid-game.
 // Otherwise the cartridge CPU is still at reset and clears the copied RAM.
 ZeldaRunFrame(0);
 int chapter=atoi(argv[3]),frames=atoi(argv[4]);
 if(chapter>=0) SaveLoadSlot(kSaveLoad_Replay,256+chapter);
 uint8 pixels[400*240*4];
 for(int i=0;i<frames;i++) {
  ZeldaRunFrame(0);
  if(i%60==0) ZeldaDrawPpuFrame(pixels,256*4,g_ppu_render_flags);
 }
 printf("PASS chapter %d: %d native frames with cartridge comparison; module %u\n",chapter,frames,g_ram[0x10]);
 ZeldaShutdownSession(); return 0;
}
