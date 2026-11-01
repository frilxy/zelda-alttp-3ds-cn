#ifndef ZELDA3_LOCALIZED_H_
#define ZELDA3_LOCALIZED_H_

#include "types.h"

// The French Canadian cartridge, "LA LEGENDE DE ZELDA" (1 MB, CRC32
// 47760381, SHA-1 C1C6C7F76FFF936C534FF11F87A54162FC0AA100), draws its menus,
// maps, title and credits differently from the USA one the engine reproduces:
// French lettering redrawn in the graphics, other tile tables, other stripe
// lists, and BG3 moved in video memory to make room for a font twice the size.
//
// A profile whose language is fr-c keeps a copy of that cartridge next to its
// assets. When it is there and is that exact cartridge, the screens the game
// draws are drawn the way it draws them, from its own tables and graphics. The
// game itself stays the USA one: only what is drawn changes.
//
// Everything that reads from it names the cartridge address, so each value can
// be checked against the cartridge it came from.

// The file the fr-c profile carries, next to zelda3_assets.dat.
#define LOCALIZED_CART_FILE_FR_C "zelda3_fr-c.sfc"
// The official French and German releases share that cartridge's screens on
// its layout, with some tables moved (localized.c); their profiles keep the
// cartridge their language patch rebuilt from the USA one, LANGUAGE_CART_FILE.

// Where the cartridge in use keeps its 256-character font (French Canadian
// $0C:D078, French and German $0C:C6E8).
uint32 Loc_FontAddr(void);

// The file screens' stripe lists the cartridge points to (index as
// nmi_load_bg_from_vram), and the frame of the game select screen with the
// size its code copies.
const uint8 *Loc_BgTilemap(int index);
const uint8 *Loc_FileSelectStripes(int *size);

// The size of a stripe list up to its end mark, at most max.
int Loc_StripesSize(const uint8 *stripes, int max);

// The German release keeps 96 menu characters apart; they go to BG3
// characters $200 of `vram`, the BG3 character base.
void Loc_UploadMenuChars(uint16 *vram);

// Non-NULL while the French Canadian cartridge is in use.
extern const uint8 *g_loc_rom;

// Loads the cartridge for the language, or releases it when the language has
// none. Returns true when localized screens are active.
bool Localized_SetLanguage(const char *language);

// True when the language's screens are drawn this way (fr-c, fr, de).
bool Localized_HasCart(const char *language);

// French maps label the ground floor RC and count upper floors from one.
// German maps retain the USA numbering with S/K lettering.
bool Loc_UsesFrenchFloorNames(void);

// The cartridge byte at a LoROM address ($80:8000-$9F:FFFF).
const uint8 *Loc_Rom(uint32 snes_addr);

// Copies count little-endian words starting at a cartridge address. Tables in
// the cartridge are often at odd addresses, which is no place to read a
// uint16 from directly on every CPU this runs on.
const uint16 *Loc_Words(uint16 *dst, uint32 snes_addr, int count);

// Graphics sheets the cartridge redraws. Returns the start of the cartridge
// copy of kBgGfx/kSprGfx sheet `index`, or NULL when the USA one is used.
const uint8 *Loc_BgSheet(int index);
const uint8 *Loc_SprSheet(int index);

// kMap16ToMap8 with the cartridge's own entries where it composes a tile
// differently, or NULL when the USA table is used.
const uint16 *Loc_Map16ToMap8(const uint16 *table, size_t size);

// Saves and snapshots made before the French Canadian screens were in use:
// the names were typed on the USA keyboard, whose codes for I, l, i, ! and
// the digits are other characters in the Canadian font, and a snapshot holds
// BG3 where the USA cartridge keeps it. Both are brought over once. Call after
// SRAM is read, and after a snapshot is loaded.
void Localized_AfterSramLoad(void);
void Localized_AfterStateLoad(void);

// BG3 register mirror the cartridge keeps at $7E009F and writes to BG3SC in
// every NMI: 0x79 (64x32 at $7800) on the title and file screens, 0x7a
// (32x64 at $7800) once a game is running.
#define BG3SC_copy (*(uint8*)(g_ram+0x9F))

#endif  // ZELDA3_LOCALIZED_H_
