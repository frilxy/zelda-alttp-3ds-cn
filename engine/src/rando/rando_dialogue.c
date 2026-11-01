#include "rando_dialogue.h"

#include "item_ids.h"
#include "location_ids.h"
#include "rando.h"
#include "rando_hints.h"
#include "goal_runtime.h"
#include "rando_logic.h"
#include "rando_placement.h"
#include "../features.h"
#include "../messaging.h"
#include "../variables.h"
#include "../zelda_rtl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kDialogueBufferCapacity = 0xE00,  // g_ram[0x11200..0x11fff]
  kCmdChoose = 0x68,
  kCmdScroll = 0x73,
  kCmdLine1 = 0x74,
  kCmdLine2 = 0x75,
  kCmdLine3 = 0x76,
  kCmdWaitKey = 0x7E,
  kCmdEnd = 0x7F,
  kMaxSafeLineChars = 13,
};

static uint8 dialogue_ascii_to_font(char ch) {
  if (ch >= 'A' && ch <= 'Z') return (uint8)(ch - 'A');
  if (ch >= 'a' && ch <= 'z') return (uint8)(26 + ch - 'a');
  if (ch >= '0' && ch <= '9') return (uint8)(52 + ch - '0');
  switch (ch) {
  case '!': return 62;
  case '?': return 63;
  case '-': return 64;
  case '.': return 65;
  case ',': return 66;
  case '>': return 68;
  case '\'': return 81;
  default: return 89;  // space (also the safe replacement for unsupported glyphs)
  }
}

static int write_ascii(uint8 *out, int pos, const char *text) {
  while (*text && pos < kDialogueBufferCapacity - 1)
    out[pos++] = dialogue_ascii_to_font(*text++);
  return pos;
}

// Three-row, word-wrapped US dialogue. Returns false if all text could not fit
// in one box; callers that promise an exact reward name compose in scratch and
// fail closed rather than copying a clipped claim into the live buffer.
static bool write_wrapped_message_complete(uint8 *out, const char *text) {
  int w = 0, col = 0, row = 0;
  const char *p = text;
  bool complete = true;
  while (*p && w < kDialogueBufferCapacity - 2) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *end = p;
    while (*end && *end != ' ') end++;
    int n = (int)(end - p);
    if (col && col + 1 + n > kMaxSafeLineChars) {
      if (row == 2) {
        complete = false;
        break;
      }
      out[w++] = (++row == 1) ? kCmdLine2 : kCmdLine3;
      col = 0;
    } else if (col) {
      out[w++] = dialogue_ascii_to_font(' ');
      col++;
    }
    while (p < end) {
      if (col == kMaxSafeLineChars) {
        if (row == 2) {
          complete = false;
          goto done;
        }
        out[w++] = (++row == 1) ? kCmdLine2 : kCmdLine3;
        col = 0;
      }
      out[w++] = dialogue_ascii_to_font(*p++);
      col++;
    }
  }
done:
  out[w] = kCmdEnd;
  return complete && *p == '\0';
}

static void write_wrapped_message(uint8 *out, const char *text) {
  (void)write_wrapped_message_complete(out, text);
}

// kTextCmd_Choose uses dialogue messages 1/2 as cursor-only redraw patches.
// Those patches erase six leading spaces and draw the active arrow after four
// spaces. Match the vanilla low-two-row layout so Up/Down changes only this
// prefix: both forms place the first option glyph at pixel x=28.
static const char kChoiceSelectedPrefix[] = "    > ";
static const char kChoiceUnselectedPrefix[] = "       ";

static void write_choice_message(uint8 *out, const char *top,
                                 const char *yes, const char *no) {
  int w = write_ascii(out, 0, top);
  out[w++] = kCmdLine2;
  w = write_ascii(out, w, kChoiceSelectedPrefix);
  w = write_ascii(out, w, yes);
  out[w++] = kCmdLine3;
  w = write_ascii(out, w, kChoiceUnselectedPrefix);
  w = write_ascii(out, w, no);
  out[w++] = kCmdChoose;
  out[w] = kCmdEnd;
}

static void split_registry_token(const char *in, char *out, size_t cap) {
  size_t w = 0;
  for (size_t i = 0; in[i] && w + 1 < cap; i++) {
    char c = in[i];
    if (c == '_') {
      if (w && out[w - 1] != ' ') out[w++] = ' ';
      continue;
    }
    bool boundary = i && ((c >= 'A' && c <= 'Z' && in[i - 1] >= 'a' && in[i - 1] <= 'z') ||
                          (c >= '0' && c <= '9' && !(in[i - 1] >= '0' && in[i - 1] <= '9')));
    if (boundary && w && out[w - 1] != ' ' && w + 1 < cap) out[w++] = ' ';
    out[w++] = c;
  }
  out[w] = 0;
}

static const char *short_dungeon_name(const char *token) {
  if (!strcmp(token, "HyruleCastleEscape")) return "HC";
  if (!strcmp(token, "EasternPalace")) return "Eastern";
  if (!strcmp(token, "DesertPalace")) return "Desert";
  if (!strcmp(token, "TowerOfHera")) return "Hera";
  if (!strcmp(token, "HyruleCastleTower")) return "Castle Tower";
  if (!strcmp(token, "PalaceOfDarkness")) return "PoD";
  if (!strcmp(token, "SwampPalace")) return "Swamp";
  if (!strcmp(token, "SkullWoods")) return "Skull";
  if (!strcmp(token, "ThievesTown")) return "Thieves";
  if (!strcmp(token, "IcePalace")) return "Ice";
  if (!strcmp(token, "MiseryMire")) return "Mire";
  if (!strcmp(token, "TurtleRock")) return "TR";
  if (!strcmp(token, "GanonsTower")) return "GT";
  return token;
}

static void format_reward_item(uint16 item_id, char *out, size_t cap) {
  const char *tok = Rando_GetItemName(item_id);
  struct NameOverride { const char *token, *display; };
  static const struct NameOverride kOverrides[] = {
    {"L1Sword", "Fighter Sword"}, {"L2Sword", "Master Sword"},
    {"L3Sword", "Tempered Sword"}, {"L4Sword", "Gold Sword"},
    {"OcarinaInactive", "Flute"}, {"BugCatchingNet", "Bug Net"},
    {"BottleEmpty", "Bottle"}, {"PieceOfHeart", "Heart Piece"},
    {"BossHeartContainer", "Heart Container"},
    {"SilverArrowUpgrade", "Silver Arrows"},
    {"Rupee1", "1 Rupee"}, {"Rupee5", "5 Rupees"},
    {"Rupee20", "20 Rupees"}, {"Rupee100", "100 Rupees"},
    {"Rupee300", "300 Rupees"}, {"Arrow1", "1 Arrow"},
    {"Arrow10", "10 Arrows"}, {"Bombs1", "1 Bomb"},
    {"Bombs3", "3 Bombs"}, {"Bombs10", "10 Bombs"},
  };
  for (size_t i = 0; i < sizeof(kOverrides) / sizeof(kOverrides[0]); i++) {
    if (!strcmp(tok, kOverrides[i].token)) {
      snprintf(out, cap, "%s", kOverrides[i].display);
      return;
    }
  }

  static const struct { const char *prefix, *kind; } kDungeonKinds[] = {
    {"SmallKey_", "Small Key"}, {"BigKey_", "Big Key"},
    {"Map_", "Map"}, {"Compass_", "Compass"}, {"KeyRing_", "Key Ring"},
  };
  for (size_t i = 0; i < sizeof(kDungeonKinds) / sizeof(kDungeonKinds[0]); i++) {
    size_t n = strlen(kDungeonKinds[i].prefix);
    if (!strncmp(tok, kDungeonKinds[i].prefix, n)) {
      snprintf(out, cap, "%s %s", short_dungeon_name(tok + n), kDungeonKinds[i].kind);
      return;
    }
  }
  if (!strncmp(tok, "Prize_Crystal", 13)) {
    snprintf(out, cap, "Crystal %s", tok + 13);
    return;
  }
  if (!strncmp(tok, "Prize_", 6)) tok += 6;
  if (!strncmp(tok, "Soul_Npc_", 9)) {
    char tmp[40];  // 39 chars + " Soul" must fit the smallest 48-byte reward buffer
    split_registry_token(tok + 9, tmp, sizeof(tmp));
    snprintf(out, cap, "%s Soul", tmp);
    return;
  }
  if (!strncmp(tok, "Soul_", 5)) {
    char tmp[40];  // 39 chars + " Soul" must fit the smallest 48-byte reward buffer
    split_registry_token(tok + 5, tmp, sizeof(tmp));
    snprintf(out, cap, "%s Soul", tmp);
    return;
  }
  split_registry_token(tok, out, cap);
}

static bool format_preview_reward_item(uint16 item_id,
                                       char *out, size_t cap) {
  const char *generated = Rando_GetHintItemDisplayName(item_id, false);
  if (generated == NULL || generated[0] == '\0') return false;
  size_t len = strlen(generated);
  if (len >= cap) return false;
  memcpy(out, generated, len + 1);
  return true;
}

static bool format_compact_reward_item(uint16 item_id,
                                       char *out, size_t cap) {
  const char *generated = Rando_GetHintItemDisplayName(item_id, true);
  if (generated == NULL || generated[0] == '\0') return false;
  size_t len = strlen(generated);
  if (len >= cap) return false;
  memcpy(out, generated, len + 1);
  return true;
}

static int dialogue_len(const uint8 *buf) {
  for (int i = 0; i < kDialogueBufferCapacity; i++)
    if (buf[i] == kCmdEnd) return i;
  return -1;
}

static int encode_search(const char *text, uint8 *out) {
  int n = 0;
  while (*text) out[n++] = dialogue_ascii_to_font(*text++);
  return n;
}

static int find_ascii_run(const uint8 *buf, const char *text) {
  uint8 needle[96];
  int nn = encode_search(text, needle);
  int bn = dialogue_len(buf);
  if (bn < 0) return -1;
  for (int i = 0; i + nn <= bn; i++)
    if (!memcmp(buf + i, needle, (size_t)nn)) return i;
  return -1;
}

static bool replace_ascii_run(uint8 *buf, const char *find, const char *replace) {
  int at = find_ascii_run(buf, find);
  int len = dialogue_len(buf);
  if (at < 0 || len < 0) return false;
  uint8 repl[96];
  int fn = (int)strlen(find), rn = encode_search(replace, repl);
  if (len - fn + rn >= kDialogueBufferCapacity) return false;
  memmove(buf + at + rn, buf + at + fn, (size_t)(len - at - fn + 1));
  memcpy(buf + at, repl, (size_t)rn);
  return true;
}

static uint16 lookup_reward(uint16 loc, uint16 vanilla_item) {
  return Placement_Lookup(loc, vanilla_item);  // deliberately side-effect free
}

static void write_item_message(uint8 *buf, const char *fmt,
                               uint16 loc, uint16 vanilla_item) {
  char item[48], text[96];
  format_reward_item(lookup_reward(loc, vanilla_item), item, sizeof(item));
  snprintf(text, sizeof(text), fmt, item);
  write_wrapped_message(buf, text);
}

// Reward previews are seed facts, not vanilla fallbacks. A missing placement
// or an already-checked source leaves the caller's truthful generic dialogue
// untouched.
static bool lookup_unchecked_placement_item(uint16 loc, uint16 *out_item) {
  uint16 item;
  if (Rando_IsLocationChecked(loc) ||
      !Placement_TryLookup(loc, &item) ||
      item >= ITEM__COUNT)
    return false;
  if (out_item != NULL) *out_item = item;
  return true;
}

bool Rando_CanPreviewNpcReward(uint16 location_id) {
  const RandoSettings *settings = Rando_GetActiveSettings();
  uint16 item;
  return Rando_IsActive() && g_zenv.dialogue_flags == 0 &&
         settings != NULL && settings->hint_npc_reward_reveal != 0 &&
         lookup_unchecked_placement_item(location_id, &item) &&
         Rando_GetHintItemDisplayName(item, false) != NULL;
}

// Try the full generated qualified name first, then its generated compact
// qualified spelling. The final short template keeps every registered item
// lossless without ever copying a clipped claim into the live buffer.
static bool render_reward_item_page(uint8 *out, const char *fmt,
                                    uint16 item_id) {
  char item[64], text[96];
  int n;
  if (format_preview_reward_item(item_id, item, sizeof(item))) {
    n = snprintf(text, sizeof(text), fmt, item);
    if (n >= 0 && (size_t)n < sizeof(text) &&
        write_wrapped_message_complete(out, text))
      return true;
  }

  if (!format_compact_reward_item(item_id, item, sizeof(item)))
    return false;
  n = snprintf(text, sizeof(text), fmt, item);
  if (n >= 0 && (size_t)n < sizeof(text) &&
      write_wrapped_message_complete(out, text))
    return true;

  n = snprintf(text, sizeof(text), "Reward %s.", item);
  return n >= 0 && (size_t)n < sizeof(text) &&
         write_wrapped_message_complete(out, text);
}

static bool write_placement_item_message(uint8 *buf, const char *fmt,
                                         uint16 loc) {
  uint16 item;
  uint8 tmp[kDialogueBufferCapacity];
  if (!lookup_unchecked_placement_item(loc, &item) ||
      !render_reward_item_page(tmp, fmt, item))
    return false;
  int len = dialogue_len(tmp);
  if (len < 0) return false;
  memcpy(buf, tmp, (size_t)len + 1);
  return true;
}

// Roll a complete three-row choice page into the box one row at a time. Each
// Scroll clears the row it replaces, so no glyph pixels from the preceding
// page survive underneath the choice text. The old Wait+one Scroll+Line1
// sequence was not a page clear: a two-line "I sell 1 Bomb." preview left
// "Bomb." in row 0 and XOR-rendered the next prompt over it.
static bool append_scrolling_choice_page(uint8 *dst, int *io_w,
                                         const uint8 *page) {
  int len = dialogue_len(page);
  int line2 = -1, line3 = -1, choose = -1;
  if (dst == NULL || io_w == NULL || len < 0) return false;
  for (int i = 0; i < len; i++) {
    if (page[i] == kCmdLine2) {
      if (line2 >= 0) return false;
      line2 = i;
    } else if (page[i] == kCmdLine3) {
      if (line3 >= 0) return false;
      line3 = i;
    } else if (page[i] == kCmdChoose) {
      if (choose >= 0) return false;
      choose = i;
    }
  }
  if (line2 <= 0 || line3 <= line2 || choose <= line3 ||
      choose != len - 1)
    return false;

  int row0_len = line2;
  int row1_len = line3 - line2 - 1;
  int row2_len = choose - line3 - 1;
  int w = *io_w;
  if (w < 0 ||
      w + 1 + 3 + row0_len + row1_len + row2_len + 2 >
          kDialogueBufferCapacity)
    return false;
  dst[w++] = kCmdWaitKey;
  dst[w++] = kCmdScroll;
  memcpy(dst + w, page, (size_t)row0_len);
  w += row0_len;
  dst[w++] = kCmdScroll;
  memcpy(dst + w, page + line2 + 1, (size_t)row1_len);
  w += row1_len;
  dst[w++] = kCmdScroll;
  memcpy(dst + w, page + line3 + 1, (size_t)row2_len);
  w += row2_len;
  dst[w++] = kCmdChoose;
  dst[w++] = kCmdEnd;
  *io_w = w;
  return true;
}

// Prepend a wrapped item page, then roll in the already-decoded choice page.
// The choice text and live Choose command are retained byte-for-byte.
static bool prepend_placement_item_page(uint8 *buf, const char *fmt,
                                        uint16 loc) {
  uint16 item;
  uint8 tmp[kDialogueBufferCapacity];
  if (!lookup_unchecked_placement_item(loc, &item) ||
      !render_reward_item_page(tmp, fmt, item))
    return false;
  int head = dialogue_len(tmp);
  if (head < 0 || !append_scrolling_choice_page(tmp, &head, buf))
    return false;
  memcpy(buf, tmp, (size_t)head);
  return true;
}

// Prefer one coherent choice box when the exact full or compact item identity
// fits its top row. Longer names use a separate exact item page followed by a
// cleanly rolled-in price page. The live buffer is replaced only after the
// entire composition succeeds.
static bool write_placement_purchase_choice(uint8 *buf, uint16 loc,
                                            uint16 price,
                                            const char *intro_fmt) {
  uint16 item;
  char item_name[64], buy_line[24], price_line[24];
  uint8 tmp[kDialogueBufferCapacity], choice_page[kDialogueBufferCapacity];
  if (!lookup_unchecked_placement_item(loc, &item)) return false;
  int buy_len = snprintf(
      buy_line, sizeof(buy_line), "Buy for %u", (unsigned)price);
  int price_len = snprintf(
      price_line, sizeof(price_line), "%u Rupees", (unsigned)price);
  if (buy_len < 0 || (size_t)buy_len >= sizeof(buy_line) ||
      buy_len > kMaxSafeLineChars ||
      price_len < 0 || (size_t)price_len >= sizeof(price_line) ||
      price_len > kMaxSafeLineChars)
    return false;

  if ((format_preview_reward_item(item, item_name, sizeof(item_name)) &&
       strlen(item_name) <= kMaxSafeLineChars) ||
      (format_compact_reward_item(item, item_name, sizeof(item_name)) &&
       strlen(item_name) <= kMaxSafeLineChars)) {
    write_choice_message(tmp, item_name, buy_line, "No thanks");
    int len = dialogue_len(tmp);
    if (len < 0) return false;
    memcpy(buf, tmp, (size_t)len + 1);
    return true;
  }

  if (!render_reward_item_page(tmp, intro_fmt, item)) return false;
  int w = dialogue_len(tmp);
  if (w < 0) return false;
  write_choice_message(choice_page, price_line, "Buy", "No thanks");
  if (!append_scrolling_choice_page(tmp, &w, choice_page)) return false;
  memcpy(buf, tmp, (size_t)w);
  return true;
}

typedef struct RewardInventoryRow {
  char position;
  uint16 item;
  uint16 price;
  bool free_item;
} RewardInventoryRow;

static bool append_scrolling_text_page(uint8 *dst, int *io_w,
                                       const uint8 *page) {
  int len = dialogue_len(page);
  int line2 = -1, line3 = -1;
  if (dst == NULL || io_w == NULL || len < 0) return false;
  for (int i = 0; i < len; i++) {
    if (page[i] == kCmdLine2) {
      if (line2 >= 0) return false;
      line2 = i;
    } else if (page[i] == kCmdLine3) {
      if (line3 >= 0) return false;
      line3 = i;
    } else if (page[i] >= 0x67) {
      return false;
    }
  }
  if (line3 >= 0 && (line2 < 0 || line3 <= line2)) return false;

  int row0_end = line2 >= 0 ? line2 : len;
  int row1_start = line2 >= 0 ? line2 + 1 : len;
  int row1_end = line3 >= 0 ? line3 : len;
  int row2_start = line3 >= 0 ? line3 + 1 : len;
  int row0_len = row0_end;
  int row1_len = row1_end - row1_start;
  int row2_len = len - row2_start;
  int w = *io_w;
  if (w < 0 ||
      w + 1 + 3 + row0_len + row1_len + row2_len >
          kDialogueBufferCapacity - 1)
    return false;
  dst[w++] = kCmdWaitKey;
  dst[w++] = kCmdScroll;
  memcpy(dst + w, page, (size_t)row0_len);
  w += row0_len;
  dst[w++] = kCmdScroll;
  memcpy(dst + w, page + row1_start, (size_t)row1_len);
  w += row1_len;
  dst[w++] = kCmdScroll;
  memcpy(dst + w, page + row2_start, (size_t)row2_len);
  w += row2_len;
  *io_w = w;
  return true;
}

// Prepend a reward page to an already multi-page interactive stream. Stumpy's
// hint subsystem owns a one-to-three-row fact page followed by WaitKey and a
// separately rolled-in choice page. Preserve that complete tail: first roll
// the fact page in cleanly, then copy its existing Wait/choice flow verbatim.
static bool prepend_placement_item_page_before_stream(
    uint8 *buf, const char *fmt, uint16 loc) {
  uint16 item;
  uint8 tmp[kDialogueBufferCapacity], first_page[kDialogueBufferCapacity];
  if (!lookup_unchecked_placement_item(loc, &item) ||
      !render_reward_item_page(tmp, fmt, item))
    return false;

  int head = dialogue_len(tmp);
  int src_len = dialogue_len(buf);
  int tail = -1;
  if (head < 0 || src_len < 0) return false;
  for (int i = 0; i < src_len; i++) {
    if (buf[i] == kCmdWaitKey) {
      tail = i;
      break;
    }
  }
  if (tail <= 0) return false;

  int p = tail;
  if (buf[p++] != kCmdWaitKey) return false;
  for (int row = 0; row < 3; row++) {
    if (p >= src_len || buf[p++] != kCmdScroll) return false;
    while (p < src_len && buf[p] < 0x67) p++;
  }
  if (p != src_len - 1 || buf[p] != kCmdChoose) return false;

  memcpy(first_page, buf, (size_t)tail);
  first_page[tail] = kCmdEnd;
  if (!append_scrolling_text_page(tmp, &head, first_page))
    return false;

  int tail_len = src_len - tail + 1;
  if (head + tail_len > kDialogueBufferCapacity) return false;
  memcpy(tmp + head, buf + tail, (size_t)tail_len);
  head += tail_len;
  memcpy(buf, tmp, (size_t)head);
  return true;
}

// One source per page keeps even the longest qualified item name lossless.
// This is intentionally a pure formatter so the selfcheck can exhaust the
// pagination/control-code contract without installing a live shopsanity slot.
static bool write_reward_inventory(uint8 *buf,
                                   const RewardInventoryRow *rows,
                                   int count) {
  if (buf == NULL || rows == NULL || count <= 0) return false;
  uint8 tmp[kDialogueBufferCapacity];
  int w = 0;
  for (int i = 0; i < count; i++) {
    char item[64], text[96];
    uint8 page[kDialogueBufferCapacity];
    int n = -1;
    bool complete = false;
    if (format_preview_reward_item(rows[i].item, item, sizeof(item))) {
      n = rows[i].free_item
          ? snprintf(text, sizeof(text), "%c %s free", rows[i].position, item)
          : snprintf(text, sizeof(text), "%c %s costs %u",
                     rows[i].position, item, (unsigned)rows[i].price);
      complete =
          n >= 0 && (size_t)n < sizeof(text) &&
          write_wrapped_message_complete(page, text);
      if (!complete) {
        n = rows[i].free_item
            ? snprintf(text, sizeof(text), "%c %s", rows[i].position, item)
            : snprintf(text, sizeof(text), "%c %s %u",
                       rows[i].position, item, (unsigned)rows[i].price);
        complete = n >= 0 && (size_t)n < sizeof(text) &&
                   write_wrapped_message_complete(page, text);
      }
    }
    if (!complete &&
        format_compact_reward_item(rows[i].item, item, sizeof(item))) {
      n = rows[i].free_item
          ? snprintf(text, sizeof(text), "%c %s", rows[i].position, item)
          : snprintf(text, sizeof(text), "%c %s %u",
                     rows[i].position, item, (unsigned)rows[i].price);
      complete = n >= 0 && (size_t)n < sizeof(text) &&
                 write_wrapped_message_complete(page, text);
    }
    if (!complete) return false;
    int page_len = dialogue_len(page);
    if (page_len < 0) return false;
    if (i == 0) {
      if (w + page_len + 1 > kDialogueBufferCapacity) return false;
      memcpy(tmp + w, page, (size_t)page_len);
      w += page_len;
    } else if (!append_scrolling_text_page(tmp, &w, page)) {
      return false;
    }
  }
  tmp[w++] = kCmdEnd;
  memcpy(buf, tmp, (size_t)w);
  return true;
}

static bool write_context_reward_inventory(
    uint8 *buf, bool takeany_context,
    const RewardInventoryRow *takeany_rows, int takeany_count,
    const RewardInventoryRow *shop_rows, int shop_count) {
  return takeany_context
      ? write_reward_inventory(buf, takeany_rows, takeany_count)
      : write_reward_inventory(buf, shop_rows, shop_count);
}

static bool write_live_shop_inventory(uint8 *buf) {
  RewardInventoryRow takeany_rows[2], shop_rows[3];
  int takeany_count = 0, shop_count = 0;
  const bool takeany_context = g_rando_takeany_door_id != 0;

  if (takeany_context) {
    for (uint8 pos = 0; pos < 2; pos++) {
      uint16 loc = Rando_TakeAnyLiveSlot(
          BYTE(dungeon_room_index), g_rando_takeany_door_id, pos);
      uint16 item;
      if (loc == 0xFFFFu || !Placement_TryLookup(loc, &item) ||
          item >= ITEM__COUNT)
        continue;
      takeany_rows[takeany_count++] = (RewardInventoryRow){
          (char)("LR"[pos]), item, 0, true};
    }
  }

  for (uint8 pos = 0; pos < 3; pos++) {
    uint16 item, price;
    // The shop key is the FULL 16-bit room plus the CAPTURED overworld door
    // (ALTTPR's PreviousOverworldDoor), which is what every other call site
    // passes. Neither substitute works: the low room byte aliases ordinary
    // rooms onto interior rooms (0x0F never matches interior 0x10F), and
    // which_entrance cannot separate shops at all — four Dark World shops
    // share id 0x60 and two share 0x58. The Take-Any lookup above genuinely
    // takes a uint8 room, so its BYTE() is correct; this one is not.
    if (!Rando_ShopSlotCheckInfo(dungeon_room_index,
                                 g_ram[kRam_RandoOverworldDoor],
                                 (uint8)(pos + 1), NULL, &item, &price) ||
        item >= ITEM__COUNT)
      continue;
    shop_rows[shop_count++] = (RewardInventoryRow){
        (char)("LMR"[pos]), item, price, false};
  }

  // Take-Any redirects deliberately reuse three regular shopsanity host
  // rooms. The captured source door is the authoritative discriminator: its
  // free irreversible choices replace (and suppress) any live host-shop rows.
  return write_context_reward_inventory(
      buf, takeany_context, takeany_rows, takeany_count,
      shop_rows, shop_count);
}

static void rewrite_uncle(uint8 *buf) {
  char item[48], repl[64];
  format_reward_item(lookup_reward(LOC_Link_s_Uncle, ITEM_L1Sword), item, sizeof(item));
  snprintf(repl, sizeof(repl), "the %s", item);
  replace_ascii_run(buf, "my sword and shield", repl);
}

static void rewrite_zora_reward(uint8 *buf) {
  int tail = find_ascii_run(buf, "I will let you use the magic");
  if (tail < 0) return;
  int len = dialogue_len(buf);
  char item[48], text[80];
  format_reward_item(lookup_reward(LOC_King_Zora, ITEM_Flippers), item, sizeof(item));
  snprintf(text, sizeof(text), "%s is yours!", item);
  uint8 tmp[kDialogueBufferCapacity];
  write_wrapped_message(tmp, text);
  int head = dialogue_len(tmp);
  int tail_len = len - tail + 1;
  if (head < 0 || head + 2 + tail_len > kDialogueBufferCapacity) return;
  tmp[head++] = kCmdWaitKey;
  tmp[head++] = kCmdScroll;
  memcpy(tmp + head, buf + tail, (size_t)tail_len);
  memcpy(buf, tmp, (size_t)(head + tail_len));
}

static void write_ganon_crystal_warning(uint8 *buf, unsigned required) {
  char text[64];
  snprintf(text, sizeof(text), "You need %u crystal%s to harm me.",
           required, required == 1 ? "" : "s");
  write_wrapped_message(buf, text);
}

static void rewrite_ganon_crystal_warning(uint8 *buf) {
  // add-rando-random-crystals: reads the RESOLVED count — this dialogue is
  // the in-world reveal of a rolled ganon requirement.
  const RandoSettings *settings = Rando_GetActiveSettings();
  uint8 required = Rando_EffectiveCrystalsGanon();
  if(settings==NULL || Rando_GoalAllowsGanonDamage()) return;
  char lines[3][32];
  const char *first=NULL,*second=NULL,*third=NULL;
  switch(settings->goal) {
  case kGoal_Pedestal:
    first="3 pendants.";second="Forest altar.";third="Pull to win.";break;
  case kGoal_TriforceHunt:
    snprintf(lines[0],sizeof(lines[0]),"Pieces: %u",settings->pieces_required);
    first=lines[0];second="Murahdahla:";third="Castle court.";break;
  case kGoal_GanonHunt:
    snprintf(lines[0],sizeof(lines[0]),"Pieces: %u",settings->pieces_required);
    snprintf(lines[1],sizeof(lines[1]),"You have %u.",Rando_CountCollectedTriforcePieces());
    first=lines[0];second=lines[1];third="Then face me.";break;
  case kGoal_Dungeons:
    first="3 pendants.";second="7 crystals.";third="Both wizards.";break;
  case kGoal_Completionist:
    first="All items.";second="Both wizards.";third="Then face me.";break;
  case kGoal_Ganon:
    if(!Rando_IsLocationChecked(LOC_Agahnim_2)) {
      snprintf(lines[1],sizeof(lines[1]),"Crystals: %u",required);
      first="Beat Agahnim 2";second=lines[1];third="Then face me.";break;
    }
    // fall through
  default:
    if(required && !Rando_HasRequiredGanonCrystals()) write_ganon_crystal_warning(buf,required);
    return;
  }
  // The shared wrapped writer deliberately stops after 3 x 13 glyphs.
  // These complete, explicit lines fit that contract without losing the rule.
  int w=write_ascii(buf,0,first);buf[w++]=kCmdLine2;
  w=write_ascii(buf,w,second);buf[w++]=kCmdLine3;
  w=write_ascii(buf,w,third);buf[w]=kCmdEnd;
}

static uint16 fairy_next_location(void) {
  if (!savegame_is_darkworld) {
    if (!Rando_IsLocationChecked(LOC_Waterfall_Fairy_Left)) return LOC_Waterfall_Fairy_Left;
    if (!Rando_IsLocationChecked(LOC_Waterfall_Fairy_Right)) return LOC_Waterfall_Fairy_Right;
  } else {
    if (!Rando_IsLocationChecked(LOC_Pyramid_Fairy_Left)) return LOC_Pyramid_Fairy_Left;
    if (!Rando_IsLocationChecked(LOC_Pyramid_Fairy_Right)) return LOC_Pyramid_Fairy_Right;
  }
  return 0xFFFFu;
}

static void rewrite_reward_dialogue_with_settings(
    uint16 msg_id, uint8 *buf, const RandoSettings *settings) {
  const bool preview =
      settings != NULL && settings->hint_npc_reward_reveal != 0;
  switch (msg_id) {
  case 0x038:
  case 0x039:
    if (preview)
      (void)write_placement_item_message(
          buf, "My reward is %s.", LOC_Sahasrahla);
    return;
  case 0x04B:
    if (preview)
      (void)write_placement_item_message(
          buf, "I am making %s.", LOC_Potion_Shop);
    return;
  case 0x04C:
    if (preview)
      (void)write_placement_item_message(
          buf, "Trade the Mushroom for %s.", LOC_Potion_Shop);
    return;
  case 0x00E: rewrite_uncle(buf); return;
  case 0x0CD: write_item_message(buf, "You won %s!", LOC_Maze_Race, ITEM_PieceOfHeart); return;
  case 0x09C:
    if (preview)
      (void)write_placement_item_message(
          buf, "Guide me home for %s.", LOC_Old_Man);
    return;
  case 0x0D1:
    write_choice_message(buf, "Mystery item", "Buy for 100", "No thanks");
    if (preview)
      (void)write_placement_purchase_choice(
          buf, LOC_Bottle_Merchant, 100, "I sell %s.");
    return;
  case 0x0D2: write_item_message(buf, "You bought %s!", LOC_Bottle_Merchant, ITEM_BottleEmpty); return;
  case 0x0D4: write_wrapped_message(buf, "I am sold out of special items."); return;
  case 0x0D7:
    if (preview)
      (void)write_placement_item_message(
          buf, "Please take %s.", LOC_Hobo);
    return;
  case 0x0D8:
    write_choice_message(buf, "Want our prize?", "Yes", "No thanks");
    if (preview)
      (void)prepend_placement_item_page(
          buf, "We can make %s.", LOC_Blacksmith);
    return;
  case 0x0D9: write_choice_message(buf, "Only 10 Rupees", "Pay", "Wait"); return;
  case 0x0DA: write_choice_message(buf, "Are you sure?", "Yes", "No"); return;
  case 0x0DB: write_wrapped_message(buf, "Your prize is already made."); return;
  case 0x0DE: write_item_message(buf, "%s is ready!", LOC_Blacksmith, ITEM_L3Sword); return;
  case 0x0DF: write_wrapped_message(buf, "Find my partner and we can make a special item."); return;
  case 0x0E0: write_wrapped_message(buf, "Thanks! Return and we will make a special item."); return;
  case 0x0E5:
    if (preview)
      (void)prepend_placement_item_page_before_stream(
          buf, "I will lend %s.", LOC_Stumpy);
    return;
  case 0x0E6: write_item_message(buf, "I lend you %s.", LOC_Stumpy, ITEM_Shovel); return;
  case 0x105: write_item_message(buf, "Please take %s.", LOC_Sick_Kid, ITEM_BugCatchingNet); return;
  case 0x109:
    if (preview)
      (void)prepend_placement_item_page(
          buf, "I will give %s.", LOC_Purple_Chest);
    return;
  case 0x10E: write_item_message(buf, "You receive %s.", LOC_Ether_Tablet, ITEM_Ether); return;
  case 0x10F: write_item_message(buf, "You receive %s.", LOC_Bombos_Tablet, ITEM_Bombos); return;
  case 0x110:
    if (preview)
      (void)write_placement_item_message(
          buf, "I change your fate to %s.", LOC_Magic_Bat);
    return;
  case 0x111: write_item_message(buf, "You got %s!", LOC_Magic_Bat, ITEM_HalfMagic); return;
  case 0x12A:
    if (preview)
      (void)write_placement_item_message(
          buf, "I will toss you %s.", LOC_Catfish);
    return;
  case 0x142:
    write_choice_message(buf, "Want my item?", "Ask price", "Just visiting");
    if (preview)
      (void)prepend_placement_item_page(
          buf, "I sell %s.", LOC_King_Zora);
    return;
  case 0x143:
    write_choice_message(buf, "It costs 500", "Pay", "Quit");
    return;
  case 0x144: rewrite_zora_reward(buf); return;
  case 0x16F: rewrite_ganon_crystal_warning(buf); return;
  case 0x176: {
    // 0x176 is shared with the Bottle Merchant's outdoor fish animation. Room
    // alone is unsafe because dungeon_room_index can remain stale outdoors.
    if (!player_is_indoors) return;
    uint16 loc = Rando_GiftThiefLocationForRoom(dungeon_room_index);
    if (loc == 0xFFFFu) return;
    write_item_message(buf, "Take %s.", loc,
                       loc == LOC_Mini_Moldorm_Cave_NPC ? ITEM_PieceOfHeart : ITEM_Rupee100);
    return;
  }
  case 0x0B7:
    replace_ascii_run(buf, "The Master Sword", "The forest treasure");
    return;
  case 0x14A:
    write_choice_message(buf, "Fairy gift?", "Accept", "Decline");
    if (preview) {
      uint16 loc = fairy_next_location();
      if (loc != 0xFFFFu)
        (void)prepend_placement_item_page(buf, "Next gift is %s.", loc);
    }
    return;
  case 0x14B:
    write_wrapped_message(buf, fairy_next_location() == 0xFFFFu
                                   ? "No gifts remain."
                                   : "Maybe next time.");
    return;
  case 0x15F:
  case 0x165:
    if (preview) (void)write_live_shop_inventory(buf);
    return;
  case 0x160:
    if (preview)
      (void)prepend_placement_item_page(
          buf, "The prize is %s.", LOC_Chest_Game);
    return;
  case 0x187:
    if (preview)
      (void)prepend_placement_item_page(
          buf, "The prize is %s.", LOC_Digging_Game);
    return;
  default:
    return;
  }
}

void Rando_RewriteRewardDialogue(uint16 msg_id, uint8 *buf) {
  // The runtime buffer preserves the selected language's command grammar and
  // font alphabet. This renderer currently emits US commands/glyph indices;
  // bit 0 selects the incompatible EU/new command grammar and bit 1 marks the
  // translated original-format languages with different glyph tables. Leave
  // every non-US buffer untouched until it has a complete locale encoder.
  if (buf == NULL || !Rando_IsActive() || g_zenv.dialogue_flags != 0) return;
  rewrite_reward_dialogue_with_settings(
      msg_id, buf, Rando_GetActiveSettings());
}

static bool buffer_contains_ascii(const uint8 *buf, const char *text) {
  return find_ascii_run(buf, text) >= 0;
}

static void selfcheck_fail(const char *what) {
  fprintf(stderr, "RandoDialogue_SelfCheck: %s\n", what);
  abort();
}

static void selfcheck_three_scroll_page_clear(void) {
  static uint8 saved_pixels[0x7E0];
  uint8 saved_scroll_speed = dialogue_scroll_speed;
  uint8 saved_scroll_phase = byte_7E1CDF;
  uint16 saved_curline = vwf_curline;
  uint16 saved_next_line = vwf_flag_next_line;
  memcpy(saved_pixels, messaging_buf, sizeof(saved_pixels));
  memset(messaging_buf, 0xA5, sizeof(saved_pixels));
  dialogue_scroll_speed = 15;
  byte_7E1CDF = 0;
  bool complete = true;
  for (int row = 0; row < 3; row++)
    complete &= RenderText_Draw_Scroll();
  bool clear = complete;
  for (size_t i = 0; i < sizeof(saved_pixels); i++)
    clear &= ((uint8 *)messaging_buf)[i] == 0;
  memcpy(messaging_buf, saved_pixels, sizeof(saved_pixels));
  dialogue_scroll_speed = saved_scroll_speed;
  byte_7E1CDF = saved_scroll_phase;
  vwf_curline = saved_curline;
  vwf_flag_next_line = saved_next_line;
  if (!clear)
    selfcheck_fail("three Scroll commands did not clear the text page");
}

void RandoDialogue_SelfCheck(void) {
  static RandoPlacement entries[] = {
    {LOC_Link_s_Uncle, ITEM_Hookshot},
    {LOC_Sahasrahla, ITEM_FireRod},
    {LOC_Potion_Shop, ITEM_IceRod},
    {LOC_Bottle_Merchant, ITEM_Bombs1},
    {LOC_Magic_Bat, ITEM_Hammer},
    {LOC_Hobo, ITEM_BookOfMudora},
    {LOC_Old_Man, ITEM_MoonPearl},
    {LOC_Catfish, ITEM_Bow},
    {LOC_Sick_Kid, ITEM_BookOfMudora},
    {LOC_Bombos_Tablet, ITEM_IceRod},
    {LOC_Mini_Moldorm_Cave_NPC, ITEM_PieceOfHeart},
    {LOC_Maze_Race, ITEM_BugCatchingNet},
    {LOC_Ether_Tablet, ITEM_FireRod},
    {LOC_Chest_Game, ITEM_SmallKey_HyruleCastleEscape},
    {LOC_Blacksmith, ITEM_L3Sword},
    {LOC_Purple_Chest, ITEM_MagicMirror},
    {LOC_Stumpy, ITEM_Shovel},
    {LOC_Digging_Game, ITEM_Soul_MiniMoldorm},
    {LOC_Hype_Cave_NPC, ITEM_Rupee100},
    {LOC_King_Zora, ITEM_Hookshot},
    {LOC_Waterfall_Fairy_Left, ITEM_BottleWithGreenPotion},
    {LOC_Waterfall_Fairy_Right, ITEM_BlueBoomerang},
    {LOC_Pyramid_Fairy_Left, ITEM_RedMail},
    {LOC_Pyramid_Fairy_Right, ITEM_SilverArrowUpgrade},
    {LOC_Dark_Desert_Fairy_0, ITEM_Hookshot},
    {LOC_Dark_Desert_Fairy_1, ITEM_FireRod},
  };
  RandoPlacementTable table = {entries, (uint16)(sizeof(entries) / sizeof(entries[0]))};
  const RandoPlacementTable *saved_table = Placement_GetActive();
  uint8 saved_active = g_rando_slot_active;
  uint8 saved_indoors = player_is_indoors;
  uint16 saved_room = dungeon_room_index;
  uint8 saved_entrance = which_entrance;
  uint8 saved_takeany_door = g_rando_takeany_door_id;
  uint8 saved_dialogue_flags = g_zenv.dialogue_flags;
  uint8 saved_checked[kRandoCheckedBitmapBytes];
  memcpy(saved_checked, g_rando_checked_bitmap, sizeof(saved_checked));
  memset(g_rando_checked_bitmap, 0, sizeof(saved_checked));
  Placement_Install(&table);
  g_rando_slot_active = 1;
  g_zenv.dialogue_flags = 0;  // exercise the supported US grammar
  selfcheck_three_scroll_page_clear();

  uint8 buf[kDialogueBufferCapacity];
  memset(buf, kCmdEnd, sizeof(buf));
  Rando_RewriteRewardDialogue(0x0CD, buf);
  if (!buffer_contains_ascii(buf, "Bug") || !buffer_contains_ascii(buf, "Net"))
    selfcheck_fail("Maze Race item name");

  memset(buf, kCmdEnd, sizeof(buf));
  int w = write_ascii(buf, 0, "Take my sword and shield and listen");
  buf[w] = kCmdEnd;
  Rando_RewriteRewardDialogue(0x00E, buf);
  if (!buffer_contains_ascii(buf, "Hookshot") || buffer_contains_ascii(buf, "sword and shield"))
    selfcheck_fail("Uncle inline item rewrite");

  memset(buf, kCmdEnd, sizeof(buf));
  w = write_ascii(buf, 0, "old flippers prelude");
  buf[w++] = kCmdWaitKey; buf[w++] = kCmdScroll;
  w = write_ascii(buf, w, "I will let you use the magic water ways");
  buf[w] = kCmdEnd;
  Rando_RewriteRewardDialogue(0x144, buf);
  if (!buffer_contains_ascii(buf, "Hookshot") ||
      !buffer_contains_ascii(buf, "I will let you use the magic"))
    selfcheck_fail("King Zora item/tutorial rewrite");

  player_is_indoors = 1;
  dungeon_room_index = 0x11E;
  memset(buf, kCmdEnd, sizeof(buf));
  Rando_RewriteRewardDialogue(0x176, buf);
  if (!buffer_contains_ascii(buf, "100") || !buffer_contains_ascii(buf, "Rupees"))
    selfcheck_fail("Hype Cave gift mapping");
  dungeon_room_index = 0x123;
  memset(buf, kCmdEnd, sizeof(buf));
  Rando_RewriteRewardDialogue(0x176, buf);
  if (!buffer_contains_ascii(buf, "Heart") || !buffer_contains_ascii(buf, "Piece"))
    selfcheck_fail("MMC gift mapping");
  dungeon_room_index = 0x127;
  memset(buf, 0x55, sizeof(buf)); buf[0] = kCmdEnd;
  Rando_RewriteRewardDialogue(0x176, buf);
  if (buf[0] != kCmdEnd) selfcheck_fail("unmapped gift room must fall through");
  player_is_indoors = 0;
  dungeon_room_index = 0x11E;
  memset(buf, 0x55, sizeof(buf)); buf[0] = kCmdEnd;
  Rando_RewriteRewardDialogue(0x176, buf);
  if (buf[0] != kCmdEnd) selfcheck_fail("outdoor fish message must fall through");

  memset(buf, kCmdEnd, sizeof(buf));
  Rando_RewriteRewardDialogue(0x0D1, buf);
  bool has_choose = false;
  for (int i = 0; i < 80 && buf[i] != kCmdEnd; i++)
    if (buf[i] == kCmdChoose) has_choose = true;
  if (!has_choose ||
      !buffer_contains_ascii(buf, "    > Buy for 100") ||
      !buffer_contains_ascii(buf, "       No thanks"))
    selfcheck_fail("generic choice cursor layout");

  // A short exact item belongs in one coherent item/price/choice box. This
  // reproduces the playtest placement that exposed the old contradictory
  // "I sell 1 Bomb" / "Mystery item" pages.
  RandoSettings preview_settings;
  Settings_SetDefaults(&preview_settings);
  preview_settings.hint_npc_reward_reveal = 1;
  memset(buf, kCmdEnd, sizeof(buf));
  rewrite_reward_dialogue_with_settings(0x0D1, buf, &preview_settings);
  if (!buffer_contains_ascii(buf, "1 Bomb") ||
      !buffer_contains_ascii(buf, "    > Buy for 100") ||
      !buffer_contains_ascii(buf, "       No thanks") ||
      buffer_contains_ascii(buf, "Mystery"))
    selfcheck_fail("Bottle Merchant reward preview");
  has_choose = false;
  int bottle_waits = 0, bottle_scrolls = 0;
  for (int i = 0; i < 120 && buf[i] != kCmdEnd; i++) {
    if (buf[i] == kCmdChoose) has_choose = true;
    bottle_waits += buf[i] == kCmdWaitKey;
    bottle_scrolls += buf[i] == kCmdScroll;
  }
  if (!has_choose || bottle_waits != 0 || bottle_scrolls != 0)
    selfcheck_fail("short Bottle preview must be one clean choice box");

  // The next playtest placed a Bottle here and moved the selection. Messages
  // 1/2 redraw only the canonical cursor prefix, so both labels must start at
  // x=28 or the overlay erases their leading glyphs.
  entries[3].item_id = ITEM_BottleEmpty;
  memset(buf, kCmdEnd, sizeof(buf));
  rewrite_reward_dialogue_with_settings(0x0D1, buf, &preview_settings);
  if (!buffer_contains_ascii(buf, "Bottle") ||
      !buffer_contains_ascii(buf, "    > Buy for 100") ||
      !buffer_contains_ascii(buf, "       No thanks") ||
      buffer_contains_ascii(buf, "Mystery"))
    selfcheck_fail("Bottle Merchant choice cursor contract");
  has_choose = false;
  bottle_waits = bottle_scrolls = 0;
  for (int i = 0; i < 120 && buf[i] != kCmdEnd; i++) {
    if (buf[i] == kCmdChoose) has_choose = true;
    bottle_waits += buf[i] == kCmdWaitKey;
    bottle_scrolls += buf[i] == kCmdScroll;
  }
  if (!has_choose || bottle_waits != 0 || bottle_scrolls != 0)
    selfcheck_fail("Bottle item must stay in one cursor-safe choice box");

  // A long exact identity needs two pages. The second page is rolled in with
  // three row Scrolls; a one-Scroll/Line1 jump is the F12-proven XOR glitch.
  entries[3].item_id = ITEM_BossHeartContainer;
  memset(buf, kCmdEnd, sizeof(buf));
  rewrite_reward_dialogue_with_settings(0x0D1, buf, &preview_settings);
  if (!buffer_contains_ascii(buf, "Boss") ||
      !buffer_contains_ascii(buf, "Heart") ||
      !buffer_contains_ascii(buf, "Container") ||
      !buffer_contains_ascii(buf, "100 Rupees") ||
      !buffer_contains_ascii(buf, "    > Buy") ||
      !buffer_contains_ascii(buf, "       No thanks") ||
      buffer_contains_ascii(buf, "Mystery"))
    selfcheck_fail("long Bottle Merchant reward preview");
  has_choose = false;
  bottle_waits = bottle_scrolls = 0;
  bool has_unsafe_top_jump = false;
  for (int i = 0; i < 180 && buf[i] != kCmdEnd; i++) {
    if (buf[i] == kCmdChoose) has_choose = true;
    bottle_waits += buf[i] == kCmdWaitKey;
    bottle_scrolls += buf[i] == kCmdScroll;
    has_unsafe_top_jump |= buf[i] == kCmdLine1;
  }
  if (!has_choose || bottle_waits != 1 || bottle_scrolls != 3 ||
      has_unsafe_top_jump)
    selfcheck_fail("long Bottle preview did not clear all three text rows");
  entries[3].item_id = ITEM_Bombs1;

  RandoSettings no_preview = preview_settings;
  no_preview.hint_npc_reward_reveal = 0;
  memset(buf, kCmdEnd, sizeof(buf));
  rewrite_reward_dialogue_with_settings(0x0D1, buf, &no_preview);
  if (buffer_contains_ascii(buf, "1 Bomb") ||
      !buffer_contains_ascii(buf, "Mystery item"))
    selfcheck_fail("disabled reward preview must stay generic");

  // Checked sources never keep advertising their original seed reward.
  g_rando_checked_bitmap[LOC_Bottle_Merchant >> 3] |=
      (uint8)(1u << (LOC_Bottle_Merchant & 7));
  memset(buf, kCmdEnd, sizeof(buf));
  rewrite_reward_dialogue_with_settings(0x0D1, buf, &preview_settings);
  if (buffer_contains_ascii(buf, "1 Bomb") ||
      !buffer_contains_ascii(buf, "Mystery item"))
    selfcheck_fail("checked reward preview must stay generic");
  g_rando_checked_bitmap[LOC_Bottle_Merchant >> 3] &=
      (uint8)~(1u << (LOC_Bottle_Merchant & 7));

  static const struct {
    uint16 msg;
    const char *item_word;
    const char *price_word;
  } kPaidPreviews[] = {
    {0x142, "Hookshot", "Ask price"},
    {0x0D8, "Tempered", "Want our prize"},
    {0x160, "Escape", "Pay 30"},
    {0x187, "Moldorm", "Pay 80"},
  };
  for (size_t i = 0;
       i < sizeof(kPaidPreviews) / sizeof(kPaidPreviews[0]); i++) {
    memset(buf, kCmdEnd, sizeof(buf));
    if (kPaidPreviews[i].msg == 0x160)
      write_choice_message(buf, "Chest game", "Pay 30", "No thanks");
    else if (kPaidPreviews[i].msg == 0x187)
      write_choice_message(buf, "Digging game", "Pay 80", "No thanks");
    rewrite_reward_dialogue_with_settings(
        kPaidPreviews[i].msg, buf, &preview_settings);
    if (!buffer_contains_ascii(buf, kPaidPreviews[i].item_word) ||
        !buffer_contains_ascii(buf, kPaidPreviews[i].price_word)) {
      fprintf(stderr,
              "RandoDialogue_SelfCheck: paid preview msg 0x%03x "
              "missing '%s' or '%s'\n",
              kPaidPreviews[i].msg, kPaidPreviews[i].item_word,
              kPaidPreviews[i].price_word);
      selfcheck_fail("paid NPC/game reward preview");
    }
    has_choose = false;
    for (int j = 0; j < 180 && buf[j] != kCmdEnd; j++)
      if (buf[j] == kCmdChoose) has_choose = true;
    if (!has_choose) selfcheck_fail("paid preview lost choice command");
  }

  // King Zora names the placement in the first offer (0x142). The follow-up
  // is only the final price confirmation; repeating the same item there makes
  // the staged conversation read as two separate offers.
  memset(buf, kCmdEnd, sizeof(buf));
  rewrite_reward_dialogue_with_settings(0x143, buf, &preview_settings);
  if (buffer_contains_ascii(buf, "Hookshot") ||
      buffer_contains_ascii(buf, "I sell") ||
      !buffer_contains_ascii(buf, "It costs 500") ||
      !buffer_contains_ascii(buf, "    > Pay") ||
      !buffer_contains_ascii(buf, "       Quit"))
    selfcheck_fail("King Zora price confirmation must not repeat item");
  int zora_waits = 0, zora_scrolls = 0, zora_choices = 0;
  for (int i = 0; i < 120 && buf[i] != kCmdEnd; i++) {
    zora_waits += buf[i] == kCmdWaitKey;
    zora_scrolls += buf[i] == kCmdScroll;
    zora_choices += buf[i] == kCmdChoose;
  }
  if (zora_waits != 0 || zora_scrolls != 0 || zora_choices != 1)
    selfcheck_fail("King Zora price confirmation must be one clean choice box");

  // Use the real two-page shape emitted by append_interactive_choice: a
  // commonly two-line Flute fact, then Wait plus three rolled-in choice rows.
  // The old synthetic three-row fixture hid a failure to prepend the reward
  // whenever that first fact page had no Line3 command.
  memset(buf, kCmdEnd, sizeof(buf));
  w = write_ascii(buf, 0, "Flute is at");
  buf[w++] = kCmdLine2;
  w = write_ascii(buf, w, "Desert Ledge");
  buf[w++] = kCmdWaitKey;
  buf[w++] = kCmdScroll;
  w = write_ascii(buf, w, "Can you help?");
  buf[w++] = kCmdScroll;
  w = write_ascii(buf, w, "    > Yes");
  buf[w++] = kCmdScroll;
  w = write_ascii(buf, w, "       No way");
  buf[w++] = kCmdChoose;
  buf[w] = kCmdEnd;
  rewrite_reward_dialogue_with_settings(0x0E5, buf, &preview_settings);
  if (!buffer_contains_ascii(buf, "Shovel") ||
      !buffer_contains_ascii(buf, "Flute is at") ||
      !buffer_contains_ascii(buf, "Desert Ledge") ||
      !buffer_contains_ascii(buf, "    > Yes") ||
      !buffer_contains_ascii(buf, "       No way"))
    selfcheck_fail("Stumpy reward preview must preserve Flute hint");
  has_choose = false;
  int stumpy_waits = 0, stumpy_scrolls = 0;
  for (int i = 0; i < 240 && buf[i] != kCmdEnd; i++) {
    if (buf[i] == kCmdChoose) has_choose = true;
    stumpy_waits += buf[i] == kCmdWaitKey;
    stumpy_scrolls += buf[i] == kCmdScroll;
  }
  if (!has_choose || stumpy_waits != 2 || stumpy_scrolls != 6)
    selfcheck_fail("Stumpy reward preview lost page/choice flow");

  // Newly gated prompts must be a byte-for-byte no-op while disabled and on
  // every failed placement/checked gate. Their decoded vanilla choice is the
  // transaction owner; the preview only prepends a successfully composed page.
  uint8 before[kDialogueBufferCapacity];
  memset(buf, 0x55, sizeof(buf));
  write_choice_message(buf, "Chest game", "Pay 30", "No thanks");
  memcpy(before, buf, sizeof(before));
  rewrite_reward_dialogue_with_settings(0x160, buf, &no_preview);
  if (memcmp(buf, before, sizeof(before)) != 0)
    selfcheck_fail("disabled Chest Game preview changed decoded dialogue");

  g_rando_checked_bitmap[LOC_Purple_Chest >> 3] |=
      (uint8)(1u << (LOC_Purple_Chest & 7));
  memset(buf, 0x55, sizeof(buf));
  write_choice_message(buf, "Keep my secret?", "Yes", "No");
  memcpy(before, buf, sizeof(before));
  rewrite_reward_dialogue_with_settings(0x109, buf, &preview_settings);
  if (memcmp(buf, before, sizeof(before)) != 0)
    selfcheck_fail("checked Purple Chest preview changed decoded dialogue");
  g_rando_checked_bitmap[LOC_Purple_Chest >> 3] &=
      (uint8)~(1u << (LOC_Purple_Chest & 7));

  RandoPlacement missing_entries[
      sizeof(entries) / sizeof(entries[0])];
  uint16 missing_count = 0;
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
    if (entries[i].location_id != LOC_Digging_Game)
      missing_entries[missing_count++] = entries[i];
  RandoPlacementTable missing_table = {missing_entries, missing_count};
  Placement_Install(&missing_table);
  memset(buf, 0x55, sizeof(buf));
  write_choice_message(buf, "Digging game", "Pay 80", "No thanks");
  memcpy(before, buf, sizeof(before));
  rewrite_reward_dialogue_with_settings(0x187, buf, &preview_settings);
  if (memcmp(buf, before, sizeof(before)) != 0)
    selfcheck_fail("missing Digging Game preview changed decoded dialogue");
  Placement_Install(&table);

  // Shop clerks can enumerate every unchecked column without adding a new
  // purchase state machine. Each row is independently paged and exact.
  static const RewardInventoryRow kInventoryRows[] = {
    {'L', ITEM_BottleWithGreenPotion, 250, false},
    {'M', ITEM_SmallKey_HyruleCastleEscape, 10, false},
    {'R', ITEM_Soul_MiniMoldorm, 0, true},
  };
  memset(buf, kCmdEnd, sizeof(buf));
  if (!write_reward_inventory(
          buf, kInventoryRows,
          (int)(sizeof(kInventoryRows) / sizeof(kInventoryRows[0]))) ||
      !buffer_contains_ascii(buf, "Green Potion") ||
      !buffer_contains_ascii(buf, "250") ||
      !buffer_contains_ascii(buf, "Small") ||
      !buffer_contains_ascii(buf, "Key") ||
      !buffer_contains_ascii(buf, "Moldorm") ||
      !buffer_contains_ascii(buf, "free"))
    selfcheck_fail("shop reward inventory pages");
  int waits = 0, scrolls = 0, tops = 0;
  for (int i = 0; i < 240 && buf[i] != kCmdEnd; i++) {
    waits += buf[i] == kCmdWaitKey;
    scrolls += buf[i] == kCmdScroll;
    tops += buf[i] == kCmdLine1;
  }
  if (waits != 2 || scrolls != 6 || tops != 0)
    selfcheck_fail("shop reward inventory pagination");

  // Collision canary: active Take-Any offers must win even when all three
  // suppressed host-shop rows are simultaneously resolvable.
  static const RewardInventoryRow kTakeAnyCollisionRows[] = {
    {'L', ITEM_Hookshot, 0, true},
    {'R', ITEM_FireRod, 0, true},
  };
  static const RewardInventoryRow kHostShopCollisionRows[] = {
    {'L', ITEM_MoonPearl, 111, false},
    {'M', ITEM_Hammer, 222, false},
    {'R', ITEM_Boots, 333, false},
  };
  memset(buf, kCmdEnd, sizeof(buf));
  if (!write_context_reward_inventory(
          buf, true,
          kTakeAnyCollisionRows,
          (int)(sizeof(kTakeAnyCollisionRows) /
                sizeof(kTakeAnyCollisionRows[0])),
          kHostShopCollisionRows,
          (int)(sizeof(kHostShopCollisionRows) /
                sizeof(kHostShopCollisionRows[0]))) ||
      !buffer_contains_ascii(buf, "Hookshot") ||
      !buffer_contains_ascii(buf, "Fire Rod") ||
      buffer_contains_ascii(buf, "Moon Pearl") ||
      buffer_contains_ascii(buf, "111"))
    selfcheck_fail("Take-Any did not suppress host-shop preview rows");

  // Take-Any uses a regular shopsanity room as a host. Its captured source
  // door must make the clerk list only the live irreversible offers.
  dungeon_room_index = 0x112;
  which_entrance = 0x58;
  g_rando_takeany_door_id = 0x56;
  if (Rando_TakeAnyLiveSlot(0x13, g_rando_takeany_door_id, 0) != 0xFFFFu)
    selfcheck_fail("Take-Any live slot accepted the wrong host room");
  memset(buf, kCmdEnd, sizeof(buf));
  if (!write_live_shop_inventory(buf) ||
      !buffer_contains_ascii(buf, "Hookshot") ||
      !buffer_contains_ascii(buf, "Fire Rod") ||
      !buffer_contains_ascii(buf, "free"))
    selfcheck_fail("Take-Any clerk reward inventory");
  g_rando_takeany_door_id = 0;

  // Exhaust every registered item through both disclosure formatters. This
  // pins exact generated-name copying and the no-clipping contract as the
  // registry grows.
  {
    char too_small[4];
    if (format_preview_reward_item(
            ITEM_Hookshot, too_small, sizeof(too_small)))
      selfcheck_fail("reward-name copy accepted a truncated destination");
  }
  for (uint16 item = 0; item < ITEM__COUNT; item++) {
    const char *expected_full = Rando_GetHintItemDisplayName(item, false);
    const char *expected_compact = Rando_GetHintItemDisplayName(item, true);
    char copied_full[64], copied_compact[64];
    if (!format_preview_reward_item(item, copied_full, sizeof(copied_full)) ||
        expected_full == NULL || strcmp(copied_full, expected_full) != 0 ||
        !format_compact_reward_item(
            item, copied_compact, sizeof(copied_compact)) ||
        expected_compact == NULL ||
        strcmp(copied_compact, expected_compact) != 0)
      selfcheck_fail("registered reward name was not copied exactly");
    uint8 rendered[kDialogueBufferCapacity];
    if (!render_reward_item_page(rendered, "Reward %s.", item)) {
      char item_name[64] = "(invalid)";
      (void)format_compact_reward_item(item, item_name, sizeof(item_name));
      fprintf(stderr,
              "RandoDialogue_SelfCheck: item %u preview does not fit: %s\n",
              (unsigned)item, item_name);
      selfcheck_fail("registered reward name does not fit preview page");
    }
    RewardInventoryRow item_rows[] = {
      {'L', item, 65535, false},
      {'R', item, 0, true},
    };
    if (!write_reward_inventory(
            rendered, item_rows,
            (int)(sizeof(item_rows) / sizeof(item_rows[0]))))
      selfcheck_fail("registered reward name does not fit inventory page");
  }

  memset(buf, kCmdEnd, sizeof(buf));
  write_ganon_crystal_warning(buf, 1);
  if (!buffer_contains_ascii(buf, "You need 1") ||
      !buffer_contains_ascii(buf, "crystal to") ||
      !buffer_contains_ascii(buf, "harm me"))
    selfcheck_fail("Ganon singular crystal warning");

  memset(buf, kCmdEnd, sizeof(buf));
  write_ganon_crystal_warning(buf, 2);
  if (!buffer_contains_ascii(buf, "You need 2") ||
      !buffer_contains_ascii(buf, "crystals to"))
    selfcheck_fail("Ganon plural crystal warning");

  // Both non-US modes must remain byte-for-byte untouched: bit 0 is the EU/new
  // command grammar; bit 1 is a translated original-format font alphabet.
  for (uint8 locale_flags = 1; locale_flags <= 2; locale_flags++) {
    g_zenv.dialogue_flags = locale_flags;
    memset(buf, 0x55, sizeof(buf)); buf[0] = kCmdEnd;
    Rando_RewriteRewardDialogue(0x0CD, buf);
    if (buf[0] != kCmdEnd) selfcheck_fail("non-US dialogue must not rewrite");
  }
  g_zenv.dialogue_flags = 0;

  g_rando_slot_active = 0;
  memset(buf, 0x55, sizeof(buf)); buf[0] = kCmdEnd;
  Rando_RewriteRewardDialogue(0x0CD, buf);
  if (buf[0] != kCmdEnd) selfcheck_fail("inactive slot must not rewrite");

  g_rando_slot_active = saved_active;
  player_is_indoors = saved_indoors;
  dungeon_room_index = saved_room;
  which_entrance = saved_entrance;
  g_rando_takeany_door_id = saved_takeany_door;
  g_zenv.dialogue_flags = saved_dialogue_flags;
  memcpy(g_rando_checked_bitmap, saved_checked, sizeof(saved_checked));
  Placement_Install(saved_table);
  fprintf(stderr, "[RandoDialogue_SelfCheck] OK\n");
}
