/* Exercises the user-facing phase/progress renderer independently from command logic. */

#include "ui.h"
#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define cup_dup _dup
#define cup_dup2 _dup2
#define cup_close _close
#define cup_fileno _fileno
#else
#include <unistd.h>
#define cup_dup dup
#define cup_dup2 dup2
#define cup_close close
#define cup_fileno fileno
#endif

static FILE *capture_file;
static int saved_stderr_fd;

static void capture_begin(void) {
    int capture_fd;

    fflush(stderr);
    capture_file = tmpfile();
    TEST_ASSERT_NOT_NULL(capture_file);
    saved_stderr_fd = cup_dup(cup_fileno(stderr));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, saved_stderr_fd);
    capture_fd = cup_fileno(capture_file);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, capture_fd);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, cup_dup2(capture_fd, cup_fileno(stderr)));
}

static void capture_end(void) {
    if (saved_stderr_fd >= 0) {
        fflush(stderr);
        (void)cup_dup2(saved_stderr_fd, cup_fileno(stderr));
        (void)cup_close(saved_stderr_fd);
        saved_stderr_fd = -1;
    }
    if (capture_file != NULL) {
        fclose(capture_file);
        capture_file = NULL;
    }
}

static void capture_read(char *buffer, size_t size) {
    size_t read_count;

    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, (uint32_t)size);
    fflush(stderr);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture_file, 0, SEEK_SET));
    read_count = fread(buffer, 1, size - 1u, capture_file);
    TEST_ASSERT_EQUAL_INT(0, ferror(capture_file));
    buffer[read_count] = '\0';
}

void setUp(void) {
    capture_file = NULL;
    saved_stderr_fd = -1;
    capture_begin();
}

void tearDown(void) {
    capture_end();
}

static void test_phase_uses_operational_stream(void) {
    char output[256];

    ui_phase("Refreshing %s", "catalog");
    capture_read(output, sizeof(output));

    TEST_ASSERT_EQUAL_STRING("==> Refreshing catalog\n", output);
}

static void test_redirected_progress_emits_one_stable_phase(void) {
    UiProgress progress;
    char output[256];

    ui_progress_begin(&progress, "Downloading package");
    TEST_ASSERT_FALSE(progress.interactive);
    ui_progress_update_bytes(&progress, 1024u * 1024u, 2u * 1024u * 1024u);
    ui_progress_update_count(&progress, 128u, "file");
    ui_progress_end(&progress);
    capture_read(output, sizeof(output));

    TEST_ASSERT_EQUAL_STRING("==> Downloading package...\n", output);
    TEST_ASSERT_FALSE(progress.open);
}

static void test_interactive_byte_progress_reports_known_and_unknown_totals(void) {
    UiProgress progress;
    char output[1024];

    memset(&progress, 0, sizeof(progress));
    progress.interactive = 1;
    progress.open = 1;
    strcpy(progress.label, "Downloading package");

    ui_progress_update_bytes(&progress, 1024u * 1024u, 2u * 1024u * 1024u);
    ui_progress_update_bytes(&progress, 3u * 1024u * 1024u, 2u * 1024u * 1024u);
    ui_progress_update_bytes(&progress, 4u * 1024u * 1024u, 0u);
    ui_progress_end(&progress);
    capture_read(output, sizeof(output));

    TEST_ASSERT_NOT_NULL(strstr(output, "\r==> Downloading package... 50% (1.0 MiB / 2.0 MiB)"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\r==> Downloading package... 100% (3.0 MiB / 2.0 MiB)"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\r==> Downloading package... 4.0 MiB"));
    TEST_ASSERT_EQUAL_CHAR('\n', output[strlen(output) - 1u]);
}

static void test_interactive_count_progress_throttles_dense_updates(void) {
    UiProgress progress;
    char output[1024];

    memset(&progress, 0, sizeof(progress));
    progress.interactive = 1;
    progress.open = 1;
    strcpy(progress.label, "Extracting package");

    ui_progress_update_count(&progress, 1u, "file");
    ui_progress_update_count(&progress, 32u, "file");
    ui_progress_update_count(&progress, 65u, "file");
    ui_progress_end(&progress);
    capture_read(output, sizeof(output));

    TEST_ASSERT_NOT_NULL(strstr(output, "\r==> Extracting package... 1 file"));
    TEST_ASSERT_NULL(strstr(output, "32 files"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\r==> Extracting package... 65 files"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase_uses_operational_stream);
    RUN_TEST(test_redirected_progress_emits_one_stable_phase);
    RUN_TEST(test_interactive_byte_progress_reports_known_and_unknown_totals);
    RUN_TEST(test_interactive_count_progress_throttles_dense_updates);
    return UNITY_END();
}
