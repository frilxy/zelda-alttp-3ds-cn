#pragma once
#include "../types.h"

// These read actual acquired items/checks, never the spoiler's placed pool.
uint8 Rando_CountCollectedTriforcePieces(void);
bool Rando_GoalAllowsGanonDamage(void);
bool Rando_RequestTriforceTurnIn(void);
void Rando_ResetGoalRuntime(void);
bool Rando_GoalCompletionPending(void);
void Rando_NotifyGoalCheck(uint16 location);
void Rando_TickGoalCompletion(void);
