// rando_textfield.h — SDL_TEXTINPUT widget (tasks.md §9.1). Stub.
//
// Single-line text buffer with cursor, backspace, paste-from-clipboard,
// base32-constrained input. Drives the share-string entry field on PC and
// Switch builds.

#ifndef ZELDA3_RANDO_TEXTFIELD_H_
#define ZELDA3_RANDO_TEXTFIELD_H_

#include "../types.h"

#define kRandoTextFieldMaxLen 64

typedef struct RandoTextField {
  char buf[kRandoTextFieldMaxLen + 1];  // +1 for NUL
  int len;
  int cursor;
  bool active;
  bool base32_only;
} RandoTextField;

// Initialize a text field. base32_only restricts input to RFC 4648 Base32
// chars (used for share-string entry).
void TextField_Init(RandoTextField *tf, bool base32_only);

// Handle a single character of input. No-op if !active or if base32_only
// and the char is outside the alphabet.
void TextField_HandleChar(RandoTextField *tf, char c);

// Handle a special key (backspace, paste, etc.). Returns true if the key
// was consumed.
typedef enum {
  kTextFieldKey_Backspace,    // delete char to the left of cursor
  kTextFieldKey_Delete,       // delete char at cursor
  kTextFieldKey_Left,         // move cursor left
  kTextFieldKey_Right,        // move cursor right
  kTextFieldKey_Home,         // cursor to start
  kTextFieldKey_End,          // cursor to end
  kTextFieldKey_Paste,        // paste from clipboard (caller passes via TextField_PasteString)
  kTextFieldKey_Clear,        // clear the field
  kTextFieldKey_Submit,       // user pressed enter; caller queries buf/len
} TextFieldKey;
bool TextField_HandleKey(RandoTextField *tf, TextFieldKey key);

// Insert a multi-character string (e.g., clipboard paste). Filters per
// base32_only constraint. Returns the number of chars actually inserted
// (may be less than strlen(s) if base32_only filters or the buffer fills).
int TextField_PasteString(RandoTextField *tf, const char *s);

// Self-check: insert / paste / backspace / cursor-move + base32 filter.
// Exits 2 on failure (per the *_SelfCheck pattern).
void TextField_SelfCheck(void);

// ---------------------------------------------------------------------------
// Host hook (tasks.md §9.1b — SDL_TEXTINPUT routing).
//
// `g_rando_active_textfield` is the widget the host's SDL event loop should
// route SDL_TEXTINPUT / SDL_KEYDOWN events into. NULL when no text-input
// surface is focused.
//
// `g_rando_text_input_active` is the host's "did the game request text input?"
// flag. When true, main.c calls SDL_StartTextInput() so the OS dispatches
// SDL_TEXTINPUT events; when false, SDL_StopTextInput() is called. While
// active, the host MUST also suppress its normal keyboard→joypad routing
// so the user's typed chars don't double-fire as menu navigation
// (otherwise typing the letter 'A' both inserts 'A' AND triggers the
// SNES "A button", running away from the picker).
//
// The host doesn't write to these — the in-game UI sub-mode (e.g., the
// alphabet picker in select_file.c) sets them when activating a text-entry
// surface and clears them when leaving.
// ---------------------------------------------------------------------------
extern RandoTextField *g_rando_active_textfield;
extern bool g_rando_text_input_active;

// One-shot flags set by the host (main.c) and consumed by the active UI
// surface (alphabet picker). These exist so PC keyboard users can submit /
// cancel via Enter / Escape without going through the on-screen controls
// row, which is the only Submit path available on controllers. The flags
// are level-triggered booleans: host sets to true on the key event, the
// UI consumer reads-and-clears on its update tick. Both are reset to false
// alongside `g_rando_text_input_active`.
extern bool g_rando_text_input_submit_pending;
extern bool g_rando_text_input_cancel_pending;

#endif  // ZELDA3_RANDO_TEXTFIELD_H_
