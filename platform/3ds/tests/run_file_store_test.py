#!/usr/bin/env python3
"""Fault-test production SD replacement and the menu INI writer with sanitizers."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[3]
source = (root/'engine/src/platform/linux/second_screen_sdl.c').read_text()
a = source.index('static void update_ini(')
b = source.index('static void write_ini_gamepad_controls(', a)
code = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
static unsigned calls, faults;
static bool short_write;
static int sd_rename(const char *from, const char *to) {
  if (faults & (1u << ++calls)) { errno=EIO; return -1; }
  struct stat st;
  if (stat(to,&st)==0) { errno=EEXIST; return -1; }
  return rename(from,to);
}
static size_t test_write(const void *p, size_t s, size_t n, FILE *f) {
  return fwrite(p,s,short_write && n ? n-1 : n,f);
}
#define rename sd_rename
#define fwrite test_write
#include "file_store.h"
#include "ini_edit.h"
#define SDL_strcasecmp strcasecmp
#define SDL_strncasecmp strncasecmp
'''+source[a:b]+r'''
static void put(const char *path, const char *text) {
  FILE *f=fopen(path,"wb"); assert(f); assert(fputs(text,f)>=0); assert(fclose(f)==0);
}
static void expect(const char *path, const char *text) {
  FILE *f=fopen(path,"rb"); assert(f); char data[8192]={0};
  size_t n=fread(data,1,sizeof(data)-1,f); assert(!ferror(f)); fclose(f);
  assert(n==strlen(text) && !strcmp(data,text));
}
int main(void) {
  const unsigned cases[]={0,2,4,12};
  for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
    put("settings","OLD"); calls=0; faults=cases[i];
    bool ok=FileStore_Write("settings","NEW",3);
    assert(ok == (faults==0)); faults=0;
    assert(FileStore_Recover("settings")); expect("settings",ok?"NEW":"OLD");
  }
  put("settings","OLD"); short_write=true;
  assert(!FileStore_Write("settings","NEW",3)); short_write=false; expect("settings","OLD");
  put("source","COPY"); assert(FileStore_Copy("source","settings")); expect("settings","COPY");
  // A stopped replacement, and a completed rename whose backup cleanup stopped.
  remove("settings"); put("settings.replace.bak","OLD"); put("settings.tmp","partial");
  assert(FileStore_Recover("settings")); expect("settings","OLD");
  put("settings.replace.bak","OLD"); put("settings","NEW");
  assert(FileStore_Recover("settings")); expect("settings","NEW");
  assert(fopen("settings.replace.bak","rb")==NULL);
  assert(mkdir("directory",0700)==0); assert(!FileStore_Write("directory","NEW",3));
  char long_path[700]; memset(long_path,'a',sizeof(long_path)-1); long_path[699]=0;
  assert(!FileStore_Write(long_path,"NEW",3));
  put("zelda3.ini","[Sound]  # comment\nPannedSoundEffects = 1\n[Sound]\npannedsoundeffects = 0\nKeep = yes");
  update_ini("[Sound]","PannedSoundEffects","1");
  expect("zelda3.ini","[Sound]  # comment\nPannedSoundEffects = 1\n[Sound]\nKeep = yes");
  update_ini("[General]","Autosave","0");
  const char *updated="[Sound]  # comment\nPannedSoundEffects = 1\n[Sound]\nKeep = yes\n[General]\nAutosave = 0\n";
  expect("zelda3.ini",updated);
  calls=0; faults=12; update_ini("[General]","Autosave","1"); faults=0;
  assert(FileStore_Recover("zelda3.ini")); expect("zelda3.ini",updated);
  put("zelda3.ini",""); update_ini("[General]","Autosave","1");
  expect("zelda3.ini","[General]\nAutosave = 1\n");
  char value[4097]; memset(value,'v',4096); value[4096]=0;
  update_ini("[General]","Long",value); // Exact maximum, checked by ASan.
  puts("PASS SD rename/rollback faults, short writes, interruption recovery, directories/long paths, duplicate INI keys, empty files and missing final newline");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-file-store-') as td:
    p=Path(td); (p/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O1','-fsanitize=address,undefined','-I'+str(root/'engine/src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],cwd=p,check=True)
