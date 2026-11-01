#pragma once
#include "types.h"
#include <SDL_keycode.h>

enum {
  kKeys_Null,
  kKeys_Controls,
  kKeys_Controls_Last = kKeys_Controls + 11,
  kKeys_Load,
  kKeys_Load_Last = kKeys_Load + 19,
  kKeys_Save,
  kKeys_Save_Last = kKeys_Save + 19,
  kKeys_Replay,
  kKeys_Replay_Last = kKeys_Replay + 19,
  kKeys_LoadRef,
  kKeys_LoadRef_Last = kKeys_LoadRef + 19,
  kKeys_ReplayRef,
  kKeys_ReplayRef_Last = kKeys_ReplayRef + 19,
  kKeys_CheatLife,
  kKeys_CheatKeys,
  kKeys_CheatEquipment,
  kKeys_CheatWalkThroughWalls,
  kKeys_ClearKeyLog,
  kKeys_StopReplay,
  kKeys_Fullscreen,
  kKeys_Reset,
  kKeys_Pause,
  kKeys_PauseDimmed,
  kKeys_Turbo,
  kKeys_ReplayTurbo,
  kKeys_WindowBigger,
  kKeys_WindowSmaller,
  kKeys_DisplayPerf,
  kKeys_ToggleRenderer,
  kKeys_VolumeUp,
  kKeys_VolumeDown,
  kKeys_Total,
};

enum {
  kOutputMethod_SDL,
  kOutputMethod_SDLSoftware,
  kOutputMethod_OpenGL,
  kOutputMethod_OpenGL_ES,
};

enum { kTextSpeed_Normal, kTextSpeed_Fast, kTextSpeed_Instant };
enum { kEnemyDropMarker_Item = 0, kEnemyDropMarker_Generic = 1 };

typedef struct Config {
  int window_width;
  int window_height;
  bool enhanced_mode7;
  bool new_renderer;
  bool ignore_aspect_ratio;
  uint8 fullscreen;
  uint8 window_scale;
  bool enable_audio;
  bool linear_filtering;
  bool crt_filter;
  uint8 output_method;
  uint16 audio_freq;
  uint8 audio_channels;
  uint16 audio_samples;
  bool autosave;
  uint8 extended_aspect_ratio;
  bool extend_y;
  // The WIDE features ExtendedAspectRatio opted out of (unchanged_sprites,
  // no_visual_fixes), so that turning WIDE on while running keeps to them.
  uint32 wide_features_off;
  // WideEdgeMode outside the 3DS: 1 fixed camera (the default), 0 standard.
  uint8 wide_edge_mode;
  bool no_sprite_limits;
  bool display_perf_title;
  uint8 enable_msu;
  bool resume_msu;
  bool panned_sound_effects;
  bool disable_frame_delay;
  uint8 msuvolume;
  uint32 features0;
  uint32 features1;
  uint8 text_speed;
  const char *rando_spoiler_dir;
  bool rando_race_mode_default;
  bool rando_debug_force_ram_compare;

  const char *link_graphics;
  const char *mods;
  char *memory_buffer;
  const char *shader;
  const char *msu_path;
  const char *language;
  // SaveFolder: the folder of the saves in place of saves/ (outside the 3DS,
  // whose profiles keep theirs), for a QUEST's own saves.
  const char *save_folder;
  // Cosmetic shuffles (see openspec add-rando-cosmetic-shuffles). These are
  // client-local presentation options driven by cosmetic_seed; they NEVER touch
  // the slot, share_string, settings_hash, or generation. All default off so an
  // unopted run (rando or vanilla) renders byte-identically to the original.
  uint64 cosmetic_seed;          // 0 = derive from the active slot's seed_u64
  uint8 cosmetic_palette_mode;   // kCosmeticPalette_* (0 = vanilla / off)
  bool cosmetic_music_shuffle;   // remap area background songs deterministically
  const char *cosmetic_sprite_dir;  // folder of .zspr to pick from; NULL = off

  // Field item sprites (add-rando-field-item-sprites): draw the placed item's
  // graphics at free-standing item locations under an active rando slot. Visual
  // only; never touches placement / share_string / settings_hash / generation.
  // Read live (per-frame) so toggling takes effect without a restart. Default
  // ON (ALTTPR-style); set FieldItemSprites = 0 in [Graphics] to see vanilla
  // sprites. Inert in non-rando play regardless.
  bool field_item_sprites;       // default true

  // Enemy-drop check carrier marker. Client-local visual preference for active
  // unchecked enemy-drop checks: placed item icon where the shared tile slot can
  // represent it unambiguously, or the neutral gold check glint. Never serialized
  // into randomizer settings or seed material.
  uint8 enemy_drop_marker;       // kEnemyDropMarker_*; default generic
} Config;

enum {
  kMsuEnabled_Msu = 1,
  kMsuEnabled_MsuDeluxe = 2,
  kMsuEnabled_Opuz = 4,
};
enum {
  kGamepadBtn_Invalid = -1,
  kGamepadBtn_A,
  kGamepadBtn_B,
  kGamepadBtn_X,
  kGamepadBtn_Y,
  kGamepadBtn_Back,
  kGamepadBtn_Guide,
  kGamepadBtn_Start,
  kGamepadBtn_L3,
  kGamepadBtn_R3,
  kGamepadBtn_L1,
  kGamepadBtn_R1,
  kGamepadBtn_DpadUp,
  kGamepadBtn_DpadDown,
  kGamepadBtn_DpadLeft,
  kGamepadBtn_DpadRight,
  kGamepadBtn_L2,
  kGamepadBtn_R2,
  kGamepadBtn_Count,
};

extern Config g_config;

void ParseConfigFile(const char *filename);
int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod);
int FindCmdForGamepadButton(int button, uint32 modifiers);
void GamepadMap_GetControls(uint8 *out);
void GamepadMap_SetControls(const uint8 *btns);

// Copy an option string into the process-lifetime configuration arena.
const char *Config_InternString(const char *s);
