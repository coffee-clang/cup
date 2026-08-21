/*
 * Provides the shared Unity wrapper for system_posix, filesystem and layout tests
 * that require one process-wide storage environment.
 */

#include "layout.h"
#include "unity.h"

void register_system_posix_tests(void);
void register_filesystem_tests(void);
void register_layout_tests(void);

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
}

void tearDown(void) {
    layout_root_snapshot_end();
}


int main(void) {
    UNITY_BEGIN();
    register_system_posix_tests();
    register_filesystem_tests();
    register_layout_tests();
    return UNITY_END();
}
