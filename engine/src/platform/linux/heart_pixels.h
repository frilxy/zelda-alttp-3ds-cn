#ifndef ZELDA3_HEART_PIXELS_H_
#define ZELDA3_HEART_PIXELS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

// Authored pixel silhouettes for every size used by the 320x240 sidebar.
// Full, half and empty share an outline, keeping shape stable during damage.
static const int kNativeHeartSizes[] = {8,9,10,12,14,16,17,22};
static const uint32_t kNativeHeartRows[8][24] = {
  { 0x66, 0xff, 0xff, 0xff, 0x7e, 0x3c, 0x18, 0x0 }, // 8 px
  { 0xc6, 0x1ef, 0x1ff, 0x1ff, 0xfe, 0x7c, 0x38, 0x10, 0x0 }, // 9 px
  { 0x186, 0x3cf, 0x3ff, 0x3ff, 0x3ff, 0x1fe, 0xfc, 0x78, 0x30, 0x0 }, // 10 px
  { 0x70e, 0xf9f, 0xfff, 0xfff, 0xfff, 0x7fe, 0x3fc, 0x1f8, 0xf0, 0x60, 0x0, 0x0 }, // 12 px
  { 0xe1c, 0x1f3e, 0x3fff, 0x3fff, 0x3fff, 0x3fff, 0x1ffe, 0xffc, 0x7f8, 0x3f0, 0x1e0, 0xc0, 0x0, 0x0 }, // 14 px
  { 0x3c3c, 0x7e7e, 0xffff, 0xffff, 0xffff, 0xffff, 0x7ffe, 0x3ffc, 0x1ff8, 0xff0, 0x7e0, 0x3c0, 0x180, 0x0, 0x0, 0x0 }, // 16 px
  { 0x783c, 0xfc7e, 0x1feff, 0x1ffff, 0x1ffff, 0x1ffff, 0x1ffff, 0xfffe, 0x7ffc, 0x3ff8, 0x1ff0, 0xfe0, 0x7c0, 0x380, 0x100, 0x0, 0x0 }, // 17 px
  { 0x7c0f8, 0xfe1fc, 0x1ff3fe, 0x3fffff, 0x3fffff, 0x3fffff, 0x3fffff, 0x3fffff, 0x3fffff, 0x1ffffe, 0xffffc, 0xffffc, 0x7fff8, 0x3fff0, 0x1ffe0, 0xffc0, 0x7f80, 0x3f00, 0x1e00, 0xc00, 0x0, 0x0 }, // 22 px
};
static uint32_t native_heart_ink[3] = {0xffffffff,0xff000000,0xffc60000};
static void NativeHearts_LoadColors(const uint32_t *sheet,int stride,const int cells[3],int columns) {
  native_heart_ink[0]=sheet[(cells[2]/columns*8+1)*stride+cells[2]%columns*8+2]|0xff000000;
  native_heart_ink[1]=sheet[(cells[0]/columns*8+3)*stride+cells[0]%columns*8+3]|0xff000000;
  native_heart_ink[2]=sheet[(cells[2]/columns*8+3)*stride+cells[2]%columns*8+3]|0xff000000;
}
static bool NativeHearts_Mask(int size,int x,int y) {
  if(x<0||y<0||x>=size||y>=size)return false;
  int index=7;
  for(int i=0;i<8;i++)if(kNativeHeartSizes[i]==size){index=i;break;}
  int source=kNativeHeartSizes[index];
  // Other desktop window sizes use symmetric center samples of the outline.
  int mx=x<size/2?x:size-1-x;
  int sx=(2*mx+1)*source/(2*size);
  if(x>=size/2)sx=source-1-sx;
  int sy=(2*y+1)*source/(2*size);
  return (kNativeHeartRows[index][sy]>>sx)&1;
}
static uint32_t NativeHearts_Pixel(int size,int x,int y,int state) {
  if(!NativeHearts_Mask(size,x,y))return 0;
  int border=size>=17?2:1;
  for(int dy=-border;dy<=border;dy++)for(int dx=-border;dx<=border;dx++) {
    if(abs(dx)+abs(dy)>border)continue;
    if(!NativeHearts_Mask(size,x+dx,y+dy))return native_heart_ink[0];
  }
  bool filled=state==2||(state==1&&x<size/2);
  return native_heart_ink[filled?2:1];
}
#endif
