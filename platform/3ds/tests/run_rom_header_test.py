#!/usr/bin/env python3
"""Probe every ROM-size exponent through the production alternative-header reader."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'engine/snes/snes_other.c').read_text()


def function(signature):
    start = source.rindex(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


structure = source[source.index('typedef struct CartHeader'):source.index('} CartHeader;') + len('} CartHeader;')]
code = '''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
''' + structure + '\n' + function('static uint32_t headerSize(') + function('static void readHeader(') + r'''
static uint8_t data[0x11000];
int main(void) {
  const unsigned locations[] = {0x7fc0, 0x81c0, 0xffc0, 0x101c0};
  for (unsigned exponent=0; exponent<256; exponent++) {
    uint64_t bytes=1024;
    if (exponent<=21) for(unsigned n=0;n<exponent;n++) bytes*=2;
    else bytes=0;
    for (unsigned i=0;i<4;i++) {
      memset(data,0,sizeof(data));
      unsigned location=locations[i];
      data[location+0x1a]=0x33; // Version 3 also probes flash/extra RAM sizes.
      data[location+0x17]=data[location+0x18]=exponent;
      data[location-4]=data[location-3]=exponent;
      CartHeader h={0};readHeader(data,location,&h);
      assert(h.headerVersion==3);
      assert(h.romSize==bytes&&h.ramSize==bytes&&h.flashSize==bytes&&h.exRamSize==bytes);
    }
  }
  puts("PASS 1024 alternative header probes, all 256 exponents, four size fields, ASan/UBSan");
}
'''
with tempfile.TemporaryDirectory(prefix='zelda-header-') as tmp:
    work = Path(tmp)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O1', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
