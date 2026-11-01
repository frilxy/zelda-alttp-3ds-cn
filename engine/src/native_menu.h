#pragma once
#include <stdbool.h>
#include <stdint.h>

// SNES joypad bytes: B/Y/Start live in H, A/X in L. Native file menus use
// A/Start to confirm, B to go back, and retain all direction/Select inputs.
// Keep this policy separate from gameplay and from the cartridge replay path.
static inline bool NativeMenu_CancelPressed(uint8_t high) {
  return (high & 0x80) != 0;
}

static inline void NativeMenu_FilterConfirmation(uint8_t *low, uint8_t *high) {
  *low &= (uint8_t)~0x40;   // X
  *high &= (uint8_t)~0xc0;  // B and Y
}

static inline bool NativeMenu_CanAutosave(unsigned module) {
  // Title, file selection, copy, erase and naming are not a gameplay resume
  // point. In particular, closing during name entry must not trap next boot.
  // Attract mode and the credits' permanent THE END are not resumable play.
  return module > 4 && module < 28 && module != 20 && module != 26;
}
