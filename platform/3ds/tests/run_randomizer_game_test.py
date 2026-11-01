#!/usr/bin/env python3
"""Exercise generated slots, real chest dispatch and cold SDL snapshot restore.

Private input assets/ROM stay outside the checkout, in the chosen work folder.
This is a host engine check, not 3DS hardware validation.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
from host_test_support import sdl_flags

ROOT = Path(__file__).resolve().parents[3]
CODE = r'''
#include <assert.h>
#define main ZeldaUnusedRandomizerMain
#include "src/main.c"
#undef main
#include "src/rando/rando_generate.h"
#include "src/rando/rando_save.h"
#include "src/rando/rando_snapshot_tail.h"
#include "src/rando/rando_placement.h"
#include "src/rando/location_ids.h"
#include "src/rando/alttpr_import.h"
#include "src/rando/dungeon_ids.h"
#include "src/rando/item_ids.h"
#include "src/rando/goal_runtime.h"
#include "src/rando/rando_dialogue.h"
#include "src/messaging.h"
#include "src/select_file.h"
#include "src/hud.h"
#include "src/sprite_main.h"
#include "src/player.h"
#include "src/overworld.h"
#include "src/sprite.h"
#include "src/rando/rando_logic.h"

static uint8 pixels[256*240*4];
static void frame(int inputs) {
  ZeldaRunFrame(inputs);
  ZeldaDrawPpuFrame(pixels, 256*4, kPpuRenderFlags_NewRenderer);
}
static void frames(int count) { while(count--) frame(0); }
static void require(bool ok, const char *reason) {
  if(!ok) { fprintf(stderr,"FAIL: %s\n",reason); exit(1); }
}

static void check_import_escape(const AlttprSeed *original) {
  uint8 *saved=malloc(sizeof(g_ram)); require(saved!=NULL,"escape test state");
  memcpy(saved,g_ram,sizeof(g_ram));
  AlttprSeed test=*original; test.settings.world_state=kWorldState_Standard;
  Alttpr_Install(&test);
  RandoCounts counts={0};
  uint16 mountain=Rando_FindRegionByName("LightWorld_DeathMountain_East");
  uint16 turtle=Rando_FindRegionByName("TurtleRock_Entrance");
  counts.by_item_id[ITEM_MoonPearl]=counts.by_item_id[ITEM_Hammer]=1;
  require(!Alttpr_AdditionalRegionRoute(mountain,turtle,&counts),"TR portal requires dark-rock lifting");
  counts.by_item_id[ITEM_ProgressiveGlove]=2;
  require(Alttpr_AdditionalRegionRoute(mountain,turtle,&counts),"TR portal does not require Mirror");
  counts.by_item_id[ITEM_MoonPearl]=0;
  require(!Alttpr_AdditionalRegionRoute(mountain,turtle,&counts),"TR portal retains Pearl requirement");
  memset(&counts,0,sizeof(counts));
  require(Alttpr_LocationReachability(LOC_Zelda,&counts)==0,"escape requires combat and sewer key");
  counts.by_item_id[ITEM_Hammer]=1;
  require(Alttpr_LocationReachability(LOC_Sewers_Dark_Cross,&counts)==1,"Standard dark cross has supplied cone");
  require(Alttpr_LocationReachability(LOC_Zelda,&counts)==0,"cone cannot bypass sewer key");
  counts.by_item_id[ITEM_SmallKey_HyruleCastleEscape]=1;
  require(Alttpr_LocationReachability(LOC_Zelda,&counts)==1,"Standard rescue does not require Lamp");
  player_is_indoors=1;cur_palace_index_x2=0;link_item_torch=0;
  dung_want_lights_out=1;hdr_dungeon_dark_with_lantern=0;dung_num_lit_torches=0;dung_hdr_bg2_properties=0;
  Hud_RestoreTorchBackground();
  require(hdr_dungeon_dark_with_lantern==1 && TS_copy==1 && link_item_torch==0,"real HUD supplies cone without awarding Lamp");
  cur_palace_index_x2=2;hdr_dungeon_dark_with_lantern=0;
  Hud_RestoreTorchBackground();require(!hdr_dungeon_dark_with_lantern,"escape cone never lights another dungeon");
  static const uint16 weapons[]={ITEM_FireRod,ITEM_Bow,ITEM_Bombs10,ITEM_Hammer};
  for(unsigned w=0;w<4;w++) {
    for(unsigned i=0;i<test.count;i++)
      if(test.placements[i].location_id==LOC_Link_s_Uncle) test.placements[i].item_id=weapons[w];
    Alttpr_Install(&test);
    for(int respawn=0;respawn<3;respawn++) {
      sram_progress_indicator=respawn?1:0;which_starting_point=respawn==0?0:respawn==1?2:4;
      link_magic_filler=link_arrow_filler=link_bomb_filler=0;
      (void)Rando_TryGrantStartingInventory(NULL);
      require(link_magic_filler==(w==0?(respawn?0x20:0x80):0),"magic supply on each escape respawn");
      require(link_arrow_filler==(w==1?(respawn?10:70):0),"arrow supply on each escape respawn");
      require(link_bomb_filler==(w==2?(respawn?3:50):0),"bomb supply on each escape respawn");
    }
    sram_progress_indicator=2;link_magic_filler=link_arrow_filler=link_bomb_filler=0;
    require(!Alttpr_ApplyEscapeRefill() && !link_magic_filler && !link_arrow_filler && !link_bomb_filler,"escape refills stop after rescue");
  }
  test.settings.world_state=kWorldState_Open;Alttpr_Install(&test);sram_progress_indicator=0;
  require(!Alttpr_ApplyEscapeRefill(),"Open receives no escape supplies");
  memcpy(g_ram,saved,sizeof(g_ram));free(saved);Alttpr_Install(original);
}

static void check_import_combat(const AlttprSeed *original) {
  AlttprSeed test=*original;test.placement_advanced=1;test.settings.mode_weapons=kModeWeapons_Randomized;Alttpr_Install(&test);
  RandoCounts n={0};
  for(unsigned d=1;d<13;d++) require(Alttpr_CanKillBoss(d,&n)==0,"empty inventory cannot defeat bosses");
  n.by_item_id[ITEM_Hammer]=n.by_item_id[ITEM_CaneOfSomaria]=1;
  n.by_item_id[ITEM_SmallKey_IcePalace]=n.by_item_id[ITEM_BigKey_IcePalace]=1;
  n.by_item_id[ITEM_ProgressiveGlove]=2;
  require(!Alttpr_CanKillBoss(kRandoDungeon_IcePalace,&n),"IP requires a way to melt the shell");
  n.by_item_id[ITEM_Bombos]=1;
  require(!Alttpr_CanKillBoss(kRandoDungeon_IcePalace,&n),"normal Bombos requires a sword");
  n.by_item_id[ITEM_ProgressiveSword]=1;
  require(Alttpr_LocationReachability(LOC_Ice_Palace_Boss,&n)==1,"advanced one-key Somaria IP route");
  require(Alttpr_AdditionalRegionRoute(Rando_FindRegionByName("IcePalace_Lobby"),
      Rando_FindRegionByName("IcePalace_BossRoom"),&n),"boss region accepts the same advanced route");
  n.by_item_id[ITEM_CaneOfSomaria]=0;
  require(!Alttpr_LocationReachability(LOC_Ice_Palace_Boss,&n),"one-key IP still requires Somaria");
  n.by_item_id[ITEM_CaneOfSomaria]=1;test.placement_advanced=0;Alttpr_Install(&test);
  require(!Alttpr_CanKillBoss(kRandoDungeon_IcePalace,&n),"basic combat retains stronger equipment rule");
  test.settings.world_state=kWorldState_Open;Alttpr_Install(&test);memset(&n,0,sizeof(n));
  require(Alttpr_LocationReachability(LOC_Sanctuary,&n)==1,"Open sanctuary needs no sewer items");
  Alttpr_Install(original);
}

// Exercise the production pedestal handoff, including the caller-owned freeze.
static void check_pedestal_receipts(void) {
  RandoSidecarSlot slot;require(Rando_LoadSidecarSlot(0,&slot),"pedestal original slot");
  uint8 *saved=malloc(sizeof(g_ram));require(saved!=NULL,"pedestal saved RAM");memcpy(saved,g_ram,sizeof(g_ram));
  static const uint16 rewards[]={ITEM_TriforcePiece,ITEM_Nothing,ITEM_L2Sword,ITEM_L2Sword};
  for(unsigned trial=0;trial<4;trial++) {
    Rando_ActivateSidecarSlot(&slot);memcpy(g_ram,saved,sizeof(g_ram));
    RandoPlacementTable *table=(RandoPlacementTable *)Placement_GetActive();
    bool found=false;for(unsigned i=0;i<table->count;i++) if(table->entries[i].location_id==LOC_Master_Sword_Pedestal) {
      table->entries[i].item_id=rewards[trial];found=true;break;
    }
    require(found,"pedestal placement fixture");
    main_module_index=9;submodule_index=0;player_is_indoors=0;overworld_screen_index=0;
    save_ow_event_info[0]&=~0x40;link_which_pendants=7;link_sword_type=0;
    flag_is_link_immobilized=link_disable_sprite_damage=1;link_player_handler_state=0;
    sprite_state[0]=9;sprite_ai_state[0]=4;sprite_delay_main[0]=0;
    if(trial==3) for(int i=0;i<5;i++) ancilla_type[i]=1;
    else for(int i=0;i<5;i++) ancilla_type[i]=0;
    MasterSword_Main(0);
    require(Rando_IsLocationChecked(LOC_Master_Sword_Pedestal) && sprite_ai_state[0]==5,"pedestal reward committed");
    if(trial==2) require(link_player_handler_state==kPlayerState_HoldUpItem && flag_is_link_immobilized,"animated pedestal retains receipt teardown");
    else require(!flag_is_link_immobilized && !link_disable_sprite_damage,"direct/no-op/saturated pedestal releases ceremony freeze");
  }
  Rando_ActivateSidecarSlot(&slot);memcpy(g_ram,saved,sizeof(g_ram));free(saved);
}

static void scene_tour(void) {
  const char *path=getenv("ZELDA3_RANDO_TOUR_STEPS");
  const char *out=getenv("ZELDA3_RANDO_TOUR_OUT");
  FILE *steps=fopen(path,"r");require(steps && out,"scene tour setup");
  char name[64];int count,inputs;
  while(fscanf(steps,"%63s %d %d",name,&count,&inputs)==3) {
    require(count>=0 && count<=10000,"bounded scene tour step");
    if(!strncmp(name,"warning-",8)) {
      require(inputs<=kGoal_Completionist,"goal warning fixture enum");
      ((RandoSettings *)Rando_GetActiveSettings())->goal=(uint8)inputs;
      Sprite_ShowMessageUnconditional(0x16f);inputs=0;
    }
    if(!strcmp(name,"enter-pedestal")) {
      require(Rando_GetActiveSettings()->goal==kGoal_Pedestal,"pedestal goal fixture");
      link_which_pendants=7; // controlled goal inventory, not a full playthrough
      dungeon_room_index=0x180;main_module_index=9;submodule_index=0x17;subsubmodule_index=0;
    }
    if(!strcmp(name,"approach-pedestal")) {
      int k=-1;for(int n=0;n<16;n++) if(sprite_state[n]==9 && sprite_type[n]==0x62 && !sprite_subtype2[n]) k=n;
      require(k>=0,"actual pedestal sprite loaded");
      link_x_coord=Sprite_GetX(k);link_y_coord=Sprite_GetY(k)-12;link_direction_facing=2;
    }
    if(!strcmp(name,"enter-castle")) {
      which_entrance=4;main_module_index=6;submodule_index=subsubmodule_index=0;
    }
    if(!strcmp(name,"approach-npc")) {
      // Controlled positioning beside the official courtyard NPC. This scene
      // proves the real interaction/ending, not a full randomized playthrough.
      require(overworld_screen_index==0x1b && !player_is_indoors,"courtyard fixture area");
      link_x_coord=0x790;link_y_coord=0x730;link_direction_facing=0;
    }
    if(!strcmp(name,"snapshot-goal")) {
      require(Rando_GoalCompletionPending(),"goal handoff armed before cold snapshot");
      SaveLoadSlot(kSaveLoad_Save,2);
      RandoSidecarSlot restarted;require(Rando_LoadSidecarSlot(0,&restarted),"pending goal source slot");
      Rando_ActivateSidecarSlot(&restarted);
      require(!Rando_GoalCompletionPending(),"file activation clears the previous ending request");
      Rando_DeactivateSlot();memset(g_ram,0,sizeof(g_ram));
      SaveLoadSlot(kSaveLoad_Load,2);
      require(Rando_IsActive() && Rando_GoalCompletionPending(),"cold snapshot preserves pending goal handoff");
    }
    if(!strcmp(name,"grant-pieces")) {
      const RandoPlacementTable *table=Placement_GetActive();
      for(unsigned i=0;i<table->count && Rando_CountCollectedTriforcePieces()<count;i++) {
        if(table->entries[i].item_id!=ITEM_TriforcePiece || Rando_IsLocationChecked(table->entries[i].location_id)) continue;
        require(Rando_GrantLocation(table->entries[i].location_id,ITEM_TriforcePiece,kRandoLttpSkip,
            kRandoGrantPresentation_None,0,0)==kRandoGrantResult_Accepted,"tour piece inventory fixture");
      }
      require(Rando_CountCollectedTriforcePieces()==count,"tour acquired target pieces");
      SaveGameFile();
    } else if(!strcmp(name,"finish-ending")) {
      for(unsigned n=0;n<count && main_module_index!=26;n++) frame(n%30==0?256:0);
      require(main_module_index==26,"actual goal reaches credits through normal text input");
    } else while(count--) {
      link_disable_sprite_damage=1; // isolate the NPC interaction from nearby guard combat
      frame(inputs);
    }
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels,256,240,32,256*4,SDL_PIXELFORMAT_ARGB8888);
    require(surface!=NULL,"tour screenshot surface");char image[1024];
    snprintf(image,sizeof(image),"%s/%s.bmp",out,name);require(SDL_SaveBMP(surface,image)==0,"tour screenshot");SDL_FreeSurface(surface);
    snprintf(image,sizeof(image),"%s/%s.ram",out,name);FILE *ram=fopen(image,"wb");
    require(ram && fwrite(g_ram,1,sizeof(g_ram),ram)==sizeof(g_ram),"private tour state");fclose(ram);
    fprintf(stderr,"TOUR %s module=%u sub=%u area=%x room=%x x=%x y=%x pieces=%u\n",name,
        main_module_index,submodule_index,overworld_screen_index,dungeon_room_index,link_x_coord,link_y_coord,Rando_CountCollectedTriforcePieces());
    fprintf(stderr,"PPU bg3=%u,%u copy=%u,%u extra=%u irq=%u msg=%x pos=%x\n",
      g_zenv.ppu->bgLayer[2].hScroll,g_zenv.ppu->bgLayer[2].vScroll,BG3HOFS_copy2,BG3VOFS_copy2,
      g_zenv.ppu->extraLeftRight,irq_flag,dialogue_message_index,text_msgbox_topleft);
  }
  fclose(steps);
}

int main(int argc, char **argv) {
  if(argc==3 && !strcmp(argv[1],"--parse-alttpr")) {
    AlttprSeed seed; char error[256]={0};
    if(!Alttpr_Load(argv[2],&seed,error,sizeof(error))) {fprintf(stderr,"%s\n",error);return 1;}
    for(unsigned i=0;i<seed.count;i++) printf("%u %u\n",seed.placements[i].location_id,seed.placements[i].item_id);
    return 0;
  }
  require((argc==2 || argc==3 || argc==4) && chdir(argv[1])==0,"work directory");
  ParseConfigFile(NULL); g_config.extended_aspect_ratio=0; LoadAssets(); ZeldaInitialize();
  g_config.language="en"; ZeldaSetLanguage("en");
  g_snes_width=256; g_snes_height=224;
  g_zenv.ppu->extraLeftRight=0;PpuSetExtraSideSpace(g_zenv.ppu,0,0,0);
  g_config.new_renderer=true;
  g_wanted_zelda_features=g_config.features0;
  FILE *rom=fopen("input.sfc","rb"); require(rom!=NULL,"local ROM");
  require(fseek(rom,0,SEEK_END)==0,"ROM size"); long size=ftell(rom);
  long header=size%1024==512?512:0;
  uint8 chest_table[504];
  require(fseek(rom,header+0xe96e,SEEK_SET)==0 && fread(chest_table,1,504,rom)==504,"local chest table");
  fclose(rom);
  require(RandoChestMap_Init(chest_table,504,kDungeonRoomChests,kDungeonRoomChests_SIZE),"chest identities");
  if(argc==3 && !strcmp(argv[2],"--dialogue-selfcheck")) {
    RandoDialogue_SelfCheck();ZeldaShutdownSession();return 0;
  }
  for(int n=0;n<2000 && !(main_module_index==0 && submodule_index==8);n++) frame(0);
  require(main_module_index==0 && submodule_index==8,"title initialized");
  main_module_index=1; submodule_index=0; subsubmodule_index=0; frames(90);
  RandoSave_SetSeedStorageActive(true);
  RandoSettings settings; Settings_SetDefaults(&settings);
  if(argc==4) require(!strcmp(argv[2],"--preset") &&
      Settings_ApplyPreset((SettingsPreset)atoi(argv[3]),&settings)==0,"native preset");
  char err[256]={0};
  AlttprSeed imported={0};
  if(argc==3) {
    require(Alttpr_Load(argv[2],&imported,err,sizeof(err)),err);
    if(getenv("ZELDA3_RANDO_DIAG")) {
      Alttpr_Install(&imported);Rando_SetDungeonPrizeAssignment(imported.prizes);
      Rando_SetMedallionAssignment(imported.medallions);Logic_SetResolvedTowerCrystals(imported.settings.crystals_tower+1);
      RandoPlacementTable t={imported.placements,imported.count};RandoSpheres sp;
      Logic_ComputeSpheres(&imported.settings,&t,&sp);RandoCounts n={0};
      n.by_item_id[ITEM_StartingHeart]=3;n.by_item_id[ITEM_RescuedZelda]=imported.settings.world_state!=kWorldState_Standard;
      Alttpr_AddStartingCounts(&n);
      for(unsigned i=0;i<t.count;i++) if(sp.sphere_index_by_placement[i]!=kSphereIndexUnreachable) n.by_item_id[t.entries[i].item_id]++;
      for(unsigned i=0;i<ITEM__COUNT;i++) if(n.by_item_id[i]) fprintf(stderr,"INVENTORY %s=%u\n",Rando_GetItemName(i),n.by_item_id[i]);
      fprintf(stderr,"IP advanced=%u combat=%d location=%d\n",imported.placement_advanced,
        Alttpr_CanKillBoss(kRandoDungeon_IcePalace,&n),Alttpr_LocationReachability(LOC_Ice_Palace_Boss,&n));
      Alttpr_Clear();
    }
    require(Alttpr_CreateSlot(&imported,0,err,sizeof(err)),err);
    fprintf(stderr,"ALttPR exact import: %u placements digest=",imported.count);
    for(int i=0;i<32;i++) fprintf(stderr,"%02x",imported.placement_digest[i]);
    fprintf(stderr," medallions=%u,%u\n",imported.medallions[0],imported.medallions[1]);
  } else require(Rando_GenerateSlot(&settings,42,0,0,g_config.features0,NULL,err,sizeof(err)),err);
  require(!memcmp(g_zenv.sram,g_zenv.sram+0xf00,0x500),"generation initializes both SRAM copies");
  // File-menu operations must be durable even before the first gameplay save.
  // Reproduce the early local build's stale source backup deliberately.
  memset(g_zenv.sram+0xf00,0,0x500);
  require(RandoSave_CopyFileSlot(0,1),"copy before first gameplay save");
  require(RandoSave_RecoverPairedSave()==1,"cold recovery of pre-gameplay copy");
  require(!memcmp(g_zenv.sram,g_zenv.sram+0xf00,0x500),"copy transaction repaired source backup");
  require(RandoSave_EraseFileSlot(1) && RandoSave_RecoverPairedSave()==1,"erase pre-gameplay copy");
  RandoSidecarSlot slot;
  require(Rando_LoadSidecarSlot(0,&slot),"generated sidecar");
  require(!Rando_DetectChecksumDrift(&slot.header,g_zenv.sram,0x500),"paired SRAM checksum");
  // Use the actual file-selector input path, which must activate the sidecar
  // before CopySaveToWRAM starts the game.
  frames(2); frame(1<<8); frames(180);
  require(Rando_IsActive(),"file selector activated native randomizer");
  if(main_module_index==0x1b) { frame(1<<8); frames(180); }
  // Standard starts with Zelda's real telepathic dialogue. Dismiss it through
  // normal input instead of mistaking the expected story module for a hang.
  for(int n=0; n<80 && main_module_index==14; n++) {
    frame(1<<8); frames(20);
  }
  fprintf(stderr,"Generated start: module=%u sub=%u progress=%u start=%u indoors=%u x=%u y=%u\n", main_module_index,submodule_index,sram_progress_indicator,which_starting_point,player_is_indoors,link_x_coord,link_y_coord);
  require(main_module_index==7 || main_module_index==9,"generated start reached gameplay");
  if(getenv("ZELDA3_RANDO_TOUR_STEPS")) {
    scene_tour();ZeldaShutdownSession();return 0;
  }
  check_pedestal_receipts();
  const RandoSettings *active_settings=Rando_GetActiveSettings();
  require(active_settings!=NULL,"canonical active settings");
  if(active_settings->goal==kGoal_FastGanon || active_settings->goal==kGoal_GanonHunt)
    require(save_ow_event_info[0x5b]&0x20,"Fast Ganon pyramid opens in actual loaded save");
  if(argc!=3 && settings.goal==kGoal_TriforceHunt) {
    const RandoPlacementTable *table=Placement_GetActive();bool granted_piece=false;
    for(unsigned i=0;i<table->count && !granted_piece;i++) {
      if(table->entries[i].item_id!=ITEM_TriforcePiece || table->entries[i].location_id==LOC_Link_s_House) continue;
      require(Rando_GrantLocation(table->entries[i].location_id,ITEM_TriforcePiece,kRandoLttpSkip,
          kRandoGrantPresentation_None,0,0)==kRandoGrantResult_Accepted,"real Triforce piece grant");
      granted_piece=true;
    }
    require(granted_piece && g_rando_triforce_piece_count==1,"piece acquired once");
  }
  if(argc==3) {
    require(link_sword_type==imported.starting_sword,"exact imported starting sword");
    check_import_combat(&imported);
    check_import_escape(&imported);
    require(!memcmp(Rando_GetDungeonPrizeAssignment(),imported.prizes,13),"exact imported dungeon prizes");
    require(!memcmp(Rando_GetMedallionAssignment(),imported.medallions,2),"exact imported medallions");
    for(int d=0;d<13;d++) if(imported.prizes[d]<10) {
      require(Rando_GrantBossPrizeReceipt(Rando_GameDungeonFromRandoDungeon(d),0x37,
        kRandoGrantPresentation_None,3,0)==kRandoGrantResult_Accepted,"imported boss prize dispatch");
      RandoCounts inventory;Rando_BuildRuntimeCounts(&inventory);
      require(inventory.by_item_id[ITEM_Prize_GreenPendant+imported.prizes[d]]>0,"correct imported prize inventory bit");
    }
    link_bottle_info[0]=2;
    require(Alttpr_RefillBottle(false),"imported fairy bottle refill");
    static const uint8 contents[]={6,7,8,3,4,5};
    require(link_bottle_info[0]==contents[imported.bottles[0]-ITEM_BottleWithFairy],"official waterfall bottle contents");
  }
  uint8 expected_digest[32]; PlacementTable_ComputeDigest(Placement_GetActive(),expected_digest);
  require(!Rando_IsLocationChecked(LOC_Link_s_House),"fresh chest unchecked");
  RandoGrantResult granted=Rando_ChestGrant(0x104,0,0x12,
      kRandoGrantPresentation_None,0,0);
  require(granted==kRandoGrantResult_Accepted,"native chest grant accepted");
  require(Rando_IsLocationChecked(LOC_Link_s_House),"chest checked after grant");
  require(Rando_ChestGrant(0x104,0,0x12,kRandoGrantPresentation_None,0,0)==
      kRandoGrantResult_AlreadyChecked,"chest cannot grant twice");
  uint8 expected_pieces=g_rando_triforce_piece_count;
  SaveGameFile();
  require(Rando_LoadSidecarSlot(0,&slot) &&
      !Rando_DetectChecksumDrift(&slot.header,g_zenv.sram,0x500),"game save remains paired");
  SaveLoadSlot(kSaveLoad_Save,1);
  Rando_DeactivateSlot(); memset(g_ram,0,sizeof(g_ram));
  SaveLoadSlot(kSaveLoad_Load,1);
  require(Rando_IsActive(),"cold SDL snapshot restored randomizer");
  require(g_rando_triforce_piece_count==expected_pieces,"cold snapshot preserves Triforce pieces");
  if(argc==3) {
    require(!memcmp(Rando_GetDungeonPrizeAssignment(),imported.prizes,13),"snapshot preserves imported prizes");
    require(!memcmp(Rando_GetMedallionAssignment(),imported.medallions,2),"snapshot preserves imported medallions");
    static const uint8 contents[]={6,7,8,3,4,5};
    require(link_bottle_info[0]==contents[imported.bottles[0]-ITEM_BottleWithFairy],"snapshot preserves fairy bottle");
  }
  require(Rando_IsLocationChecked(LOC_Link_s_House),"snapshot restored checked chest");
  uint8 actual_digest[32]; PlacementTable_ComputeDigest(Placement_GetActive(),actual_digest);
  require(!memcmp(actual_digest,expected_digest,32),"snapshot preserved exact placements");
  frames(30);
  // Kill-between-replacements recovery uses the committed pair, even when both
  // compatibility mirrors are missing or damaged. A pending .tmp is ignored.
  uint8 saved_sram[8192]; memcpy(saved_sram,g_zenv.sram,sizeof(saved_sram));
  require(remove("saves/sram.dat")==0,"remove SRAM mirror fixture");
  FILE *broken=fopen("saves/sram_rando.dat","wb"); require(broken!=NULL,"damage sidecar mirror fixture");
  fputs("interrupted mirror",broken); fclose(broken);
  memset(g_zenv.sram,0,8192);
  require(rename("saves/seed_save.dat","saves/seed_save.dat.replace.bak")==0,"interrupted rename fixture");
  broken=fopen("saves/seed_save.dat.tmp","wb"); require(broken!=NULL,"pending temporary fixture");
  fputs("unfinished",broken); fclose(broken);
  require(RandoSave_RecoverPairedSave()==1,"pair recovered after interrupted replacement");
  require(!memcmp(g_zenv.sram,saved_sram,8192),"recovered exact SRAM");
  require(Rando_LoadSidecarSlot(0,&slot),"recovered exact sidecar");
  require(!Rando_DetectChecksumDrift(&slot.header,g_zenv.sram,0x500),"recovered pair checksum");
  // Drive the production copy and erase confirmations, including SRAM backups.
  filtered_joypad_L=0x80; filtered_joypad_H=0;
  g_ram[0xc8]=0; WORD(g_ram[0xca])=2; WORD(g_ram[0xcc])=0;
  CopyFile_HandleConfirmation();
  require(!memcmp(g_zenv.sram,g_zenv.sram+0x500,0x500),"copy SRAM");
  require(!memcmp(g_zenv.sram+0x500,g_zenv.sram+0x1400,0x500),"copy backup SRAM");
  require(Rando_LoadSidecarSlot(1,&slot) &&
          !Rando_DetectChecksumDrift(&slot.header,g_zenv.sram+0x500,0x500),"copy paired sidecar");
  Rando_ActivateSidecarSlot(&slot);
  require(Rando_IsLocationChecked(LOC_Link_s_House),"copy preserves checked locations");
  g_rando_triforce_piece_count=0;
  Rando_ActivateSidecarSlot(&slot);
  require(g_rando_triforce_piece_count==expected_pieces,"copied cold file reconstructs Triforce count");
  const uint8 custom_name[12]={4,0,0x22,0,0x23,0,4,0,1,0,0,0};
  require(RandoSave_CreateNamedFileSlot(2,custom_name),"new named file uses immutable seed image");
  require(!memcmp(g_zenv.sram+0xa00+0x3d9,custom_name,12),"typed name preserved");
  require(!memcmp(g_zenv.sram+0xa00,g_zenv.sram+0x1900,0x500),"named SRAM backup");
  require(Rando_LoadSidecarSlot(2,&slot),"fresh seed sidecar");
  Rando_ActivateSidecarSlot(&slot);
  require(!Rando_IsLocationChecked(LOC_Link_s_House),"fresh file has no copied progress");
  require(g_rando_triforce_piece_count==0,"fresh file resets Triforce count");
  if(argc==3) require(g_zenv.sram[0xa00+0x359]==imported.starting_sword,"fresh file preserves exact starting sword");
  PlacementTable_ComputeDigest(Placement_GetActive(),actual_digest);
  require(!memcmp(actual_digest,expected_digest,32),"fresh file preserves exact seed");
  g_ram[0xc8]=0; subsubmodule_index=1;
  SelectFile_Func16();
  require(Rando_LoadSidecarSlot(1,&slot) && slot.header.slot_kind!=kSlotKind_Randomizer,"erase clears sidecar");
  require(g_zenv.sram[0x500+0x3e5]==0 && g_zenv.sram[0x1400+0x3e5]==0,"erase clears both SRAM copies");
  require(RandoSave_RecoverPairedSave()==1,"copy/erase committed bundle remains recoverable");
  require(RandoSave_OfferNamedFiles(),"offer empty named files after generation");
  require(!WORD(g_zenv.sram[0x3e5])&&!WORD(g_zenv.sram[0x12e5]),"internal RANDO file hidden in both SRAM copies");
  require(RandoSave_SeedStorageHealthy()&&RandoSave_RecoverPairedSave()==1,"empty seed is recoverable");
  selectfile_arr1[0]=0;g_ram[0xc8]=0;filtered_joypad_L=0x80;filtered_joypad_H=0;
  main_module_index=1;FileSelect_Main();require(main_module_index==4,"empty seed file opens normal naming keyboard");
  require(RandoSave_CreateNamedFileSlot(0,custom_name),"name fresh primary file after empty seed boot");
  require(Rando_LoadSidecarSlot(0,&slot)&&!Rando_DetectChecksumDrift(&slot.header,g_zenv.sram,0x500),"named primary file remains paired");
  // An unreadable sidecar cannot silently become a vanilla copied file.
  broken=fopen("saves/sram_rando.dat","wb"); require(broken!=NULL,"corrupt sidecar fixture");
  fputs("bad",broken); fclose(broken);
  require(!RandoSave_CopyFileSlot(0,1),"corrupt source refuses copy");
  require(RandoSave_RecoverPairedSave()==1,"restore sidecar from authoritative bundle");
  broken=fopen("saves/seed_save.dat","r+b"); require(broken!=NULL,"bundle tamper fixture");
  require(fseek(broken,24,SEEK_SET)==0,"seek bundle"); int old=fgetc(broken);
  require(fseek(broken,24,SEEK_SET)==0 && fputc(old^1,broken)!=EOF,"damage bundle checksum"); fclose(broken);
  require(RandoSave_RecoverPairedSave()==-1 && !RandoSave_SeedStorageHealthy(),"corrupt bundle fails closed");
  RandoSave_SetSeedStorageActive(false);
  ZeldaShutdownSession();
  puts("PASS: native generation, file selection, chest dispatch, save pairing, cold SDL snapshot, copy/erase/fresh files and interrupted-write recovery");
  return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--assets', type=Path, required=True)
    p.add_argument('--alttpr-seed', type=Path)
    p.add_argument('--preset', type=int, choices=range(5))
    p.add_argument('--tour-steps', type=Path)
    p.add_argument('--qol-all', action='store_true', help='Enable every QoL feature for compatibility checks')
    a = p.parse_args()
    if a.alttpr_seed and a.preset is not None:
        p.error('Choose an official import or a native preset')
    out = a.work_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        p.error('Private inputs must stay outside the checkout')
    out.mkdir(parents=True, exist_ok=True)
    data = out / 'data'
    # The suite deliberately leaves a corrupt bundle at its end. Preserve the
    # evidence, but never reuse that state as a new generation's baseline.
    if data.exists():
        previous = out / 'data.previous'
        suffix = 1
        while previous.exists():
            previous = out / f'data.previous-{suffix}'
            suffix += 1
        data.rename(previous)
    data.mkdir()
    (data / 'saves').mkdir(exist_ok=True)
    shutil.copyfile(a.rom, data / 'input.sfc')
    shutil.copyfile(a.assets, data / 'zelda3_assets.dat')
    (data / 'zelda3.ini').write_text('[Graphics]\nNewRenderer=1\n[General]\nLanguage=en\n')
    source = out / 'randomizer_game_test.c'
    code=CODE
    if a.qol_all:
        code=code.replace('  ParseConfigFile(NULL);', '  Qol_LoadPreferences("qol.ini");for(int i=0;i<kQolCount;i++)require(Qol_SetOption(i,true),"QoL preference");Qol_LatchForBoot();\n  ParseConfigFile(NULL);',1)
    source.write_text(code)
    engine = ROOT / 'engine'
    sources = [s for s in (engine / 'src').glob('*.c')
               if s.name not in ('main.c', 'opengl.c', 'glsl_shader.c')]
    sources += list((engine / 'src/rando').glob('*.c')) + list((engine / 'snes').glob('*.c'))
    sources += [engine / 'src/platform/linux/second_screen_sdl.c',
                engine / 'third_party/sha256/sha256.c',
                engine / 'third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c']
    binary = out / 'randomizer_game_test'
    command = ['clang', '-O1', '-g', '-w', '-DSDL_MAIN_HANDLED',
               '-DSYSTEM_VOLUME_MIXER_AVAILABLE=0', '-ffunction-sections', '-fdata-sections',
               '-Wl,-dead_strip', '-fsanitize=address', '-fno-omit-frame-pointer',
               '-I', str(engine), '-iquote', str(engine / 'src'),
               '-I', str(engine / 'third_party/opus-1.3.1-stripped'),
               str(source), *(str(s) for s in sources), *sdl_flags(), '-o', str(binary)]
    with (out / 'build.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / 'build.log').read_text()[-8000:])
        return result.returncode
    run_args = [str(binary), str(data)]
    if a.alttpr_seed:
        run_args.append(str(a.alttpr_seed.resolve()))
    elif a.preset is not None:
        run_args.extend(['--preset', str(a.preset)])
    env=dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy')
    if a.tour_steps:
        (out/'tour').mkdir(exist_ok=True)
        env.update(ZELDA3_RANDO_TOUR_STEPS=str(a.tour_steps.resolve()),ZELDA3_RANDO_TOUR_OUT=str(out/'tour'))
    result = subprocess.run(run_args,
                            env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'test.log').write_text(result.stdout)
    print(result.stdout[-8000:])
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
