/*
 * Test focus: Exercises archive preflight fault decisions with a deterministic libarchive
 * facade; real archive compatibility stays in the package-archive and extraction suites.
 */

#include "error.h"
#include "system.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "../../src/package_archive_format.c"

#define archive_read_new fake_read_new
#define archive_read_support_filter_none fake_filter_none
#define archive_read_support_filter_gzip fake_filter_gzip
#define archive_read_support_filter_xz fake_filter_xz
#define archive_read_support_format_tar fake_format_tar
#define archive_read_support_format_zip fake_format_zip
#define archive_read_open_filename fake_open_file
#define archive_read_close fake_read_close
#define archive_read_free fake_read_free
#define archive_read_next_header fake_next_header
#define archive_read_data_block fake_data_block
#define archive_entry_filetype fake_entry_type
#define archive_entry_hardlink fake_hardlink
#define archive_entry_size fake_entry_size
#define archive_format fake_archive_format
#define archive_filter_code fake_filter_code
#include "../../src/package_archive.c"
#undef archive_read_new
#undef archive_read_support_filter_none
#undef archive_read_support_filter_gzip
#undef archive_read_support_filter_xz
#undef archive_read_support_format_tar
#undef archive_read_support_format_zip
#undef archive_read_open_filename
#undef archive_read_close
#undef archive_read_free
#undef archive_read_next_header
#undef archive_read_data_block
#undef archive_entry_filetype
#undef archive_entry_hardlink
#undef archive_entry_size
#undef archive_format
#undef archive_filter_code

#define MAX_DATA_STEPS 6

static unsigned char reader_token;
static unsigned char entry_token;
static int new_is_null;
static int filter_status;
static int format_status;
static int detected_format;
static int detected_filter;
static int open_status;
static int close_status;
static int free_status;
static int header_status;
static size_t header_count;
static size_t header_calls;
static int data_status[MAX_DATA_STEPS];
static size_t data_sizes[MAX_DATA_STEPS];
static la_int64_t data_offsets[MAX_DATA_STEPS];
static size_t data_count;
static size_t data_calls;
static __LA_MODE_T entry_type;
static const char *hardlink_name;
static la_int64_t entry_size;
static CupError kind_result;
static SystemPathKind path_kind;
static CupError size_result;
static long long file_size;
static size_t interrupt_calls;
static size_t interrupt_at;

static void reset_scenario(void) {
    size_t i;
    new_is_null = 0;
    filter_status = ARCHIVE_OK;
    format_status = ARCHIVE_OK;
    detected_format = ARCHIVE_FORMAT_TAR;
    detected_filter = ARCHIVE_FILTER_GZIP;
    open_status = ARCHIVE_OK;
    close_status = ARCHIVE_OK;
    free_status = ARCHIVE_OK;
    header_status = ARCHIVE_OK;
    header_count = 1;
    header_calls = 0;
    data_count = 1;
    data_calls = 0;
    entry_type = AE_IFREG;
    hardlink_name = NULL;
    entry_size = 4;
    kind_result = CUP_OK;
    path_kind = SYSTEM_PATH_REGULAR_FILE;
    size_result = CUP_OK;
    file_size = 10;
    interrupt_calls = 0;
    interrupt_at = 0;
    for (i = 0; i < MAX_DATA_STEPS; ++i) {
        data_status[i] = ARCHIVE_EOF;
        data_sizes[i] = 0;
        data_offsets[i] = 0;
    }
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

struct archive *fake_read_new(void) {
    return new_is_null ? NULL : (struct archive *)&reader_token;
}

int fake_filter_none(struct archive *reader) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    return filter_status;
}
int fake_filter_gzip(struct archive *reader) {
    return fake_filter_none(reader);
}
int fake_filter_xz(struct archive *reader) {
    return fake_filter_none(reader);
}
int fake_format_tar(struct archive *reader) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    return format_status;
}
int fake_format_zip(struct archive *reader) {
    return fake_format_tar(reader);
}

int fake_open_file(struct archive *reader, const char *path, size_t block_size) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_size_t(ARCHIVE_READ_BLOCK_SIZE, block_size);
    return open_status;
}

int fake_read_close(struct archive *reader) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    return close_status;
}

int fake_read_free(struct archive *reader) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    return free_status;
}

int fake_next_header(struct archive *reader, struct archive_entry **entry) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    if (header_status != ARCHIVE_OK) {
        return header_status;
    }
    if (header_calls++ >= header_count) {
        return ARCHIVE_EOF;
    }
    *entry = (struct archive_entry *)&entry_token;
    return ARCHIVE_OK;
}

int fake_data_block(struct archive *reader, const void **buffer, size_t *size, la_int64_t *offset) {
    static const unsigned char data[8] = {0};
    size_t index = data_calls++;
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    if (index >= data_count) {
        return ARCHIVE_EOF;
    }
    *buffer = data;
    *size = data_sizes[index];
    *offset = data_offsets[index];
    return data_status[index];
}

__LA_MODE_T fake_entry_type(struct archive_entry *entry) {
    TEST_ASSERT_TRUE(entry == (struct archive_entry *)&entry_token);
    return entry_type;
}

const char *fake_hardlink(struct archive_entry *entry) {
    TEST_ASSERT_TRUE(entry == (struct archive_entry *)&entry_token);
    return hardlink_name;
}

la_int64_t fake_entry_size(struct archive_entry *entry) {
    TEST_ASSERT_TRUE(entry == (struct archive_entry *)&entry_token);
    return entry_size;
}

int fake_archive_format(struct archive *reader) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    return detected_format;
}

int fake_filter_code(struct archive *reader, int index) {
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
    TEST_ASSERT_EQUAL_INT(0, index);
    return detected_filter;
}
CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    TEST_ASSERT_NOT_NULL(path);
    if (kind_result == CUP_OK) {
        *kind = path_kind;
    }
    return kind_result;
}

CupError system_file_size(const char *path, long long *size) {
    TEST_ASSERT_NOT_NULL(path);
    if (size_result == CUP_OK) {
        *size = file_size;
    }
    return size_result;
}

int interrupt_requested(void) {
    interrupt_calls++;
    return interrupt_at != 0 && interrupt_calls == interrupt_at;
}

int text_is_empty(const char *value) {
    return value == NULL || value[0] == '\0';
}

static void set_data(int status, size_t size, la_int64_t offset) {
    data_count = 1;
    data_status[0] = status;
    data_sizes[0] = size;
    data_offsets[0] = offset;
}

static void test_open_failures(void) {
    struct archive *reader = (struct archive *)1;

    new_is_null = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, package_archive_open_reader(&reader, "package.tar"));
    TEST_ASSERT_NULL(reader);

    reset_scenario();
    filter_status = ARCHIVE_FATAL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, package_archive_open_reader(&reader, "package.tar"));

    reset_scenario();
    format_status = ARCHIVE_FATAL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, package_archive_open_reader(&reader, "package.tar"));

    reset_scenario();
    open_status = ARCHIVE_FATAL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, package_archive_open_reader(&reader, "package.tar"));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_open_reader(&reader, "package.tar"));
    TEST_ASSERT_TRUE(reader == (struct archive *)&reader_token);
}

static void test_close_failures(void) {
    TEST_ASSERT_TRUE(close_reader((struct archive *)&reader_token));
    close_status = ARCHIVE_FATAL;
    TEST_ASSERT_FALSE(close_reader((struct archive *)&reader_token));
    close_status = ARCHIVE_OK;
    free_status = ARCHIVE_FATAL;
    TEST_ASSERT_FALSE(close_reader((struct archive *)&reader_token));
}

static void test_declared_sizes(void) {
    uint64_t total = 2;
    struct archive_entry *entry = (struct archive_entry *)&entry_token;

    entry_type = AE_IFDIR;
    TEST_ASSERT_EQUAL_INT(CUP_OK, account_declared_size(entry, &total));
    TEST_ASSERT_TRUE(total == 2u);

    entry_type = AE_IFREG;
    hardlink_name = "target";
    TEST_ASSERT_EQUAL_INT(CUP_OK, account_declared_size(entry, &total));
    TEST_ASSERT_TRUE(total == 2u);

    hardlink_name = NULL;
    entry_size = -1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE, account_declared_size(entry, &total));

    entry_size = (la_int64_t)MAX_PACKAGE_EXTRACTED_BYTES;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE, account_declared_size(entry, &total));

    total = 0;
    entry_size = 4;
    TEST_ASSERT_EQUAL_INT(CUP_OK, account_declared_size(entry, &total));
    TEST_ASSERT_TRUE(total == 4u);
}

static void test_data_failures(void) {
    uint64_t total;

    interrupt_at = 1;
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          read_entry_data((struct archive *)&reader_token, 4, &total));

    reset_scenario();
    set_data(ARCHIVE_FATAL, 0, 0);
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE,
                          read_entry_data((struct archive *)&reader_token, 4, &total));

    reset_scenario();
    set_data(ARCHIVE_OK, 1, -1);
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE,
                          read_entry_data((struct archive *)&reader_token, 4, &total));

    reset_scenario();
    set_data(ARCHIVE_OK, (size_t)-1, 1);
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE,
                          read_entry_data((struct archive *)&reader_token, -1, &total));

    reset_scenario();
    set_data(ARCHIVE_OK, 4, 0);
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, read_entry_data((struct archive *)&reader_token, -1, &total));

    reset_scenario();
    set_data(ARCHIVE_OK, 5, 0);
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          read_entry_data((struct archive *)&reader_token, 4, &total));

    reset_scenario();
    set_data(ARCHIVE_OK, 2, 0);
    total = MAX_PACKAGE_EXTRACTED_BYTES - 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          read_entry_data((struct archive *)&reader_token, 4, &total));

    reset_scenario();
    data_count = 2;
    data_status[0] = ARCHIVE_OK;
    data_sizes[0] = 4;
    data_offsets[0] = 0;
    data_status[1] = ARCHIVE_EOF;
    total = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, read_entry_data((struct archive *)&reader_token, 4, &total));
    TEST_ASSERT_TRUE(total == 4u);
}

static void test_preflight_failures(void) {
    int valid = 1;

    kind_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_archive_is_valid("package.tar", "tar.gz", &valid));

    reset_scenario();
    size_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_archive_is_valid("package.tar", "tar.gz", &valid));

    reset_scenario();
    interrupt_at = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          package_archive_is_valid("package.tar", "tar.gz", &valid));

    reset_scenario();
    header_status = ARCHIVE_FATAL;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    entry_size = -1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    set_data(ARCHIVE_FATAL, 0, 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    set_data(ARCHIVE_OK, 5, 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    interrupt_at = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          package_archive_is_valid("package.tar", "tar.gz", &valid));
}

static void test_preflight_limits(void) {
    int valid = 1;

    header_count = MAX_PACKAGE_ARCHIVE_ENTRIES + 1u;
    entry_type = AE_IFDIR;
    entry_size = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    entry_size = (la_int64_t)MAX_PACKAGE_ENTRY_BYTES + 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    entry_type = AE_IFDIR;
    entry_size = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    close_status = ARCHIVE_FATAL;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_FALSE(valid);

    reset_scenario();
    data_count = 2;
    data_status[0] = ARCHIVE_OK;
    data_sizes[0] = 4;
    data_offsets[0] = 0;
    data_status[1] = ARCHIVE_EOF;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid("package.tar", "tar.gz", &valid));
    TEST_ASSERT_TRUE(valid);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_open_failures);
    RUN_TEST(test_close_failures);
    RUN_TEST(test_declared_sizes);
    RUN_TEST(test_data_failures);
    RUN_TEST(test_preflight_failures);
    RUN_TEST(test_preflight_limits);
    return UNITY_END();
}
