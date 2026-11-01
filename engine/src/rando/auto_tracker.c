// auto_tracker.c — local, opt-in TCP server emitting the rando tracker snapshot
// as newline-delimited JSON. See auto_tracker.h for the design contract and
// docs/randomizer.md "Auto-tracker (external clients)" for the wire protocol.
//
// This file re-emits the SAME state the in-game ImGui trackers render (item
// view + live reachability + checked bitmap), sourced through the existing
// rando accessors so the two stay byte-consistent. It NEVER writes game state.
//
// The whole transport (raw Winsock / BSD sockets) is contained here behind a
// handful of static helpers (at_listen / at_accept / at_send / at_close), so the
// socket dependency can be swapped for a shared net library in ONE file if the
// P2P-multiplayer roadmap item standardizes one. No socket type leaks into the
// header or into main.c.

// Windows: winsock2.h must be established BEFORE any windows.h pull-in, and
// WIN32_LEAN_AND_MEAN avoids the legacy winsock.h double-declaration (the
// "ExitThread redefinition" SDK clash). Keep these FIRST in the translation unit
// — before auto_tracker.h / types.h / the rando headers.
#if defined(_WIN32) && !defined(__SWITCH__)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif

#include "auto_tracker.h"

#if defined(__SWITCH__)
// ===========================================================================
// Switch: networking deferred (libnx). The Switch Makefile's non-recursive
// src/rando/*.c glob still compiles this TU, so provide no-op stubs — the
// server is simply omitted and the build links cleanly. (auto_tracker.h's
// entry points are called unconditionally from main.c.)
// ===========================================================================
void AutoTracker_Init(const AutoTrackerConfig *cfg) { (void)cfg; }
void AutoTracker_ServiceFrame(void) {}
void AutoTracker_Shutdown(void) {}
bool AutoTracker_IsRunning(void) { return false; }
bool AutoTracker_SetEnabled(bool enable) { (void)enable; return false; }
int AutoTracker_GetClientCount(void) { return 0; }
void AutoTracker_GetBindInfo(uint16 *port, bool *allow_remote) {
  if (port) *port = 0;
  if (allow_remote) *allow_remote = false;
}

#else  // ----------------------------- PC (Winsock / BSD sockets) -----------------------------

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rando.h"            // Rando_IsActive / Rando_FillItemView / Rando_IsLocationChecked /
                             // Rando_GetReachabilityCounter / Rando_GetActiveSettings / ...
#include "door_runtime.h"    // DoorRt_GetLink / DoorRt_IsDiscovered
#include "dungeon_ids.h"      // shared game/key/logic dungeon row descriptors
#include "rando_logic.h"     // RandoReachability / Reachability_HasLocation / kRandoLocations / names
#include "rando_placement.h" // Placement_GetActive / RandoPlacementTable
#include "rando_settings.h"  // WorldState / Goal enums
#include "../types.h"
#include "../variables.h"    // main_module_index (read-only: goal-completion signal)

// ---------------------------------------------------------------------------
// Transport layer — the only socket-aware code in the module.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
  // <winsock2.h> / <ws2tcpip.h> are included at the very top of this file.
  #pragma comment(lib, "ws2_32.lib")   // MSVC: link Winsock without a .vcxproj edit
  typedef SOCKET at_sock;
  #define AT_BAD_SOCK   INVALID_SOCKET
  #define AT_SEND_FLAGS 0
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  #include <signal.h>
  typedef int at_sock;
  #define AT_BAD_SOCK   (-1)
  #if defined(MSG_NOSIGNAL)
    #define AT_SEND_FLAGS MSG_NOSIGNAL   // Linux: suppress SIGPIPE on send to a dead peer
  #else
    #define AT_SEND_FLAGS 0
  #endif
#endif

static const uint16 kAtDefaultPort = 17400;
static const uint8  kAtLocTypeMedallion = 13;  // logic.schema.yaml: MM/TR medallion-config
static const uint8  kAtLocTypePot       = 17;  // add-rando-pot-sanity dungeon pot (tagged in the catalog)
                                               // pseudo-locations (not real checks) — excluded.
static const uint8  kAtLocTypeGrass     = 20;  // add-rando-grass-rock-shuffle bush/thick-grass check
static const uint8  kAtLocTypeRock      = 21;  // add-rando-grass-rock-shuffle light/heavy rock check

static bool at_last_would_block(void) {
#if defined(_WIN32)
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static bool at_last_intr(void) {
#if defined(_WIN32)
  return WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

static bool at_net_startup(void) {
#if defined(_WIN32)
  WSADATA w;
  return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
  // Ignore SIGPIPE process-wide so a write to a client that vanished returns
  // EPIPE (handled as "drop this client") instead of killing the game. This is
  // a belt-and-suspenders guarantee alongside MSG_NOSIGNAL / SO_NOSIGPIPE, which
  // are not universally available across libc/platform combos. Only reached when
  // the server is explicitly enabled; ignoring SIGPIPE is benign for an SDL app
  // (it uses no pipes that rely on the default terminate disposition).
  signal(SIGPIPE, SIG_IGN);
  return true;
#endif
}

static void at_net_cleanup(void) {
#if defined(_WIN32)
  WSACleanup();
#endif
}

static void at_close(at_sock s) {
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

static void at_set_nonblocking(at_sock s) {
#if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
#else
  int fl = fcntl(s, F_GETFL, 0);
  if (fl != -1) fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

// macOS lacks MSG_NOSIGNAL; suppress SIGPIPE per-socket instead so a write to a
// dropped client returns EPIPE rather than killing the process.
static void at_set_nosigpipe(at_sock s) {
#if defined(SO_NOSIGPIPE)
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof yes);
#else
  (void)s;
#endif
}

// Create a non-blocking listening socket bound to localhost (or 0.0.0.0 when
// allow_remote). Returns AT_BAD_SOCK on any failure (caller leaves server off).
static at_sock at_listen(uint16 port, bool allow_remote) {
  at_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == AT_BAD_SOCK) return AT_BAD_SOCK;

  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  // Network byte order is big-endian. Assemble the port + address bytes
  // explicitly rather than via the BSD byte-swap macros: the byte-order pin
  // (check_byte_order) forbids them in src/rando/, and writing the bytes
  // directly is host-endian-independent (and byte-identical to what the macros
  // would produce). memset already zeroed both fields, so INADDR_ANY (0.0.0.0)
  // needs no further writes.
  uint8 *port_be = (uint8 *)&addr.sin_port;
  port_be[0] = (uint8)(port >> 8);
  port_be[1] = (uint8)(port & 0xff);
  if (!allow_remote) {
    // INADDR_LOOPBACK = 127.0.0.1 -> big-endian bytes {127, 0, 0, 1}.
    uint8 *loopback_be = (uint8 *)&addr.sin_addr.s_addr;
    loopback_be[0] = 127;
    loopback_be[3] = 1;
  }

  if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) { at_close(s); return AT_BAD_SOCK; }
  if (listen(s, 4) != 0) { at_close(s); return AT_BAD_SOCK; }
  at_set_nonblocking(s);
  return s;
}

// Non-blocking accept. Returns AT_BAD_SOCK when there is no pending connection
// (or on error) — caller treats both the same (stop accepting this frame).
static at_sock at_accept(at_sock listener) {
  return accept(listener, NULL, NULL);
}

// Non-blocking send. Return: >0 bytes written, 0 == would-block (retry later),
// -1 == fatal (drop client). Retries through EINTR.
static int at_send(at_sock s, const char *buf, int len) {
  for (;;) {
    int rv = (int)send(s, buf, (size_t)len, AT_SEND_FLAGS);
    if (rv > 0) return rv;
    if (rv == 0) return 0;
    if (at_last_intr()) continue;
    if (at_last_would_block()) return 0;
    return -1;  // EPIPE / ECONNRESET / WSAECONNRESET / ... — drop this client
  }
}

// ---------------------------------------------------------------------------
// Growable string builder (heap). Only integers/strings are formatted — no
// floating point (determinism guard forbids float/double in src/rando/).
// ---------------------------------------------------------------------------
typedef struct AtStr {
  char *p;
  int len;
  int cap;
  bool err;  // sticky: any allocation failure poisons the builder
} AtStr;

static void at_str_init(AtStr *s) {
  s->cap = 8192;
  s->len = 0;
  s->err = false;
  s->p = (char *)malloc((size_t)s->cap);
  if (!s->p) s->err = true;
}

static void at_str_free(AtStr *s) {
  free(s->p);
  s->p = NULL;
  s->len = s->cap = 0;
}

static void at_str_reserve(AtStr *s, int extra) {
  if (s->err) return;
  if (s->len + extra + 1 <= s->cap) return;
  int nc = s->cap * 2;
  while (nc < s->len + extra + 1) nc *= 2;
  char *np = (char *)realloc(s->p, (size_t)nc);
  if (!np) { s->err = true; return; }
  s->p = np;
  s->cap = nc;
}

static void at_str_putn(AtStr *s, const char *d, int n) {
  at_str_reserve(s, n);
  if (s->err) return;
  memcpy(s->p + s->len, d, (size_t)n);
  s->len += n;
  s->p[s->len] = 0;
}

static void at_str_puts(AtStr *s, const char *d) { at_str_putn(s, d, (int)strlen(d)); }

static void at_str_printf(AtStr *s, const char *fmt, ...) {
  if (s->err) return;
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) { s->err = true; return; }
  if (n < (int)sizeof tmp) { at_str_putn(s, tmp, n); return; }
  // Rare: the formatted text exceeds the stack temp — format straight into the
  // builder after reserving the exact size.
  at_str_reserve(s, n);
  if (s->err) return;
  va_start(ap, fmt);
  vsnprintf(s->p + s->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  s->len += n;
}

// Append a JSON string literal (with surrounding quotes), escaping per RFC 8259.
static void at_json_str(AtStr *s, const char *str) {
  at_str_putn(s, "\"", 1);
  if (str) {
    for (const char *c = str; *c; c++) {
      unsigned char ch = (unsigned char)*c;
      if (ch == '"' || ch == '\\') {
        char e[2]; e[0] = '\\'; e[1] = (char)ch;
        at_str_putn(s, e, 2);
      } else if (ch == '\n') {
        at_str_puts(s, "\\n");
      } else if (ch == '\r') {
        at_str_puts(s, "\\r");
      } else if (ch == '\t') {
        at_str_puts(s, "\\t");
      } else if (ch < 0x20) {
        at_str_printf(s, "\\u%04x", (unsigned)ch);
      } else {
        char one = (char)ch;
        at_str_putn(s, &one, 1);  // pass UTF-8 / high bytes through verbatim
      }
    }
  }
  at_str_putn(s, "\"", 1);
}

static const char *at_b(bool x) { return x ? "true" : "false"; }

static const char *at_world_name(uint8 ws) {
  switch (ws) {
    case kWorldState_Open:     return "open";
    case kWorldState_Standard: return "standard";
    case kWorldState_Inverted: return "inverted";
    case kWorldState_Retro:    return "retro";
    default:                   return "unknown";
  }
}

static const char *at_goal_name(uint8 g) {
  switch (g) {
    case kGoal_Ganon:         return "ganon";
    case kGoal_FastGanon:     return "fast_ganon";
    case kGoal_Dungeons:      return "dungeons";
    case kGoal_Pedestal:      return "pedestal";
    case kGoal_TriforceHunt:  return "triforce_hunt";
    case kGoal_GanonHunt:     return "ganon_hunt";
    case kGoal_Completionist: return "completionist";
    default:                  return "unknown";
  }
}

// location_id -> type lookup (built once), used to skip medallion-config slots
// so the catalog and the checked/reachable id-spaces match exactly.
static uint8 s_loc_type[kRandoLocationCapacity];
static bool s_loc_type_built = false;
static void at_build_loc_type(void) {
  for (int i = 0; i < kRandoLocationCapacity; i++) s_loc_type[i] = 0xFF;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    uint16 id = kRandoLocations[i].id;
    if (id < kRandoLocationCapacity) s_loc_type[id] = kRandoLocations[i].type;
  }
  s_loc_type_built = true;
}
static bool at_loc_hidden(uint16 loc) {
  return loc < kRandoLocationCapacity && s_loc_type[loc] == kAtLocTypeMedallion;
}

// ---------------------------------------------------------------------------
// JSON builders.
// ---------------------------------------------------------------------------
static void at_append_settings(AtStr *s) {
  const RandoSettings *rs = Rando_GetActiveSettings();  // NULL on snapshot-restore / v1 slot
  if (!rs) { at_str_puts(s, "null"); return; }
  at_str_puts(s, "{");
  at_str_puts(s, "\"world_state\":"); at_json_str(s, at_world_name(rs->world_state));
  at_str_puts(s, ",\"goal\":"); at_json_str(s, at_goal_name(rs->goal));
  // add-rando-random-crystals: external trackers need actionable thresholds,
  // so emit the RESOLVED counts (deliberate divergence from the spoiler's
  // requested field — documented in docs/randomizer.md).
  at_str_printf(s, ",\"crystals_ganon\":%u,\"crystals_tower\":%u",
                (unsigned)Rando_EffectiveCrystalsGanon(),
                (unsigned)Rando_EffectiveCrystalsTower());
  if (rs->goal == kGoal_TriforceHunt || rs->goal == kGoal_GanonHunt)
    at_str_printf(s, ",\"pieces_required\":%u", (unsigned)rs->pieces_required);
  at_str_puts(s, "}");
}

static void at_append_items(AtStr *s, const RandoItemView *v) {
  at_str_puts(s, "{");
  at_str_printf(s, "\"sword\":%u,\"shield\":%u,\"mail\":%u,\"gloves\":%u,",
                (unsigned)v->sword, (unsigned)v->shield, (unsigned)v->mail, (unsigned)v->gloves);
  at_str_printf(s, "\"bow\":%u,\"boomerang\":%u,\"bottles\":%u,\"magic\":%u,",
                (unsigned)v->bow, (unsigned)v->boomerang, (unsigned)v->bottles, (unsigned)v->magic);
  at_str_printf(s, "\"hearts\":%u,\"heart_pieces\":%u,\"crystals\":%u,\"pendants\":%u,",
                (unsigned)v->hearts, (unsigned)v->heart_pieces, (unsigned)v->crystals,
                (unsigned)v->pendants);
  at_str_printf(s, "\"crystal_mask\":%u,\"pendant_mask\":%u,",
                (unsigned)v->crystal_mask, (unsigned)v->pendant_mask);
  at_str_printf(s, "\"hookshot\":%s,\"firerod\":%s,\"icerod\":%s,\"hammer\":%s,",
                at_b(v->hookshot), at_b(v->firerod), at_b(v->icerod), at_b(v->hammer));
  at_str_printf(s, "\"lamp\":%s,\"net\":%s,\"book\":%s,\"somaria\":%s,",
                at_b(v->lamp), at_b(v->net), at_b(v->book), at_b(v->somaria));
  at_str_printf(s, "\"byrna\":%s,\"cape\":%s,\"mirror\":%s,\"boots\":%s,",
                at_b(v->byrna), at_b(v->cape), at_b(v->mirror), at_b(v->boots));
  at_str_printf(s, "\"flippers\":%s,\"moon_pearl\":%s,\"bombos\":%s,\"ether\":%s,",
                at_b(v->flippers), at_b(v->moon_pearl), at_b(v->bombos), at_b(v->ether));
  at_str_printf(s, "\"quake\":%s,\"mushroom\":%s,\"powder\":%s,\"flute\":%s,",
                at_b(v->quake), at_b(v->mushroom), at_b(v->powder), at_b(v->flute));
  at_str_printf(s, "\"shovel\":%s,\"agahnim\":%s,",
                at_b(v->shovel), at_b(v->agahnim));
  at_str_printf(s, "\"skeleton_key_enabled\":%s,\"skeleton_key_owned\":%s",
                at_b(v->skeleton_key_enabled), at_b(v->skeleton_key_owned));
  at_str_puts(s, "}");
}

static void at_append_dungeons(AtStr *s, const RandoItemView *v) {
  at_str_puts(s, "[");
  int count = (int)kRandoDungeonRuntimeRowCount;
  for (int i = 0; i < count; i++) {
    const RandoDungeonRuntimeRow *row = &kRandoDungeonRuntimeRows[i];
    uint16 bigkey_bit = Rando_DungeonBitForGameDungeon(row->bigkey_game_dungeon);
    uint16 map_bit = Rando_DungeonBitForGameDungeon(row->map_game_dungeon);
    uint16 compass_bit = Rando_DungeonBitForGameDungeon(row->compass_game_dungeon);
    if (i) at_str_puts(s, ",");
    at_str_puts(s, "{\"name\":");
    at_json_str(s, row->name);
    uint8 key_slot = Rando_EffectiveKeySlot(row->key_slot);
    // Do not expose selected/owned ring state: that would reveal whether this
    // dungeon rolled a ring before the player finds the item. The live numeric
    // counter starts at zero for either pool shape and rises on actual receipt.
    at_str_printf(s, ",\"small_keys\":%u,\"big_key\":%s,\"map\":%s,\"compass\":%s}",
                  (unsigned)v->dungeon_small_keys[key_slot],
                  at_b((v->bigkey_bits & bigkey_bit) != 0),
                  at_b((v->map_bits & map_bit) != 0),
                  at_b((v->compass_bits & compass_bit) != 0));
  }
  at_str_puts(s, "]");
}

static void at_append_connections(AtStr *s) {
  at_str_puts(s, ",\"discovered_connections\":[");
  int first = 1;
  uint32 n = Rando_EntranceConnectionCount();
  for (uint32 i = 0; i < n; i++) {
    uint8 from = 0, to = 0;
    if (!Rando_EntranceConnection((uint16)i, &from, &to))
      continue;
    at_str_printf(s, "%s{\"door\":%u,\"from\":%u,\"to\":%u}",
                  first ? "" : ",", (unsigned)i, (unsigned)from, (unsigned)to);
    first = 0;
  }
  at_str_puts(s, "],\"discovered_doors\":[");
  first = 1;
  for (uint32 i = 0; i < kDoorTbl_DoorCount; i++) {
    uint16 to = DoorRt_GetLink((uint16)i);
    if (to == kDoorRt_NoOverride || to >= kDoorTbl_DoorCount ||
        !DoorRt_IsDiscovered((uint16)i)) {
      continue;
    }
    at_str_printf(s, "%s{\"from\":%u,\"to\":%u,\"from_name\":",
                  first ? "" : ",", (unsigned)i, (unsigned)to);
    at_json_str(s, kDoorTblNames + kDoorTblDoors[i].name_off);
    at_str_puts(s, ",\"to_name\":");
    at_json_str(s, kDoorTblNames + kDoorTblDoors[to].name_off);
    at_str_puts(s, "}");
    first = 0;
  }
  at_str_puts(s, "]");
}

static void at_digest_u16(uint64 *h, uint16 v) {
  *h = (*h ^ (uint8)(v & 0xff)) * 0x100000001b3ull;
  *h = (*h ^ (uint8)(v >> 8)) * 0x100000001b3ull;
}

static uint32 at_connection_digest(void) {
  uint64 h = 0xcbf29ce484222325ull;
  uint32 n = Rando_EntranceConnectionCount();
  for (uint32 i = 0; i < n; i++) {
    uint8 from = 0, to = 0;
    if (!Rando_EntranceConnection((uint16)i, &from, &to))
      continue;
    at_digest_u16(&h, 1);
    at_digest_u16(&h, (uint16)i);
    at_digest_u16(&h, from);
    at_digest_u16(&h, to);
  }
  for (uint32 i = 0; i < kDoorTbl_DoorCount; i++) {
    uint16 to = DoorRt_GetLink((uint16)i);
    if (to == kDoorRt_NoOverride || to >= kDoorTbl_DoorCount ||
        !DoorRt_IsDiscovered((uint16)i)) {
      continue;
    }
    at_digest_u16(&h, 2);
    at_digest_u16(&h, (uint16)i);
    at_digest_u16(&h, to);
  }
  return (uint32)(h ^ (h >> 32));
}

// Full state snapshot, terminated by '\n'. When no rando slot is active the line
// is minimal ({type,msg,counter,active:false}) so a connecting client still gets
// an immediate, framed ack but no inventory/reachability flows until activation.
//
// SPOILER-SAFE BY CONSTRUCTION (tracker-player-knowledge invariant): this
// never emits placement (which item sits at an unchecked location), and the
// reachability it emits is the KNOWLEDGE-LIMITED live view — undiscovered
// shuffled topology (dungeon identities, cave contents, un-ridden whirlpools,
// untraversed decoupled exits) is excluded at the flood, so every statement
// here is true under all assignments consistent with what the player has
// observed. Exposes only the player's own inventory, their CHECKED locations,
// DISCOVERED connections, and that knowledge-limited availability. Safe even
// for race seeds without any race-mode gate.
static void at_build_snapshot(AtStr *s, uint32 msg, uint32 counter, bool active,
                              bool completed) {
  at_str_puts(s, "{\"type\":\"state\"");
  at_str_printf(s, ",\"msg\":%u,\"counter\":%u,\"active\":%s",
                (unsigned)msg, (unsigned)counter, at_b(active));
  if (!active) { at_str_puts(s, "}\n"); return; }

  at_str_printf(s, ",\"game_completed\":%s", at_b(completed));
  at_str_puts(s, ",\"settings\":");
  at_append_settings(s);

  RandoItemView v;
  Rando_FillItemView(&v);
  at_str_puts(s, ",\"items\":");
  at_append_items(s, &v);
  at_str_puts(s, ",\"dungeons\":");
  at_append_dungeons(s, &v);

  const RandoPlacementTable *pt = Placement_GetActive();      // NULL when no placement installed
  const RandoReachability *reach = Rando_GetLiveReachability();  // NULL when settings unavailable
  int n = pt ? (int)pt->count : 0;

  at_str_printf(s, ",\"reachable_available\":%s", at_b(reach != NULL));

  at_str_puts(s, ",\"checked\":[");
  {
    int first = 1;
    for (int i = 0; i < n; i++) {
      uint16 loc = pt->entries[i].location_id;
      if (at_loc_hidden(loc)) continue;
      if (!Rando_IsLocationChecked(loc)) continue;
      at_str_printf(s, "%s%u", first ? "" : ",", (unsigned)loc);
      first = 0;
    }
  }
  at_str_puts(s, "],\"reachable\":[");
  if (reach) {
    int first = 1;
    for (int i = 0; i < n; i++) {
      uint16 loc = pt->entries[i].location_id;
      if (at_loc_hidden(loc)) continue;
      if (Rando_IsLocationChecked(loc)) continue;  // reachable list = unchecked-but-reachable
      if (!Reachability_HasLocation(reach, loc)) continue;
      at_str_printf(s, "%s%u", first ? "" : ",", (unsigned)loc);
      first = 0;
    }
  }
  at_str_puts(s, "]");
  at_append_connections(s);
  at_str_puts(s, "}\n");
}

// Static location catalog (id -> name + region), sent once per connection so an
// external client can resolve the numeric ids in every later state message
// without hardcoding this fork's id space. Location names/regions are not
// spoiler data (they identify slots, not placed items).
static void at_build_catalog(AtStr *s) {
  at_str_puts(s, "{\"type\":\"catalog\",\"locations\":[");
  int first = 1;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    uint16 id = kRandoLocations[i].id;
    if (kRandoLocations[i].type == kAtLocTypeMedallion) continue;
    at_str_printf(s, "%s{\"id\":%u,\"name\":", first ? "" : ",", (unsigned)id);
    at_json_str(s, Rando_GetLocationName(id));
    uint16 region = kRandoLocations[i].region_id;
    if (region != 0xFFFF) {
      at_str_puts(s, ",\"region\":");
      at_json_str(s, Rando_GetRegionName(region));
    }
    // add-rando-pot-sanity — tag pot slots so a connected tracker can filter
    // the ~800 dungeon-pot checks (a pot_shuffle=All seed) into their own view.
    if (kRandoLocations[i].type == kAtLocTypePot)
      at_str_puts(s, ",\"pot\":true");
    // add-rando-grass-rock-shuffle — tag terrain slots so a tracker can filter
    // the ~3900 overworld bush/grass/rock checks into their own view.
    if (kRandoLocations[i].type == kAtLocTypeGrass)
      at_str_puts(s, ",\"grass\":true");
    if (kRandoLocations[i].type == kAtLocTypeRock)
      at_str_puts(s, ",\"rock\":true");
    at_str_puts(s, "}");
    first = 0;
  }
  at_str_puts(s, "]}\n");
}

// ---------------------------------------------------------------------------
// Client + server state.
// ---------------------------------------------------------------------------
#define AT_MAX_CLIENTS 8
#define AT_CLIENT_BUF_INITIAL (128 * 1024)
#define AT_CLIENT_BUF_MAX     (4 * 1024 * 1024)  // hard ceiling for a genuinely stuck client

typedef struct AtClient {
  at_sock sock;
  bool in_use;
  bool need_full;  // freshly accepted; still owes the catalog + first snapshot
  int head, tail;  // pending-send window into buf
  int cap;
  char *buf;       // heap, grows on demand through AT_CLIENT_BUF_MAX
} AtClient;

static struct {
  bool enabled;     // master switch was on AND the listener came up
  bool started;     // net stack initialized (so Shutdown knows to clean it up)
  at_sock listener;
  uint16 port;
  bool allow_remote;
  AtClient clients[AT_MAX_CLIENTS];
  bool have_last;
  uint32 last_counter;
  bool last_active;
  bool last_completed;
  uint8 last_prog;   // last sram_progress_indicator (Standard castle-escape bumps
                     // RescuedZelda-gated reachability WITHOUT moving the counter)
  uint32 last_connections;
  uint32 msg_seq;
} g_at;

static void at_drop_client(AtClient *c) {
  if (!c->in_use) return;
  at_close(c->sock);
  free(c->buf);
  c->buf = NULL;
  c->in_use = false;
  c->need_full = false;
  c->head = c->tail = 0;
  c->cap = 0;
  c->sock = AT_BAD_SOCK;
}

// Queue bytes for a client. Compacts any partially-sent prefix first, then grows
// the backlog as needed. The location catalog used to fit the original 128 KiB
// allocation, but terrain/enemy check registries can push it well beyond that;
// dropping a fresh subscriber there makes the wire protocol unusable. Keep the
// small initial allocation and grow only clients that need it, with a hard cap
// that still drops a genuinely slow/stuck subscriber.
static bool at_client_queue(AtClient *c, const char *data, int len) {
  if (c->head > 0) {
    memmove(c->buf, c->buf + c->head, (size_t)(c->tail - c->head));
    c->tail -= c->head;
    c->head = 0;
  }
  if (len < 0 || c->tail > AT_CLIENT_BUF_MAX - len) return false;
  int need = c->tail + len;
  if (need > c->cap) {
    int cap = c->cap;
    while (cap < need && cap < AT_CLIENT_BUF_MAX)
      cap *= 2;
    if (cap < need) return false;
    char *p = (char *)realloc(c->buf, (size_t)cap);
    if (!p) return false;
    c->buf = p;
    c->cap = cap;
  }
  memcpy(c->buf + c->tail, data, (size_t)len);
  c->tail += len;
  return true;
}

// Push as much of the backlog as the socket will take without blocking; drop on
// fatal error. Leaves any unsent remainder buffered for the next frame.
static void at_flush_client(AtClient *c) {
  while (c->head < c->tail) {
    int rv = at_send(c->sock, c->buf + c->head, c->tail - c->head);
    if (rv > 0) {
      c->head += rv;
    } else if (rv == 0) {
      break;  // would-block — retry next frame
    } else {
      at_drop_client(c);
      return;
    }
  }
  if (c->head >= c->tail) c->head = c->tail = 0;
}

// Discard any inbound bytes (subscribe-only — clients never command us) and
// detect a peer close. Returns false when the connection is gone (drop it).
//
// BOUNDED per frame: a client that streams bytes continuously keeps recv()
// returning >0, so an unbounded loop here would stall the game frame. Drain at
// most a fixed budget and resume next frame — we discard the data anyway, so a
// flooding client only fills its own send buffer, never blocks us.
static bool at_drain_client(AtClient *c) {
  char tmp[512];
  for (int iter = 0; iter < 64; iter++) {  // <= 32 KB drained per client per frame
    int rv = (int)recv(c->sock, tmp, sizeof tmp, 0);
    if (rv > 0) continue;       // ignore whatever the client sent
    if (rv == 0) return false;  // orderly peer close
    if (at_last_intr()) continue;
    if (at_last_would_block()) return true;  // nothing pending — still connected
    return false;               // hard error — drop
  }
  return true;  // budget exhausted this frame; keep the client, drain more next frame
}

static void at_accept_pending(void) {
  for (;;) {
    at_sock cs = at_accept(g_at.listener);
    if (cs == AT_BAD_SOCK) break;  // no more pending connections this frame

    int slot = -1;
    for (int i = 0; i < AT_MAX_CLIENTS; i++)
      if (!g_at.clients[i].in_use) { slot = i; break; }
    if (slot < 0) { at_close(cs); continue; }  // at capacity — refuse this connection

    char *buf = (char *)malloc(AT_CLIENT_BUF_INITIAL);
    if (!buf) { at_close(cs); continue; }

    at_set_nonblocking(cs);
    at_set_nosigpipe(cs);
    AtClient *c = &g_at.clients[slot];
    c->sock = cs;
    c->in_use = true;
    c->need_full = true;
    c->head = c->tail = 0;
    c->cap = AT_CLIENT_BUF_INITIAL;
    c->buf = buf;
  }
}

// Queue the connection handshake (catalog, then the current snapshot) to a fresh
// client. Returns false if the data didn't fit (caller drops the client).
static bool at_send_handshake(AtClient *c, const char *snapshot, int snap_len) {
  AtStr cat;
  at_str_init(&cat);
  at_build_catalog(&cat);
  if (cat.err) { at_str_free(&cat); return false; }
  bool ok = at_client_queue(c, cat.p, cat.len);
  at_str_free(&cat);
  if (!ok) return false;
  return at_client_queue(c, snapshot, snap_len);
}

// ---------------------------------------------------------------------------
// Start / stop (shared by Init + the runtime toggle). g_at.port / allow_remote
// must already be set. at_start opens the listener (lazily initializing the net
// stack the first time); at_stop drops clients + closes the listener but leaves
// the net stack up so a later restart is cheap (cleaned up once in Shutdown).
// ---------------------------------------------------------------------------
static bool at_start(void) {
  if (g_at.enabled) return true;  // already running
  const char *bind_ip = g_at.allow_remote ? "0.0.0.0" : "127.0.0.1";

  if (!g_at.started) {
    if (!at_net_startup()) {
      fprintf(stderr, "[AutoTracker] network init failed; server not started.\n");
      return false;
    }
    g_at.started = true;
  }

  g_at.listener = at_listen(g_at.port, g_at.allow_remote);
  if (g_at.listener == AT_BAD_SOCK) {
    fprintf(stderr, "[AutoTracker] failed to bind %s:%u; server not started.\n", bind_ip,
            (unsigned)g_at.port);
    return false;
  }

  if (!s_loc_type_built) at_build_loc_type();
  g_at.have_last = false;  // force a fresh snapshot to the first subscriber
  g_at.enabled = true;
  fprintf(stderr,
          "[AutoTracker] listening on %s:%u (subscribe-only, observation; "
          "newline-delimited JSON).\n",
          bind_ip, (unsigned)g_at.port);
  if (g_at.allow_remote)
    fprintf(stderr, "[AutoTracker] WARNING: bound to all interfaces (allow_remote) — "
                    "the state stream is reachable from the local network.\n");
  return true;
}

static void at_stop(void) {
  for (int i = 0; i < AT_MAX_CLIENTS; i++)
    if (g_at.clients[i].in_use) at_drop_client(&g_at.clients[i]);
  if (g_at.listener != AT_BAD_SOCK) {
    at_close(g_at.listener);
    g_at.listener = AT_BAD_SOCK;
  }
  if (g_at.enabled) fprintf(stderr, "[AutoTracker] server stopped.\n");
  g_at.enabled = false;
}

// ---------------------------------------------------------------------------
// Public lifecycle.
// ---------------------------------------------------------------------------
void AutoTracker_Init(const AutoTrackerConfig *cfg) {
  memset(&g_at, 0, sizeof g_at);
  g_at.listener = AT_BAD_SOCK;
  for (int i = 0; i < AT_MAX_CLIENTS; i++) g_at.clients[i].sock = AT_BAD_SOCK;

  // Always record the resolved bind config so the runtime toggle (the PC
  // settings-window Trackers tab) can start the server later even if it boots
  // disabled. Nothing is opened and the net stack is untouched until enabled —
  // a default-off launch stays byte-identical / zero-overhead.
  g_at.port = (cfg && cfg->port) ? cfg->port : kAtDefaultPort;
  g_at.allow_remote = cfg ? cfg->allow_remote : false;

  if (cfg && cfg->enabled) at_start();
}

void AutoTracker_ServiceFrame(void) {
  if (!g_at.enabled) return;  // dormant / failed to start: zero overhead

  at_accept_pending();

  uint32 counter = Rando_GetReachabilityCounter();
  bool active = Rando_IsActive();
  uint32 connections = active ? at_connection_digest() : 0;
  // The player beat the seed only at TriforceRoom (0x19) or Credits (0x1A).
  // Deliberately NOT `>= 0x18`: that also matches GanonEmerges (0x18, the fight
  // is only just starting) and SpawnSelect (0x1B), the spawn-point menu the load
  // path transiently passes through (Module05_LoadFile routes past main_module
  // 0x1B) — which otherwise blips game_completed=true for one frame on slot load.
  bool completed = active && (main_module_index == 0x19 || main_module_index == 0x1A);

  // sram_progress_indicator is folded into the change test (and Rando_GetLive-
  // Reachability's own memo key): in a Standard seed the castle-escape sets it to
  // 2, enabling every RescuedZelda-gated region WITHOUT bumping the reachability
  // counter — without this, subscribers keep a stale `reachable` set until the
  // next location check.
  bool changed = !g_at.have_last || counter != g_at.last_counter ||
                 active != g_at.last_active || completed != g_at.last_completed ||
                 (uint8)sram_progress_indicator != g_at.last_prog ||
                 connections != g_at.last_connections;

  bool any_new = false;
  for (int i = 0; i < AT_MAX_CLIENTS; i++)
    if (g_at.clients[i].in_use && g_at.clients[i].need_full) { any_new = true; break; }

  if (changed || any_new) {
    AtStr snap;
    at_str_init(&snap);
    at_build_snapshot(&snap, ++g_at.msg_seq, counter, active, completed);
    if (!snap.err) {
      for (int i = 0; i < AT_MAX_CLIENTS; i++) {
        AtClient *c = &g_at.clients[i];
        if (!c->in_use) continue;
        if (c->need_full) {
          if (!at_send_handshake(c, snap.p, snap.len)) {
            // Only reachable if the catalog+snapshot exceeds the hard ceiling or
            // allocation fails — make a future regression loud rather than a
            // silent drop.
            fprintf(stderr, "[AutoTracker] handshake too large for client buffer; "
                            "dropping subscriber.\n");
            at_drop_client(c);
            continue;
          }
          c->need_full = false;
        } else if (changed) {
          if (!at_client_queue(c, snap.p, snap.len)) { at_drop_client(c); continue; }
        }
      }
      // Only advance the change baseline once the snapshot was built OK, so a
      // transient allocation failure simply retries next frame.
      g_at.have_last = true;
      g_at.last_counter = counter;
      g_at.last_active = active;
      g_at.last_completed = completed;
      g_at.last_prog = (uint8)sram_progress_indicator;
      g_at.last_connections = connections;
    }
    at_str_free(&snap);
  }

  for (int i = 0; i < AT_MAX_CLIENTS; i++) {
    AtClient *c = &g_at.clients[i];
    if (!c->in_use) continue;
    if (!at_drain_client(c)) { at_drop_client(c); continue; }
    at_flush_client(c);
  }
}

void AutoTracker_Shutdown(void) {
  at_stop();
  if (g_at.started) {
    at_net_cleanup();
    g_at.started = false;
  }
}

// ---- Runtime control (PC settings-window Trackers tab) ---------------------
bool AutoTracker_IsRunning(void) { return g_at.enabled; }

bool AutoTracker_SetEnabled(bool enable) {
  if (enable) {
    if (!g_at.enabled) at_start();
  } else {
    if (g_at.enabled) at_stop();
  }
  return g_at.enabled;
}

int AutoTracker_GetClientCount(void) {
  int n = 0;
  for (int i = 0; i < AT_MAX_CLIENTS; i++)
    if (g_at.clients[i].in_use) n++;
  return n;
}

void AutoTracker_GetBindInfo(uint16 *port, bool *allow_remote) {
  if (port) *port = g_at.port;
  if (allow_remote) *allow_remote = g_at.allow_remote;
}

#endif  // __SWITCH__

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(auto_tracker)
