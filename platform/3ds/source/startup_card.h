#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "src/platform/linux/generated_ui_sprites.h"
static inline void StartupCardPixels(void *pixels,bool argb) {
  memset(pixels,0,512*256*(argb?4:2));
  for(int y=0;y<240;y++)for(int x=0;x<320;x++) {
    uint32_t color=0xff000000;
    if(((x==6||x==313)&&y>=6&&y<234)||((y==6||y==233)&&x>=6&&x<314))color=0xff7a581e;
    if(x>=136&&x<184&&y>=99&&y<141) {
      uint32_t c=kNativeUiTriforceSprite[(y-99)*48+x-136];if(c>>24)color=c;
    }
    if(argb)((uint32_t*)pixels)[y*512+x]=color;
    else ((uint16_t*)pixels)[y*512+x]=((color>>19&31)<<11)|((color>>10&63)<<5)|(color>>3&31);
  }
}
