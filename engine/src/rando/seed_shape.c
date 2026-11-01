// seed_shape.c - generator-side seed-shape filters.
#include "seed_shape.h"

#include "item_ids.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg) {
  if (err == NULL || cap == 0) return;
  snprintf(err, cap, "%s", msg);
}

static void set_err2(char *err, size_t cap, const char *prefix,
                     const char *detail, size_t detail_len) {
  if (err == NULL || cap == 0) return;
  if (detail == NULL) detail = "";
  snprintf(err, cap, "%s%.*s", prefix, (int)detail_len, detail);
}

static int ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool is_alnum_ascii(int c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z');
}

static void normalize_token(const char *src, size_t len,
                            char *dst, size_t dst_cap) {
  size_t w = 0;
  if (dst_cap == 0) return;
  for (size_t i = 0; i < len && w + 1 < dst_cap; i++) {
    unsigned char c = (unsigned char)src[i];
    if (!is_alnum_ascii(c)) continue;
    dst[w++] = (char)ascii_tolower(c);
  }
  dst[w] = '\0';
}

static bool token_eq(const char *start, size_t len, const char *lit) {
  size_t lit_len = strlen(lit);
  if (len != lit_len) return false;
  for (size_t i = 0; i < len; i++) {
    if (ascii_tolower((unsigned char)start[i]) != ascii_tolower((unsigned char)lit[i]))
      return false;
  }
  return true;
}

static int parse_u8(const char *p, size_t len, uint8 *out) {
  if (len == 0 || out == NULL) return -1;
  uint32 v = 0;
  for (size_t i = 0; i < len; i++) {
    char c = p[i];
    if (c < '0' || c > '9') return -1;
    v = v * 10u + (uint32)(c - '0');
    if (v > 255u) return -1;
  }
  *out = (uint8)v;
  return 0;
}

static int add_clause(SeedShapeFilter *out, uint8 kind,
                      uint16 item_id, uint8 value,
                      char *err, size_t err_cap) {
  if (out->count >= kSeedShapeMaxClauses) {
    set_err(err, err_cap, "too many shape filter clauses");
    return -1;
  }
  out->clauses[out->count].kind = kind;
  out->clauses[out->count].item_id = item_id;
  out->clauses[out->count].value = value;
  out->count++;
  out->enabled = 1;
  return 0;
}

static int find_item_id_by_name(const char *name, size_t len) {
  char wanted[96];
  normalize_token(name, len, wanted, sizeof wanted);
  if (wanted[0] == '\0') return -1;
  for (uint16 id = 0; id < ITEM__COUNT && id < 256; id++) {
    const char *item_name = Rando_GetItemName(id);
    char got[96];
    normalize_token(item_name, strlen(item_name), got, sizeof got);
    if (strcmp(wanted, got) == 0) return id;
  }
  return -1;
}

static int add_early_item(SeedShapeFilter *out, uint16 item_id,
                          char *err, size_t err_cap) {
  return add_clause(out, kSeedShapeClause_ItemAtMost, item_id, 2, err, err_cap);
}

static int parse_key_value_clause(const char *tok, size_t len,
                                  const char *key,
                                  uint8 kind,
                                  SeedShapeFilter *out,
                                  char *err, size_t err_cap) {
  size_t key_len = strlen(key);
  if (len <= key_len + 1 || !token_eq(tok, key_len, key) || tok[key_len] != '=')
    return 1;  // not this clause type
  uint8 value = 0;
  if (parse_u8(tok + key_len + 1, len - key_len - 1, &value) != 0) {
    set_err2(err, err_cap, "bad numeric value in shape token: ", tok, len);
    return -1;
  }
  return add_clause(out, kind, 0, value, err, err_cap);
}

static int parse_item_clause(const char *tok, size_t len,
                             SeedShapeFilter *out,
                             char *err, size_t err_cap) {
  const char *p = tok;
  size_t n = len;
  if (n > 5 && token_eq(p, 5, "item:")) {
    p += 5;
    n -= 5;
  } else if (n > 5 && token_eq(p, 5, "item=")) {
    p += 5;
    n -= 5;
  } else {
    return 1;  // not an item clause
  }

  const char *op = NULL;
  uint8 kind = 0;
  for (size_t i = 0; i + 1 < n; i++) {
    if (p[i] == '<' && p[i + 1] == '=') {
      op = p + i;
      kind = kSeedShapeClause_ItemAtMost;
      break;
    }
    if (p[i] == '>' && p[i + 1] == '=') {
      op = p + i;
      kind = kSeedShapeClause_ItemAtLeast;
      break;
    }
  }
  if (op == NULL || op == p) {
    set_err2(err, err_cap,
             "item shape tokens use item:<name><=N or item:<name>>=N: ", tok, len);
    return -1;
  }
  uint8 value = 0;
  const char *value_start = op + 2;
  size_t value_len = (size_t)(p + n - value_start);
  if (parse_u8(value_start, value_len, &value) != 0) {
    set_err2(err, err_cap, "bad item sphere in shape token: ", tok, len);
    return -1;
  }
  int item_id = find_item_id_by_name(p, (size_t)(op - p));
  if (item_id < 0) {
    set_err2(err, err_cap, "unknown item in shape token: ", tok, len);
    return -1;
  }
  return add_clause(out, kind, (uint16)item_id, value, err, err_cap);
}

int SeedShape_Parse(const char *text, SeedShapeFilter *out,
                    char *err, size_t err_cap) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  if (err != NULL && err_cap > 0) err[0] = '\0';
  if (text == NULL || *text == '\0') return 0;

  const char *p = text;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ';') p++;
    const char *end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t len = (size_t)(end - start);
    if (len == 0) continue;

    if (token_eq(start, len, "short")) {
      if (add_clause(out, kSeedShapeClause_MaxSphere, 0,
                     kSeedShapePresetShortMaxSphere, err, err_cap) != 0)
        return -1;
    } else if (token_eq(start, len, "long")) {
      if (add_clause(out, kSeedShapeClause_MinSphere, 0,
                     kSeedShapePresetLongMinSphere, err, err_cap) != 0)
        return -1;
    } else if (token_eq(start, len, "no_unreachable") ||
               token_eq(start, len, "nounreachable")) {
      if (add_clause(out, kSeedShapeClause_NoUnreachable, 0, 0, err, err_cap) != 0) return -1;
    } else if (token_eq(start, len, "no_forward_fill") ||
               token_eq(start, len, "noforwardfill")) {
      if (add_clause(out, kSeedShapeClause_NoForwardFill, 0, 0, err, err_cap) != 0) return -1;
    } else if (token_eq(start, len, "early_boots")) {
      if (add_early_item(out, ITEM_Boots, err, err_cap) != 0) return -1;
    } else if (token_eq(start, len, "early_flute")) {
      if (add_early_item(out, ITEM_OcarinaInactive, err, err_cap) != 0) return -1;
    } else if (token_eq(start, len, "early_mirror")) {
      if (add_early_item(out, ITEM_MagicMirror, err, err_cap) != 0) return -1;
    } else if (token_eq(start, len, "early_hookshot")) {
      if (add_early_item(out, ITEM_Hookshot, err, err_cap) != 0) return -1;
    } else if (token_eq(start, len, "early_lamp")) {
      if (add_early_item(out, ITEM_Lamp, err, err_cap) != 0) return -1;
    } else {
      int rc = parse_key_value_clause(start, len, "max_sphere",
                                      kSeedShapeClause_MaxSphere,
                                      out, err, err_cap);
      if (rc == 1)
        rc = parse_key_value_clause(start, len, "min_sphere",
                                    kSeedShapeClause_MinSphere,
                                    out, err, err_cap);
      if (rc == 1)
        rc = parse_item_clause(start, len, out, err, err_cap);
      if (rc != 0) {
        if (rc > 0) set_err2(err, err_cap, "unknown shape filter token: ", start, len);
        return -1;
      }
    }
  }
  return 0;
}

static uint8 earliest_item_sphere(uint16 item_id,
                                  const RandoPlacementTable *placements,
                                  const RandoSpheres *spheres) {
  uint8 best = kSphereIndexUnreachable;
  if (placements == NULL || spheres == NULL) return best;
  for (uint16 i = 0; i < placements->count; i++) {
    if (placements->entries[i].item_id != item_id) continue;
    uint8 sp = spheres->sphere_index_by_placement[i];
    if (sp < best) best = sp;
  }
  return best;
}

static void compute_metrics(const RandoSpheres *spheres,
                            const PlacementStats *stats,
                            SeedShapeMetrics *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (spheres != NULL) {
    out->max_sphere = spheres->max_sphere;
    out->unreachable_count = spheres->unreachable_count;
  }
  if (stats != NULL) {
    out->forward_fill_fallback_count = stats->forward_fill_fallback_count;
  }
}

static void rejectf(char *buf, size_t cap, const char *fmt, ...) {
  if (buf == NULL || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
}

bool SeedShape_Evaluate(const SeedShapeFilter *filter,
                        const RandoSettings *settings,
                        const RandoPlacementTable *placements,
                        const RandoSpheres *spheres,
                        const PlacementStats *stats,
                        SeedShapeMetrics *metrics_out,
                        char *reject_reason,
                        size_t reject_reason_cap) {
  (void)settings;
  compute_metrics(spheres, stats, metrics_out);
  if (reject_reason != NULL && reject_reason_cap > 0) reject_reason[0] = '\0';
  if (filter == NULL || !filter->enabled) return true;

  SeedShapeMetrics metrics;
  compute_metrics(spheres, stats, &metrics);
  for (uint8 i = 0; i < filter->count; i++) {
    const SeedShapeClause *c = &filter->clauses[i];
    switch (c->kind) {
      case kSeedShapeClause_MaxSphere:
        if (metrics.max_sphere > c->value) {
          rejectf(reject_reason, reject_reason_cap,
                  "max sphere %u exceeds %u",
                  (unsigned)metrics.max_sphere, (unsigned)c->value);
          return false;
        }
        break;
      case kSeedShapeClause_MinSphere:
        if (metrics.max_sphere < c->value) {
          rejectf(reject_reason, reject_reason_cap,
                  "max sphere %u below %u",
                  (unsigned)metrics.max_sphere, (unsigned)c->value);
          return false;
        }
        break;
      case kSeedShapeClause_NoUnreachable:
        if (metrics.unreachable_count != 0) {
          rejectf(reject_reason, reject_reason_cap,
                  "%u unreachable placement(s)",
                  (unsigned)metrics.unreachable_count);
          return false;
        }
        break;
      case kSeedShapeClause_NoForwardFill:
        if (metrics.forward_fill_fallback_count != 0) {
          rejectf(reject_reason, reject_reason_cap,
                  "%u forward-fill fallback(s)",
                  (unsigned)metrics.forward_fill_fallback_count);
          return false;
        }
        break;
      case kSeedShapeClause_ItemAtMost: {
        uint8 sp = earliest_item_sphere(c->item_id, placements, spheres);
        if (sp == kSphereIndexUnreachable || sp > c->value) {
          rejectf(reject_reason, reject_reason_cap,
                  "%s earliest sphere %u exceeds %u",
                  Rando_GetItemName(c->item_id),
                  sp == kSphereIndexUnreachable ? 255u : (unsigned)sp,
                  c->value);
          return false;
        }
        break;
      }
      case kSeedShapeClause_ItemAtLeast: {
        uint8 sp = earliest_item_sphere(c->item_id, placements, spheres);
        if (sp == kSphereIndexUnreachable || sp < c->value) {
          rejectf(reject_reason, reject_reason_cap,
                  "%s earliest sphere %u below %u",
                  Rando_GetItemName(c->item_id),
                  sp == kSphereIndexUnreachable ? 255u : (unsigned)sp,
                  c->value);
          return false;
        }
        break;
      }
      default:
        rejectf(reject_reason, reject_reason_cap,
                "unknown shape clause %u", (unsigned)c->kind);
        return false;
    }
  }
  return true;
}

static void append_text(char *out, size_t cap, const char *text) {
  if (out == NULL || cap == 0 || text == NULL) return;
  size_t used = strlen(out);
  if (used + 1 >= cap) return;
  snprintf(out + used, cap - used, "%s", text);
}

void SeedShape_Describe(const SeedShapeFilter *filter, char *out, size_t out_cap) {
  if (out == NULL || out_cap == 0) return;
  out[0] = '\0';
  if (filter == NULL || !filter->enabled) {
    append_text(out, out_cap, "(none)");
    return;
  }
  for (uint8 i = 0; i < filter->count; i++) {
    char one[128];
    const SeedShapeClause *c = &filter->clauses[i];
    if (i) append_text(out, out_cap, ",");
    switch (c->kind) {
      case kSeedShapeClause_MaxSphere:
        snprintf(one, sizeof one, "max_sphere<=%u", c->value);
        break;
      case kSeedShapeClause_MinSphere:
        snprintf(one, sizeof one, "max_sphere>=%u", c->value);
        break;
      case kSeedShapeClause_NoUnreachable:
        snprintf(one, sizeof one, "no_unreachable");
        break;
      case kSeedShapeClause_NoForwardFill:
        snprintf(one, sizeof one, "no_forward_fill");
        break;
      case kSeedShapeClause_ItemAtMost:
        snprintf(one, sizeof one, "%s<=%u", Rando_GetItemName(c->item_id), c->value);
        break;
      case kSeedShapeClause_ItemAtLeast:
        snprintf(one, sizeof one, "%s>=%u", Rando_GetItemName(c->item_id), c->value);
        break;
      default:
        snprintf(one, sizeof one, "unknown");
        break;
    }
    append_text(out, out_cap, one);
  }
}

void SeedShape_FormatMetrics(const SeedShapeMetrics *metrics,
                             char *out, size_t out_cap) {
  if (out == NULL || out_cap == 0) return;
  if (metrics == NULL) {
    out[0] = '\0';
    return;
  }
  snprintf(out, out_cap, "max_sphere=%u,unreachable=%u,forward_fill=%u",
           (unsigned)metrics->max_sphere,
           (unsigned)metrics->unreachable_count,
           (unsigned)metrics->forward_fill_fallback_count);
}

static void shape_sc_die(const char *msg) {
  fprintf(stderr, "[SeedShape_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void SeedShape_SelfCheck(void) {
  SeedShapeFilter f;
  char err[128];
  if (SeedShape_Parse("early_boots,short,no_unreachable,item:Hookshot<=3",
                      &f, err, sizeof err) != 0)
    shape_sc_die("valid filter failed to parse");
  if (!f.enabled || f.count != 4)
    shape_sc_die("valid filter clause count wrong");
  if (f.clauses[1].kind != kSeedShapeClause_MaxSphere ||
      f.clauses[1].value != kSeedShapePresetShortMaxSphere)
    shape_sc_die("short alias threshold drifted");
  if (SeedShape_Parse("long", &f, err, sizeof err) != 0)
    shape_sc_die("long alias failed to parse");
  if (f.count != 1 || f.clauses[0].kind != kSeedShapeClause_MinSphere ||
      f.clauses[0].value != kSeedShapePresetLongMinSphere)
    shape_sc_die("long alias threshold drifted");
  if (SeedShape_Parse("max_sphere=bad", &f, err, sizeof err) == 0)
    shape_sc_die("invalid max_sphere parsed successfully");
  if (SeedShape_Parse("item:NoSuchItem<=2", &f, err, sizeof err) == 0)
    shape_sc_die("unknown item parsed successfully");

  RandoPlacement entries[2] = {
    { 1, ITEM_Boots },
    { 2, ITEM_Hookshot },
  };
  RandoPlacementTable t = { entries, 2 };
  RandoSpheres s;
  memset(&s, 0, sizeof s);
  for (uint16 i = 0; i < (uint16)(sizeof(s.sphere_index_by_placement) /
                                  sizeof(s.sphere_index_by_placement[0])); i++)
    s.sphere_index_by_placement[i] = kSphereIndexUnreachable;
  s.sphere_index_by_placement[0] = 2;
  s.sphere_index_by_placement[1] = 3;
  s.max_sphere = 3;
  PlacementStats st;
  memset(&st, 0, sizeof st);
  if (SeedShape_Parse("early_boots,item:Hookshot<=3,max_sphere=3",
                      &f, err, sizeof err) != 0)
    shape_sc_die("valid eval filter failed to parse");
  if (!SeedShape_Evaluate(&f, NULL, &t, &s, &st, NULL, err, sizeof err))
    shape_sc_die("expected filter to pass");
  if (SeedShape_Parse("early_hookshot", &f, err, sizeof err) != 0)
    shape_sc_die("early_hookshot failed to parse");
  if (SeedShape_Evaluate(&f, NULL, &t, &s, &st, NULL, err, sizeof err))
    shape_sc_die("late hookshot should fail early_hookshot");
  fprintf(stderr, "[SeedShape_SelfCheck] OK\n");
}
