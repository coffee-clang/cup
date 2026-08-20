#ifndef CUP_WINDOWS_UTF_H
#define CUP_WINDOWS_UTF_H

#if defined(_WIN32)

#include "constants.h"
#include "error.h"
#include "path.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>
#if defined(CUP_PATH_OPS_MSYS_WINDOWS_BACKEND)
#include <sys/cygwin.h>
#endif

/* Preserve the useful distinction between malformed text, insufficient storage and native I/O
 * failures at the Windows API boundary. */
static inline CupError windows_text_conversion_error(void) {
    DWORD error = GetLastError();

    if (error == ERROR_INSUFFICIENT_BUFFER) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (error == ERROR_NO_UNICODE_TRANSLATION) {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_ERR_FILESYSTEM;
}

/* Convert one complete UTF-8 string at the Windows API boundary without accepting invalid input. */
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
    return written == 0 ? windows_text_conversion_error() : CUP_OK;
}


static inline CupError windows_prepare_utf8_path(const char *input,
                                                 char *output,
                                                 size_t output_size) {
    size_t input_length;

    if (input == NULL || output == NULL || output_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
#if defined(CUP_PATH_OPS_MSYS_WINDOWS_BACKEND)
    if (input[0] == '/') {
        wchar_t native[MAX_PATH_LEN];
        ssize_t required = cygwin_conv_path(CCP_POSIX_TO_WIN_W, input, NULL, 0);
        int written;

        if (required < 0) {
            return errno == ENAMETOOLONG || errno == ENOSPC
                       ? CUP_ERR_BUFFER_TOO_SMALL
                       : CUP_ERR_FILESYSTEM;
        }
        if ((size_t)required > sizeof(native)) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        if (cygwin_conv_path(CCP_POSIX_TO_WIN_W, input, native, sizeof(native)) != 0) {
            return errno == ENAMETOOLONG || errno == ENOSPC
                       ? CUP_ERR_BUFFER_TOO_SMALL
                       : CUP_ERR_FILESYSTEM;
        }
        if (output_size > INT_MAX) {
            return CUP_ERR_INVALID_INPUT;
        }
        written = WideCharToMultiByte(CP_UTF8,
                                      WC_ERR_INVALID_CHARS,
                                      native,
                                      -1,
                                      output,
                                      (int)output_size,
                                      NULL,
                                      NULL);
        if (written == 0) {
            return windows_text_conversion_error();
        }
        return path_normalize(output);
    }
#endif
    input_length = strlen(input);
    if (input_length >= output_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(output, input, input_length + 1);
    return path_normalize(output);
}

/* Convert a UTF-8 filesystem path to one absolute Windows long-path name. */
static inline CupError windows_utf8_to_wide_path(const char *input,
                                                  wchar_t *output,
                                                  size_t output_count) {
    char normalized[MAX_PATH_LEN];
    wchar_t converted[MAX_PATH_LEN];
    wchar_t absolute[MAX_PATH_LEN];
    DWORD length;
    size_t i;
    size_t required;

    if (input == NULL || output == NULL || output_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    {
        CupError err = windows_prepare_utf8_path(input, normalized, sizeof(normalized));

        if (err != CUP_OK) {
            return err;
        }
        err = windows_utf8_to_wide(normalized, converted, MAX_PATH_LEN);
        if (err != CUP_OK) {
            return err;
        }
    }

    for (i = 0; converted[i] != L'\0'; ++i) {
        if (converted[i] == L'/') {
            converted[i] = L'\\';
        }
    }

    length = GetFullPathNameW(converted, MAX_PATH_LEN, absolute, NULL);
    if (length == 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (length >= MAX_PATH_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (absolute[0] == L'\\' && absolute[1] == L'\\') {
        required = 8 + wcslen(absolute + 2) + 1;
        if (required > output_count) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        wcscpy(output, L"\\\\?\\UNC\\");
        wcscat(output, absolute + 2);
    } else {
        required = 4 + wcslen(absolute) + 1;
        if (required > output_count) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        wcscpy(output, L"\\\\?\\");
        wcscat(output, absolute);
    }
    return CUP_OK;
}

/* External process arguments use ordinary absolute paths, not Windows device prefixes. */
static inline CupError windows_utf8_to_wide_process_path(const char *input,
                                                          wchar_t *output,
                                                          size_t output_count) {
    char normalized[MAX_PATH_LEN];
    wchar_t converted[MAX_PATH_LEN];
    wchar_t absolute[MAX_PATH_LEN];
    DWORD length;
    size_t i;
    size_t required;

    if (input == NULL || output == NULL || output_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    {
        CupError err = windows_prepare_utf8_path(input, normalized, sizeof(normalized));

        if (err != CUP_OK) {
            return err;
        }
        err = windows_utf8_to_wide(normalized, converted, MAX_PATH_LEN);
        if (err != CUP_OK) {
            return err;
        }
    }

    for (i = 0; converted[i] != L'\0'; ++i) {
        if (converted[i] == L'/') {
            converted[i] = L'\\';
        }
    }

    length = GetFullPathNameW(converted, MAX_PATH_LEN, absolute, NULL);
    if (length == 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (length >= MAX_PATH_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    required = wcslen(absolute) + 1;
    if (required > output_count) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(output, absolute, required * sizeof(*output));
    return CUP_OK;
}

#endif

#endif /* CUP_WINDOWS_UTF_H */
