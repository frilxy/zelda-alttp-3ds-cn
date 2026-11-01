#include "goal_runtime.h"
#include "rando.h"
#include "rando_logic.h"
#include "rando_placement.h"
#include "item_ids.h"
#include "location_ids.h"
#include "../variables.h"
#include "../features.h"

// Reserve the last explicitly free randomizer-owned WRAM byte. Unlike C
// statics this survives a cold SDL snapshot taken during dialogue/receipt.
#define goal_pending (g_ram[kRam_RandoGoalPending])
enum { kPendingPedestal=0xa1, kPendingTurnIn=0xa2 };
bool Rando_GoalCompletionPending(void) {
  return goal_pending==kPendingPedestal || goal_pending==kPendingTurnIn;
}

uint8 Rando_CountCollectedTriforcePieces(void) {
  const RandoPlacementTable *table=Placement_GetActive();
  unsigned count=0;
  if(Rando_IsActive() && table) for(unsigned i=0;i<table->count;i++)
    if(table->entries[i].item_id==ITEM_TriforcePiece &&
       Rando_IsLocationChecked(table->entries[i].location_id)) count++;
  return count>255?255:(uint8)count;
}

static bool both_agahnims_defeated(void) {
  return Rando_IsLocationChecked(LOC_Agahnim) && Rando_IsLocationChecked(LOC_Agahnim_2);
}

bool Rando_GoalAllowsGanonDamage(void) {
  const RandoSettings *s=Rando_GetActiveSettings();
  if(!Rando_IsActive() || !s) return true;
  switch(s->goal) {
  case kGoal_Pedestal:
  case kGoal_TriforceHunt:
    return false;
  case kGoal_GanonHunt:
    return Rando_CountCollectedTriforcePieces()>=s->pieces_required;
  case kGoal_Dungeons:
    return both_agahnims_defeated() && (link_which_pendants&7)==7 && (link_has_crystals&0x7f)==0x7f;
  case kGoal_Completionist: {
    if(!both_agahnims_defeated()) return false;
    const RandoPlacementTable *table=Placement_GetActive();
    if(!table) return false;
    for(unsigned i=0;i<table->count;i++) {
      const RandoLocationDef *loc=Rando_FindLocationDef(table->entries[i].location_id);
      if(loc && loc->id!=LOC_Ganon && Rando_LocationTypeCountsAsCheck(loc->type) &&
         !Rando_IsLocationChecked(loc->id)) return false;
    }
    return true;
  }
  case kGoal_Ganon:
    return Rando_IsLocationChecked(LOC_Agahnim_2) && Rando_HasRequiredGanonCrystals();
  case kGoal_FastGanon:
    return Rando_HasRequiredGanonCrystals();
  default:
    return false;
  }
}

bool Rando_RequestTriforceTurnIn(void) {
  const RandoSettings *s=Rando_GetActiveSettings();
  if(!Rando_IsActive() || !s || s->goal!=kGoal_TriforceHunt ||
     Rando_CountCollectedTriforcePieces()<s->pieces_required) return false;
  goal_pending=kPendingTurnIn;
  return true;
}

void Rando_ResetGoalRuntime(void) {
  if(Rando_IsActive()) goal_pending=0;
}

void Rando_NotifyGoalCheck(uint16 location) {
  const RandoSettings *s=Rando_GetActiveSettings();
  if(Rando_IsActive() && s && s->goal==kGoal_Pedestal &&
     location==LOC_Master_Sword_Pedestal && (link_which_pendants&7)==7)
    goal_pending=kPendingPedestal;
}

void Rando_TickGoalCompletion(void) {
  const RandoSettings *s=Rando_GetActiveSettings();
  if(!Rando_IsActive() || !s) return;
  bool pedestal=s->goal==kGoal_Pedestal && goal_pending==kPendingPedestal;
  bool turn_in=s->goal==kGoal_TriforceHunt && goal_pending==kPendingTurnIn;
  // Finish item receipt/dialogue first. ALTTPR's ActivateGoal enters the
  // existing Triforce-room state machine (newitems.asm / stats.asm).
  if((!pedestal && !turn_in) ||
     (main_module_index!=7 && main_module_index!=9) || submodule_index ||
     link_player_handler_state || flag_is_link_immobilized) return;
  goal_pending=0;
  main_module_index=0x19;submodule_index=0;subsubmodule_index=0;
}
