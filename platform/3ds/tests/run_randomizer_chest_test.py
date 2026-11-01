#!/usr/bin/env python3
"""Test runtime chest identities; optional local ROM/assets never leave RAM."""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[3]
CODE = r'''
#include "src/rando/chest_map.h"
#include "src/rando/location_ids.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned read32(const unsigned char *p) {
  return p[0] | (unsigned)p[1]<<8 | (unsigned)p[2]<<16 | (unsigned)p[3]<<24;
}
static unsigned char *read_file(const char *path, size_t *size) {
  FILE *f=fopen(path,"rb"); assert(f);
  assert(!fseek(f,0,SEEK_END)); long n=ftell(f); assert(n>0);
  assert(!fseek(f,0,SEEK_SET));
  unsigned char *p=malloc(n); assert(p);
  assert(fread(p,1,n,f)==(size_t)n); fclose(f); *size=n; return p;
}
int main(int argc, char **argv) {
  unsigned char table[504];
  for(unsigned i=0;i<168;i++) {
    unsigned word=i/6 | (i%4==0 ? 0x8000 : 0);
    table[i*3]=word; table[i*3+1]=word>>8; table[i*3+2]=i;
  }
  assert(RandoChestMap_Init(table,sizeof(table),table,sizeof(table)));
  assert(RandoChestMap_Ready() && RandoChestMap_Count()==164);
  assert(RandoChestMap_Find(0,0)==LOC_Sewers_Dark_Cross);
  assert(RandoChestMap_Find(4,2)==LOC_Link_s_House);
  assert(RandoChestMap_Find(319,5)==0xffff);
  assert(RandoChestMap_Find(320,0)==0xffff);
  unsigned mapped=0;
  for(unsigned i=0;i<168;i++) mapped+=RandoChestMap_Find(i/6,i%6)!=0xffff;
  assert(mapped==164);
  assert(!RandoChestMap_Init(table,503,table,504));
  assert(!RandoChestMap_Ready() && RandoChestMap_Find(0,0)==0xffff);
  unsigned char changed[504]; memcpy(changed,table,504); changed[2]^=1;
  assert(!RandoChestMap_Init(table,504,changed,504));
  memcpy(changed,table,504); changed[0]=0xff; changed[1]=0x7f;
  assert(!RandoChestMap_Init(changed,504,table,504));
  assert(!RandoChestMap_Init(NULL,504,table,504));
  if(argc==3) {
    size_t rom_size,asset_size;
    unsigned char *rom=read_file(argv[1],&rom_size);
    unsigned char *assets=read_file(argv[2],&asset_size);
    size_t header=rom_size%1024==512 ? 512:0;
    assert(rom_size >= header + 0xe96e + 504 && asset_size>=88);
    unsigned count=read32(assets+80),extra=read32(assets+84);
    assert(count==165 && asset_size>=88+count*4);
    size_t offset=88+count*4+extra;
    for(unsigned i=0;i<=8;i++) {
      unsigned n=read32(assets+88+i*4); offset=(offset+3)&~(size_t)3;
      assert(offset<=asset_size && n<=asset_size-offset);
      if(i==8) assert(RandoChestMap_Init(rom+header+0xe96e,504,assets+offset,n));
      offset+=n;
    }
    assert(RandoChestMap_Count()==164);
    // These room/ordinal identities are independent of generated fake data.
    assert(RandoChestMap_Find(0x104,0)==LOC_Link_s_House);
    assert(RandoChestMap_Find(114,0)==LOC_Hyrule_Castle_Map_Chest);
    assert(RandoChestMap_Find(168,0)==LOC_Eastern_Palace_Compass_Chest);
    assert(RandoChestMap_Find(47,4)==LOC_Kakariko_Well_Bottom);
    free(rom); free(assets);
    puts("PASS: local cartridge matches loaded assets; 164 chest locations resolved");
  }
  RandoChestMap_Clear(); assert(!RandoChestMap_Ready());
  puts("PASS: runtime catalog, unmapped chests, bounds, mismatch rejection and reset");
  return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--rom', type=Path)
    p.add_argument('--assets', type=Path)
    a = p.parse_args()
    if bool(a.rom) != bool(a.assets):
        p.error('--rom and --assets must be supplied together')
    out = a.work_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'chest_test.c'
    source.write_text(CODE)
    binary = out / 'chest_test'
    subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'engine'), str(source),
                    str(ROOT / 'engine/src/rando/chest_map.c'), '-o', str(binary)], check=True)
    args = [str(binary)]
    if a.rom:
        args += [str(a.rom.resolve()), str(a.assets.resolve())]
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'test.log').write_text(result.stdout)
    print(result.stdout)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
