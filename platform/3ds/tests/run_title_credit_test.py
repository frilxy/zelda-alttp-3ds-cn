#!/usr/bin/env python3
"""Check the native title credit's exact ink, centering and scene boundaries."""
from pathlib import Path
import argparse,subprocess,struct,zlib
r=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--title',type=Path);p.add_argument('--vram',type=Path);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "native_title_credit.h"
static uint32_t pixels[400*240];
static int minx=400,miny=240,maxx=-1,maxy=-1;
static void rectangle(void *ctx,int x,int y,int w,int h,uint32_t color){
 assert(x>=0&&y>=0&&x+w<=400&&y+h<=240&&h==1);
 for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)pixels[yy*400+xx]=color;
 if(x<minx)minx=x;if(y<miny)miny=y;if(x+w-1>maxx)maxx=x+w-1;if(y+h-1>maxy)maxy=y+h-1;
}
int main(int argc,char**argv){
 assert(argc==2||argc==3);
 if(argc==3){FILE*f=fopen(argv[2],"rb");assert(f);uint8_t row[256*4];for(int y=0;y<224;y++){assert(fread(row,4,256,f)==256);for(int x=0;x<256;x++)pixels[(y+8)*400+x+72]=(row[x*4+2]<<16)|(row[x*4+1]<<8)|row[x*4];}fclose(f);}
 for(unsigned module=0;module<28;module++)for(unsigned sub=0;sub<16;sub++)assert(NativeTitleCredit_Visible(module,sub)==(module==0&&sub>=3&&sub<=8));
 assert(NativeTitleCredit_Glyph('d')&&!NativeTitleCredit_Glyph('D'));
 NativeTitleCredit_Draw(400,204,0xffffff,0x800000,rectangle,NULL);
 assert(minx==167&&maxx==232&&minx==399-maxx); // centered on the native half-pixel axis
 assert(miny==204&&maxy==211);
 FILE*f=fopen(argv[1],"wb");assert(f);fprintf(f,"P6\n400 240\n255\n");
 for(int i=0;i<400*240;i++){uint8_t rgb[3]={pixels[i]>>16,pixels[i]>>8,pixels[i]};fwrite(rgb,1,3,f);}fclose(f);
 puts("PASS native title credit: exact case, 66px ink width, centering, 1px vertical shadow, integer runs and title-only scene guard");
}
'''
(out/'credit.c').write_text(code)
subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(r/'engine/src'),str(out/'credit.c'),'-o',str(out/'credit')],check=True)
subprocess.run([str(out/'credit'),str(out/'title-credit.ppm'),*([str(a.title.resolve())]if a.title else[])],check=True)
if a.vram:
 import re
 v=struct.unpack('<32768H',a.vram.read_bytes());seq=[0x44,0x50,0x51,0x52,0x53,0x54];bitmap=[]
 for y in range(7):
  row=[]
  for tile in seq:
   lo=v[0x4000+tile*16+y];hi=v[0x4000+tile*16+y+8]
   for x in range(8):
    color=((lo>>(7-x))&1)|(((lo>>(15-x))&1)<<1)|(((hi>>(7-x))&1)<<2)|(((hi>>(15-x))&1)<<3)
    row.append(color==9)
  bitmap.append(row)
 source=(r/'engine/src/native_title_credit.h').read_text()
 for char,start,end in [('N',2,9),('n',13,18),('t',19,23),('e',24,29)]:
  actual=list(map(int,re.search(r"\{'"+char+r"',\d+,\{([^}]+)\}",source)[1].split(',')))
  expected=[sum(int(bitmap[y][x])<<(end-1-x)for x in range(start,end))for y in range(7)]
  assert actual==expected,(char,actual,expected)
 print('PASS N/n/t/e match the supplied cartridge copyright pixel for pixel')
raw=(out/'title-credit.ppm').read_bytes().split(b'\n',3)[3]
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
scan=b''.join(b'\0'+raw[y*400*3:(y+1)*400*3]for y in range(240))
(out/'title-credit.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',400,240,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
