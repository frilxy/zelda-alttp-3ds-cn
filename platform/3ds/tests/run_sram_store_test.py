#!/usr/bin/env python3
"""Fault injection for the native SRAM store, with SD non-overwrite renames."""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[3]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
static unsigned calls, faults, writes, short_at;
static int sd_rename(const char *a, const char *b) {
  if (faults & (1u << ++calls)) { errno=EIO; return -1; }
  struct stat st; if (!stat(b,&st)) { errno=EEXIST; return -1; }
  return rename(a,b);
}
static size_t sd_write(const void *p, size_t s, size_t n, FILE *f) {
  return fwrite(p,s,++writes==short_at && n ? n-1 : n,f);
}
#define rename sd_rename
#define fwrite sd_write
#include "sram_store.h"
#undef rename
#undef fwrite
static uint8_t old[8192], next[8192], saved[8192], out[8192];
static void put(const char *path, const void *data, size_t n) {
  FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(data,1,n,f)==n); assert(!fclose(f));
}
static void setup(void) {
  const char *paths[]={"sram.dat","sram.bak","sram.dat.tmp","sram.bak.tmp",
    "sram.dat.replace.bak","sram.bak.replace.bak"};
  for(unsigned i=0;i<6;i++) remove(paths[i]);
  put("sram.dat",old,8192);put("sram.bak",saved,8192);
  calls=faults=writes=short_at=0;
}
int main(void) {
  memset(old,0x23,8192);memset(next,0x71,8192);memset(saved,0x42,8192);
  setup();assert(SramStore_Write("sram.dat","sram.bak",next));
  assert(SramStore_Read("sram.dat","sram.bak",out));assert(!memcmp(out,next,8192));
  assert(SramStore_ReadExact("sram.bak",out));assert(!memcmp(out,old,8192));
  // Fail each rename, including install plus rollback, in BOTH transactions.
  const unsigned masks[]={2,4,12,8,16,48};
  for(unsigned i=0;i<6;i++) {
    setup();faults=masks[i];assert(!SramStore_Write("sram.dat","sram.bak",next));faults=0;
    assert(SramStore_Read("sram.dat","sram.bak",out));assert(!memcmp(out,old,8192));
  }
  for(unsigned i=1;i<=2;i++) {
    setup();short_at=i;assert(!SramStore_Write("sram.dat","sram.bak",next));short_at=0;
    assert(SramStore_Read("sram.dat","sram.bak",out));assert(!memcmp(out,old,8192));
  }
  setup();put("sram.dat",next,17);
  assert(SramStore_Read("sram.dat","sram.bak",out));assert(!memcmp(out,saved,8192));
  assert(SramStore_Write("sram.dat","sram.bak",next));
  assert(SramStore_ReadExact("sram.bak",out));assert(!memcmp(out,saved,8192));
  setup();assert(!rename("sram.dat","sram.dat.replace.bak"));put("sram.dat.tmp",next,100);
  assert(SramStore_Read("sram.dat","sram.bak",out));assert(!memcmp(out,old,8192));
  setup();put("sram.dat",next,17);put("sram.bak",next,16);memset(out,0xa5,8192);
  assert(!SramStore_Read("sram.dat","sram.bak",out));
  for(unsigned i=0;i<8192;i++)assert(out[i]==0xa5);
  setup();FILE *f=fopen("sram.dat","ab");assert(f);fputc(1,f);fclose(f);
  assert(!SramStore_ReadExact("sram.dat",out));
  puts("PASS native SRAM: backup rotation, six rename/rollback faults, two short writes, interrupted replacement, truncated/oversized files, unchanged RAM on failure");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-sram-') as directory:
    work = Path(directory)
    (work/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O1','-fsanitize=address,undefined',
                    '-I'+str(root/'engine/src'),str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],cwd=work,check=True)
