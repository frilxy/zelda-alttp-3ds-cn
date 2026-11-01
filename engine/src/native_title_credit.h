#pragma once
#include <stdbool.h>
#include <stdint.h>

// Seven-row, two-pixel-stem lettering matching the title's copyright sprites.
// Lowercase shapes retain the copyright's x-height, one-pixel spacing and
// vertical one-pixel shadow. Drawn after composition, without touching ROM/SRAM.
typedef struct NativeTitleGlyph { char letter; uint8_t width,rows[7]; } NativeTitleGlyph;
static const NativeTitleGlyph kNativeTitleGlyphs[]={
  {'E',6,{63,48,48,62,48,48,63}},
  {'s',5,{0,0,15,24,14,3,30}},
  {'t',4,{0,6,15,6,6,6,6}},
  {'e',5,{0,0,14,27,31,24,14}},
  {'b',5,{24,24,24,30,27,27,30}},
  {'a',5,{0,0,14,3,15,27,15}},
  {'n',5,{0,0,22,31,27,27,27}},
  {'P',6,{62,51,51,62,48,48,48}},
  {'d',5,{3,3,3,15,27,27,15}},
  {'N',7,{99,115,123,127,111,103,99}},
};
static inline const NativeTitleGlyph *NativeTitleCredit_Glyph(char letter){
  for(unsigned i=0;i<sizeof(kNativeTitleGlyphs)/sizeof(*kNativeTitleGlyphs);i++)
    if(kNativeTitleGlyphs[i].letter==letter)return &kNativeTitleGlyphs[i];
  return 0;
}
typedef void NativeTitleCreditRect(void *context,int x,int y,int w,int h,uint32_t color);
static inline bool NativeTitleCredit_Visible(unsigned module,unsigned submodule){
  return module==0&&submodule>=3&&submodule<=8;
}
static inline void NativeTitleCredit_Draw(int screen_width,int y,uint32_t foreground,uint32_t shadow,
                                          NativeTitleCreditRect *draw,void *context){
  static const char text[]="Esteban PdN";
  int width=-1;
  for(const char *p=text;*p;p++){const NativeTitleGlyph *g=NativeTitleCredit_Glyph(*p);width+=g?g->width+1:4;}
  for(int phase=0;phase<2;phase++){
    int x=(screen_width-width)/2;
    for(const char *p=text;*p;p++){
      const NativeTitleGlyph *g=NativeTitleCredit_Glyph(*p);
      if(!g){x+=4;continue;}
      for(int row=0;row<7;row++)for(int col=0;col<g->width;){
        if(!(g->rows[row]&(1u<<(g->width-1-col)))){col++;continue;}
        int begin=col++;
        while(col<g->width&&(g->rows[row]&(1u<<(g->width-1-col))))col++;
        draw(context,x+begin,y+row+(phase?0:1),col-begin,1,phase?foreground:shadow);
      }
      x+=g->width+1;
    }
  }
}
