/* Renders compact operation phases without mixing progress with command results. */

#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

static int stderr_is_interactive(void) {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

static void render_interactive(UiProgress *progress, const char *suffix) {
    char line[256];
    int written;
    size_t width;

    if (progress == NULL || !progress->open || !progress->interactive || suffix == NULL) {
        return;
    }
    written = snprintf(line, sizeof(line), "==> %s...%s", progress->label, suffix);
    if (written < 0) {
        return;
    }
    width = (size_t)written;
    if (width >= sizeof(line)) {
        width = sizeof(line) - 1u;
    }

    fputc('\r', stderr);
    fputs(line, stderr);
    if (progress->width > width) {
        size_t padding = progress->width - width;
        while (padding-- > 0) {
            fputc(' ', stderr);
        }
    }
    fflush(stderr);
    progress->width = width;
}

void ui_phase(const char *format, ...) {
    va_list args;

    if (format == NULL) {
        return;
    }
    fputs("==> ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

void ui_progress_begin(UiProgress *progress, const char *label) {
    if (progress == NULL || (label == NULL || label[0] == '\0')) {
        return;
    }

    memset(progress, 0, sizeof(*progress));
    if (snprintf(progress->label, sizeof(progress->label), "%s", label) < 0) {
        return;
    }
    progress->label[sizeof(progress->label) - 1u] = '\0';
    progress->interactive = stderr_is_interactive();
    progress->open = 1;

    if (progress->interactive) {
        render_interactive(progress, "");
    } else {
        fprintf(stderr, "==> %s...\n", progress->label);
        fflush(stderr);
    }
}

void ui_progress_update_bytes(UiProgress *progress, uint64_t current, uint64_t total) {
    char suffix[128];

    if (progress == NULL || !progress->open || !progress->interactive || current == 0) {
        return;
    }
    if (total > 0) {
        unsigned percent = (unsigned)((double)current * 100.0 / (double)total);
        if (percent > 100u) {
            percent = 100u;
        }
        (void)snprintf(suffix,
                       sizeof(suffix),
                       " %u%% (%.1f MiB / %.1f MiB)",
                       percent,
                       (double)current / (1024.0 * 1024.0),
                       (double)total / (1024.0 * 1024.0));
    } else {
        (void)snprintf(suffix,
                       sizeof(suffix),
                       " %.1f MiB",
                       (double)current / (1024.0 * 1024.0));
    }
    render_interactive(progress, suffix);
}

void ui_progress_update_count(UiProgress *progress, size_t current, const char *unit) {
    char suffix[96];

    if (progress == NULL || !progress->open || !progress->interactive || current == 0 ||
        (unit == NULL || unit[0] == '\0')) {
        return;
    }
    if (progress->last_count != 0 && current != 1 && current < progress->last_count + 64u) {
        return;
    }
    (void)snprintf(suffix,
                   sizeof(suffix),
                   " %zu %s%s",
                   current,
                   unit,
                   current == 1 ? "" : "s");
    render_interactive(progress, suffix);
    progress->last_count = current;
}

void ui_progress_end(UiProgress *progress) {
    if (progress == NULL || !progress->open) {
        return;
    }
    if (progress->interactive) {
        fputc('\n', stderr);
        fflush(stderr);
    }
    memset(progress, 0, sizeof(*progress));
}
