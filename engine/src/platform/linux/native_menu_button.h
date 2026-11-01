#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "operation_plaque.h"

// One native raster for startup and in-game buttons. The caller only supplies
// a rectangle sink (SDL spans or direct RGB565); colors and corners are shared.
typedef void (*NativeMenuSpan)(void *,int,int,int,int,uint32_t);
static inline void NativeMenuRound(NativeMenuSpan span,void *ctx,
    float x,float y,float w,float h,float radius,uint32_t color) {
  int left=(int)floorf(x+.5f),top=(int)floorf(y+.5f);
  int width=(int)floorf(x+w+.5f)-left,height=(int)floorf(y+h+.5f)-top;
  if(width<=0||height<=0)return;
  int r=(int)floorf(radius+.5f);
  if(r<0)r=0;if(r>width/2)r=width/2;if(r>height/2)r=height/2;
  if(height>2*r)span(ctx,left,top+r,width,height-2*r,color);
  for(int row=0;row<r;row++) {
    float dy=r-row-.5f;
    int inset=(int)ceilf(r-sqrtf((float)r*r-dy*dy)-.5f);
    span(ctx,left+inset,top+row,width-2*inset,1,color);
    span(ctx,left+inset,top+height-1-row,width-2*inset,1,color);
  }
}
static inline void NativeMenuButton(NativeMenuSpan span,void *ctx,
    float x,float y,float w,float h,float unit,bool selected) {
  NativeMenuRound(span,ctx,x,y,w,h,8*unit,selected?0xffe8c260:0xff7a581e);
  NativeMenuRound(span,ctx,x+3*unit,y+3*unit,w-6*unit,h-6*unit,6*unit,
                  selected?0xff3a300c:0xff1c1c1c);
}

static inline void NativeMenuPanel(NativeMenuSpan span,void *ctx,
    float x,float y,float w,float h) {
  NativeMenuRound(span,ctx,x,y,w,h,4,0xff7a581e);
  NativeMenuRound(span,ctx,x+1,y+1,w-2,h-2,3,0xffe8c260);
  NativeMenuRound(span,ctx,x+2,y+2,w-4,h-4,2,0xff080c08);
}

// Original ornamental title/loading frame. Caps retain their native pixels.
static inline void NativeMenuPlaque(NativeMenuSpan span,void *ctx,int x,int y,int width) {
  span(ctx,x+17,y+8,width-34,24,0xff000000);
  for(int yy=0;yy<38;yy++)for(int xx=0;xx<width;xx++) {
    int sx=xx<24?xx:xx>=width-24?192-(width-xx):24+(xx-24)%8;
    unsigned index=kOperationPlaque[yy*192+sx];if(!index)continue;
    unsigned c=kOperationPlaquePalette[index];
    unsigned red=(c>>11)*255/31,green=((c>>5)&63)*255/63,blue=(c&31)*255/31;
    span(ctx,x+xx,y+yy,1,1,0xff000000|red<<16|green<<8|blue);
  }
}

static const int kNativeMenuFooterX[4]={10,54,158,274};
static const int kNativeMenuFooterW[4]={36,96,108,36};
