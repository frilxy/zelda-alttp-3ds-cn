#!/usr/bin/env python3
"""Check the production menu composition pixel-for-pixel, without game assets."""
from pathlib import Path
import argparse, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--rectangles',type=Path);args=p.parse_args()
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "file_menu_present.h"
static uint32_t source[400*240],result[400*240];
static uint8_t coverage[400*240];
static int sw,sh,rects;
static FILE *capture;
static uint32_t pattern(int x,int y) { return 1+((x+400)%16)+16*((y+240)%16); }
static bool draw(void *ctx,int sx,int sy,int x,int y,int w,int h) {
  assert(w>0&&h>0&&sx>=0&&sy>=0&&sx+w<=sw&&sy+h<=sh);
  assert(x>=0&&y>=0&&x+w<=400&&y+h<=240);rects++;
  if(capture)fprintf(capture,"%d %d %d %d %d %d\n",sx,sy,x,y,w,h);
  for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++) {
    int dst=(y+yy)*400+x+xx;coverage[dst]++;
    result[dst]=source[(sy+yy)*sw+sx+xx];
  }
  return true;
}
static bool fail(void *ctx,int sx,int sy,int x,int y,int w,int h) { return false; }
int main(int argc,char **argv) {
  for(int mode=0;mode<2;mode++) {
    sw=mode?400:256;sh=mode?240:224;int ox=(sw-256)/2,oy=(240-sh)/2;
    memset(source,0,sizeof(source));memset(result,0,sizeof(result));memset(coverage,0,sizeof(coverage));rects=0;
    for(int y=0;y<sh;y++)for(int x=0;x<256;x++)
      source[y*sw+ox+x]=x<16?pattern(x,y):(0x10000+y*256+x);
    if(mode&&argc==2){capture=fopen(argv[1],"w");assert(capture);}
    assert(FileMenu_DrawBackground(sw,sh,draw,NULL));assert(rects<=64);
    if(capture){fclose(capture);capture=NULL;}
    for(int y=0;y<240;y++)for(int x=0;x<400;x++) {
      assert(coverage[y*400+x]==1);
      uint32_t want=(x>=72&&x<328&&y>=oy&&y<oy+224)?
        source[(y-oy)*sw+ox+x-72]:pattern(x-72,y-oy);
      assert(result[y*400+x]==want);
    }
  }
  static uint16_t vram[0x8000];
  assert(FileMenu_TitleWidth(vram)==160);
  for (int title_width=160; title_width<=224; title_width+=8)
  for (unsigned module=1;module<=3;module++) {
    memset(vram,0,sizeof(vram));
    vram[0x1042]=0x3589;vram[0x1041+title_width/8]=0x7589;
    assert(FileMenu_TitleWidth(vram)==title_width);
    sw=400;sh=240;rects=0;memset(coverage,0,sizeof(coverage));
    assert(FileMenu_DrawWide(sw,sh,module,title_width,draw,NULL));assert(rects<240);
    for(int y=0;y<240;y++)for(int x=0;x<400;x++)assert(coverage[y*400+x]);
    for(int y=16;y<48;y++)for(int x=0;x<title_width;x++)
      assert(result[y*400+(400-title_width)/2+x]==source[y*400+72+16+x]);
    if(module==1) {
      for(int y=64;y<168;y++) {
        for(int x=0;x<112;x++)assert(result[y*400+24+x]==source[y*400+72+24+x]);
        for(int x=0;x<96;x++)assert(result[y*400+280+x]==source[y*400+72+136+x]);
      }
      for(int y=168;y<208;y++) {
        for(int x=0;x<24;x++)assert(result[y*400+24+x]==source[y*400+72+24+x]);
        for(int x=0;x<184;x++)assert(result[y*400+72+x]==source[y*400+72+48+x]);
      }
    } else {
      for(int y=64;y<208;y++)for(int x=0;x<208;x++)
        assert(result[y*400+96+x]==source[y*400+72+24+x]);
    }
  }
  assert(!FileMenu_DrawWide(256,224,1,160,draw,NULL));
  assert(!FileMenu_DrawWide(400,240,4,160,draw,NULL));
  assert(!FileMenu_DrawWide(400,240,1,160,fail,NULL));
  assert(!FileMenu_DrawBackground(255,224,draw,NULL));
  assert(!FileMenu_DrawBackground(256,223,draw,NULL));
  assert(!FileMenu_DrawBackground(400,240,fail,NULL));
  puts("PASS 192000 pixels: Original/Wide, continuous 16px phase, exact menu pixels, one write per screen pixel, bounded source reads and fewer than 64 quads; expanded panels preserve all text/cursors/hearts below 240 quads; native title widths 160..224 are centered and copied without clipping");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-file-menu-') as td:
    d=Path(td);(d/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(root/'platform/3ds/source'),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test'),*([str(args.rectangles.resolve())] if args.rectangles else [])],check=True)
