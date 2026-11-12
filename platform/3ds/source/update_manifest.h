#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define UPDATE_REPOSITORY "frilxy/zelda-alttp-3ds-cn"
#define UPDATE_MAX_FILE (32u * 1024u * 1024u)
typedef struct UpdateRelease {
  char version[48];
  char notes[12289];
  char url[512];
  char sha256[65];
  uint32_t size;
} UpdateRelease;
// -1: invalid response, 0: no publication in this channel, 1: valid candidate.
int Update_ParseRelease(const char *data, size_t size, bool prerelease,
                        bool homebrew, UpdateRelease *out);
bool Update_IsNewer(const char *candidate, const char *installed);
bool Update_ValidVersion(const char *version);
bool Update_AllowedDownloadUrl(const char *url);

// A sha256sum(1) file: "<64 hex>  <name>" per line.
int Update_ParseSha256Sums(const char *data, size_t size, const char *asset,
                           char sha256[65]);
// The first release tag in an Atom feed, which GitHub lists newest first.
int Update_TagFromReleaseList(const char *data, size_t size, char *tag, size_t cap);
// The tag out of https://github.com/<repo>/releases/tag/<tag>.
int Update_TagFromReleaseUrl(const char *url, char *tag, size_t cap);

unsigned Update_FormatNotes(const char *markdown, char lines[][43], unsigned capacity);
