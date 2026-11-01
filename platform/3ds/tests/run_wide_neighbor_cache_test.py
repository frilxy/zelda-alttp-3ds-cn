#!/usr/bin/env python3
"""Keep all four neighbor pointers valid when hits and misses share a frame."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[3];s=(r/'engine/src/zelda_rtl.c').read_text()
a=s.index('static const uint16 *WideNeighborMaps(');b=s.index('\nstatic void WideHideSpritesOutside',a)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uint8_t uint8;typedef uint16_t uint16;
static struct {int room[4];bool valid[4];uint8 next,pinned;uint16 maps[4][0x2000];}g_wide_rooms;
static bool Dungeon_DecodeRoomMaps(uint16 room,uint16 *out){for(int i=0;i<0x2000;i++)out[i]=room;return true;}
'''+s[a:b]+r'''
int main(void){
 unsigned checks=0;
 for(int cursor=0;cursor<4;cursor++)for(int first=0;first<4;first++){
  memset(&g_wide_rooms,0,sizeof(g_wide_rooms));
  for(int i=0;i<4;i++)assert(WideNeighborMaps(i));
  g_wide_rooms.pinned=0;g_wide_rooms.next=cursor;
  const uint16 *held[4];int rooms[4]={first,20,30,40};
  for(int i=0;i<4;i++){
   held[i]=WideNeighborMaps(rooms[i]);assert(held[i]);
   for(int j=0;j<=i;j++)for(int cell=0;cell<0x2000;cell++){assert(held[j][cell]==rooms[j]);checks++;}
  }
  assert(!WideNeighborMaps(50)); // bounded fail rather than overwrite a live map
 }
 printf("PASS %u retained neighbor words: every cursor and cache-hit placement, mixed hits/misses, four simultaneous previews and bounded full cache\n",checks);
}
'''
with tempfile.TemporaryDirectory()as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
