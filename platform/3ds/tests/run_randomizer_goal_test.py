#!/usr/bin/env python3
"""Exercise production victory rules with controlled inventory/check inputs.

This component test complements the full-engine save/receipt/ending tests. It
does not simulate a complete randomized playthrough or physical hardware.
"""
from pathlib import Path
import argparse, subprocess, os

ROOT=Path(__file__).resolve().parents[3]
CODE=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/rando/goal_runtime.h"
#include "src/rando/rando.h"
#include "src/rando/rando_logic.h"
#include "src/rando/rando_placement.h"
#include "src/rando/item_ids.h"
#include "src/rando/location_ids.h"
#include "src/variables.h"
#include "src/features.h"
uint8 g_ram[0x20000];
static bool active;
static RandoSettings settings;
static bool checked[1024];
static RandoPlacement entries[32];
static RandoPlacementTable table={entries,30};
bool Rando_IsActive(void) {return active;}
const RandoSettings *Rando_GetActiveSettings(void) {return active?&settings:NULL;}
const RandoPlacementTable *Placement_GetActive(void) {return &table;}
bool Rando_IsLocationChecked(uint16 id) {return id<1024 && checked[id];}
bool Rando_HasRequiredGanonCrystals(void) {
  unsigned count=0;for(unsigned i=0;i<7;i++) count+=(link_has_crystals>>i)&1;
  return count>=settings.crystals_ganon;
}
const RandoLocationDef *Rando_FindLocationDef(uint16 id) {
  static RandoLocationDef def;memset(&def,0,sizeof(def));def.id=id;return &def;
}
static void reset(unsigned goal) {
  memset(g_ram,0,sizeof(g_ram));memset(checked,0,sizeof(checked));
  memset(&settings,0,sizeof(settings));settings.goal=goal;settings.crystals_ganon=7;
  settings.pieces_required=20;active=true;main_module_index=9;
  Rando_ResetGoalRuntime();
  for(unsigned i=0;i<30;i++) entries[i]=(RandoPlacement){(uint16)(300+i),ITEM_TriforcePiece};
}
int main(void) {
  reset(kGoal_Ganon);active=false;assert(Rando_GoalAllowsGanonDamage());
  g_ram[kRam_RandoGoalPending]=0x5a;Rando_TickGoalCompletion();Rando_ResetGoalRuntime();
  assert(g_ram[kRam_RandoGoalPending]==0x5a);g_ram[kRam_RandoGoalPending]=0;
  active=true;link_has_crystals=0x7f;assert(!Rando_GoalAllowsGanonDamage());
  checked[LOC_Agahnim_2]=true;assert(Rando_GoalAllowsGanonDamage());
  link_has_crystals=0x3f;assert(!Rando_GoalAllowsGanonDamage());
  reset(kGoal_FastGanon);link_has_crystals=0x7f;
  assert(Rando_GoalAllowsGanonDamage());
  settings.crystals_ganon=0;link_has_crystals=0;assert(Rando_GoalAllowsGanonDamage());
  reset(kGoal_Dungeons);link_has_crystals=0x7f;link_which_pendants=7;
  assert(!Rando_GoalAllowsGanonDamage());
  checked[LOC_Agahnim]=checked[LOC_Agahnim_2]=true;assert(Rando_GoalAllowsGanonDamage());
  for(unsigned bit=0;bit<3;bit++) {
    link_which_pendants=7^(1u<<bit);assert(!Rando_GoalAllowsGanonDamage());
  }
  link_which_pendants=7;
  for(unsigned bit=0;bit<7;bit++) {
    link_has_crystals=0x7f^(1u<<bit);assert(!Rando_GoalAllowsGanonDamage());
  }
  reset(kGoal_TriforceHunt);assert(!Rando_GoalAllowsGanonDamage());
  for(unsigned i=0;i<19;i++) checked[300+i]=true;
  assert(Rando_CountCollectedTriforcePieces()==19 && !Rando_RequestTriforceTurnIn());
  checked[319]=true;assert(Rando_CountCollectedTriforcePieces()==20);
  assert(Rando_RequestTriforceTurnIn());main_module_index=14;
  Rando_TickGoalCompletion();assert(main_module_index==14);
  main_module_index=9;submodule_index=1;Rando_TickGoalCompletion();assert(main_module_index==9);
  submodule_index=0;link_player_handler_state=0x15;Rando_TickGoalCompletion();assert(main_module_index==9);
  link_player_handler_state=0;Rando_TickGoalCompletion();assert(main_module_index==0x19);
  reset(kGoal_GanonHunt);for(unsigned i=0;i<19;i++) checked[300+i]=true;
  assert(!Rando_GoalAllowsGanonDamage());checked[319]=true;assert(Rando_GoalAllowsGanonDamage());
  reset(kGoal_Pedestal);assert(!Rando_GoalAllowsGanonDamage());link_which_pendants=7;
  Rando_TickGoalCompletion();assert(main_module_index==9);
  checked[LOC_Master_Sword_Pedestal]=true;flag_is_link_immobilized=1;
  Rando_NotifyGoalCheck(LOC_Master_Sword_Pedestal);
  Rando_TickGoalCompletion();assert(main_module_index==9);
  flag_is_link_immobilized=0;Rando_TickGoalCompletion();assert(main_module_index==0x19);
  main_module_index=9;Rando_ResetGoalRuntime();Rando_TickGoalCompletion();
  assert(main_module_index==9); // reloading a completed file never loops credits
  reset(kGoal_Completionist);checked[LOC_Agahnim]=checked[LOC_Agahnim_2]=true;
  for(unsigned i=0;i<29;i++) checked[300+i]=true;
  assert(!Rando_GoalAllowsGanonDamage());checked[329]=true;assert(Rando_GoalAllowsGanonDamage());
  puts("PASS: actual acquired pieces, all victory gates, delayed ending and turn-in reset");
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir',type=Path,required=True)
    a=p.parse_args();out=a.work_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    if out==ROOT or ROOT in out.parents:p.error('Keep generated artifacts outside source')
    source=out/'goal_test.c';source.write_text(CODE);binary=out/'goal_test'
    command=['clang','-std=c11','-g','-O1','-fsanitize=address,undefined',
             '-I',str(ROOT/'engine'),'-I',str(ROOT/'engine/src'),str(source),
             str(ROOT/'engine/src/rando/goal_runtime.c'),'-o',str(binary)]
    result=subprocess.run(command,capture_output=True,text=True)
    (out/'build.log').write_text(result.stdout+result.stderr)
    if result.returncode:print(result.stderr);return result.returncode
    result=subprocess.run([str(binary)],capture_output=True,text=True,
                          env=dict(os.environ,UBSAN_OPTIONS="halt_on_error=1",ASAN_OPTIONS="detect_leaks=0"))
    (out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr)
    return result.returncode

if __name__=='__main__':raise SystemExit(main())
