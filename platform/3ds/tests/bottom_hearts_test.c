// Compare the retained layer with actual draw_sidebar output, including its
// background, square equipment box and adaptive native heart geometry.
#include <SDL.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "src/platform/linux/bottom_hearts.h"
static BottomHearts captured;
static bool capture_enabled;
static void capture_bottom_hearts(int count, int size, int columns, int step, int x, int y);
#include "hardware_profile.h"
static const Platform3DSHardwareProfile *Platform3DS_GetHardwareProfile(void) { return Platform3DS_ProfileForModel(false); }
#define ZELDA3_TEST_3DS_UI 1
#define ZELDA3_TEST_BOTTOM_HEARTS 1
#include "src/platform/linux/second_screen_sdl.c"
int SS_GetEquippedSlot(void) { return 0; }
int SS_GetInventorySlot(int slot) { return -1; }
void SS_ReadSram(uint8_t *out, int n) { memset(out, 0, n); }
static void capture_bottom_hearts(int count, int size, int columns, int step, int x, int y) {
  if (capture_enabled)
    BottomHearts_Capture(&captured, ss_r, count, size, columns, step, x, y,
                        sram8(0x6d), sram8(0x6c), sram8(0x7b) >= 1);
}
static void Draw(bool dungeon) {
  draw_tiled(dungeon ? tex_bg_stone : tex_bg_menu,
    dungeon ? kSSTexStone_W : kSSTexMenu_W,
    dungeon ? kSSTexStone_H : kSSTexMenu_H,
    (RectFS){0,0,320,240}, COL_BG_MENU);
  draw_sidebar(222,5,93,191,dungeon);
  SDL_RenderFlush(ss_r);
}
int main(void) {
  assert(SDL_Init(0)==0);
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,320,240,16,SDL_PIXELFORMAT_RGB565);
  assert(surface);ss_r=SDL_CreateSoftwareRenderer(surface);assert(ss_r);
  W=320;H=240;u=.5f;
  tex_bg_menu=make_tex(kSSTexMenu_W,kSSTexMenu_H,kSSTexMenu,false);
  tex_bg_stone=make_tex(kSSTexStone_W,kSSTexStone_H,kSSTexStone,false);
  // A synthetic atlas supplies varied palette colors without a private ROM;
  // authored silhouettes are checked independently below.
  uint32_t sheet[SS_GLYPH_COLS*8*((kGlyphCount+kGlyphCols-1)/kGlyphCols)*8];
  for(unsigned i=0;i<sizeof(sheet)/sizeof(*sheet);i++)
    sheet[i]=(i%5==0) ? 0 : 0xff000000u|((i*7919u)&0xffffff);
  tex_glyphs=make_tex(SS_GLYPH_COLS*8,((kGlyphCount+kGlyphCols-1)/kGlyphCols)*8,sheet,true);
  const int cells[]={SS_GLYPH_HEART_EMPTY,SS_GLYPH_HEART_HALF,SS_GLYPH_HEART_FULL};
  NativeHearts_LoadColors(sheet,SS_GLYPH_COLS*8,cells,SS_GLYPH_COLS);
  // Every authored shape is symmetric, with identical outlines at all health states.
  for(unsigned n=0;n<sizeof(kNativeHeartSizes)/sizeof(*kNativeHeartSizes);n++) {
    int size=kNativeHeartSizes[n];
    for(int y=0;y<size;y++)for(int x=0;x<size;x++) {
      assert(NativeHearts_Pixel(size,x,y,2)==NativeHearts_Pixel(size,size-1-x,y,2));
      for(int state=0;state<3;state++)assert(((NativeHearts_Pixel(size,x,y,state)>>24)!=0)==NativeHearts_Mask(size,x,y));
    }
  }
  uint8_t actual[320*240*2];
  unsigned cases=0;
  for(int dungeon=0;dungeon<2;dungeon++) for(int half=0;half<2;half++) for(int cap=1;cap<=20;cap++) {
    memset(sram,0,sizeof(sram));sram[0x6c]=cap*8;sram[0x6d]=cap*8;sram[0x7b]=half;
    capture_enabled=true;Draw(dungeon);assert(captured.valid);capture_enabled=false;
    // Independent packing oracle: no larger integer heart fits this life area.
    for(int columns=1;columns<=cap;columns++)for(int size=1;size<=16;size++) {
      int rows=(cap+columns-1)/columns;
      if(columns*size+columns-1<=91&&rows*size+rows-1<=22)
        assert(size<=captured.size);
    }
    for(int i=0;i<cap;i++) {
      SDL_Rect a=captured.rect[i];
      assert(a.x>=223&&a.x+a.w<=314&&a.y>=17&&a.y+a.h<=39);
      for(int j=0;j<i;j++){SDL_Rect b=captured.rect[j];assert(a.x>=b.x+b.w||b.x>=a.x+a.w||a.y>=b.y+b.h||b.y>=a.y+a.h);}
    }
    for(int i=1;i<cap;i++)if(captured.rect[i].y!=captured.rect[i-1].y)
      assert(captured.rect[i].x==captured.rect[0].x);
    assert(captured.size<=16);
    int columns=92/(captured.size+1);if(columns>10)columns=10;if(columns>cap)columns=cap;
    int width=columns*captured.size+columns-1;
    int origin=223+(cap<=columns?(91-width)/2:0);
    assert(captured.rect[0].x==origin);
    for(int i=0;i<cap;i++) {
      assert(captured.rect[i].x==origin+(i%columns)*(captured.size+1));
      assert(captured.rect[i].y==captured.rect[0].y+(i/columns)*(captured.size+1));
    }
    memcpy(actual,surface->pixels,sizeof(actual));
    // Damage down to zero, then healing up to capacity, including sub-half-heart
    // changes which must not touch any pixel or trigger an upload.
    for(int t=0;t<=cap*16;t++) {
      int health=t<=cap*8 ? cap*8-t : t-cap*8;
      int changed=BottomHearts_Apply(&captured,actual,640,health);
      assert(changed<=1);
      sram[0x6d]=health;Draw(dungeon);
      if(memcmp(actual,surface->pixels,sizeof(actual))) {
        for(int i=0;i<320*240;i++) if(((uint16_t*)actual)[i]!=((uint16_t*)surface->pixels)[i]) {
          fprintf(stderr,"mismatch cap=%d half=%d dungeon=%d health=%d x=%d y=%d actual=%04x expected=%04x\n",cap,half,dungeon,health,i%320,i/320,((uint16_t*)actual)[i],((uint16_t*)surface->pixels)[i]);break;
        }
        return 1;
      }
      cases++;
    }
    // A large change is bounded by the capacity (maximum 20 cells).
    assert(BottomHearts_Apply(&captured,actual,640,0)<=20);
    sram[0x6d]=0;Draw(dungeon);assert(!memcmp(actual,surface->pixels,sizeof(actual)));
  }
  captured.valid=false;assert(!BottomHearts_Apply(&captured,actual,640,100));
  SDL_DestroyTexture(tex_glyphs);SDL_DestroyTexture(tex_bg_menu);SDL_DestroyTexture(tex_bg_stone);
  SDL_DestroyRenderer(ss_r);SDL_FreeSurface(surface);SDL_Quit();
  printf("PASS %u full-screen pixel comparisons: damage/healing, 1-20 hearts, half magic, both backgrounds, capped 8-16px glyphs; centered single row and left-aligned full first row; at most one cell per half-heart step\n",cases);
}
