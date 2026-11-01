#pragma once
#include <stdbool.h>
#include <stdint.h>

// The first nine options apply at the next logic frame. Later entries are
// latched only at boot, after the frontend has confirmed the restart.
enum QolOption {
  kQolFastSwitch, kQolPegasus, kQolKakariko, kQolDimFlashes,
  kQolFairyHearts, kQolSwordPickup, kQolBrightRooms, kQolMuteBeep,
  kQolRepeatBlocks, kQolBombs99, kQolArrows99, kQolWallet9999,
  kQolInventory, kQolShovel, kQolMirror, kQolCount,
  kQolLiveCount = kQolBombs99,
};
void Qol_LoadPreferences(const char *path);
void Qol_LatchForBoot(void);
bool Qol_Ready(void);
bool Qol_Selected(int option);
bool Qol_SetOption(int option, bool enabled);
bool Qol_NeedsRestart(int option);
bool Qol_IsCapacity(int option);
const char *Qol_Label(int option);
const char *Qol_Description(int option);
int Qol_Page(int option);
int Qol_PageFirst(int page);
int Qol_PageSize(int page);
int Qol_StepPage(int option,int direction);
uint32_t Qol_Features0(uint32_t previous);
uint32_t Qol_Features1(uint32_t previous);
