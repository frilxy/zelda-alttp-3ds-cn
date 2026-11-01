#!/usr/bin/env python3
"""Check production seed namespace selection and failure rollback with sanitizers."""
from pathlib import Path
import argparse
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir', type=Path, required=True)
    a = p.parse_args()
    work = a.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    text = (ROOT / 'platform/3ds/source/platform_3ds.c').read_text()
    functions = '\n'.join(function(text, name) for name in (
        'static bool EnsureDirectory(const char *path) {',
        'void Platform3DS_FormatSavePath(',
        'const char *Platform3DS_RandomizerSeedKey(',
        'bool Platform3DS_SelectRandomizerStorage('))
    code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "src/file_store.h"
#include "src/rando/seed_identity.h"
static void RandoSave_SetSeedStorageActive(bool active) { (void)active; }
static char g_active_save_directory[512]="saves/profile-a";
static char g_active_profile[512]="profiles/profile-a";
static char g_quest_save_prefix[128]="quest-example/";
static char g_randomizer_seed_key[65];
static char g_randomizer_save_prefix[96];
@FUNCTIONS@
static void marker(const char *path, char value) {
  FILE *f=fopen(path,"wb"); assert(f); assert(fputc(value,f)==value); assert(!fclose(f));
}
static void check_marker(const char *path, char value) {
  FILE *f=fopen(path,"rb"); assert(f); assert(fgetc(f)==value); fclose(f);
}
int main(int argc,char **argv) {
  assert(argc==2 && chdir(argv[1])==0);
  assert(EnsureDirectory("saves") && EnsureDirectory("saves/profile-a"));
  assert(EnsureDirectory("saves/profile-a/quest-example"));
  assert(EnsureDirectory("profiles") && EnsureDirectory("profiles/profile-a"));
  char key_a[65],key_b[65]; memset(key_a,'a',64); key_a[64]=0; memset(key_b,'b',64); key_b[64]=0;
  char ordinary[512],first[512],second[512],ref[512];
  Platform3DS_FormatSavePath("saves/sram.dat",ordinary,sizeof(ordinary));
  assert(!strcmp(ordinary,"saves/profile-a/quest-example/sram.dat")); marker(ordinary,'O');
  assert(Platform3DS_SelectRandomizerStorage(key_a,true));
  assert(!strcmp(Platform3DS_RandomizerSeedKey(),key_a));
  Platform3DS_FormatSavePath("saves/save0.sav",first,sizeof(first)); marker(first,'A');
  assert(strstr(first,key_a) && !strstr(first,"quest-example"));
  assert(Platform3DS_SelectRandomizerStorage(Platform3DS_RandomizerSeedKey(),false));
  assert(Platform3DS_SelectRandomizerStorage(key_b,true));
  Platform3DS_FormatSavePath("saves/save0.sav",second,sizeof(second));
  assert(strcmp(first,second)); marker(second,'B');
  assert(Platform3DS_SelectRandomizerStorage(key_a,true));
  check_marker(first,'A'); check_marker(second,'B'); check_marker(ordinary,'O');
  assert(!Platform3DS_SelectRandomizerStorage("../../another-profile",true));
  assert(!strcmp(Platform3DS_RandomizerSeedKey(),key_a));
  strcpy(g_active_profile,"profiles/missing/child");
  assert(!Platform3DS_SelectRandomizerStorage(key_b,true));
  assert(!strcmp(Platform3DS_RandomizerSeedKey(),key_a));
  strcpy(g_active_profile,"profiles/profile-a");
  Platform3DS_FormatSavePath("saves/ref/chapter.sav",ref,sizeof(ref));
  assert(!strcmp(ref,"saves/ref/chapter.sav"));
  assert(Platform3DS_SelectRandomizerStorage(NULL,true));
  Platform3DS_FormatSavePath("saves/sram.dat",ref,sizeof(ref)); assert(!strcmp(ref,ordinary));
  check_marker(ordinary,'O'); check_marker(first,'A'); check_marker(second,'B');
  puts("PASS: ordinary/quest/seed save isolation, selection persistence, alias safety and failure rollback");
}
'''.replace('@FUNCTIONS@', functions)
    source = work / 'storage_test.c'
    source.write_text(code)
    binary = work / 'storage_test'
    linker = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    subprocess.run(['clang', '-O1', '-g', '-ffunction-sections', '-fdata-sections', linker,
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'engine'),
                    str(source), str(ROOT / 'engine/src/rando/seed_identity.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), str(work)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (work / 'test.log').write_text(result.stdout)
    print(result.stdout)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
