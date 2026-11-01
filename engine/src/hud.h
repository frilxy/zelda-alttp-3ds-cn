#pragma once
#include "types.h"

enum kHudItems {

  kHudItem_Bow = 1,
  kHudItem_Boomerang = 2,
  kHudItem_Bombs = 4,
  kHudItem_Mushroom = 5,
  kHudItem_Hammer = 12,
  kHudItem_Flute = 13,
  kHudItem_BookMudora = 15,
  kHudItem_BottleOld = 16,
  
  kHudItem_Shovel = 16,
  kHudItem_Bottle1 = 21,
  kHudItem_Bottle2 = 22,
  kHudItem_Bottle3 = 23,
  kHudItem_Bottle4 = 24,
};

void Hud_RefreshIcon();
uint8 CheckPalaceItemPosession();
void Hud_FloorIndicator();
void Hud_RemoveSuperBombIndicator();
void Hud_SuperBombIndicator();
void Hud_RefillLogic();
// A level's capacity in the table whose address the game's code keeps at
// operand ($0D:table without a mod), and the bombs and arrows Link can carry.
uint8 Hud_Capacity(uint32 operand, uint16 table, int level);
uint8 Hud_MaxBombs(void);
uint8 Hud_MaxArrows(void);
void Hud_Module_Run();
void Hud_ClearTileMap();
void Hud_Init();
void Hud_BringMenuDown();
void Hud_ChooseNextMode();
void Hud_NormalMenu();
void Hud_UpdateHud();
uint8 Hud_LookupInventoryItem(uint8 item);
void Hud_UpdateEquippedItem();
void Hud_CloseMenu();
void Hud_GotoBottleMenu();
void Hud_InitBottleMenu();
void Hud_ExpandBottleMenu();
void Hud_BottleMenu();
void Hud_DrawBottleMenu_Update();
void Hud_EraseBottleMenu();
void Hud_RestoreNormalMenu();
void Hud_SearchForEquippedItem();
void Hud_DrawYButtonItems();
void Hud_DrawAbilityBox();
void Hud_DrawProgressIcons();
void Hud_DrawProgressIcons_Pendants();
void Hud_DrawProgressIcons_Crystals();
void Hud_DrawSelectedYButtonItem();
void Hud_DrawEquipmentBox();
void Hud_DrawBottleMenu();
bool Hud_RefillHealth();
void Hud_AnimateHeartRefill();
bool Hud_RefillMagicPower();
void Hud_RestoreTorchBackground();
void Hud_RebuildIndoor();
void Hud_Rebuild();
const uint16 *Hud_GetItemBoxPtr(int item);
int GetCurrentItemButtonIndex();
uint8 *GetCurrentItemButtonPtr(int i);

void Hud_HandleItemSwitchInputs();
bool Hud_QolHasShovel(void);
void Hud_QolNormalizeInventory(void);
void Hud_QolTranslateInventory(bool expanded);

// ---------------------------------------------------------------------------
// Randomizer in-game OAM overlays.
//
// The optional item/location OAM trackers remain non-native overlays controlled
// by g_rando_show_item_tracker / g_rando_show_location_tracker (owned by rando.c;
// main.c/config.c flip them via kKeys_RandoToggle*).
//
// Hud_RandoDrawTrackers is the single per-frame entry point. It must be called
// AFTER the game has finished building OAM for the frame (i.e. after
// Module_MainRouting) and BEFORE NMI_PrepareSprites packs the extended-OAM
// table, so the overlay sprites get uploaded with the rest of OAM. The
// individual Hud_RandoDraw{Item,Location}Tracker functions are exposed for
// testing / reuse but normal callers use the combined entry point.
// ---------------------------------------------------------------------------
void Hud_RandoDrawTrackers(void);
void Hud_RandoDrawItemTracker(void);
void Hud_RandoDrawLocationTracker(void);
bool Hud_RandoOamTrackerWillDrawThisFrame(void);

// ---------------------------------------------------------------------------
// Item-icon atlas for the rich ImGui item tracker (PC). Decodes the HUD
// item-box 2bpp icons (kHudItemBoxGfxPtrs order) + palette into an RGBA atlas.
// State-immune: decodes from the HUD graphics packs into a scratch buffer (not
// live VRAM), so it can run any time after assets load. Atlas is a horizontal
// strip kRandoIconCount*kRandoIconSize wide, kRandoIconSize tall.
// ---------------------------------------------------------------------------
#define kRandoIconCount 48
#define kRandoIconSize 16
// Slot indices into the atlas == kHudItemBoxGfxPtrs order.
enum {
  kRandoIcon_Bow = 0, kRandoIcon_Boomerang, kRandoIcon_Hookshot, kRandoIcon_Bombs,
  kRandoIcon_Mushroom, kRandoIcon_FireRod, kRandoIcon_IceRod, kRandoIcon_Bombos,
  kRandoIcon_Ether, kRandoIcon_Quake, kRandoIcon_Lamp, kRandoIcon_Hammer,
  kRandoIcon_Flute, kRandoIcon_Net, kRandoIcon_Book, kRandoIcon_Bottle,
  kRandoIcon_Somaria, kRandoIcon_Byrna, kRandoIcon_Cape, kRandoIcon_Mirror,
  kRandoIcon_Gloves, kRandoIcon_Boots, kRandoIcon_Flippers, kRandoIcon_MoonPearl,
  kRandoIcon_Empty, kRandoIcon_Sword, kRandoIcon_Shield, kRandoIcon_Armor,
  // Dungeon-item icons (decoded from the dungeon HUD's own tiles, not the
  // kHudItemBoxGfxPtrs inventory table) into spare atlas slots 28-30, plus
  // shovel (kHudItemFlute[1]) and a full-heart icon at 32-33.
  kRandoIcon_BigKey = 28, kRandoIcon_Map = 29, kRandoIcon_Compass = 30,
  // Magic Powder = kHudItemMushroom[2] (the mushroom box's value-2 tiles). Rando
  // decouples mushroom/powder, so the tracker needs a distinct powder icon.
  kRandoIcon_Powder = 31,
  kRandoIcon_Shovel = 32, kRandoIcon_Heart = 33,
  // Prize icons (pendants + crystal), decoded from HUD scratch tiles.
  kRandoIcon_PendantGreen = 34, kRandoIcon_PendantRed = 35,
  kRandoIcon_PendantBlue = 36, kRandoIcon_Crystal = 37,
  // Tier/colour variants. The inventory table renders each item at a single
  // tier (boomerang/gloves at [1], armor at [0]=green), so an upgraded item
  // would otherwise show the base sprite. The tracker selects these slots when
  // the player holds the upgraded form so the icon matches the real item.
  kRandoIcon_BoomerangRed = 38,  // kHudItemBoomerang[2] (magic/red boomerang)
  kRandoIcon_GlovesTitan  = 39,  // kHudItemGloves[2]   (Titan's Mitt)
  kRandoIcon_ArmorBlue    = 40,  // kHudItemArmor[1]    (blue mail)
  kRandoIcon_ArmorRed     = 41,  // kHudItemArmor[2]    (red mail)
  kRandoIcon_SwordMaster  = 42,  // kHudItemSword[2]    (master sword)
  kRandoIcon_SwordTempered = 43,  // kHudItemSword[3]    (tempered sword)
  kRandoIcon_SwordGold    = 44,  // kHudItemSword[4]    (gold sword)
  kRandoIcon_ShieldRed    = 45,  // kHudItemShield[2]   (red shield)
  kRandoIcon_ShieldMirror = 46,  // kHudItemShield[3]   (mirror shield)
  kRandoIcon_BowSilver    = 47,  // kHudItemBowRandoSilverArrows
};
// Build the atlas into `out` (kRandoIconCount*kRandoIconSize*kRandoIconSize
// uint32 RGBA8888). Transparent pixels have alpha 0. Returns the icon count.
int Hud_RandoBuildIconAtlas(uint32 *out);
