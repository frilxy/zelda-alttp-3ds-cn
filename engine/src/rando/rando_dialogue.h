#ifndef ZELDA3_RANDO_DIALOGUE_H_
#define ZELDA3_RANDO_DIALOGUE_H_

#include "../types.h"

// Rewrite reward-specific vanilla dialogue after Text_LoadCharacterBuffer has
// expanded the ROM text into font codes. Includes the opt-in pre-commit
// NPC/shop reward disclosures. No-op outside an active rando slot or for any
// non-US language; generated reward text currently uses the US command grammar
// and font alphabet.
void Rando_RewriteRewardDialogue(uint16 msg_id, uint8 *buffer);

// True only when the active Original/US slot can render an exact unchecked
// reward preview for this location. Transaction handlers use this to insert an
// unavoidable pre-commit information step without changing missing/unsupported
// contexts.
bool Rando_CanPreviewNpcReward(uint16 location_id);

// Focused renderer/mapping regression checks, invoked by --rando-selftest.
void RandoDialogue_SelfCheck(void);

#endif  // ZELDA3_RANDO_DIALOGUE_H_
