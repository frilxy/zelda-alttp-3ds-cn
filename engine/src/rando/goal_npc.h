#pragma once
#include "../types.h"
#define kRandoGoalDialogueId 0x0222u
void Rando_ResetGoalNpc(void);
void Rando_AddGoalNpcMapEntry(uint16 area);
bool Rando_HandleGoalNpc(int sprite);
void Rando_DrawGoalNpcOverlay(void);
bool Rando_RenderGoalMessage(uint16 message,uint8 *buffer);
