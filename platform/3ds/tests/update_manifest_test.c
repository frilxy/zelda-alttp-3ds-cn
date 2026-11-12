// Host checks for the pieces the update check relies on: the API manifest, the
// tag out of a release URL, and the sha256sum(1) file the check falls back to
// when GitHub's anonymous API answers 403.
#include <stdio.h>
#include <string.h>
#include "update_manifest.h"

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } } while (0)

static const char kManifest[] =
  "{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,"
  "\"body\":\"notes\",\"assets\":["
  "{\"name\":\"zelda3-3ds-v9.9.9.cia\",\"size\":40960,"
  "\"browser_download_url\":\"https://github.com/frilxy/zelda-alttp-3ds-cn/releases/download/v9.9.9/zelda3-3ds-v9.9.9.cia\","
  "\"digest\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\"}]}";

int main(void) {
  UpdateRelease r;
  int res = Update_ParseRelease(kManifest, strlen(kManifest), false, false, &r);
  CHECK(res == 1, "manifest parses");
  CHECK(res == 1 && !strcmp(r.version, "v9.9.9"), "manifest version");
  CHECK(res == 1 && r.size == 40960, "manifest size");
  CHECK(res == 1 && !strcmp(r.sha256,
        "0000000000000000000000000000000000000000000000000000000000000000"), "manifest digest");

  // A missing digest is a rejected candidate: nothing to verify the download with.
  const char *nodigest = "{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,"
    "\"assets\":[{\"name\":\"zelda3-3ds-v9.9.9.cia\",\"size\":40960,"
    "\"browser_download_url\":\"https://github.com/frilxy/zelda-alttp-3ds-cn/releases/download/v9.9.9/zelda3-3ds-v9.9.9.cia\"}]}";
  CHECK(Update_ParseRelease(nodigest, strlen(nodigest), false, false, &r) == -1, "digest required");

  char tag[48];
  CHECK(Update_TagFromReleaseUrl("https://github.com/frilxy/zelda-alttp-3ds-cn/releases/tag/v4.1.5",
                                 tag, sizeof(tag)) && !strcmp(tag, "v4.1.5"), "tag from release url");
  CHECK(Update_TagFromReleaseUrl("https://github.com/frilxy/zelda-alttp-3ds-cn/releases/tag/v4.1.5?x=1",
                                 tag, sizeof(tag)) && !strcmp(tag, "v4.1.5"), "tag stops at query");
  CHECK(!Update_TagFromReleaseUrl("https://github.com/frilxy/zelda-alttp-3ds-cn/releases", tag, sizeof(tag)),
        "no tag in a plain releases url");
  CHECK(!Update_TagFromReleaseUrl("https://github.com/frilxy/zelda-alttp-3ds-cn/releases/tag/nonsense",
                                  tag, sizeof(tag)), "invalid tag rejected");

  const char *atom =
    "<feed><entry><link rel=\"alternate\" type=\"text/html\" "
    "href=\"https://github.com/frilxy/zelda-alttp-3ds-cn/releases/tag/v4.1.6\"/></entry>"
    "<entry><link href=\"https://github.com/frilxy/zelda-alttp-3ds-cn/releases/tag/v4.1.5\"/></entry></feed>";
  CHECK(Update_TagFromReleaseList(atom, strlen(atom), tag, sizeof(tag)) && !strcmp(tag, "v4.1.6"),
        "newest tag from the atom feed");
  CHECK(!Update_TagFromReleaseList("<feed></feed>", 13, tag, sizeof(tag)), "empty feed has no tag");

  char sha[65];
  const char *sums =
    "1111111111111111111111111111111111111111111111111111111111111111  other-file.cia\r\n"
    "ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789 *zelda3-3ds-v9.9.9.cia\n";
  CHECK(Update_ParseSha256Sums(sums, strlen(sums), "zelda3-3ds-v9.9.9.cia", sha), "sums finds the asset");
  CHECK(!strcmp(sha, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"),
        "sums lowercases the digest");
  CHECK(!Update_ParseSha256Sums(sums, strlen(sums), "missing.cia", sha), "sums misses an unknown name");

  CHECK(Update_IsNewer("v4.1.6", "4.1.5"), "newer patch");
  CHECK(!Update_IsNewer("v4.1.5", "4.1.5"), "same version is not newer");
  CHECK(!Update_IsNewer("v4.0.14", "4.1.5"), "older is not newer");
  CHECK(Update_AllowedDownloadUrl("https://github.com/frilxy/zelda-alttp-3ds-cn/releases/download/v1/x.cia"),
        "download url allowed");
  CHECK(!Update_AllowedDownloadUrl("https://evil.example/x.cia"), "foreign url rejected");

  if (failures) { printf("%d update manifest checks failed\n", failures); return 1; }
  printf("PASS update manifest: api manifest, digest requirement, release-url tag, sha256sums, versions\n");
  return 0;
}
