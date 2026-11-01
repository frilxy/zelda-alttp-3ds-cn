#!/usr/bin/env python3
"""Check retained-plane merging at exact allocation boundaries and alignments."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/ppu_retained.h"
int main(void) {
  unsigned cases=0;
  for(unsigned layer=0;layer<3;layer++)
    for(unsigned length=0;length<65;length++)
      for(unsigned sa=0;sa<2;sa++) for(unsigned da=0;da<2;da++) {
        uint16_t *src=malloc(2*(length+sa)+1),*dst=malloc(2*(length+da)+1);
        uint16_t expected[65]; assert(src && dst);
        for(unsigned i=0;i<length;i++) {
          src[sa+i]=(uint16_t)(i*3791+length*7919);
          dst[da+i]=(uint16_t)(i*871+length*3001);
          expected[i]=(src[sa+i] & (layer==2 ? 0xfffc : 0xfff0))>dst[da+i]
                        ? src[sa+i] : dst[da+i];
        }
        PpuMergeSpan(dst+da,src+sa,length,layer);
        assert(!memcmp(dst+da,expected,length*2));
        free(src); free(dst); cases++;
      }
  printf("PASS %u retained merge spans: exact tails, zero length, both halfword alignments and all layers\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-merge-span-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-O2', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(root / 'engine'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
