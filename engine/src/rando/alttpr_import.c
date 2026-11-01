#include "alttpr_import.h"
#include "customizer.h"
#include "item_ids.h"
#include "location_ids.h"
#include "dungeon_ids.h"
#include "rando_generate.h"
#include "rando_share.h"
#include "rando.h"
#include "../select_file.h"
#include "../config.h"
#include "../zelda_rtl.h"
#include "../variables.h"
#include "third_party/cjson/cJSON.h"
#include "third_party/sha256/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static RandoErrorCode last_error;
RandoErrorCode Alttpr_LastError(void) { return last_error; }
static bool fail(RandoErrorCode code, char *error, size_t size, const char *message) {
  last_error=code;
  if (error && size) snprintf(error, size, "%s", message);
  return false;
}
static const cJSON *get(const cJSON *object, const char *key) {
  return cJSON_GetObjectItemCaseSensitive(object, key);
}
static bool is(const cJSON *object, const char *key, const char *value) {
  const cJSON *node = get(object, key);
  return cJSON_IsString(node) && !strcmp(node->valuestring, value);
}
static bool unique_tree(const cJSON *node, unsigned depth, unsigned *count) {
  if (!node || depth > 32 || ++*count > 65536) return false;
  if(cJSON_IsObject(node) && cJSON_GetArraySize(node)>512) return false;
  for (const cJSON *a=node->child; a; a=a->next) {
    if (cJSON_IsObject(node)) {
      if (!a->string) return false;
      for (const cJSON *b=a->next; b; b=b->next)
        if (b->string && !strcmp(a->string,b->string)) return false;
    }
    if (!unique_tree(a,depth+1,count)) return false;
  }
  return true;
}
// Official multiworld-capable exporters suffix every location and item with
// :world-id. Accept only world 1; never silently discard another player's item.
static bool single_world_name(const char *input, char output[160]) {
  if (!input || strlen(input) >= 160) return false;
  strcpy(output,input);
  char *colon=strrchr(output,':');
  if (colon) { if (strcmp(colon,":1")) return false; *colon=0; }
  return output[0] != 0;
}
typedef struct Alias { const char *name; uint16 id; } Alias;
static const Alias aliases[] = {
  {"Boomerang",ITEM_BlueBoomerang},{"Powder",ITEM_MagicPowder},
  {"PegasusBoots",ITEM_Boots},{"Bow",ITEM_Bow},
  {"BlueShield",ITEM_FighterShield},{"HeartContainer",ITEM_BossHeartContainer},
  {"Bottle",ITEM_BottleEmpty},{"BottleWithGoldBee",ITEM_BottleWithGoodBee},
  {"OneRupee",ITEM_Rupee1},{"FiveRupees",ITEM_Rupee5},
  {"TwentyRupees",ITEM_Rupee20},{"FiftyRupees",ITEM_Rupee50},
  {"TwentyRupees2",ITEM_Rupee20},
  {"OneHundredRupees",ITEM_Rupee100},{"ThreeHundredRupees",ITEM_Rupee300},
  {"Arrow",ITEM_Arrow1},{"TenArrows",ITEM_Arrow10},
  {"ThreeBombs",ITEM_Bombs3},{"TenBombs",ITEM_Bombs10},
  {"PendantOfCourage",ITEM_Prize_GreenPendant},
  {"PendantOfPower",ITEM_Prize_RedPendant},
  {"PendantOfWisdom",ITEM_Prize_BluePendant},
  {"KeyH2",ITEM_SmallKey_HyruleCastleEscape},
  {"KeyA1",ITEM_SmallKey_HyruleCastleTower},{"KeyA2",ITEM_SmallKey_GanonsTower},
  {"MapH2",ITEM_Map_HyruleCastleEscape},
};
static uint16 resolve_item(const char *text) {
  char name[160]; if (!single_world_name(text,name)) return 0xffff;
  for (unsigned i=0;i<sizeof(aliases)/sizeof(*aliases);i++)
    if (!strcmp(name,aliases[i].name)) return aliases[i].id;
  if (!strncmp(name,"Crystal",7) && name[7]>='1' && name[7]<='7' && !name[8])
    return ITEM_Prize_Crystal1 + name[7]-'1';
  // ALTTPR dungeon family codes (P=pendant, D=dark-world, A=tower).
  static const char *suffix[]={"P1","P2","P3","D1","D2","D3","D4","D5","D6","D7","A2"};
  static const char *prefix[]={"Key","BigKey","Map","Compass"};
  static const uint16 base[]={ITEM_SmallKey_EasternPalace,ITEM_BigKey_EasternPalace,
                            ITEM_Map_EasternPalace,ITEM_Compass_EasternPalace};
  for (int p=0;p<4;p++) for (int d=0;d<11;d++) {
    char candidate[24]; snprintf(candidate,sizeof(candidate),"%s%s",prefix[p],suffix[d]);
    if (!strcmp(candidate,name)) return base[p]+d+(p==0 && d>=3 ? 1:0);
  }
  return Customizer_ResolveItem(name);
}
static bool numeric_crystals(const cJSON *meta, const char *new_key,
                              const char *old_key, uint8 *out) {
  const cJSON *node=get(meta,new_key); if(!node) node=get(meta,old_key);
  if (cJSON_IsString(node) && strlen(node->valuestring)==1 &&
      node->valuestring[0]>='0' && node->valuestring[0]<='7') {
    *out=node->valuestring[0]-'0'; return true;
  }
  if (cJSON_IsNumber(node) && node->valuedouble>=0 && node->valuedouble<=7 &&
      node->valuedouble==node->valueint) { *out=(uint8)node->valueint; return true; }
  return false;
}
static bool optional_is(const cJSON *object,const char *key,const char *value) {
  return !get(object,key) || is(object,key,value);
}

// Read only the explicit goal-count bytes from an official response. Never
// apply the patch to the ROM or infer a custom hunt target from its item count.
static bool goal_count(const cJSON *root,uint16 *out) {
  const cJSON *patch=get(root,"patch");if(!cJSON_IsArray(patch)) return false;
  unsigned seen=0,value=0;
  for(const cJSON *block=patch->child;block;block=block->next) {
    if(!cJSON_IsObject(block)) return false;
    for(const cJSON *write=block->child;write;write=write->next) {
      if(!write->string || !*write->string || !cJSON_IsArray(write)) return false;
      for(const char *c=write->string;*c;c++) if(*c<'0'||*c>'9') return false;
      char *end;unsigned long address=strtoul(write->string,&end,10);
      if(*end || address>0x400000ul) return false;
      unsigned index=0;
      for(const cJSON *byte=write->child;byte;byte=byte->next,index++) {
        unsigned long position=address+index;
        if(position!=0x180167ul && position!=0x180168ul) continue;
        unsigned bit=1u<<(position-0x180167ul);
        if((seen&bit) || !cJSON_IsNumber(byte) || byte->valuedouble!=byte->valueint ||
           byte->valueint<0 || byte->valueint>255) return false;
        seen|=bit;value|=(unsigned)byte->valueint<<((position-0x180167ul)*8);
      }
    }
  }
  if(seen!=3 || !value || value>232) return false;
  *out=(uint16)value;return true;
}

static bool vanilla_shops(const cJSON *shops) {
  if(!shops) return true;
  if(!cJSON_IsArray(shops) || cJSON_GetArraySize(shops)>10) return false;
  static const char *names[]={"Light World Kakariko Shop","Light World Lake Hylia Shop",
    "Light World Death Mountain Shop","Dark World Death Mountain Shop",
    "Dark World Potion Shop","Dark World Forest Shop","Dark World Lumberjack Hut Shop",
    "Dark World Outcasts Shop","Dark World Lake Hylia Shop","Capacity Upgrade"};
  unsigned seen=0;
  for(const cJSON *shop=shops->child;shop;shop=shop->next) {
    int kind=-1;
    for(int i=0;i<10;i++) if(is(shop,"location",names[i])) kind=i;
    if(kind<0 || (seen&(1u<<kind)) || !is(shop,"type","Shop")) return false;
    seen|=1u<<kind;
    int count=kind==9?2:3;
    for(int i=0;i<count;i++) {
      char key[16];snprintf(key,sizeof(key),"item_%d",i);
      const cJSON *entry=get(shop,key),*item=get(entry,"item"),*price=get(entry,"price");
      char actual[160];
      const char *want=i==0?"RedPotion":i==1?"Heart":"TenBombs";
      int cost=i==0?150:i==1?10:50;
      if(kind==4 || kind==6 || kind==7 || kind==8) {
        if(i==1) {want="BlueShield";cost=50;}
      } else if(kind==5) {
        want=i==0?"RedShield":i==1?"Bee":"TenArrows";
        cost=i==0?500:i==1?10:30;
      } else if(kind==9) {want=i==0?"BombUpgrade5":"ArrowUpgrade5";cost=100;}
      if(!cJSON_IsString(item)||!single_world_name(item->valuestring,actual)||strcmp(actual,want)||
         !cJSON_IsNumber(price)||price->valuedouble!=cost) return false;
    }
    if(kind==9 && get(shop,"item_2")) return false;
  }
  return true;
}

bool Alttpr_Parse(const char *json,size_t length,AlttprSeed *out,char *error,size_t size) {
  if (!json || !out || !length || length>kAlttprImportMaxBytes || memchr(json,0,length))
    return fail(kRandoError_InvalidFile,error,size,"Invalid or oversized ALttPR JSON file.");
  // cJSON exposes decoded strings as C strings; reject escaped NULs rather
  // than letting a hidden suffix alias a known metadata key or item name.
  for(size_t i=0;i+5<length;i++)
    if(json[i]=='\\' && json[i+1]=='u' && !memcmp(json+i+2,"0000",4))
      return fail(kRandoError_InvalidFile,error,size,"Embedded NUL escapes are not valid seed names.");
  // cJSON's length-bounded parser does not require an extra readable byte.
  const char *end=NULL;
  cJSON *root=cJSON_ParseWithLengthOpts(json,length,&end,0);
  unsigned nodes=0;
  bool valid=root && unique_tree(root,0,&nodes);
  while(valid && end<json+length && isspace((unsigned char)*end)) end++;
  valid=valid && end==json+length;
  if (!valid) { cJSON_Delete(root); return fail(kRandoError_InvalidFile,error,size,"Malformed JSON or duplicate fields."); }
  const cJSON *spoiler=get(root,"spoiler"); if(!spoiler) spoiler=root;
  const cJSON *meta=get(spoiler,"meta");
  const char *reason=NULL;
  RandoErrorCode error_kind=kRandoError_UnsupportedRules;
  AlttprSeed seed={0}; Settings_SetDefaults(&seed.settings);
  const cJSON *worlds=get(meta,"worlds");
  if (!cJSON_IsObject(meta) || (worlds && (!cJSON_IsNumber(worlds)||worlds->valuedouble!=1)))
    reason="Only single-world ALttPR spoiler files are supported.";
  if (!reason && (!is(meta,"logic","NoGlitches") || !is(meta,"rom_mode","NoGlitches") ||
      !is(meta,"item_pool","normal") || !is(meta,"item_functionality","normal") ||
      (!is(meta,"dungeon_items","standard") && !is(meta,"dungeon_items","mc") &&
       !is(meta,"dungeon_items","mcs") && !is(meta,"dungeon_items","full")) ||
      (!is(meta,"weapons","randomized") && !is(meta,"weapons","swordless") && !is(meta,"weapons","assured")) ||
      (!is(meta,"goal","ganon") && !is(meta,"goal","fast_ganon") && !is(meta,"goal","dungeons") && !is(meta,"goal","pedestal") &&
       !is(meta,"goal","triforce-hunt") && !is(meta,"goal","ganonhunt") && !is(meta,"goal","completionist")) ||
      (!is(meta,"mode","open")&&!is(meta,"mode","standard"))))
    reason="Unsupported ALttPR world, goal, item or weapon rules.";
  if (!reason && (!optional_is(meta,"enemizer.boss_shuffle","none") ||
      !optional_is(meta,"enemizer.enemy_shuffle","none") ||
      !optional_is(meta,"enemizer.enemy_damage","default") ||
      !optional_is(meta,"enemizer.enemy_health","default") ||
      !optional_is(meta,"enemizer.pot_shuffle","off") || cJSON_IsTrue(get(meta,"pseudoboots"))))
    reason="This seed needs unsupported enemy, pot or pseudoboots patches.";
  seed.settings.world_state=is(meta,"mode","standard") ? kWorldState_Standard:kWorldState_Open;
  seed.placement_advanced=is(meta,"item_placement","advanced");
  if(!reason && !seed.placement_advanced && !is(meta,"item_placement","basic"))
    reason="Unknown ALttPR item placement rules.";
  seed.settings.goal=kGoal_Ganon;
  if(is(meta,"goal","fast_ganon")) seed.settings.goal=kGoal_FastGanon;
  if(is(meta,"goal","dungeons")) seed.settings.goal=kGoal_Dungeons;
  if(is(meta,"goal","pedestal")) seed.settings.goal=kGoal_Pedestal;
  if(is(meta,"goal","triforce-hunt")) seed.settings.goal=kGoal_TriforceHunt;
  if(is(meta,"goal","ganonhunt")) seed.settings.goal=kGoal_GanonHunt;
  bool hunt=seed.settings.goal==kGoal_TriforceHunt || seed.settings.goal==kGoal_GanonHunt;
  if(!reason && hunt && !goal_count(root,&seed.settings.pieces_required))
    reason="Hunt import needs the official patch's explicit Triforce target.";
  if(is(meta,"weapons","swordless")) seed.settings.mode_weapons=kModeWeapons_Swordless;
  if(is(meta,"weapons","assured")) seed.settings.mode_weapons=kModeWeapons_Assured;
  if(is(meta,"goal","completionist")) seed.settings.goal=kGoal_Completionist;
  seed.settings.dungeon_small_keys_mode=kDungeonItemMode_Dungeon;
  seed.settings.dungeon_big_keys_mode=kDungeonItemMode_Dungeon;
  seed.settings.dungeon_maps_mode=kDungeonItemMode_Dungeon;
  seed.settings.dungeon_compasses_mode=kDungeonItemMode_Dungeon;
  if(!is(meta,"dungeon_items","standard")) {
    seed.settings.dungeon_maps_mode=seed.settings.dungeon_compasses_mode=kDungeonItemMode_Wild;
    if(is(meta,"dungeon_items","mcs") || is(meta,"dungeon_items","full"))
      seed.settings.dungeon_small_keys_mode=kDungeonItemMode_Wild;
    if(is(meta,"dungeon_items","full")) seed.settings.dungeon_big_keys_mode=kDungeonItemMode_Wild;
  }
  seed.settings.hints=0; // Exact official hint text is not part of the placement import.
  seed.settings.prize_shuffle=0; seed.settings.medallion_shuffle=0;
  if (!reason && (!numeric_crystals(meta,"entry_crystals_ganon","crystals_ganon",&seed.settings.crystals_ganon) ||
      !numeric_crystals(meta,"entry_crystals_tower","crystals_tower",&seed.settings.crystals_tower)))
    reason="Missing resolved crystal requirements.";
  if (!reason && !vanilla_shops(get(spoiler,"Shops")))
    reason="Custom shops, prices and take-any caves are not supported yet.";
  const cJSON *equipped=get(spoiler,"Equipped");
  if(!reason) {
    unsigned hearts=0,swords=0;
    if(!equipped || !cJSON_IsObject(equipped)) reason="Missing explicit starting inventory.";
    else for(const cJSON *v=equipped->child;!reason && v;v=v->next) {
      uint16 item=cJSON_IsString(v)?resolve_item(v->valuestring):0xffff;
      if(item==ITEM_BossHeartContainer) hearts++;
      else if((item==ITEM_ProgressiveSword || item==ITEM_L1Sword) &&
              seed.settings.mode_weapons==kModeWeapons_Assured) swords++;
      else reason="Custom starting inventory is not supported yet.";
    }
    if(!reason && (hearts!=3 || swords!=(seed.settings.mode_weapons==kModeWeapons_Assured)))
      reason="Starting inventory does not match these weapon rules.";
    seed.starting_sword=(uint8)swords;
  }
  unsigned bottle_mask=0;
  uint16 items[236]; for(int i=0;i<236;i++) items[i]=0xffff;
  for (const cJSON *section=spoiler->child; !reason && section; section=section->next) {
    const char *name=section->string;
    if (!name) {reason="Invalid spoiler section.";break;}
    if (!strcmp(name,"meta")||!strcmp(name,"playthrough")||!strcmp(name,"Bosses")||
        !strcmp(name,"Shops")||!strcmp(name,"Equipped")) continue;
    if (!cJSON_IsObject(section)) {reason="Invalid placement section.";break;}
    for (const cJSON *entry=section->child; entry; entry=entry->next) {
      char location[160];
      if (!single_world_name(entry->string,location)||!cJSON_IsString(entry)) {
        reason="Invalid or multiworld placement.";break;
      }
      if (!strcmp(location,"Waterfall Bottle")||!strcmp(location,"Pyramid Bottle")) {
        int index=!strcmp(location,"Pyramid Bottle"); uint16 item=resolve_item(entry->valuestring);
        if ((bottle_mask&(1u<<index)) || item<ITEM_BottleWithFairy||item>ITEM_BottleWithBluePotion) {reason="Unsupported or duplicate fairy bottle.";break;}
        bottle_mask|=1u<<index;
        seed.bottles[index]=(uint8)item; continue;
      }
      uint16 id=Customizer_ResolveLocation(location), item=resolve_item(entry->valuestring);
      if (id>=236 || item==0xffff || (item>=ITEM_StartingHeart && item<=ITEM_DefeatAgahnim) || items[id]!=0xffff ||
          (item==ITEM_Triforce && (id!=LOC_Master_Sword_Pedestal || seed.settings.goal!=kGoal_Pedestal))) {
        if (error && size) snprintf(error,size,"Unknown, duplicate or unsupported placement: %s",location);
        reason="";break;
      }
      items[id]=item;
    }
  }
  if(!reason && seed.settings.goal==kGoal_Pedestal && items[LOC_Master_Sword_Pedestal]!=ITEM_Triforce)
    reason="Pedestal goal requires its exact Triforce reward.";
  // The official spoiler excludes fixed story events; these are identities,
  // never randomized fill. Every physical location and both medallions remain
  // mandatory, including dungeon prizes and boss heart drops.
  items[LOC_Zelda]=ITEM_RescuedZelda;
  items[LOC_Agahnim]=ITEM_DefeatAgahnim;
  items[LOC_Agahnim_2]=ITEM_DefeatAgahnim;
  items[LOC_Ganon]=ITEM_DefeatAgahnim;
  memset(seed.prizes,0xff,sizeof(seed.prizes)); unsigned prize_mask=0;
  for (uint32 i=0;!reason && i<kRandoLocationsCount;i++) {
    const RandoLocationDef *location=&kRandoLocations[i];
    if (location->id>=236) continue;
    uint16 item=items[location->id];
    if (item==0xffff || seed.count>=kAlttprImportLocations) {error_kind=kRandoError_MissingPlacements;reason="Incomplete spoiler: exact placements are required; hidden/race seeds cannot be imported.";break;}
    seed.placements[seed.count++]=(RandoPlacement){location->id,item};
    if (item>=ITEM_Prize_GreenPendant && item<=ITEM_Prize_Crystal7) {
      int dungeon=-1;
      for(int d=0;d<kRandoDungeon_Count;d++) if(Rando_GetDungeonPrizeLocation(d)==location->id) dungeon=d;
      unsigned bit=1u<<(item-ITEM_Prize_GreenPendant);
      if (dungeon<0 || (prize_mask&bit)) {reason="Invalid dungeon prize assignment.";break;}
      prize_mask|=bit; seed.prizes[dungeon]=(uint8)(item-ITEM_Prize_GreenPendant);
    }
  }
  if(!reason && hunt) {
    unsigned pieces=0;for(unsigned i=0;i<seed.count;i++) pieces+=seed.placements[i].item_id==ITEM_TriforcePiece;
    seed.settings.pieces_placed=(uint16)pieces;
    if(pieces<seed.settings.pieces_required) reason="Hunt target exceeds the exact placed pieces.";
  }
  seed.medallions[0]=(uint8)items[LOC_Misery_Mire_Medallion];
  seed.medallions[1]=(uint8)items[LOC_Turtle_Rock_Medallion];
  if (!reason && (seed.count!=kAlttprImportLocations || prize_mask!=1023 ||
      seed.medallions[0]<ITEM_Bombos||seed.medallions[0]>ITEM_Quake||
      seed.medallions[1]<ITEM_Bombos||seed.medallions[1]>ITEM_Quake ||
      seed.bottles[0]<ITEM_BottleWithFairy||seed.bottles[0]>ITEM_BottleWithBluePotion ||
      seed.bottles[1]<ITEM_BottleWithFairy||seed.bottles[1]>ITEM_BottleWithBluePotion))
    reason="Missing or invalid prizes, medallions or fairy bottles.";
  if (!reason) {
    RandoPlacementTable table={seed.placements,seed.count};
    PlacementTable_ComputeDigest(&table,seed.placement_digest);
    SHA256_CTX sha; sha256_init(&sha);sha256_update(&sha,(const uint8 *)json,length);sha256_final(&sha,seed.source_digest);
    for(int i=0;i<8;i++) seed.identity_seed|=(uint64)seed.source_digest[i]<<(8*i);
    const cJSON *hash=get(root,"hash");
    if(cJSON_IsString(hash) && strlen(hash->valuestring)<sizeof(seed.public_hash)) {
      bool printable=true;
      for(const unsigned char *p=(const unsigned char *)hash->valuestring;*p;p++)
        if(!isalnum(*p)) printable=false;
      if(printable) snprintf(seed.public_hash,sizeof(seed.public_hash),"%s",hash->valuestring);
    }
    *out=seed;
  }
  cJSON_Delete(root);
  if (reason) {last_error=error_kind;return reason[0] ? fail(error_kind,error,size,reason):false;}
  last_error=kRandoError_None;
  if(error && size) error[0]=0;
  return true;
}

bool Alttpr_Load(const char *path,AlttprSeed *out,char *error,size_t size) {
  FILE *f=path?fopen(path,"rb"):NULL;
  if(!f) return fail(kRandoError_Storage,error,size,"Unable to read ALttPR JSON.");
  bool ok=fseek(f,0,SEEK_END)==0;long n=ok?ftell(f):-1;
  ok=n>0 && n<=kAlttprImportMaxBytes && fseek(f,0,SEEK_SET)==0;
  char *text=ok?malloc((size_t)n):NULL;
  ok=text && fread(text,1,(size_t)n,f)==(size_t)n;
  fclose(f);
  if(ok) ok=Alttpr_Parse(text,(size_t)n,out,error,size);
  else fail(kRandoError_Storage,error,size,"Unable to read bounded ALttPR JSON.");
  free(text);return ok;
}

static bool active;
static AlttprSeed active_seed;
static uint16 desert_from,desert_to,turtle_from,turtle_to,ice_from,ice_to;
void Alttpr_Clear(void) { active=false; memset(&active_seed,0,sizeof(active_seed)); }
void Alttpr_Install(const AlttprSeed *seed) {
  if(!seed) { Alttpr_Clear();return; }
  active_seed=*seed; active=true;
  desert_from=Rando_FindRegionByName("LightWorld_South");
  desert_to=Rando_FindRegionByName("DesertPalace_Lobby");
  turtle_from=Rando_FindRegionByName("LightWorld_DeathMountain_East");
  turtle_to=Rando_FindRegionByName("TurtleRock_Entrance");
  ice_from=Rando_FindRegionByName("IcePalace_Lobby");
  ice_to=Rando_FindRegionByName("IcePalace_BossRoom");
}
bool Alttpr_IsActive(void) { return active; }
void Alttpr_AddStartingCounts(RandoCounts *counts) {
  if(active && counts && active_seed.starting_sword)
    counts->by_item_id[ITEM_ProgressiveSword]+=active_seed.starting_sword;
}
bool Alttpr_HasSewerLightCone(void) {
  // ALTTPR lampmantlecone.asm: LampCheck / Rom::setStandardMode.
  // The sewer cone is independent of Lamp ownership, including after rescue.
  return active && Rando_IsActive() && active_seed.settings.world_state==kWorldState_Standard &&
         player_is_indoors && cur_palace_index_x2==0;
}
bool Alttpr_ApplyEscapeRefill(void) {
  if(!active || !Rando_IsActive() || active_seed.settings.world_state!=kWorldState_Standard ||
     sram_progress_indicator>=2) return false;
  uint16 item=0xffff;
  for(unsigned i=0;i<active_seed.count;i++)
    if(active_seed.placements[i].location_id==LOC_Link_s_Uncle) item=active_seed.placements[i].item_id;
  // World::setEscapeFills selects the supply from Uncle, not from
  // unopened chests. Module05_LoadFile calls this after every load/death;
  // progress 1 is still the escape, so the native once-per-boot gate must not
  // suppress the Zelda/mantle respawn refills.
  bool uncle=which_starting_point!=2 && which_starting_point!=4;
  if(item==ITEM_FireRod || item==ITEM_CaneOfSomaria || item==ITEM_CaneOfByrna)
    link_magic_filler=uncle?0x80:0x20;
  else if(item==ITEM_Bow || item==ITEM_ProgressiveBow)
    link_arrow_filler=uncle?70:10;
  else if(item==ITEM_Bombs1 || item==ITEM_Bombs3 || item==ITEM_Bombs10)
    link_bomb_filler=uncle?50:3;
  else return false;
  return true;
}
bool Alttpr_Assignments(uint8 prizes[13],uint8 medallions[2]) {
  if(!active) return false;
  memcpy(prizes,active_seed.prizes,13);memcpy(medallions,active_seed.medallions,2);
  return true;
}
bool Alttpr_ValidateSlot(const RandoSidecarSlot *slot) {
  if(!active) return true;
  if(!slot || slot->placement_count!=active_seed.count ||
     memcmp(slot->placements,active_seed.placements,sizeof(RandoPlacement)*active_seed.count)) return false;
  uint8 canonical[kSettingsCanonicalLen]; Settings_CanonicalSerialize(&active_seed.settings,canonical);
  return slot->header.settings_present && !memcmp(canonical,slot->settings_canonical,sizeof(canonical));
}
bool Alttpr_RefillBottle(bool dark) {
  if(!active) return false;
  // Engine bottle contents: 3 red, 4 green, 5 blue, 6 fairy, 7 bee, 8 good bee.
  static const uint8 contents[]={6,7,8,3,4,5};
  uint8 item=active_seed.bottles[dark?1:0];
  if(item<ITEM_BottleWithFairy || item>ITEM_BottleWithBluePotion) return false;
  for(int i=0;i<4;i++) if(link_bottle_info[i]==2) {
    link_bottle_info[i]=contents[item-ITEM_BottleWithFairy]; return true;
  }
  return false;
}

bool Alttpr_CreateSlot(const AlttprSeed *seed,int index,char *error,size_t size) {
  if(!seed || index<0 || index>=3 || seed->count!=kAlttprImportLocations ||
     !Settings_Validate(&seed->settings)) return fail(kRandoError_InvalidFile,error,size,"Invalid imported seed.");
  RandoPlacementTable table={(RandoPlacement *)seed->placements,seed->count};
  // Validate this EXACT table. No native generation, filler or RNG translation.
  Rando_SetDungeonPrizeAssignment(seed->prizes);
  Rando_SetMedallionAssignment(seed->medallions);
  bool previously_active=active;
  AlttprSeed previous=active_seed;
  Alttpr_Install(seed);
  bool acceptable=Goal_IsCompletable(&seed->settings,seed->identity_seed,&table);
  if(!acceptable) {
    RandoSpheres spheres; (void)Logic_ComputeSpheres(&seed->settings,&table,&spheres);
    for(unsigned i=0;i<table.count;i++) if(spheres.sphere_index_by_placement[i]==kSphereIndexUnreachable)
      fprintf(stderr,"ALttPR unreachable: %u %s -> %u %s\n",table.entries[i].location_id,
          Rando_GetLocationName(table.entries[i].location_id),table.entries[i].item_id,
          Rando_GetItemName(table.entries[i].item_id));
  }
  if(previously_active) Alttpr_Install(&previous); else Alttpr_Clear();
  Rando_SetDungeonPrizeAssignment(NULL);Rando_SetMedallionAssignment(NULL);
  if(!acceptable) return fail(kRandoError_Uncertified,error,size,"The native logic cannot certify this exact seed. No save was created.");
  RandoSidecarSlot *slot=calloc(1,sizeof(*slot));
  if(!slot) return fail(kRandoError_Storage,error,size,"Not enough memory for imported seed.");
  slot->header.slot_kind=kSlotKind_Randomizer;
  slot->header.generator_version=kGeneratorVersion;
  uint8 settings_hash[32]; Settings_ComputeHash(&seed->settings,settings_hash);
  memcpy(slot->header.settings_hash,settings_hash,16);
  ShareString share={.version=kGeneratorVersion,.seed_u64=seed->identity_seed};
  memcpy(share.settings_hash,settings_hash,16);
  Share_PackBinary(&share,slot->header.share_string);
  slot->header.settings_ext_present=1;slot->header.settings_present=1;
  slot->header.world_state=seed->settings.world_state;
  slot->header.goal=seed->settings.goal;
  Settings_CanonicalSerialize(&seed->settings,slot->settings_canonical);
  Rando_StampSlotRegistries(&slot->header);
  slot->header.recommended_features0=g_config.features0;
  slot->header.recommended_features0_present=1;
  slot->placement_count=seed->count;
  memcpy(slot->placements,seed->placements,seed->count*sizeof(RandoPlacement));
  slot->header.placement_table_size=(seed->placements[seed->count-1].location_id+1)*2;
  uint8 sram[0x500];Rando_InitNewSlotSram(sram,seed->settings.world_state);
  sram[0x359]=seed->starting_sword; // exact Equipped sword; never re-granted on reload
  Intro_FixCksum(sram);
  bool ok=Rando_WriteSidecarSlot(index,slot,sram,sizeof(sram));free(slot);
  if(!ok) return fail(kRandoError_Storage,error,size,"Unable to commit imported seed save.");
  memcpy(g_zenv.sram+index*0x500,sram,sizeof(sram));
  memcpy(g_zenv.sram+index*0x500+0xf00,sram,sizeof(sram));
  ZeldaWriteSram();SelectFile_NotifySlotWritten(index);Alttpr_Install(seed);
  last_error=kRandoError_None;
  return true;
}

// ALTTPR ThievesTown.php:77-84 has a placement-dependent self-key branch.
// The native generator deliberately uses the conservative else branch, which
// cannot certify an official small key placed in this particular big chest.
// Apply the official branch only to an exact imported table, after the normal
// region gate; do not grant a free key or relax any other location.
static uint16 imported_item(uint16 location) {
  for(unsigned i=0;i<active_seed.count;i++)
    if(active_seed.placements[i].location_id==location) return active_seed.placements[i].item_id;
  return 0xffff;
}
bool Alttpr_AdditionalRegionRoute(uint16 from,uint16 to,const RandoCounts *counts) {
  if(!active || !counts) return false;
  // Standard/DesertPalace.php:122-128. Mirror from the reachable Mire
  // overworld is a normal NoGlitches entrance, not a boots-clip trick.
  const uint16 *n=counts->by_item_id;
  bool dark_rocks=n[ITEM_TitanMitt] || n[ITEM_ProgressiveGlove]>=2;
  if(from==desert_from && to==desert_to)
    return n[ITEM_RescuedZelda] && n[ITEM_MagicMirror] && n[ITEM_OcarinaInactive] && dark_rocks;
  // Standard/TurtleRock.php:87-101: the upper approach uses the liftable
  // portal rock. Mirror is not required; retain the separate medallion,
  // sword/swordless and Somaria lobby gate.
  if(from==turtle_from && to==turtle_to)
    return n[ITEM_MoonPearl] && n[ITEM_Hammer] && dark_rocks;
  // Codegen splits the boss into its own region. Its access edge must use
  // the same one-key advanced route as the boss/prize, not the old two-key gate.
  if(from==ice_from && to==ice_to)
    return Alttpr_LocationReachability(LOC_Ice_Palace_Boss,counts)==1;
  return false;
}
// Ordinary boss combat rules from ALTTPR app/Boss.php:70-132 (MIT).
// Imported seeds prohibit boss/enemy shuffle and non-normal functionality.
int Alttpr_CanKillBoss(uint8 dungeon,const RandoCounts *counts) {
  if(!active || !counts) return -1;
  const uint16 *n=counts->by_item_id;
  unsigned sword=n[ITEM_ProgressiveSword]+(n[ITEM_L1Sword]!=0);
  if(n[ITEM_L2Sword] && sword<2) sword=2;
  if(n[ITEM_L3Sword] && sword<3) sword=3;
  if(n[ITEM_L4Sword] && sword<4) sword=4;
  unsigned bottles=0;for(unsigned i=ITEM_BottleEmpty;i<=ITEM_BottleWithBluePotion;i++) bottles+=n[i];
  if(bottles>4) bottles=4;
  unsigned magic=(n[ITEM_QuarterMagic]?4:n[ITEM_HalfMagic]?2:1)*(1+bottles);
  bool advanced=active_seed.placement_advanced;
  bool swordless=active_seed.settings.mode_weapons==kModeWeapons_Swordless;
  bool bow=n[ITEM_Bow] || n[ITEM_ProgressiveBow];
  bool hammer=n[ITEM_Hammer],fire=n[ITEM_FireRod],ice=n[ITEM_IceRod];
  bool canes=n[ITEM_CaneOfSomaria] || n[ITEM_CaneOfByrna];
  bool melt=fire || (n[ITEM_Bombos] && (swordless || sword));
  switch(dungeon) {
  case kRandoDungeon_EasternPalace:
    return sword || hammer || bow || n[ITEM_BlueBoomerang] || n[ITEM_RedBoomerang] ||
           (magic>=4 && (fire || ice)) || (magic>=2 && canes);
  case kRandoDungeon_DesertPalace: return sword || hammer || bow || fire || ice || canes;
  case kRandoDungeon_TowerOfHera: return sword || hammer;
  case kRandoDungeon_HyruleCastleTower:
  case kRandoDungeon_GanonsTower: return sword || hammer || n[ITEM_BugCatchingNet];
  case kRandoDungeon_PalaceOfDarkness: return sword>=2 || bow || (advanced && sword);
  case kRandoDungeon_SwampPalace:
    return (advanced || swordless || sword>=2) && n[ITEM_Hookshot] &&
           (hammer || sword || ((magic>=2 || bow) && (fire || ice)));
  case kRandoDungeon_SkullWoods:
    return (advanced || sword>=2 || (magic>=2 && fire)) &&
           (sword || hammer || (magic>=2 && (fire || canes)) ||
            (n[ITEM_BugCatchingNet] && bottles && (n[ITEM_Boots] || (sword && n[ITEM_Quake]))));
  case kRandoDungeon_ThievesTown:
    return (advanced || swordless || (sword && (n[ITEM_Cape] || n[ITEM_CaneOfByrna]))) &&
           (sword || hammer || canes);
  case kRandoDungeon_IcePalace:
    return (advanced || sword>=2 || (magic>=3 && fire) ||
            (n[ITEM_Bombos] && (swordless || sword) && magic>=2 && fire)) && melt &&
           (hammer || sword || (magic>=3 && fire) || (magic>=2 && fire && n[ITEM_Bombos] && swordless));
  case kRandoDungeon_MiseryMire:
    return (advanced || sword>=2 || bow) && (hammer || sword || bow);
  case kRandoDungeon_TurtleRock:
    return fire && ice && (advanced || swordless || sword>=3 || (magic>=2 && sword>=2)) &&
           (sword>=3 || hammer || (magic>=2 && sword>=2) || (magic>=4 && sword));
  default: return -1;
  }
}

int Alttpr_LocationReachability(uint16 location,const RandoCounts *counts) {
  if(!active || !counts) return -1;
  const uint16 *n=counts->by_item_id;
  // The Open sanctuary can be entered directly from the overworld; it does
  // not inherit Standard's forward-sewer escort/key/Lamp requirements.
  if(active_seed.settings.world_state==kWorldState_Open && location==LOC_Sanctuary) return 1;
  if(active_seed.settings.world_state==kWorldState_Standard) {
    const uint16 *n=counts->by_item_id;
    // Standard/HyruleCastleEscape.php:67-97. Standard supplies its sewer
    // cone without a Lamp; the native generator's conservative Lamp gate
    // must not strand an exact official seed. Retain weapon and key gates.
    bool combat=n[ITEM_ProgressiveSword] || n[ITEM_L1Sword] || n[ITEM_L2Sword] ||
        n[ITEM_L3Sword] || n[ITEM_L4Sword] || n[ITEM_Hammer] || n[ITEM_FireRod] ||
        n[ITEM_CaneOfSomaria] || n[ITEM_CaneOfByrna] || n[ITEM_Bombs1] ||
        n[ITEM_Bombs3] || n[ITEM_Bombs10] || n[ITEM_Bow] || n[ITEM_ProgressiveBow];
    if(location==LOC_Sewers_Dark_Cross || location==LOC_Hyrule_Castle_Map_Chest ||
       location==LOC_Secret_Passage || location==LOC_Hyrule_Castle_Boomerang_Chest ||
       location==LOC_Hyrule_Castle_Zelda_s_Cell) return combat;
    if(location==LOC_Sanctuary || location==LOC_Zelda ||
       (location>=LOC_Sewers_Secret_Room_Left && location<=LOC_Sewers_Secret_Room_Right))
      return combat && n[ITEM_SmallKey_HyruleCastleEscape];
  }
  if(location==LOC_Thieves_Town_Big_Chest && imported_item(location)==ITEM_SmallKey_ThievesTown)
    return counts->by_item_id[ITEM_Hammer]>0 && counts->by_item_id[ITEM_BigKey_ThievesTown]>0;
  // Standard/PalaceOfDarkness.php:89-164. The key inside either locked
  // check changes the route cost; the conservative native graph omits this.
  bool bow=n[ITEM_Bow] || n[ITEM_ProgressiveBow];
  bool pod_back=n[ITEM_Hammer] && bow && n[ITEM_Lamp];
  unsigned pod_keys=n[ITEM_SmallKey_PalaceOfDarkness];
  if(location==LOC_Palace_of_Darkness_Big_Key_Chest)
    return pod_keys>=(imported_item(location)==ITEM_SmallKey_PalaceOfDarkness?1:pod_back?6:5);
  if(location==LOC_Palace_of_Darkness_Harmless_Hellway)
    return pod_keys>=(imported_item(location)==ITEM_SmallKey_PalaceOfDarkness?(pod_back?4:3):(pod_back?6:5));
  if(location==LOC_Palace_of_Darkness_Compass_Chest)
    return pod_keys>=(pod_back?4:3);
  if(location==LOC_Palace_of_Darkness_Big_Chest ||
     location==LOC_Palace_of_Darkness_Dark_Maze_Top || location==LOC_Palace_of_Darkness_Dark_Maze_Bottom)
    return n[ITEM_Lamp] && (location!=LOC_Palace_of_Darkness_Big_Chest || n[ITEM_BigKey_PalaceOfDarkness]) &&
           pod_keys>=((n[ITEM_Hammer] && bow)?6:5);
  // Standard/MiseryMire.php:94-112: either west chest containing the big
  // key saves a small key. No free key, torch, or dungeon-entry bypass.
  if(location==LOC_Misery_Mire_Big_Key_Chest || location==LOC_Misery_Mire_Compass_Chest) {
    bool west_big=imported_item(LOC_Misery_Mire_Big_Key_Chest)==ITEM_BigKey_MiseryMire ||
                  imported_item(LOC_Misery_Mire_Compass_Chest)==ITEM_BigKey_MiseryMire;
    return (n[ITEM_Lamp] || n[ITEM_FireRod]) && n[ITEM_SmallKey_MiseryMire]>=(west_big?2:3);
  }
  // IcePalace.php:108-117: advanced placement permits the Somaria block
  // route with one key. Preserve the native boss combat predicate.
  if(active_seed.placement_advanced && (location==LOC_Ice_Palace_Boss || location==LOC_Ice_Palace_Prize)) {
    static const uint8 kill[]={OP_CAN_KILL_BOSS,kRandoDungeon_IcePalace};
    bool rocks=n[ITEM_PowerGlove] || n[ITEM_TitanMitt] || n[ITEM_ProgressiveGlove];
    return n[ITEM_Hammer] && rocks && n[ITEM_BigKey_IcePalace] &&
           (n[ITEM_SmallKey_IcePalace]>=2 || (n[ITEM_SmallKey_IcePalace] && n[ITEM_CaneOfSomaria])) &&
           Predicate_Evaluate(kill,sizeof(kill),counts,&active_seed.settings);
  }
  // Standard/GanonsTower.php:214-252,267-274,295-333. A big key in another
  // chest in the same group reduces four small keys to three; exclude self.
  uint16 group=0xffff;
  if(location>=LOC_Ganon_s_Tower_Randomizer_Room_Top_Left && location<=LOC_Ganon_s_Tower_Randomizer_Room_Bottom_Right)
    group=LOC_Ganon_s_Tower_Randomizer_Room_Top_Left;
  if(location>=LOC_Ganon_s_Tower_Compass_Room_Top_Left && location<=LOC_Ganon_s_Tower_Compass_Room_Bottom_Right)
    group=LOC_Ganon_s_Tower_Compass_Room_Top_Left;
  if(group!=0xffff) {
    bool early=false;
    for(uint16 id=group;id<group+4;id++)
      if(id!=location && imported_item(id)==ITEM_BigKey_GanonsTower) early=true;
    bool tools=group==LOC_Ganon_s_Tower_Randomizer_Room_Top_Left ?
      n[ITEM_Hammer] && n[ITEM_Hookshot] : n[ITEM_FireRod] && n[ITEM_CaneOfSomaria];
    return tools && n[ITEM_SmallKey_GanonsTower]>=(early?3:4);
  }
  if(location==LOC_Ganon_s_Tower_Map_Chest) {
    uint16 item=imported_item(location);
    return n[ITEM_Hammer] && (n[ITEM_Hookshot] || (active_seed.placement_advanced && n[ITEM_Boots])) &&
           n[ITEM_SmallKey_GanonsTower]>=((item==ITEM_SmallKey_GanonsTower || item==ITEM_BigKey_GanonsTower)?3:4);
  }
  // GanonsTower.php:254-263: Firesnake itself may hold the third key;
  // alternatively, a big key in any Randomizer Room chest opens this route.
  if(location==LOC_Ganon_s_Tower_Firesnake_Room) {
    bool early=imported_item(location)==ITEM_SmallKey_GanonsTower;
    for(uint16 id=LOC_Ganon_s_Tower_Randomizer_Room_Top_Left;
        id<=LOC_Ganon_s_Tower_Randomizer_Room_Bottom_Right;id++)
      early=early || imported_item(id)==ITEM_BigKey_GanonsTower;
    return counts->by_item_id[ITEM_Hammer]>0 && counts->by_item_id[ITEM_Hookshot]>0 &&
           counts->by_item_id[ITEM_SmallKey_GanonsTower]>=(early?2:3);
  }
  return -1;
}
