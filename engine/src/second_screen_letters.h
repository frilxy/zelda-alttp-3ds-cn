// Accented capitals for the second-screen UI.
//
// The letter sheet is 16x2 cells of 8x8 and only the 26 ASCII capitals were
// used, so the four spare cells hold the accents French needs. The sheet keeps
// its size, so nothing about the texture or its callers changes.
//
// SS_RenderLetterSheet builds these cells from the ROM's own HUD font (see
// second_screen.c); the renderers map UTF-8 to them with the helper below.
#pragma once

enum {
  kSSLetterCell_EAcute = 26,    // E accent aigu
  kSSLetterCell_EGrave = 27,    // E accent grave
  kSSLetterCell_AGrave = 28,    // A accent grave
  kSSLetterCell_CCedilla = 29,  // C cedille
  kSSLetterCellCount = 30,
};

// Accent shapes, used both when baking the sheet and when drawing the small
// 5x7 font that has no room to bake them in.
enum {
  kSSAccent_Acute = 0,
  kSSAccent_Grave = 1,
  kSSAccent_Cedilla = 2,
};

// If `s` points at the UTF-8 sequence of one of the four accented capitals,
// return its letter-sheet cell and fill in the plain letter and accent shape;
// the sequence is two bytes, so the caller skips one extra byte. Returns -1
// for anything else. `plain` and `accent` may be NULL.
static inline int SS_DecodeAccented(const char *s, char *plain, int *accent) {
  const unsigned char *p = (const unsigned char *)s;
  int cell, shape;
  char letter;
  if (p[0] != 0xC3)
    return -1;
  switch (p[1]) {
  case 0x89: cell = kSSLetterCell_EAcute;   letter = 'E'; shape = kSSAccent_Acute;   break;
  case 0x88: cell = kSSLetterCell_EGrave;   letter = 'E'; shape = kSSAccent_Grave;   break;
  case 0x80: cell = kSSLetterCell_AGrave;   letter = 'A'; shape = kSSAccent_Grave;   break;
  case 0x87: cell = kSSLetterCell_CCedilla; letter = 'C'; shape = kSSAccent_Cedilla; break;
  default: return -1;
  }
  if (plain)
    *plain = letter;
  if (accent)
    *accent = shape;
  return cell;
}
