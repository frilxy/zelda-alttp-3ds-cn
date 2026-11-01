#include "quality_of_life.h"
#include "features.h"
#include "file_store.h"
#include "ui_strings.h"
#include <stdio.h>
#include <string.h>

#define QOL_DEFAULTS ((1u << kQolFastSwitch) | (1u << kQolMuteBeep))
#define QOL_ALL ((1u << kQolCount) - 1)
static uint32_t selected = QOL_DEFAULTS, active = QOL_DEFAULTS;
static bool ready;
static char preferences_path[256];
static const uint32_t flags0[kQolCount] = {
  [kQolFastSwitch]=kFeatures0_SwitchLR, [kQolPegasus]=kFeatures0_TurnWhileDashing,
  [kQolDimFlashes]=kFeatures0_DimFlashes, [kQolSwordPickup]=kFeatures0_CollectItemsWithSword,
  [kQolMuteBeep]=kFeatures0_DisableLowHealthBeep, [kQolWallet9999]=kFeatures0_CarryMoreRupees,
  [kQolMirror]=kFeatures0_MirrorToDarkworld,
};
static const uint32_t flags1[kQolCount] = {
  [kQolKakariko]=kFeatures1_QolKakariko, [kQolFairyHearts]=kFeatures1_QolFairyHearts,
  [kQolBrightRooms]=kFeatures1_QolBrightRooms, [kQolRepeatBlocks]=kFeatures1_QolRepeatBlocks,
  [kQolBombs99]=kFeatures1_QolBombs99, [kQolArrows99]=kFeatures1_QolArrows99,
  [kQolInventory]=kFeatures1_QolInventory, [kQolShovel]=kFeatures1_QolShovel,
};

void Qol_LoadPreferences(const char *path) {
  selected=QOL_DEFAULTS;
  snprintf(preferences_path,sizeof(preferences_path),"%s",path?path:"");
  if (path && FileStore_Recover(path)) {
    FILE *file=fopen(path,"rb");
    if(file) {
      unsigned version,options;char tail;
      if(fscanf(file,"Z3QOL %u %u %c",&version,&options,&tail)==2 &&
         version==1 && !(options & ~QOL_ALL)) selected=options;
      fclose(file);
    }
  }
  Qol_LatchForBoot();ready=true;
}
void Qol_LatchForBoot(void) { __atomic_store_n(&active,selected,__ATOMIC_RELEASE); }
bool Qol_Ready(void) { return ready; }
bool Qol_Selected(int option) { return option>=0&&option<kQolCount&&(selected&(1u<<option)); }
bool Qol_NeedsRestart(int option) { return option>=kQolLiveCount&&option<kQolCount; }
bool Qol_IsCapacity(int option) { return option>=kQolBombs99&&option<=kQolWallet9999; }
const char *Qol_Label(int option) { return UI_Text(kUi_QolFastSwitch+option); }
const char *Qol_Description(int option) { return UI_Text(kUi_QolDescFastSwitch+option); }
bool Qol_SetOption(int option,bool enabled) {
  if(option<0||option>=kQolCount||!preferences_path[0])return false;
  uint32_t next=(selected&~(1u<<option))|(enabled?1u<<option:0);
  char text[64];int len=snprintf(text,sizeof(text),"Z3QOL 1 %u\n",next);
  if(!FileStore_Write(preferences_path,text,(size_t)len))return false;
  selected=next;
  if(!Qol_NeedsRestart(option)) {
    uint32_t live_mask=(1u<<kQolLiveCount)-1;
    uint32_t old=__atomic_load_n(&active,__ATOMIC_ACQUIRE);
    __atomic_store_n(&active,(old&~live_mask)|(selected&live_mask),__ATOMIC_RELEASE);
  }
  return true;
}
static uint32_t apply(uint32_t previous,const uint32_t *flags) {
  if(!ready)return previous;
  uint32_t value=__atomic_load_n(&active,__ATOMIC_ACQUIRE);
  for(int i=0;i<kQolCount;i++)previous=(previous&~flags[i])|((value&(1u<<i))?flags[i]:0);
  return previous;
}
uint32_t Qol_Features0(uint32_t previous) {
  return apply(previous,flags0)|(ready?kFeatures0_SkipIntroOnKeypress:0);
}
uint32_t Qol_Features1(uint32_t previous) { return apply(previous,flags1)|(ready?kFeatures1_QolManaged:0); }

// Three equally spaced pages of five options.
int Qol_Page(int option) { return option<5?0:option<10?1:2; }
int Qol_PageFirst(int page) { return page<=0?0:page==1?5:10; }
int Qol_PageSize(int page) { (void)page;return 5; }
int Qol_StepPage(int option,int direction) {
  int page=Qol_Page(option),offset=option-Qol_PageFirst(page),next=page+direction;
  if(next<0)next=0;if(next>2)next=2;
  if(offset>=Qol_PageSize(next))offset=Qol_PageSize(next)-1;
  return Qol_PageFirst(next)+offset;
}
