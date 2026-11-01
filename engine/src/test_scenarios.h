#ifndef ZELDA3_TEST_SCENARIOS_H_
#define ZELDA3_TEST_SCENARIOS_H_

#include <stdbool.h>
#include "developer_tests.h"
#include "types.h"

// Developer scenarios are constructed by the engine from authored SRAM flags
// and the selected user's ROM. No external state files or game art are bundled.
// While active, ordinary SRAM and automatic states stay untouched until reset.

enum {
  kTestScenario_Entrance = 0,  // enters through underworld entrance `target`
  kTestScenario_Exit = 1,      // leaves room `target` for the overworld
};

// A byte of the save area ($7E:F000), set on top of the chapter's save.
typedef struct TestScenarioSet {
  uint16 addr;
  uint8 value;
} TestScenarioSet;

enum { kTestScenarioMaxSets = 48 };

typedef struct TestScenario {
  const char *name;    // the list's line
  const char *detail;  // what it is for, under the list
  uint8 chapter;       // reference save, 0 (Zelda's rescue) .. 11 (after Turtle Rock);
                       // 12 is taken during the credits
  uint8 kind;
  uint16 target;
  bool dark_world;
  TestScenarioSet sets[kTestScenarioMaxSets];  // ends at addr 0
} TestScenario;

extern const TestScenario kTestScenarios[];
extern const int kTestScenarioCount;

// True from loading a scenario until the next reset.
extern bool g_test_scenario_active;

// Loads on the game thread, keeping the player's SRAM intact.
bool ZeldaLoadTestScenario(int index);
int ZeldaGetTestScenarioIndex(void);
bool ZeldaRestartTestScenario(void);
void TestScenario_InitSram(int index, uint8 *sram);
const char *TestScenario_Name(int index, int language);
const char *TestScenario_Detail(int index, int language);

// The generator's half: writes the running game as a scenario state, without
// the key log the reference saves carry.
bool ZeldaWriteTestScenarioState(const char *path);

#endif  // ZELDA3_TEST_SCENARIOS_H_
