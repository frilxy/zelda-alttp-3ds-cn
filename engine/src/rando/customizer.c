// customizer.c — customizer-mode manifest parsing + name resolution + install.
// See customizer.h for the design + manifest format.

#include "customizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rando_logic.h"  // Rando_GetLocationName, Rando_GetItemName
#include "rando_settings.h"   // RandoSettings, Settings_SetDefaults (placement selfcheck)
#include "rando_placement.h"  // Place_AssumedFill, PlacementTable_ComputeDigest
#include "item_ids.h"         // ITEM_Prize_GreenPendant .. ITEM_DefeatAgahnim
#include "dungeon_ids.h"      // Rando_IsFixedKeyCardinalityItem
#include "third_party/sha256/sha256.h"  // sha256_buffer

// MSVC has no strtok_r; strtok_s is signature-identical (C11 Annex K aside).
#ifdef _MSC_VER
#define strtok_r strtok_s
#endif

#define kCustomizerItemIdProbeMax 256

// ---------------------------------------------------------------------------
// Name normalization + reverse resolution
// ---------------------------------------------------------------------------

// Lowercase, alphanumeric-only. "Eastern_Palace_Boss", "Eastern Palace - Boss",
// and "easternpalaceboss" all normalize identically. Truncates at outlen-1.
static void normalize_name(const char *in, char *out, size_t outlen) {
  size_t o = 0;
  for (const char *p = in; *p && o + 1 < outlen; p++) {
    unsigned char c = (unsigned char)*p;
    if (c >= 'A' && c <= 'Z') out[o++] = (char)(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = (char)c;
    // everything else (space, '-', '_', '(', ')', ...) is dropped
  }
  out[o] = '\0';
}

static bool is_sentinel_name(const char *n) {
  return strcmp(n, "(unknown)") == 0 || strcmp(n, "(unnamed)") == 0;
}

typedef struct CustomizerNameEntry {
  char norm[160];
  const char *display;
  uint16 id;
} CustomizerNameEntry;

static CustomizerNameEntry *g_customizer_loc_names = NULL;
static uint32 g_customizer_loc_name_count = 0;
static CustomizerNameEntry *g_customizer_item_names = NULL;
static uint32 g_customizer_item_name_count = 0;

static int customizer_name_entry_cmp(const void *va, const void *vb) {
  const CustomizerNameEntry *a = (const CustomizerNameEntry *)va;
  const CustomizerNameEntry *b = (const CustomizerNameEntry *)vb;
  int c = strcmp(a->norm, b->norm);
  if (c != 0) return c;
  return (int)a->id - (int)b->id;
}

static uint16 customizer_find_name(const CustomizerNameEntry *entries,
                                   uint32 count, const char *name) {
  if (entries == NULL || name == NULL) return 0xFFFF;
  char want[160];
  normalize_name(name, want, sizeof want);
  if (want[0] == '\0') return 0xFFFF;

  uint32 lo = 0, hi = count;
  while (lo < hi) {
    uint32 mid = lo + (hi - lo) / 2;
    int c = strcmp(entries[mid].norm, want);
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < count && strcmp(entries[lo].norm, want) == 0)
    return entries[lo].id;
  return 0xFFFF;
}

static bool customizer_build_location_name_cache(void) {
  if (g_customizer_loc_names != NULL) return true;
  CustomizerNameEntry *entries = (CustomizerNameEntry *)calloc(
      kRandoLocationsCount ? kRandoLocationsCount : 1, sizeof(CustomizerNameEntry));
  if (entries == NULL) return false;

  uint32 count = 0;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    uint16 id = kRandoLocations[i].id;
    const char *n = Rando_GetLocationName(id);
    if (is_sentinel_name(n)) continue;
    entries[count].display = n;
    entries[count].id = id;
    normalize_name(n, entries[count].norm, sizeof entries[count].norm);
    count++;
  }
  qsort(entries, count, sizeof entries[0], customizer_name_entry_cmp);
  g_customizer_loc_names = entries;
  g_customizer_loc_name_count = count;
  return true;
}

static bool customizer_build_item_name_cache(void) {
  if (g_customizer_item_names != NULL) return true;
  CustomizerNameEntry *entries = (CustomizerNameEntry *)calloc(
      kCustomizerItemIdProbeMax, sizeof(CustomizerNameEntry));
  if (entries == NULL) return false;

  uint32 count = 0;
  for (uint32 id = 0; id < kCustomizerItemIdProbeMax; id++) {
    const char *n = Rando_GetItemName((uint16)id);
    if (is_sentinel_name(n)) continue;
    entries[count].display = n;
    entries[count].id = (uint16)id;
    normalize_name(n, entries[count].norm, sizeof entries[count].norm);
    count++;
  }
  qsort(entries, count, sizeof entries[0], customizer_name_entry_cmp);
  g_customizer_item_names = entries;
  g_customizer_item_name_count = count;
  return true;
}

uint16 Customizer_ResolveLocation(const char *name) {
  if (!customizer_build_location_name_cache()) return 0xFFFF;
  return customizer_find_name(g_customizer_loc_names,
                              g_customizer_loc_name_count, name);
}

uint16 Customizer_ResolveItem(const char *name) {
  if (!customizer_build_item_name_cache()) return 0xFFFF;
  return customizer_find_name(g_customizer_item_names,
                              g_customizer_item_name_count, name);
}

// ---------------------------------------------------------------------------
// Manifest parsing (strict line-based YAML subset)
// ---------------------------------------------------------------------------

static void rtrim(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}
static const char *ltrim(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

enum { SEC_NONE = 0, SEC_PLACEMENTS, SEC_POOL };

bool Customizer_IsNonGrantableItem(uint16 item_id) {
  return (item_id >= ITEM_Prize_GreenPendant && item_id <= ITEM_DefeatAgahnim) ||
         item_id == ITEM_HyruleCastleBigKey;
}

// Parse a pool_overrides list value: "[A, B, C]" or a bare "A". Resolves each
// item name and appends its id to out[*count] (capped at cap). Rejects unknown
// items; when reject_non_grantable is set, also rejects prize/event/virtual
// items that have no pool/grant path. Returns 0 on success.
static int parse_pool_list(const char *value, int line, const char *which,
                           uint16 *out, uint16 *count, uint16 cap,
                           bool reject_non_grantable, char *err, size_t errlen) {
  char buf[256];
  snprintf(buf, sizeof buf, "%s", value);
  char *s = (char *)ltrim(buf);
  rtrim(s);
  size_t n = strlen(s);
  if (n > 0 && s[0] == '[') {
    if (s[n - 1] != ']') {
      snprintf(err, errlen, "line %d: %s list missing closing ']'", line, which);
      return 1;
    }
    s[n - 1] = '\0';
    s++;
  }
  char *save = NULL;
  for (char *tok = strtok_r(s, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
    char item[160];
    snprintf(item, sizeof item, "%s", tok);
    char *t = (char *)ltrim(item);
    rtrim(t);
    if (t[0] == '\0') continue;  // tolerate trailing comma / empty list
    uint16 id = Customizer_ResolveItem(t);
    if (id == 0xFFFF) {
      snprintf(err, errlen, "line %d: unknown item '%s' in %s list", line, t, which);
      return 1;
    }
    if (reject_non_grantable && Customizer_IsNonGrantableItem(id)) {
      snprintf(err, errlen, "line %d: item '%s' is not grantable and cannot be added to the pool",
               line, t);
      return 1;
    }
    if (Rando_IsFixedKeyCardinalityItem(id)) {
      snprintf(err, errlen,
               "line %d: item '%s' cannot appear in pool_overrides; "
               "small keys, Key Rings, and Skeleton Key have fixed cardinality",
               line, t);
      return 1;
    }
    if (*count >= cap) {
      snprintf(err, errlen, "line %d: too many %s items (max %u)", line, which, (unsigned)cap);
      return 1;
    }
    out[(*count)++] = id;
  }
  return 0;
}

int Customizer_Parse(const char *text, size_t len, CustomizerManifest *out,
                     char *err, size_t errlen) {
  memset(out, 0, sizeof *out);
  if (errlen) err[0] = '\0';

  // customizer_seed = sha256(manifest_bytes)[0..8] — reproducible across users
  // who share the same manifest file.
  uint8 h[32];
  sha256_buffer((const uint8 *)text, len, h);
  memcpy(out->seed8, h, 8);

  int section = SEC_NONE;
  size_t i = 0;
  int line = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && text[i] != '\n') i++;
    size_t end = i;          // exclusive (points at '\n' or len)
    if (i < len) i++;        // consume '\n'
    line++;

    size_t blen = end - start;
    if (blen > 0 && text[start + blen - 1] == '\r') blen--;  // strip CR
    char buf[256];
    if (blen >= sizeof buf) { snprintf(err, errlen, "line %d: line too long", line); return 1; }
    memcpy(buf, text + start, blen);
    buf[blen] = '\0';

    // Strip a trailing comment (first unquoted '#'). Manifest values are bare
    // tokens with no '#', so a plain first-'#' rule is sufficient.
    char *hash = strchr(buf, '#');
    if (hash) *hash = '\0';

    bool indented = (buf[0] == ' ' || buf[0] == '\t');
    const char *s = ltrim(buf);
    char body[256];
    snprintf(body, sizeof body, "%s", s);
    rtrim(body);
    if (body[0] == '\0') continue;  // blank / comment-only line

    if (!indented) {
      // Top-level section header: "<name>:" with no value.
      char *colon = strchr(body, ':');
      if (colon == NULL || *(colon + 1) != '\0') {
        snprintf(err, errlen, "line %d: expected a section header (e.g. 'placements:')", line);
        return 1;
      }
      *colon = '\0';
      rtrim(body);
      if (strcmp(body, "placements") == 0) {
        section = SEC_PLACEMENTS;
      } else if (strcmp(body, "pool_overrides") == 0) {
        section = SEC_POOL;
      } else {
        snprintf(err, errlen, "line %d: unknown section '%s'", line, body);
        return 1;
      }
      continue;
    }

    // Indented entry.
    if (section == SEC_NONE) {
      snprintf(err, errlen, "line %d: entry before any section header", line);
      return 1;
    }
    if (section == SEC_POOL) {
      // "add: [A, B]" / "remove: [C]".
      char *colon = strchr(body, ':');
      if (colon == NULL) {
        snprintf(err, errlen, "line %d: expected 'add: [...]' or 'remove: [...]'", line);
        return 1;
      }
      *colon = '\0';
      char key[64];
      snprintf(key, sizeof key, "%s", body);
      rtrim(key);
      const char *val = ltrim(colon + 1);
      if (strcmp(key, "add") == 0) {
        if (parse_pool_list(val, line, "add", out->pool_add, &out->pool_add_count,
                            kCustomizerMaxPoolOps, /*reject_non_grantable=*/true, err, errlen) != 0)
          return 1;
      } else if (strcmp(key, "remove") == 0) {
        if (parse_pool_list(val, line, "remove", out->pool_remove, &out->pool_remove_count,
                            kCustomizerMaxPoolOps, /*reject_non_grantable=*/false, err, errlen) != 0)
          return 1;
      } else {
        snprintf(err, errlen, "line %d: unknown pool_overrides key '%s' (expected add/remove)",
                 line, key);
        return 1;
      }
      continue;
    }

    // section == SEC_PLACEMENTS: "<location>: <item>".
    char *colon = strchr(body, ':');
    if (colon == NULL) {
      snprintf(err, errlen, "line %d: expected '<location>: <item>'", line);
      return 1;
    }
    *colon = '\0';
    char loc_name[256], item_name[256];
    snprintf(loc_name, sizeof loc_name, "%s", body);
    rtrim(loc_name);
    snprintf(item_name, sizeof item_name, "%s", ltrim(colon + 1));
    rtrim(item_name);
    if (loc_name[0] == '\0') { snprintf(err, errlen, "line %d: empty location", line); return 1; }
    if (item_name[0] == '\0') { snprintf(err, errlen, "line %d: empty item for '%s'", line, loc_name); return 1; }

    uint16 loc_id = Customizer_ResolveLocation(loc_name);
    if (loc_id == 0xFFFF) {
      snprintf(err, errlen, "line %d: unknown location '%s'", line, loc_name);
      return 1;
    }
    uint16 item_id = Customizer_ResolveItem(item_name);
    if (item_id == 0xFFFF) {
      snprintf(err, errlen, "line %d: unknown item '%s'", line, item_name);
      return 1;
    }
    // Reject a double-pinned location (ambiguous).
    for (uint16 p = 0; p < out->pin_count; p++) {
      if (out->pins[p].location_id == loc_id) {
        snprintf(err, errlen, "line %d: location '%s' pinned more than once", line, loc_name);
        return 1;
      }
    }
    if (out->pin_count >= kCustomizerMaxPins) {
      snprintf(err, errlen, "line %d: too many placements (max %d)", line, kCustomizerMaxPins);
      return 1;
    }
    out->pins[out->pin_count].location_id = loc_id;
    out->pins[out->pin_count].item_id = item_id;
    out->pin_count++;
  }

  return 0;
}

int Customizer_LoadFile(const char *path, CustomizerManifest *out,
                        char *err, size_t errlen) {
  if (errlen) err[0] = '\0';
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    snprintf(err, errlen, "cannot open manifest '%s'", path);
    return 1;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); snprintf(err, errlen, "seek failed on '%s'", path); return 1; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); snprintf(err, errlen, "tell failed on '%s'", path); return 1; }
  if (sz > (1L << 20)) { fclose(f); snprintf(err, errlen, "manifest '%s' too large (> 1 MiB)", path); return 1; }
  rewind(f);
  char *text = (char *)malloc((size_t)sz + 1);
  if (text == NULL) { fclose(f); snprintf(err, errlen, "out of memory reading '%s'", path); return 1; }
  size_t got = fread(text, 1, (size_t)sz, f);
  fclose(f);
  text[got] = '\0';
  int rc = Customizer_Parse(text, got, out, err, errlen);
  free(text);
  return rc;
}

// ---------------------------------------------------------------------------
// Active manifest + hard-error channel (read by the placer)
// ---------------------------------------------------------------------------

static const CustomizerManifest *g_active_customizer = NULL;
static char g_customizer_error[160] = {0};

void Customizer_Install(const CustomizerManifest *m) { g_active_customizer = m; }
const CustomizerManifest *Customizer_GetActive(void) { return g_active_customizer; }

const char *Customizer_LastError(void) { return g_customizer_error; }
void Customizer__SetError(const char *msg) {
  snprintf(g_customizer_error, sizeof g_customizer_error, "%s", msg ? msg : "");
}
void Customizer__ClearError(void) { g_customizer_error[0] = '\0'; }

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------

static void customizer_selfcheck_die(const char *msg) {
  fprintf(stderr, "Customizer_SelfCheck: %s\n", msg);
  exit(2);
}

static void customizer_selfcheck_unique_names(const CustomizerNameEntry *entries,
                                              uint32 count,
                                              const char *kind) {
  for (uint32 i = 0; i < count; i++) {
    if (entries[i].norm[0] == '\0') {
      fprintf(stderr, "Customizer_SelfCheck: a %s name normalizes to empty\n", kind);
      exit(2);
    }
    if (i == 0 || strcmp(entries[i - 1].norm, entries[i].norm) != 0)
      continue;
    fprintf(stderr,
            "Customizer_SelfCheck: %s names '%s' (id %u) and '%s' (id %u) "
            "normalize identically\n",
            kind,
            entries[i - 1].display, entries[i - 1].id,
            entries[i].display, entries[i].id);
    exit(2);
  }
}

void Customizer_SelfCheck(void) {
  // 1. Normalized location names must be unique — the reverse resolver returns
  //    the FIRST match, so a collision would silently resolve to the wrong id.
  //    Build the resolver caches once, sort by normalized name, then adjacent
  //    entries are the only possible collisions.
  if (!customizer_build_location_name_cache())
    customizer_selfcheck_die("out of memory building location-name cache");
  customizer_selfcheck_unique_names(g_customizer_loc_names,
                                    g_customizer_loc_name_count,
                                    "location");
  // 2. Item names likewise unique.
  if (!customizer_build_item_name_cache())
    customizer_selfcheck_die("out of memory building item-name cache");
  customizer_selfcheck_unique_names(g_customizer_item_names,
                                    g_customizer_item_name_count,
                                    "item");

  // 3. Resolution sanity: a known location + item resolve, and both the symbol
  //    and human forms hit the same id.
  uint16 ep_boss_sym = Customizer_ResolveLocation("Eastern_Palace_Boss");
  uint16 ep_boss_hum = Customizer_ResolveLocation("Eastern Palace - Boss");
  if (ep_boss_sym == 0xFFFF || ep_boss_sym != ep_boss_hum)
    customizer_selfcheck_die("Eastern Palace - Boss must resolve in both name forms to one id");
  if (Customizer_ResolveItem("Hookshot") == 0xFFFF)
    customizer_selfcheck_die("item 'Hookshot' must resolve");
  if (Customizer_ResolveLocation("Definitely Not A Real Location") != 0xFFFF)
    customizer_selfcheck_die("a bogus location must NOT resolve");

  // 4. Parse round-trip: a small manifest parses to the expected pins, and a
  //    duplicate location is rejected.
  {
    const char *ok_text =
        "# sample\n"
        "placements:\n"
        "  Eastern_Palace_Boss: Hookshot\n"
        "  Master Sword Pedestal: TitanMitt\n";
    CustomizerManifest m;
    char err[160];
    if (Customizer_Parse(ok_text, strlen(ok_text), &m, err, sizeof err) != 0)
      customizer_selfcheck_die(err);
    if (m.pin_count != 2) customizer_selfcheck_die("expected 2 pins");
    if (m.pins[0].location_id != ep_boss_sym ||
        m.pins[0].item_id != Customizer_ResolveItem("Hookshot"))
      customizer_selfcheck_die("first pin mismatch");
  }
  {
    const char *dup_text =
        "placements:\n"
        "  Eastern_Palace_Boss: Hookshot\n"
        "  Eastern Palace - Boss: Bow\n";
    CustomizerManifest m;
    char err[160];
    if (Customizer_Parse(dup_text, strlen(dup_text), &m, err, sizeof err) == 0)
      customizer_selfcheck_die("a double-pinned location must be rejected");
  }
  {
    const char *bad_text = "placements:\n  Not A Place: Hookshot\n";
    CustomizerManifest m;
    char err[160];
    if (Customizer_Parse(bad_text, strlen(bad_text), &m, err, sizeof err) == 0)
      customizer_selfcheck_die("an unknown location must be rejected");
  }
  {
    const char *bad_pool_text =
        "placements:\n"
        "  Eastern_Palace_Boss: Hookshot\n"
        "pool_overrides:\n"
        "  add: [HyruleCastleBigKey]\n";
    CustomizerManifest m;
    char err[160];
    if (Customizer_Parse(bad_pool_text, strlen(bad_pool_text), &m, err, sizeof err) == 0)
      customizer_selfcheck_die("a virtual non-grantable pool add must be rejected");
  }
  {
    const char *bad_key_pool_text =
        "placements:\n"
        "  Eastern_Palace_Boss: Hookshot\n"
        "pool_overrides:\n"
        "  remove: [SmallKey_PalaceOfDarkness]\n";
    CustomizerManifest m;
    char err[160];
    if (Customizer_Parse(bad_key_pool_text, strlen(bad_key_pool_text),
                         &m, err, sizeof err) == 0 ||
        strstr(err, "fixed cardinality") == NULL)
      customizer_selfcheck_die("key items in pool_overrides must be rejected");
  }

  fprintf(stderr, "[Customizer_SelfCheck] OK\n");
}

// Placement-path regression guard (CI via --rando-selftest). The corpus only
// covers customizer-OFF seeds; this exercises the §3c pin block end-to-end:
// install a manifest, run the real placer, and assert (a) every pin is honored
// in the output table and (b) two runs of the same (manifest, seed, settings)
// produce an identical placement digest. A future placer refactor that breaks
// customizer placement fails here on every platform.
void Customizer_PlacementSelfCheck(void) {
  extern const uint32 kRandoLocationsCount;

  CustomizerManifest m;
  memset(&m, 0, sizeof m);
  static const struct { const char *loc; const char *item; } kPins[] = {
    {"Eastern Palace - Big Chest", "Hookshot"},
    {"Link's Uncle", "ProgressiveSword"},
    {"Desert Palace - Big Chest", "ProgressiveBow"},
    // L1Sword is grantable but absent from the default progressive-sword pool.
    // This creates the surplus-junk edge case that must not discard an enabled
    // logic-neutral Skeleton Key.
    {"Kakariko Well - Top", "L1Sword"},
  };
  for (unsigned i = 0; i < sizeof kPins / sizeof kPins[0]; i++) {
    uint16 lid = Customizer_ResolveLocation(kPins[i].loc);
    uint16 iid = Customizer_ResolveItem(kPins[i].item);
    if (lid == 0xFFFF || iid == 0xFFFF)
      customizer_selfcheck_die("placement selfcheck: a pin name failed to resolve");
    m.pins[m.pin_count].location_id = lid;
    m.pins[m.pin_count].item_id = iid;
    m.pin_count++;
  }

  RandoSettings s;
  Settings_SetDefaults(&s);  // Open / Fast Ganon — these pins are reachable
  s.customizer_active = 1;
  s.skeleton_key = 1;

  RandoPlacement *e1 = (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
  RandoPlacement *e2 = (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
  if (e1 == NULL || e2 == NULL) customizer_selfcheck_die("placement selfcheck: out of memory");
  RandoPlacementTable t1 = { e1, 0 };
  RandoPlacementTable t2 = { e2, 0 };

  Customizer_Install(&m);
  bool ok1 = Place_AssumedFill(&s, 0x00C0FFEEull, 0, &t1);
  bool ok2 = Place_AssumedFill(&s, 0x00C0FFEEull, 0, &t2);
  Customizer_Install(NULL);
  if (!ok1 || !ok2) {
    const char *e = Customizer_LastError();
    fprintf(stderr, "Customizer_PlacementSelfCheck: Place_AssumedFill failed%s%s\n",
            e[0] ? ": " : "", e);
    free(e1); free(e2);
    exit(2);
  }

  // (a) every pin honored in the output table.
  for (uint16 i = 0; i < m.pin_count; i++) {
    uint16 got = 0xFFFF;
    for (uint16 k = 0; k < t1.count; k++) {
      if (t1.entries[k].location_id == m.pins[i].location_id) { got = t1.entries[k].item_id; break; }
    }
    if (got != m.pins[i].item_id) {
      fprintf(stderr, "Customizer_PlacementSelfCheck: pin %u (loc %u) resolved to %u, want %u\n",
              i, m.pins[i].location_id, got, m.pins[i].item_id);
      free(e1); free(e2);
      exit(2);
    }
  }

  // The out-of-pool pin above consumes a location without consuming a pool
  // copy. Skeleton Key must still survive exactly once rather than becoming
  // the surplus junk item silently omitted by fill.
  uint16 skeleton_count_1 = 0, skeleton_count_2 = 0;
  for (uint16 k = 0; k < t1.count; k++) {
    if (t1.entries[k].item_id == ITEM_SkeletonKey) skeleton_count_1++;
  }
  for (uint16 k = 0; k < t2.count; k++) {
    if (t2.entries[k].item_id == ITEM_SkeletonKey) skeleton_count_2++;
  }
  if (skeleton_count_1 != 1 || skeleton_count_2 != 1) {
    free(e1); free(e2);
    customizer_selfcheck_die(
        "placement selfcheck: enabled Skeleton Key cardinality drifted under out-of-pool pin");
  }

  // (b) determinism: identical digest across two runs.
  uint8 d1[32], d2[32];
  PlacementTable_ComputeDigest(&t1, d1);
  PlacementTable_ComputeDigest(&t2, d2);
  free(e1); free(e2);
  if (memcmp(d1, d2, 32) != 0)
    customizer_selfcheck_die("placement selfcheck: non-deterministic digest for a fixed (manifest, seed)");

  {
    CustomizerManifest bad;
    memset(&bad, 0, sizeof bad);
    bad.pins[0].location_id = Customizer_ResolveLocation("Eastern_Palace_Boss");
    bad.pins[0].item_id = Customizer_ResolveItem("HyruleCastleBigKey");
    bad.pin_count = 1;
    if (bad.pins[0].location_id == 0xFFFF || bad.pins[0].item_id == 0xFFFF)
      customizer_selfcheck_die("placement selfcheck: virtual item names failed to resolve");
    RandoPlacement *bad_entries =
        (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
    if (bad_entries == NULL)
      customizer_selfcheck_die("placement selfcheck: out of memory for bad pin");
    RandoPlacementTable bad_table = { bad_entries, 0 };
    Customizer_Install(&bad);
    bool bad_ok = Place_AssumedFill(&s, 0x00C0FFEEull, 0, &bad_table);
    Customizer_Install(NULL);
    free(bad_entries);
    if (bad_ok || strstr(Customizer_LastError(), "not grantable") == NULL)
      customizer_selfcheck_die("placement selfcheck: virtual non-grantable pin must be rejected");
  }

  // add-rando-shopsanity — shop-slot pins follow the axis: with shopsanity ON
  // the slot is an ordinary open location and the pin is honored; with it OFF
  // (Retro, where the slot IS collected but identity-pinned) the pin is
  // refused with the non-customizable-type message. Closes the deferred
  // tasks.md 3.5 prong.
  {
    CustomizerManifest shop;
    memset(&shop, 0, sizeof shop);
    shop.pins[0].location_id = Customizer_ResolveLocation("Dark World Forest Shop - 0");
    shop.pins[0].item_id = Customizer_ResolveItem("Hookshot");
    shop.pin_count = 1;
    if (shop.pins[0].location_id == 0xFFFF || shop.pins[0].item_id == 0xFFFF)
      customizer_selfcheck_die("placement selfcheck: shop pin names failed to resolve");
    RandoPlacement *shop_entries =
        (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
    if (shop_entries == NULL)
      customizer_selfcheck_die("placement selfcheck: out of memory for shop pin");
    RandoPlacementTable shop_table = { shop_entries, 0 };
    RandoSettings ss = s;
    ss.shopsanity = 1;
    Customizer_Install(&shop);
    bool shop_ok = Place_AssumedFill(&ss, 0x00C0FFEEull, 0, &shop_table);
    Customizer_Install(NULL);
    if (!shop_ok) {
      fprintf(stderr, "Customizer_PlacementSelfCheck: shopsanity shop pin refused: %s\n",
              Customizer_LastError());
      exit(2);
    }
    bool honored = false;
    for (uint16 pi2 = 0; pi2 < shop_table.count; pi2++) {
      if (shop_table.entries[pi2].location_id == shop.pins[0].location_id &&
          shop_table.entries[pi2].item_id == shop.pins[0].item_id) {
        honored = true;
        break;
      }
    }
    free(shop_entries);
    if (!honored)
      customizer_selfcheck_die("placement selfcheck: shopsanity shop pin was not honored");
    RandoPlacement *off_entries =
        (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
    if (off_entries == NULL)
      customizer_selfcheck_die("placement selfcheck: out of memory for shop-off pin");
    RandoPlacementTable off_table = { off_entries, 0 };
    RandoSettings off = s;
    off.world_state = kWorldState_Retro;  // slot collected but identity-pinned
    Customizer_Install(&shop);
    bool off_ok = Place_AssumedFill(&off, 0x00C0FFEEull, 0, &off_table);
    Customizer_Install(NULL);
    free(off_entries);
    if (off_ok || strstr(Customizer_LastError(), "shop slot without shopsanity") == NULL)
      customizer_selfcheck_die(
          "placement selfcheck: shop pin without shopsanity must be refused with the type message");
  }

  // External-review P1 negative pins — each reproduced a shipped self-lock the
  // placer ACCEPTED; after the logic fixes both must REFUSE at the default
  // items accessibility. They pin exactly the reviewer's reproducers so the
  // circular placements can never regress silently.
  //
  // (1) Inverted LW Death Mountain Shop holding the Hookshot: the shop's
  // location predicate requires the Hookshot (upstream Inverted East.php:37
  // callback; the region's dark-rock-lift entry admits Mitt-only arrivals,
  // so region reachability proves nothing) — Hookshot inside it is circular.
  {
    CustomizerManifest lock;
    memset(&lock, 0, sizeof lock);
    lock.pins[0].location_id =
        Customizer_ResolveLocation("Light World Death Mountain Shop - 0");
    lock.pins[0].item_id = Customizer_ResolveItem("Hookshot");
    lock.pin_count = 1;
    if (lock.pins[0].location_id == 0xFFFF || lock.pins[0].item_id == 0xFFFF)
      customizer_selfcheck_die("placement selfcheck: inverted shop self-lock names failed to resolve");
    RandoPlacement *e =
        (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
    if (e == NULL)
      customizer_selfcheck_die("placement selfcheck: out of memory for inverted self-lock");
    RandoPlacementTable t = { e, 0 };
    RandoSettings inv = s;
    inv.world_state = kWorldState_Inverted;
    inv.shopsanity = 1;
    inv.accessibility = kAccessibility_Items;
    Customizer_Install(&lock);
    bool filled = Place_AssumedFill(&inv, 0x00C0FFEEull, 0, &t);
    // The fill itself may complete; the REFUSAL lives in the pipeline's
    // acceptance gate (Goal_ShouldRefuse — beatability + accessibility tier),
    // exactly as --generate-seed applies it. Base seed, never attempt seed.
    bool refused = !filled || Goal_ShouldRefuse(&inv, 0x00C0FFEEull, &t);
    Customizer_Install(NULL);
    free(e);
    if (!refused)
      customizer_selfcheck_die(
          "placement selfcheck: Hookshot pinned inside the Inverted LW DM shop must refuse (self-lock)");
  }

  // (2) Standard bonk-sanity with the Lamp on a post-rescue bonk tree: the
  // stage-0-absent rows carry HAS_ITEM(RescuedZelda), the escort needs the
  // Lamp, so Lamp-on-the-tree is circular. Registry-gated: an assetless
  // build has zero bonk rows and the pin target does not exist.
  {
    uint16 bonk_locs = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++)
      if (kRandoLocations[i].type == LOCTYPE_Bonk) bonk_locs++;
    if (bonk_locs > 0) {
      CustomizerManifest lock;
      memset(&lock, 0, sizeof lock);
      lock.pins[0].location_id =
          Customizer_ResolveLocation("HyruleCastleEscape BonkApple A1B B0546");
      lock.pins[0].item_id = Customizer_ResolveItem("Lamp");
      lock.pin_count = 1;
      if (lock.pins[0].location_id == 0xFFFF || lock.pins[0].item_id == 0xFFFF)
        customizer_selfcheck_die("placement selfcheck: bonk self-lock names failed to resolve");
      RandoPlacement *e =
          (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
      if (e == NULL)
        customizer_selfcheck_die("placement selfcheck: out of memory for bonk self-lock");
      RandoPlacementTable t = { e, 0 };
      RandoSettings std = s;
      std.world_state = kWorldState_Standard;
      std.bonk_shuffle = kTerrainShuffle_All;
      std.accessibility = kAccessibility_Items;
      Customizer_Install(&lock);
      bool filled = Place_AssumedFill(&std, 0x00C0FFEEull, 0, &t);
      bool refused = !filled || Goal_ShouldRefuse(&std, 0x00C0FFEEull, &t);
      Customizer_Install(NULL);
      free(e);
      if (!refused)
        customizer_selfcheck_die(
            "placement selfcheck: Lamp pinned on the Standard post-rescue bonk tree must refuse (hardlock)");
    }
  }

  fprintf(stderr, "[Customizer_PlacementSelfCheck] OK\n");
}
