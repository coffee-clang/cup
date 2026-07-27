#ifndef CUP_WINDOWS_UTF_H
#define CUP_WINDOWS_UTF_H

#if defined(_WIN32)

#include "error.h"

#include <limits.h>
#include <windows.h>
#include <wchar.h>

/* Convert one complete UTF-8 string at the Win32 boundary without accepting invalid input. */
static inline CupError windows_utf8_to_wide(const char *input,
                                             wchar_t *output,
                                             size_t output_count) {
    int written;

    if (input == NULL || input[0] == '\0' || output == NULL || output_count == 0 ||
        output_count > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output, (int)output_count);
    return written == 0 ? CUP_ERR_FILESYSTEM : CUP_OK;
}

#endif

#endif /* CUP_WINDOWS_UTF_H */
