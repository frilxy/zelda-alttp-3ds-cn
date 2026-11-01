#ifndef ZELDA3_PPU_RETAINED_H_
#define ZELDA3_PPU_RETAINED_H_
#include "ppu.h"
#include <stdlib.h>
#include <stddef.h>
_Static_assert(offsetof(Ppu, vram) % 4 == 0, "VRAM word comparison requires alignment");
#include <string.h>
// Where the 3DS ARM11 path below does not apply, the merge of a plane row
// takes eight pixels a step with the platform's vector unit.
#if !defined(__3DS__) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define PPU_MERGE_NEON 1
#elif !defined(__3DS__) && defined(__SSE2__)
#include <emmintrin.h>
#define PPU_MERGE_SSE2 1
#endif
typedef struct PpuRetainedLayer {
  uint16_t pixels[512 * 512];
  uint64_t active[512];
  uint16_t tiles[4096];
  uint16_t mapAddress, tileAddress, width, height;
  bool valid;
} PpuRetainedLayer;
typedef struct PpuRetainedMaps {
  PpuRetainedLayer layers[3];
  uint16_t vram[0x8000];
  uint8_t changed[4096];
  // changedBefore[b]: how many of the blocks before b changed this frame.
  uint16_t changedBefore[4097];
  // A plane byte spread to one bit per 4-bit pixel, left pixel first
  // (spread[0]) or mirrored (spread[1]).
  uint32_t spread[2][256];
  bool spreadReady;
  uint32_t rebuiltTiles;
} PpuRetainedMaps;

// Whether a block in [first, first + count) changed, the range wrapping
// around VRAM's 4096 blocks.
static inline bool PpuRetainedChanged(const PpuRetainedMaps *cache, unsigned first, unsigned count) {
  const uint16_t *before=cache->changedBefore;
  if(first+count<=4096) return before[first+count]!=before[first];
  return before[4096]!=before[first] || before[first+count-4096]!=0;
}

// Retain indexed background planes across frames. Palette changes do not
// rebuild geometry/pixels. Compare actual VRAM bytes, including direct DMA
// writes, and redraw only changed map cells or the cells using changed art.
static void PpuRetainMaps(Ppu *ppu) {
  PpuRetainedMaps *cache = ppu->retained;
  if (!cache) return;
  cache->rebuiltTiles = 0;
  if (ppu->mode != 1) return;
  if (!cache->spreadReady) {
    for (unsigned b=0; b<256; b++) {
      uint32_t left=0, mirrored=0;
      for (unsigned x=0; x<8; x++) {
        left|=((b>>(7-x))&1u)<<(4*x);
        mirrored|=((b>>x)&1u)<<(4*x);
      }
      cache->spread[0][b]=left; cache->spread[1][b]=mirrored;
    }
    cache->spreadReady=true;
  }
  bool any_changed=false;
  cache->changedBefore[0]=0;
  for (unsigned block=0; block<4096; block++) {
    const uint32_t *src=(const uint32_t *)(ppu->vram+block*8);
    uint32_t *dst=(uint32_t *)(cache->vram+block*8);
    bool changed=src[0]!=dst[0] || src[1]!=dst[1] || src[2]!=dst[2] || src[3]!=dst[3];
    cache->changed[block]=changed;
    cache->changedBefore[block+1]=cache->changedBefore[block]+changed;
    if (changed) {dst[0]=src[0];dst[1]=src[1];dst[2]=src[2];dst[3]=src[3];any_changed=true;}
  }
  for (unsigned layer=0; layer<3; layer++) {
    PpuRetainedLayer *out=&cache->layers[layer];
    if (!((ppu->screenEnabled[0]|ppu->screenEnabled[1]) & (1u<<layer))) {
      // Missed VRAM mutations while hidden must not leave a stale plane.
      out->valid=false; continue;
    }
    const BgLayer *bg=&ppu->bgLayer[layer];
    unsigned width=bg->tilemapWider?512:256, height=bg->tilemapHigher?512:256;
    bool full=!out->valid || out->mapAddress!=bg->tilemapAdr || out->tileAddress!=bg->tileAdr || out->width!=width || out->height!=height;
    // Same VRAM, same layout: every cell is already what it would be rebuilt as.
    if(!full && !any_changed) continue;
    unsigned stride=layer==2?8:16;
    // Nor when no block of its map or of its tile art changed: a cell is
    // rebuilt only when its map entry or one of its tile's blocks changed.
    if(!full && !PpuRetainedChanged(cache,(bg->tilemapAdr&0x7fff)/8,(width/256)*(height/256)*1024/8) &&
        !PpuRetainedChanged(cache,(bg->tileAdr&0x7fff)/8,1024*stride/8)) continue;
    out->mapAddress=bg->tilemapAdr;out->tileAddress=bg->tileAdr;out->width=width;out->height=height;
    if(full) memset(out->active,0,sizeof(out->active));
    for(unsigned ty=0;ty<height/8;ty++) {
     unsigned rowMap=bg->tilemapAdr+(ty&31)*32+(ty>=32?(width==512?0x800:0x400):0);
     for(unsigned tx=0;tx<width/8;tx++) {
      unsigned map=(rowMap+(tx&31)+(tx>=32?0x400:0))&0x7fff;
      uint16_t tile=ppu->vram[map];unsigned cell=ty*64+tx;
      unsigned addr=(bg->tileAdr+(tile&1023)*stride)&0x7fff;
      // BG tile bases and strides are 8-word aligned by SNES register layout.
      if(!full && out->tiles[cell]==tile && !cache->changed[addr/8] &&
          (layer==2 || !cache->changed[((addr+8)&0x7fff)/8])) continue;
      out->tiles[cell]=tile; cache->rebuiltTiles++;
      unsigned z=layer==0?((tile&0x2000)?0xc000:0x8000):layer==1?((tile&0x2000)?0xb100:0x7100):((tile&0x2000)?0xf200:0x1200);
      z+=(tile&0x1c00)>>(layer==2?8:6);
      const uint32_t *spread=cache->spread[(tile&0x4000)?1:0];
      for(unsigned y=0;y<8;y++) {
        unsigned row=(tile&0x8000)?7-y:y;
        uint32_t bits=ppu->vram[(addr+row)&0x7fff];
        if(layer!=2)bits|=(uint32_t)ppu->vram[(addr+row+8)&0x7fff]<<16;
        uint16_t *dst=out->pixels+(ty*8+y)*512+tx*8;
        // The eight 4-bit pixels of the row, pixel x in bits 4x to 4x+3.
        uint32_t pixels=spread[bits&0xff]|spread[(bits>>8)&0xff]<<1|
                        spread[(bits>>16)&0xff]<<2|spread[bits>>24]<<3;
        bool active=pixels!=0;
        for(unsigned x=0;x<8;x++) {
          unsigned pixel=(pixels>>(4*x))&15;
          dst[x]=pixel?z+pixel:0;
        }
        uint64_t mask=(uint64_t)1<<tx;
        out->active[ty*8+y]=(out->active[ty*8+y]&~mask)|(active?mask:0);
      }
     }
    }
    out->valid=true;
  }
}
static inline uint32_t PpuMergePair(uint32_t old,uint32_t src,uint32_t mask) {
  uint32_t z=src&mask;
#if defined(__3DS__) && defined(__arm__) && !defined(__thumb__)
  uint32_t result;
  __asm__("usub16 %0, %2, %3\n\tsel %0, %2, %1" : "=&r"(result) : "r"(src),"r"(old),"r"(z) : "cc");
  return result;
#else
  return (((uint16_t)old>=(uint16_t)z?old:src)&0xffffu)|(((old>>16)>=(z>>16)?old:src)&0xffff0000u);
#endif
}
static inline void PpuMergeSpan(uint16_t *dst,const uint16_t *src,unsigned n,unsigned layer) {
  if (!n) return;
  uint32_t mask=layer==2?0xfffcfffcu:0xfff0fff0u;
  // A pixel of the plane replaces the one below when its masked value is
  // higher, as PpuMergePair does two at a time.
#if defined(PPU_MERGE_NEON)
  uint16x8_t vmask=vdupq_n_u16((uint16_t)mask);
  for(;n>=8;n-=8,dst+=8,src+=8) {
    uint16x8_t old=vld1q_u16(dst),in=vld1q_u16(src);
    vst1q_u16(dst,vbslq_u16(vcgtq_u16(vandq_u16(in,vmask),old),in,old));
  }
#elif defined(PPU_MERGE_SSE2)
  // SSE2 compares signed: flipping the top bit makes it unsigned.
  __m128i vmask=_mm_set1_epi16((short)mask),bias=_mm_set1_epi16((short)0x8000);
  for(;n>=8;n-=8,dst+=8,src+=8) {
    __m128i old=_mm_loadu_si128((const __m128i *)dst),in=_mm_loadu_si128((const __m128i *)src);
    __m128i take=_mm_cmpgt_epi16(_mm_xor_si128(_mm_and_si128(in,vmask),bias),_mm_xor_si128(old,bias));
    _mm_storeu_si128((__m128i *)dst,_mm_or_si128(_mm_and_si128(take,in),_mm_andnot_si128(take,old)));
  }
#endif
  // SIMD can consume the whole span while leaving a halfword-aligned end
  // pointer. Do not make a word-alignment promise for an empty remainder.
  if (!n) return;
  if(((uintptr_t)dst&2) && n) {
    if((src[0]&mask)>dst[0])dst[0]=src[0];dst++;src++;n--;
  }
  uint32_t *out=(uint32_t *)__builtin_assume_aligned(dst,4);
  unsigned pairs=n/2;
  if(!((uintptr_t)src&2)) {
    const uint32_t *in=(const uint32_t *)__builtin_assume_aligned(src,4);
    for(unsigned i=0;i<pairs;i++)out[i]=PpuMergePair(out[i],in[i],mask);
  } else if(pairs) {
    // Read only the requested pixels. Aligning src down and reading the next
    // word touched a pixel before/after the span at allocation boundaries.
    for(unsigned i=0;i<pairs;i++) {
      uint32_t in=(uint32_t)src[2*i]|((uint32_t)src[2*i+1]<<16);
      out[i]=PpuMergePair(out[i],in,mask);
    }
  }
  if(n&1) {unsigned i=n-1;if((src[i]&mask)>dst[i])dst[i]=src[i];}
}
#endif
