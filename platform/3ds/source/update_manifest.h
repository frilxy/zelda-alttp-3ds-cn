#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// The in-game updater must look at this fork's releases: pointing it at upstream
// would offer, download and install a build without the Chinese translation.
// Releases must be tagged with a version the parser accepts (e.g. "v3.2-E1",
// never a rolling "latest") and carry zelda3-3ds-v<version>.cia / .3dsx, both of
// which platform/3ds/build.sh produces.
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

unsigned Update_FormatNotes(const char *markdown, char lines[][43], unsigned capacity);
