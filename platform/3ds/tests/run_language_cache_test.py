#!/usr/bin/env python3
"""Check that installed language caches expire when the bundled patch changes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'platform/3ds/source/platform_3ds.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define LANGUAGE_CART_FILE "zelda3_language.sfc"
typedef struct { const char *code; int menu; const char *name, *patch; } GameLanguage;
static const char kProfileLanguageFile[] = "assets-language.txt";
'''
for name in ['static char *Trim(', 'static bool IsRegularFile(const char *path) {',
             'static bool LanguageCacheStamp(', 'static bool ProfileLanguageMatches(']:
    code += function(name)
code += r'''
static void Write(const char *path, const void *data, size_t size) {
  FILE *f = fopen(path, "wb"); assert(f);
  assert(fwrite(data, 1, size, f) == size); assert(fclose(f) == 0);
}
int main(void) {
  GameLanguage en = {"en", 0, "English", NULL};
  GameLanguage pt = {"pt", 0, "Portuguese", "pt.bps"};
  uint8_t patch[16] = {'B','P','S','1',0,0,0,0,0,0,0,0,0x12,0x34,0x56,0x78};
  char stamp[64];
  assert(mkdir("profile", 0700) == 0);
  assert(LanguageCacheStamp(&en, stamp) && !strcmp(stamp, "en:2:00000000"));
  assert(!ProfileLanguageMatches("profile", &en));
  Write("profile/assets-language.txt", stamp, strlen(stamp));
  assert(ProfileLanguageMatches("profile", &en));
  assert(!LanguageCacheStamp(&pt, stamp));
  Write("pt.bps", patch, sizeof(patch));
  assert(LanguageCacheStamp(&pt, stamp) && !strcmp(stamp, "pt:2:78563412"));
  Write("profile/assets-language.txt", "pt", 2);
  Write("profile/zelda3_language.sfc", "fixture", 7);
  assert(!ProfileLanguageMatches("profile", &pt));
  Write("profile/assets-language.txt", stamp, strlen(stamp));
  assert(ProfileLanguageMatches("profile", &pt));
  patch[15]++;
  Write("pt.bps", patch, sizeof(patch));
  assert(!ProfileLanguageMatches("profile", &pt));
  patch[15]--;
  Write("pt.bps", patch, sizeof(patch));
  remove("profile/zelda3_language.sfc");
  assert(!ProfileLanguageMatches("profile", &pt));
  Write("pt.bps", patch, 7);
  assert(!LanguageCacheStamp(&pt, stamp));
  patch[0] = '?';
  Write("pt.bps", patch, sizeof(patch));
  assert(!LanguageCacheStamp(&pt, stamp));
  puts("PASS language cache: new patch, legacy marker, missing cartridge and malformed patch");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-language-cache-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], cwd=work, check=True)
