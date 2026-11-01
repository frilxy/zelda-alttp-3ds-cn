#ifndef ZELDA3_DEVELOPER_TESTS_H_
#define ZELDA3_DEVELOPER_TESTS_H_

// Production defaults to off. Named host fixtures opt in so development tools
// remain testable without exposing save-replacing scenarios in shipped builds.
#ifndef ZELDA3_ENABLE_DEVELOPER_TESTS
#if defined(ZELDA3_TEST_NATIVE_FILE_MENU) || defined(ZELDA3_TEST_DUMP_STATE) || defined(ZELDA3_TEST_NATIVE_MENUS)
#define ZELDA3_ENABLE_DEVELOPER_TESTS 1
#else
#define ZELDA3_ENABLE_DEVELOPER_TESTS 0
#endif
#endif

#endif
