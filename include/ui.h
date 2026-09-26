#ifndef CUP_UI_H
#define CUP_UI_H

/* Compact user-facing operation progress. Results stay on stdout; transient phases and
 * progress use stderr so query output remains machine-friendly when redirected. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int interactive;
    int open;
    size_t width;
    size_t last_count;
    char label[96];
} UiProgress;

/* Print one non-progress operational phase to stderr. */
void ui_phase(const char *format, ...);

/* Start one potentially long phase. Interactive terminals reuse one line; redirected output
 * receives exactly one stable phase line. */
void ui_progress_begin(UiProgress *progress, const char *label);
void ui_progress_update_bytes(UiProgress *progress, uint64_t current, uint64_t total);
void ui_progress_update_count(UiProgress *progress, size_t current, const char *unit);
void ui_progress_end(UiProgress *progress);

#endif /* CUP_UI_H */
