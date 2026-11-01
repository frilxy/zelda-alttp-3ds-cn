#ifndef ZELDA3_INI_EDIT_H_
#define ZELDA3_INI_EDIT_H_

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "file_store.h"

// Rewrite one `key = value` line inside a section of an INI file, adding the
// section or the key when missing and dropping later duplicates. The file is
// replaced through FileStore_Write, so a failed write keeps the old one.
static inline bool IniEdit_SetValue(const char *path, const char *section,
                                    const char *key, const char *value) {
  size_t slen = strlen(section), klen = strlen(key), vlen = strlen(value);
  if (slen >= 64 || !klen || klen > 256 || vlen > 4096 ||
      strpbrk(value, "\r\n") || !FileStore_Recover(path)) return false;
  char *buf = NULL;
  long size = 0;
  FILE *f = fopen(path, "rb");
  if (f) {
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || size > 1 << 20 ||
        fseek(f, 0, SEEK_SET) != 0 || !(buf = malloc((size_t)size + 1)) ||
        fread(buf, 1, (size_t)size, f) != (size_t)size) {
      free(buf); fclose(f); return false;
    }
    fclose(f);
  } else if (!(buf = malloc(1))) {
    return false;
  }
  buf[size] = 0;

  size_t capacity = (size_t)size + slen + klen + vlen + 8;
  char *out = malloc(capacity);
  if (!out) { free(buf); return false; }
  size_t olen = 0;
  bool done = false, in_section = false;
  const char *line = buf;
  while (*line) {
    const char *nl = strchr(line, '\n');
    size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
    const char *end = line + len, *t = line;
    while (t < end && (*t == ' ' || *t == '\t')) t++;
    if (t < end && *t == '[') {
      const char *close = memchr(t, ']', end - t);
      in_section = close && (size_t)(close - t + 1) == slen &&
                   !strncasecmp(t, section, slen);
    } else if (in_section && (size_t)(end - t) >= klen &&
               !strncasecmp(t, key, klen)) {
      const char *after = t + klen;
      while (after < end && (*after == ' ' || *after == '\t')) after++;
      if (after < end && *after == '=') {
        if (!done) olen += snprintf(out + olen, capacity - olen, "%s = %s\n", key, value);
        done = true;  // Remove later duplicates, including repeated sections.
        line = end;
        continue;
      }
    }
    memcpy(out + olen, line, len);
    olen += len;
    line = end;
  }
  if (!done) {
    if (olen && out[olen - 1] != '\n') out[olen++] = '\n';
    olen += snprintf(out + olen, capacity - olen, "%s\n%s = %s\n", section, key, value);
  }
  bool saved = FileStore_Write(path, out, olen);
  free(out);
  free(buf);
  return saved;
}

#endif  // ZELDA3_INI_EDIT_H_
