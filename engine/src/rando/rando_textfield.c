// rando_textfield.c — SDL_TEXTINPUT widget (tasks.md §9.1a).
//
// Pure C state machine — no SDL dependency. Caller (main.c on PC,
// libnx swkbd wrapper on Switch) feeds chars/keys; the widget tracks buffer +
// cursor + active flag.
//
// Used for share-string entry in the settings screen and any other
// base32-constrained text input.

#include "rando_textfield.h"
#include "../types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Host hook — see header comment. Set by the alphabet picker (select_file.c);
// read by the SDL event loop (main.c).
RandoTextField *g_rando_active_textfield = NULL;
bool g_rando_text_input_active = false;
bool g_rando_text_input_submit_pending = false;
bool g_rando_text_input_cancel_pending = false;

// RFC 4648 Base32 alphabet (uppercase A-Z + digits 2-7). Lowercase letters
// are auto-uppercased before the check.
static bool is_base32(char c) {
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '2' && c <= '7') return true;
  return false;
}

static char normalize_base32(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  // Common typo: '0' -> 'O' is NOT in base32 (the alphabet skips O); leave as-is.
  // '1' / 'l' / 'i' are NOT in base32. We pass-through; the filter rejects.
  return c;
}

void TextField_Init(RandoTextField *tf, bool base32_only) {
  if (tf == NULL) return;
  memset(tf, 0, sizeof(*tf));
  tf->active = false;
  tf->base32_only = base32_only;
  tf->buf[0] = 0;
}

void TextField_HandleChar(RandoTextField *tf, char c) {
  if (tf == NULL || !tf->active) return;
  if (tf->len >= kRandoTextFieldMaxLen) return;

  if (tf->base32_only) {
    c = normalize_base32(c);
    if (!is_base32(c)) return;
  } else {
    // Reject control chars (excluding the special keys handled by HandleKey)
    // AND bytes >= 0x7F: SDL_TEXTINPUT delivers UTF-8, so a non-ASCII char
    // arrives as multiple 0x80+ bytes — inserted byte-wise they become N
    // garbage "chars" with per-byte cursor/backspace (mid-codepoint states).
    // The only non-base32 field is the numeric seed field; ASCII suffices.
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7f) return;
  }

  // Insert at cursor.
  if (tf->cursor < tf->len) {
    memmove(tf->buf + tf->cursor + 1, tf->buf + tf->cursor, (size_t)(tf->len - tf->cursor));
  }
  tf->buf[tf->cursor] = c;
  tf->cursor++;
  tf->len++;
  tf->buf[tf->len] = 0;
}

bool TextField_HandleKey(RandoTextField *tf, TextFieldKey key) {
  if (tf == NULL || !tf->active) return false;
  switch (key) {
    case kTextFieldKey_Backspace:
      if (tf->cursor > 0) {
        memmove(tf->buf + tf->cursor - 1, tf->buf + tf->cursor, (size_t)(tf->len - tf->cursor));
        tf->cursor--;
        tf->len--;
        tf->buf[tf->len] = 0;
      }
      return true;
    case kTextFieldKey_Delete:
      if (tf->cursor < tf->len) {
        memmove(tf->buf + tf->cursor, tf->buf + tf->cursor + 1, (size_t)(tf->len - tf->cursor - 1));
        tf->len--;
        tf->buf[tf->len] = 0;
      }
      return true;
    case kTextFieldKey_Left:
      if (tf->cursor > 0) tf->cursor--;
      return true;
    case kTextFieldKey_Right:
      if (tf->cursor < tf->len) tf->cursor++;
      return true;
    case kTextFieldKey_Home:
      tf->cursor = 0;
      return true;
    case kTextFieldKey_End:
      tf->cursor = tf->len;
      return true;
    case kTextFieldKey_Clear:
      tf->len = 0;
      tf->cursor = 0;
      tf->buf[0] = 0;
      return true;
    case kTextFieldKey_Paste:
      // Caller is expected to use TextField_PasteString separately for
      // the actual clipboard content. Returning true marks the key consumed
      // so the host doesn't double-handle the platform paste shortcut.
      return true;
    case kTextFieldKey_Submit:
      // Caller is expected to read buf / len after the submit key. Returning
      // true marks consumption; the widget doesn't internally change state.
      return true;
  }
  return false;
}

int TextField_PasteString(RandoTextField *tf, const char *s) {
  if (tf == NULL || s == NULL || !tf->active) return 0;
  int inserted = 0;
  while (*s != 0 && tf->len < kRandoTextFieldMaxLen) {
    char c = *s++;
    if (tf->base32_only) {
      c = normalize_base32(c);
      if (!is_base32(c)) continue;
    } else {
      // Mirror HandleChar's filter: printable ASCII only (see its comment).
      if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7f) continue;
    }
    // Insert at cursor (mirror HandleChar but inline for the loop).
    if (tf->cursor < tf->len) {
      memmove(tf->buf + tf->cursor + 1, tf->buf + tf->cursor, (size_t)(tf->len - tf->cursor));
    }
    tf->buf[tf->cursor] = c;
    tf->cursor++;
    tf->len++;
    inserted++;
  }
  tf->buf[tf->len] = 0;
  return inserted;
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------
static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[TextField_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void TextField_SelfCheck(void) {
  RandoTextField tf;
  TextField_Init(&tf, false);
  tf.active = true;

  // Type "hello".
  for (const char *p = "hello"; *p; p++) TextField_HandleChar(&tf, *p);
  if (tf.len != 5 || strcmp(tf.buf, "hello") != 0) selfcheck_die("basic insert");
  if (tf.cursor != 5) selfcheck_die("cursor at end after insert");

  // Backspace twice → "hel".
  TextField_HandleKey(&tf, kTextFieldKey_Backspace);
  TextField_HandleKey(&tf, kTextFieldKey_Backspace);
  if (strcmp(tf.buf, "hel") != 0 || tf.len != 3) selfcheck_die("backspace");

  // Home + insert "Z" at start → "Zhel".
  TextField_HandleKey(&tf, kTextFieldKey_Home);
  if (tf.cursor != 0) selfcheck_die("home");
  TextField_HandleChar(&tf, 'Z');
  if (strcmp(tf.buf, "Zhel") != 0) selfcheck_die("insert at cursor");

  // After insert 'Z': buf="Zhel", cursor=1. Right once → cursor=2 (at 'e').
  // Delete deletes char AT cursor → removes 'e' → "Zhl".
  TextField_HandleKey(&tf, kTextFieldKey_Right);
  TextField_HandleKey(&tf, kTextFieldKey_Delete);
  if (strcmp(tf.buf, "Zhl") != 0) selfcheck_die("delete");

  // Clear.
  TextField_HandleKey(&tf, kTextFieldKey_Clear);
  if (tf.len != 0 || tf.cursor != 0) selfcheck_die("clear");

  // base32 filter rejects non-base32 chars.
  TextField_Init(&tf, true);
  tf.active = true;
  // 'A' OK, '1' rejected (not in base32), '2' OK, '@' rejected.
  TextField_HandleChar(&tf, 'A');
  TextField_HandleChar(&tf, '1');
  TextField_HandleChar(&tf, '2');
  TextField_HandleChar(&tf, '@');
  if (strcmp(tf.buf, "A2") != 0) selfcheck_die("base32 filter");

  // Lowercase auto-uppercases under base32 constraint.
  TextField_HandleChar(&tf, 'b');
  if (strcmp(tf.buf, "A2B") != 0) selfcheck_die("base32 lowercase->upper");

  // Paste filters non-base32.
  TextField_HandleKey(&tf, kTextFieldKey_Clear);
  int n = TextField_PasteString(&tf, "ZRSS-Abc123XYZ");
  // 'Z','R','S','S' ok; '-' rejected; 'A','B','C' (b,c lowercased then upper) ok;
  // '1' rejected; '2','3' ok; 'X','Y','Z' ok. Total = 12 inserts.
  if (n != 12 || strcmp(tf.buf, "ZRSSABC23XYZ") != 0) {
    fprintf(stderr, "got n=%d buf=%s\n", n, tf.buf);
    selfcheck_die("paste filtered insert");
  }

  // Inactive field ignores input.
  TextField_Init(&tf, false);
  tf.active = false;
  TextField_HandleChar(&tf, 'x');
  if (tf.len != 0) selfcheck_die("inactive field accepted input");

  // Length cap holds.
  TextField_Init(&tf, false);
  tf.active = true;
  for (int i = 0; i < kRandoTextFieldMaxLen + 10; i++) TextField_HandleChar(&tf, 'a');
  if (tf.len != kRandoTextFieldMaxLen) selfcheck_die("length cap");

  fprintf(stderr, "[TextField_SelfCheck] OK\n");
}
