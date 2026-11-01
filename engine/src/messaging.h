#pragma once
#include "types.h"

// Vanilla overworld pause-map prize-marker data. Declared here (rather than as
// local externs at the consumer) so the compiler type-checks these against
// their definitions in messaging.c. rando/ow_map_prizes.c re-keys the markers
// by DUNGEON under prize_shuffle and shares this one source of truth with its
// oracle; see fix-ow-map-prize-markers.
//   kOwMapCrystal_tab[slot][k] — icon word `ch << 8 | flags` for marker slot
//     0..6 in map-icon state k (savegame_map_icons_indicator, 0..8); a 0 word
//     means vanilla's blinking "unknown" marker.
//   kOverworldMapData[spr - 8] — crystal-NUMBER glyph baked into each OAM slot
//     (marker slot i draws as sprite 14 - i).
//   kPendantBitMask / kCrystalBitMask — the prize bit each marker slot tests in
//     link_which_pendants / link_has_crystals. Cross-referencing these against
//     kDungeonCrystalPendantBit (zelda_rtl.c) is what identifies the DUNGEON a
//     slot points at, independently of the icon tables.
extern const uint16 *const kOwMapCrystal_tab[7];
extern const uint8 kOverworldMapData[7];
extern const uint8 kPendantBitMask[3];
extern const uint8 kCrystalBitMask[7];

const uint8 *GetDungmapFloorLayout();
uint8 GetOtherDungmapInfo(int count);
const uint16 *GetDungmapRoomShape(int idx);
void DungMap_4();
void Module_Messaging_6();
void OverworldMap_SetupHdma();
const uint8 *GetLightOverworldTilemap();
void SaveGameFile();
void SaveGameFile_StagingSelfCheck(void);
void TransferMode7Characters();
void Module0E_Interface();
void Module_Messaging_0();
void Module0E_05_DesertPrayer();
void Module0E_04_RedPotion();
void Module0E_08_GreenPotion();
void Module0E_09_BluePotion();
void Module0E_0B_SaveMenu();
void Module1B_SpawnSelect();
void CleanUpAndPrepDesertPrayerHDMA();
void DesertPrayer_InitializeIrisHDMA();
void DesertPrayer_BuildIrisHDMATable();
Pair16U DesertHDMA_CalculateIrisShapeLine();
void Animate_GAMEOVER_Letters();
void GameOverText_SweepLeft();
void GameOverText_UnfurlRight();
void Module12_GameOver();
void GameOver_AdvanceImmediately();
void Death_Func1();
void GameOver_DelayBeforeIris();
void GameOver_IrisWipe();
void GameOver_SplatAndFade();
void Death_Func6();
void Death_Func4();
void Animate_GAMEOVER_Letters_bounce();
void GameOver_Finalize_GAMEOVR();
void GameOver_SaveAndOrContinue();
void Death_Func15(bool count_as_death);
void GameOver_AnimateChoiceFairy();
void GameOver_InitializeRevivalFairy();
void RevivalFairy_Main_bounce();
void GameOver_RiseALittle();
void GameOver_Restore0D();
void GameOver_Restore0E();
void GameOver_ResituateLink();
void Module0E_0A_FluteMenu();
void FluteMenu_HandleSelection();
void FluteMenu_LoadSelectedScreen();
void Overworld_LoadOverlayAndMap();
void FluteMenu_FadeInAndQuack();
void BirdTravel_Finish_Doit();
void Messaging_OverworldMap();
void WorldMap_FadeOut();
void WorldMap_LoadLightWorldMap();
void WorldMap_LoadDarkWorldMap();
void WorldMap_LoadSpriteGFX();
void WorldMap_Brighten();
void WorldMap_PlayerControl();
void WorldMap_RestoreGraphics();
void Attract_SetUpConclusionHDMA();
void WorldMap_ExitMap();
void WorldMap_SetUpHDMA();
void WorldMap_FillTilemapWithEF();
void WorldMap_HandleSprites();
bool OverworldMap_CheckForPendant(int k);
bool OverworldMap_CheckForCrystal(int k);
void Module0E_03_DungeonMap();
void Module0E_03_01_DrawMap();
void Module0E_03_01_00_PrepMapGraphics();
void Module0E_03_01_01_DrawLEVEL();
void Module0E_03_01_02_DrawFloorsBackdrop();
void DungeonMap_BuildFloorListBoxes(uint8 t5, uint16 r14);
void Module0E_03_01_03_DrawRooms();
void DungeonMap_DrawBorderForRooms(uint16 pd, uint16 mask);
void DungeonMap_DrawFloorNumbersByRoom(uint16 pd, uint16 r8);
void DungeonMap_DrawDungeonLayout(int pd);
void DungeonMap_DrawSingleRowOfRooms(int i, int arg_x);
void DungeonMap_DrawRoomMarkers();
void DungeonMap_HandleInputAndSprites();
void DungeonMap_HandleInput();
void DungeonMap_HandleMovementInput();
void DungeonMap_HandleFloorSelect();
void DungeonMap_ScrollFloors();
void DungeonMap_DrawSprites();
void DungeonMap_DrawLinkPointing(int spr_pos, uint8 r2, uint8 r3);
int DungeonMap_DrawBlinkingIndicator(int spr_pos);
int DungeonMap_DrawLocationMarker(int spr_pos, uint16 r14);
int DungeonMap_DrawFloorNumberObjects(int spr_pos);
void DungeonMap_DrawFloorBlinker();
int DungeonMap_DrawBossIcon(int spr_pos);
int DungeonMap_DrawBossIconByFloor(int spr_pos);
void DungeonMap_RecoverGFX();
void ToggleStarTilesAndAdvance();
void Death_InitializeGameOverLetters();
void CopySaveToWRAM();
void RenderText();
void RenderText_PostDeathSaveOptions();
void Text_Initialize();
void Text_Initialize_initModuleStateLoop();
void Text_InitVwfState();
// True when this runtime message is an eligible story fast-forward surface.
// Active generated/dynamic randomizer hints are always excluded.
bool Text_ShouldFastForwardStoryMessage(uint16 msg_id);
void Text_LoadCharacterBuffer();
uint8 *Text_WritePlayerName(uint8 *p);
uint8 Text_FilterPlayerNameCharacters(uint8 a);
void Text_Render();
void RenderText_Draw_Border();
void RenderText_Draw_BorderIncremental();
void RenderText_Draw_CharacterTilemap();
void RenderText_Draw_MessageCharacters();
void RenderText_Draw_Finish();
void VWF_RenderSingle(int c);
void RenderText_Draw_Choose2LowOr3();
void RenderText_Draw_ChooseItem();
void RenderText_FindYItem_Previous();
void RenderText_FindYItem_Next();
void RenderText_DrawSelectedYItem();
void RenderText_Draw_Choose2HiOr3();
void RenderText_Draw_Choose3();
void RenderText_Draw_Choose1Or2();
bool RenderText_Draw_Scroll();
void RenderText_SetDefaultWindowPosition();
void RenderText_DrawBorderInitialize();
uint16 *RenderText_DrawBorderRow(uint16 *d, int y);
void Text_BuildCharacterTilemap();
void RenderText_Refresh();
void Text_GenerateMessagePointers();
void DungMap_LightenUpMap();
void DungMap_Backup();
void DungMap_FadeMapToBlack();
void DungMap_RestoreOld();
void Death_PlayerSwoon();
void Death_PrepFaint();
void DisplaySelectMenu();
bool DidPressButtonForMap();
