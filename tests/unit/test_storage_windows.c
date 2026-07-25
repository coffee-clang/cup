/*
 * Test focus: Provides the Windows Unity wrapper for portable filesystem and
 * layout tests. Native system primitives remain in test_system_windows.
 */

#include "unity.h"

void register_filesystem_tests(void);
void register_layout_tests(void);

void setUp(void) {
}

void tearDown(void) {
}

int main(void) {
    UNITY_BEGIN();
    register_filesystem_tests();
    register_layout_tests();
    return UNITY_END();
}
