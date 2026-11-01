#include "goal_npc.h"
#include "goal_runtime.h"
#include "rando.h"
#include "rando_placement.h"
#include "../variables.h"
#include "../sprite.h"
#include "../sprite_main.h"
#include "../dungeon.h"
#include "../load_gfx.h"
#include "../zelda_rtl.h"
#include <stdio.h>

// An ordinary Elder in Hyrule Castle's courtyard, as in ALTTPR elder.asm.
// All art comes from the already-loaded local game assets, never from a ROM
// image embedded in source. A small OBJ scratch lease avoids replacing guards.
static uint8 elder_sheet[0x600];
static bool sheet_ready;
static int drawn_sprite=-1;
static uint8 drawn_frame;

void Rando_ResetGoalNpc(void) {sheet_ready=false;drawn_sprite=-1;}

static bool enabled(void) {
  const RandoSettings *s=Rando_GetActiveSettings();
  return Rando_IsActive() && s && s->goal==kGoal_TriforceHunt;
}

void Rando_AddGoalNpcMapEntry(uint16 area) {
  if(!enabled() || area!=0x1b || sram_progress_indicator<2) return;
  // ALTTPR hooks.asm writes {Y=0x12,X=0x19,type=0x16} into the castle
  // sprite list. At area base (0x600,0x600) that is (0x790,0x720).
  // Use the same quadrant-packed map cell, retaining an authored occupant.
  const unsigned cell=0x529;
  if(!sprite_where_in_overworld[cell]) sprite_where_in_overworld[cell]=0x16+1;
}

bool Rando_HandleGoalNpc(int k) {
  if(!enabled() || player_is_indoors || (uint8)overworld_screen_index!=0x1b) return false;
  drawn_sprite=k;drawn_frame=frame_counter;
  sprite_oam_flags[k]=7;
  if(Sprite_ReturnIfInactive(k)) return true;
  Sprite_BehaveAsBarrier(k);
  if(Sprite_ShowSolicitedMessage(k,kRandoGoalDialogueId)&0x100)
    (void)Rando_RequestTriforceTurnIn();
  sprite_graphics[k]=(frame_counter>>5)&1;
  return true;
}

void Rando_DrawGoalNpcOverlay(void) {
  int k=drawn_sprite;
  if(!enabled() || k<0 || drawn_frame!=frame_counter || player_is_indoors ||
     (uint8)overworld_screen_index!=0x1b || sprite_state[k]!=9 || sprite_type[k]!=0x16) return;
  // All normal sprites have drawn by now. Lease two currently unused scratch
  // slots, then upload AFTER the vanilla restoration pass. Never change a tile
  // used by an on-screen ordinary sprite or another overlay.
  uint8 occupied=Rando_ObjScratchVisibleSlotMask();int slots[2],count=0;
  for(int s=0;s<4 && count<2;s++) if(!(occupied&(1u<<s))) slots[count++]=s;
  if(count!=2 || !Rando_ObjScratchReserveForFrame(kRandoObjScratchOwner_GoalNpc)) return;
  if(!sheet_ready) {
    const uint8 *header=GetRoomHeaderPtr(0x105); // existing Sahasrahla room
    uint8 sheet=GetSpriteTilesetPacks(header[3]+0x40)[2];
    if(Decomp_spr(elder_sheet,sheet)!=sizeof(elder_sheet)) return;
    sheet_ready=true;
  }
  DrawMultipleData parts[8];unsigned pose=sprite_graphics[k]&1;
  for(unsigned part=0;part<8;part++) {
    unsigned body=part/4,quad=part%4;
    unsigned source=32+(body?(pose?4:2):0)+(quad%2)+(quad/2)*16;
    bool flip=body && pose;
    unsigned tile=0xf0+slots[body]*4+quad;
    Do3To4Low16Bit((uint8 *)&g_zenv.vram[0x5000+tile*16],elder_sheet+source*24,1);
    // 0x5000 + tile*16 == 0x5f00 + slot*0x40 for tile 0xf0.
    int x=(int)(quad%2)*8;
    if(flip) x=8-x;
    parts[part]=(DrawMultipleData){x,(int)(body?0:pose?-8:-9)+(int)(quad/2)*8,
                                  (uint16)(tile|(flip?0x4000:0)),0};
  }
  Rando_ObjScratchMarkSlotsDirty((uint8)((1u<<slots[0])|(1u<<slots[1])));
  uint8 previous=sprite_flags2[k];sprite_flags2[k]=(previous&0xe0)|8; // eight tiles + shadow
  // Sprite_Main's cached coordinates belong to its last processed sprite.
  // Rebind them for this deferred draw and restore them for later overlays.
  uint16 previous_x=cur_sprite_x,previous_y=cur_sprite_y;
  cur_sprite_x=Sprite_GetX(k);cur_sprite_y=Sprite_GetY(k);
  PrepOamCoordsRet info;Sprite_DrawMultiplePlayerDeferred(k,parts,8,&info);SpriteDraw_Shadow(k,&info);
  cur_sprite_x=previous_x;cur_sprite_y=previous_y;
  sprite_flags2[k]=previous;
}

static uint8 glyph(char ch) {
  if(ch>='A' && ch<='Z') return ch-'A';
  if(ch>='a' && ch<='z') return ch-'a'+26;
  if(ch>='0' && ch<='9') return ch-'0'+52;
  return ch=='!'?62:ch=='.'?65:89;
}

bool Rando_RenderGoalMessage(uint16 message,uint8 *buffer) {
  if(message!=kRandoGoalDialogueId || !buffer || !enabled()) return false;
  const RandoSettings *s=Rando_GetActiveSettings();
  uint8 pieces=Rando_CountCollectedTriforcePieces();char lines[3][32];
  if(pieces>=s->pieces_required) {
    snprintf(lines[0],32,"The pieces are here!");
    snprintf(lines[1],32,"Let us restore the");
    snprintf(lines[2],32,"Triforce!");
  } else {
    snprintf(lines[0],32,"I am Murahdahla.");
    snprintf(lines[1],32,"%u pieces needed.",s->pieces_required);
    snprintf(lines[2],32,"%u collected.",pieces);
  }
  unsigned o=0;
  for(unsigned row=0;row<3;row++) {
    if(row) buffer[o++]=row==1?0x75:0x76;
    for(unsigned col=0;lines[row][col] && col<24;col++) buffer[o++]=glyph(lines[row][col]);
  }
  buffer[o]=0x7f;return true;
}
