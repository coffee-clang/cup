/*
 * Creates a release-test executable by replacing one same-length byte string in a binary.
 * The helper is test-only and does not interpret executable formats.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, unsigned char **data, size_t *size) {
    FILE *file;
    long length;
    unsigned char *buffer;

    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    buffer = malloc(length == 0 ? 1u : (size_t)length);
    if (buffer == NULL || fread(buffer, 1, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        free(buffer);
        return 0;
    }

    *data = buffer;
    *size = (size_t)length;
    return 1;
}

static int write_file(const char *path, const unsigned char *data, size_t size) {
    FILE *file = fopen(path, "wb");
    int ok;

    if (file == NULL) {
        return 0;
    }
    ok = fwrite(data, 1, size, file) == size && fflush(file) == 0 && fclose(file) == 0;
    if (!ok) {
        remove(path);
    }
    return ok;
}

int main(int argc, char **argv) {
    unsigned char *data = NULL;
    size_t size = 0;
    size_t old_length;
    size_t offset;
    size_t replacements = 0;

    if (argc != 5) {
        fprintf(stderr, "Usage: binary-patch <source> <destination> <old> <new>\n");
        return 2;
    }

    old_length = strlen(argv[3]);
    if (old_length == 0 || old_length != strlen(argv[4])) {
        fprintf(stderr, "Replacement strings must be non-empty and have equal length.\n");
        return 2;
    }
    if (!read_file(argv[1], &data, &size)) {
        fprintf(stderr, "Could not read source binary.\n");
        return 1;
    }

    for (offset = 0; offset + old_length <= size; ++offset) {
        if (memcmp(data + offset, argv[3], old_length) == 0) {
            memcpy(data + offset, argv[4], old_length);
            replacements++;
            offset += old_length - 1;
        }
    }

    if (replacements == 0 || !write_file(argv[2], data, size)) {
        fprintf(stderr, replacements == 0 ? "Version string was not found.\n"
                                          : "Could not write destination binary.\n");
        free(data);
        return 1;
    }

    free(data);
    printf("Patched %zu occurrence(s).\n", replacements);
    return 0;
}
