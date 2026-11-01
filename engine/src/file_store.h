#ifndef ZELDA3_FILE_STORE_H_
#define ZELDA3_FILE_STORE_H_

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

// SD archive rename cannot overwrite a destination. Keep the last complete
// file until its replacement is closed, and retain a recoverable backup if
// either rename fails. Callers serialize writes to each destination.
static inline bool FileStore_Path(char *out, size_t size,
                                  const char *path, const char *suffix) {
  int n = snprintf(out, size, "%s%s", path, suffix);
  return n >= 0 && (size_t)n < size;
}

static inline bool FileStore_Exists(const char *path, bool *exists) {
  struct stat st;
  if (stat(path, &st) == 0) {
    *exists = true;
    return S_ISREG(st.st_mode);
  }
  *exists = false;
  return errno == ENOENT;
}

static inline bool FileStore_Recover(const char *path) {
  char backup[640];
  bool present, saved;
  if (!FileStore_Path(backup, sizeof(backup), path, ".replace.bak") ||
      !FileStore_Exists(path, &present) ||
      !FileStore_Exists(backup, &saved))
    return false;
  if (!saved) return true;
  return present ? remove(backup) == 0 : rename(backup, path) == 0;
}

static inline bool FileStore_Commit(const char *temporary, const char *path) {
  char backup[640];
  bool present;
  if (!FileStore_Path(backup, sizeof(backup), path, ".replace.bak") ||
      !FileStore_Recover(path) || !FileStore_Exists(path, &present))
    return false;
  if (present && rename(path, backup) != 0) return false;
  if (rename(temporary, path) != 0) {
    if (present) rename(backup, path); // On failure, recovery keeps the backup.
    return false;
  }
  if (present) remove(backup); // A leftover backup is resolved on next access.
  return true;
}

static inline bool FileStore_Write(const char *path, const void *data, size_t size) {
  char temporary[640];
  if (!FileStore_Path(temporary, sizeof(temporary), path, ".tmp") ||
      !FileStore_Recover(path)) return false;
  FILE *f = fopen(temporary, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, size, f) == size;
  if (fclose(f) != 0) ok = false;
  if (ok) ok = FileStore_Commit(temporary, path);
  if (!ok) remove(temporary);
  return ok;
}

static inline bool FileStore_Copy(const char *source, const char *path) {
  char temporary[640];
  if (!FileStore_Path(temporary, sizeof(temporary), path, ".tmp") ||
      !FileStore_Recover(path)) return false;
  FILE *input = fopen(source, "rb");
  if (!input) return false;
  FILE *output = fopen(temporary, "wb");
  if (!output) { fclose(input); return false; }
  bool ok = true;
  char buffer[4096];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
    if (fwrite(buffer, 1, count, output) != count) { ok = false; break; }
  }
  if (ferror(input)) ok = false;
  if (fclose(input) != 0) ok = false;
  if (fclose(output) != 0) ok = false;
  if (ok) ok = FileStore_Commit(temporary, path);
  if (!ok) remove(temporary);
  return ok;
}

#endif
