// auto_tracker.h — local, opt-in TCP server that re-emits the in-game tracker's
// state snapshot as newline-delimited JSON for external tracker tools
// (EmoTracker, PopTracker, OBS overlays). See openspec/changes/
// add-rando-auto-tracker/ and the "Auto-tracker (external clients)" section of
// docs/randomizer.md for the wire protocol.
//
// Design contract:
//   - DISABLED BY DEFAULT. When disabled the whole module is a no-op and the
//     binary behaves byte-identically to a build without it (no socket opened,
//     no per-frame work beyond a single early-return).
//   - OBSERVATION-ONLY. The server reads game state through the existing rando
//     accessors (Rando_FillItemView / Rando_GetLiveReachability /
//     Rando_IsLocationChecked / ...) and NEVER writes g_ram or any game state.
//   - SUBSCRIBE-ONLY. Clients connect and receive the stream; there is no
//     inbound command / state-injection path. Bytes a client sends are ignored.
//   - LOCALHOST BY DEFAULT. The listener binds 127.0.0.1 unless allow_remote is
//     explicitly opted in (then 0.0.0.0).
//   - The transport (raw Winsock / BSD sockets) is fully contained in
//     auto_tracker.c behind a tiny internal interface, so it can be swapped for
//     a shared net library in one file if the P2P-multiplayer roadmap item
//     standardizes one. Nothing socket-related leaks into this header or main.c.
//   - OPTIONAL ON SWITCH. auto_tracker.c compiles to no-op stubs when sockets
//     are unavailable (__SWITCH__), so the Switch build links cleanly with the
//     server omitted. main.c calls these three entry points unconditionally.

#ifndef ZELDA3_RANDO_AUTO_TRACKER_H_
#define ZELDA3_RANDO_AUTO_TRACKER_H_

#include "../types.h"

// Resolved configuration handed to AutoTracker_Init by main.c (sourced from the
// [AutoTracker] INI section + the --auto-tracker CLI flag). Plain values only —
// no socket types — so main.c stays platform-agnostic.
typedef struct AutoTrackerConfig {
  bool enabled;       // master switch: [AutoTracker] enabled OR --auto-tracker
  uint16 port;        // TCP listen port (0 => default 17400)
  bool allow_remote;  // false => bind 127.0.0.1; true => bind 0.0.0.0
} AutoTrackerConfig;

// Start the listener if cfg->enabled. Safe to call once at startup. A NULL or
// disabled cfg leaves the module dormant (no socket, no state). Never fails the
// game: bind/listen errors are logged to stderr and leave the server off.
void AutoTracker_Init(const AutoTrackerConfig *cfg);

// Service the server for one game frame: accept pending connections, and — when
// the rando reachability/checked/goal state has changed since the last frame —
// build one JSON snapshot and queue it to every subscriber. Non-blocking; a
// slow or dead client is dropped, never blocks the frame. No-op when disabled.
void AutoTracker_ServiceFrame(void);

// Close all client connections and the listener; release resources. No-op when
// disabled. Safe to call even if Init was never called / left the server off.
void AutoTracker_Shutdown(void);

// ---------------------------------------------------------------------------
// Runtime control — used by the PC settings-window Trackers tab to start/stop
// the server live, and to show its status. The bind config (port / allow_remote)
// is fixed at Init from the INI; the toggle only opens/closes the listener on
// that config. Call from the main/game thread only (same thread as
// AutoTracker_ServiceFrame). All no-ops / false on Switch.
// ---------------------------------------------------------------------------
bool AutoTracker_IsRunning(void);            // listener currently open
bool AutoTracker_SetEnabled(bool enable);    // start/stop now; returns running state
int AutoTracker_GetClientCount(void);        // currently-connected subscribers
void AutoTracker_GetBindInfo(uint16 *port, bool *allow_remote);  // for the status line

#endif  // ZELDA3_RANDO_AUTO_TRACKER_H_
