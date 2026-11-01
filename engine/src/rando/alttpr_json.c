// Bound recursion for game-thread parsing on the 3DS main stack. cJSON remains
// an unmodified, attributed dependency; this is its sole compilation unit.
#define CJSON_NESTING_LIMIT 32
#include "third_party/cjson/cJSON.c"
