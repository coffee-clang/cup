/*
 * Exercises SHA-256 vectors, file hashing and strict SHA256SUMS record selection.
 */

#include "checksum.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static int interrupt_after_calls;
static int interrupt_calls;

int interrupt_requested(void) {
    if (interrupt_after_calls < 0) {
        return 0;
    }
    return interrupt_calls++ >= interrupt_after_calls;
}

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
    interrupt_after_calls = -1;
    interrupt_calls = 0;
}

void tearDown(void) {
}

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Test cases grouped by the public contract they exercise. */

static void test_digest_format(void) {
    const char *valid =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    TEST_ASSERT_TRUE(checksum_digest_is_canonical(valid));
    TEST_ASSERT_FALSE(checksum_digest_is_canonical(NULL));
    TEST_ASSERT_FALSE(checksum_digest_is_canonical(""));
    TEST_ASSERT_FALSE(checksum_digest_is_canonical(
        "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_FALSE(checksum_digest_is_canonical(
        "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_FALSE(checksum_digest_is_canonical(
        "123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
}

static void test_sha256_vectors(void) {
    char path[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    unsigned char binary[] = {0x00, 0x01, 0x02, 0xff, 0x80};
    unsigned char blocks[20000];
    size_t i;

    build_path(path, sizeof(path), "empty");
    write_bytes(path, "", 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                             digest);

    build_path(path, sizeof(path), "abc");
    write_bytes(path, "abc", 3);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             digest);

    build_path(path, sizeof(path), "binary");
    write_bytes(path, binary, sizeof(binary));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("e3a900fde4e52eea4ebedc82e7790ccf830552111fca7172c30561558d86d827",
                             digest);

    for (i = 0; i < sizeof(blocks); ++i) {
        blocks[i] = (unsigned char)(i * 31u + 7u);
    }
    build_path(path, sizeof(path), "blocks");
    write_bytes(path, blocks, sizeof(blocks));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("f8aed270a592b255d90b04785c0b130a968ae204948fc2755db1861c810c6c83",
                             digest);
}

static void test_padding_boundaries(void) {
    static const struct {
        size_t length;
        const char *digest;
    } vectors[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    };
    unsigned char data[65];
    char path[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    size_t i;

    memset(data, 'a', sizeof(data));
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        TEST_ASSERT_TRUE(
            snprintf(path, sizeof(path), "%s/padding-%zu", temp_dir, vectors[i].length) > 0);
        write_bytes(path, data, vectors[i].length);
        TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
        TEST_ASSERT_EQUAL_STRING(vectors[i].digest, digest);
    }
}

static void test_sha256_million_a(void) {
    unsigned char block[4096];
    char path[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    FILE *file;
    size_t remaining = 1000000;

    memset(block, 'a', sizeof(block));
    build_path(path, sizeof(path), "million-a");
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    while (remaining > 0) {
        size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
        TEST_ASSERT_EQUAL_size_t(count, fwrite(block, 1, count, file));
        remaining -= count;
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                             digest);
}

static void test_hash_output_contracts(void) {
    static const unsigned char data[] = { 'a', 'b', 'c' };
    char asset[256], sums[256], missing[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    ChecksumDocument document;
    FILE *file;
    int matches;

    build_path(asset, sizeof(asset), "output-contract.bin");
    build_path(sums, sizeof(sums), "output-contract-SHA256SUMS");
    build_path(missing, sizeof(missing), "output-contract-missing");
    write_bytes(asset, data, sizeof(data));

    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          checksum_sha256_bytes(data, sizeof(data), digest, 1));
    TEST_ASSERT_EQUAL_STRING("", digest);
    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          checksum_sha256_bytes(NULL, sizeof(data), digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("", digest);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_bytes(data, sizeof(data), digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest);

    file = fopen(asset, "rb");
    TEST_ASSERT_NOT_NULL(file);
    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          checksum_sha256_stream(file, digest, CHECKSUM_SHA256_HEX_LENGTH));
    TEST_ASSERT_EQUAL_STRING("", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          checksum_sha256_file(asset, digest, CHECKSUM_SHA256_HEX_LENGTH));
    TEST_ASSERT_EQUAL_STRING("", digest);

    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(asset, digest, sizeof(digest)));
    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  output-contract.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    checksum_document_init(&document);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_document_load(&document, sums));
    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        checksum_document_find_expected(
            &document, "output-contract.bin", digest, CHECKSUM_SHA256_HEX_LENGTH));
    TEST_ASSERT_EQUAL_STRING("", digest);

    matches = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        checksum_document_verify_file(NULL, "output-contract.bin", asset, &matches));
    TEST_ASSERT_FALSE(matches);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, checksum_document_load(&document, missing));
    TEST_ASSERT_NULL(document.entries);
    TEST_ASSERT_EQUAL_size_t(0, document.count);
    TEST_ASSERT_FALSE(document.identity.valid);
    checksum_document_free(&document);
}

static void test_hashing_honors_interrupts(void) {
    unsigned char blocks[20000];
    char path[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    size_t i;

    for (i = 0; i < sizeof(blocks); ++i) {
        blocks[i] = (unsigned char)(i * 17u + 3u);
    }
    build_path(path, sizeof(path), "interrupt-blocks");
    write_bytes(path, blocks, sizeof(blocks));
    interrupt_after_calls = 1;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("", digest);
    TEST_ASSERT_TRUE(interrupt_calls >= 2);

    /* A request arriving during the final short read is consumed before digest publication. */
    build_path(path, sizeof(path), "interrupt-final-read");
    write_bytes(path, "abc", 3);
    interrupt_calls = 0;
    interrupt_after_calls = 1;
    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("", digest);
    TEST_ASSERT_EQUAL_INT(2, interrupt_calls);
}

static CupError load_and_find_expected(const char *path,
                                       const char *asset_name,
                                       char *digest,
                                       size_t digest_size) {
    ChecksumDocument document;
    CupError err;

    checksum_document_init(&document);
    err = checksum_document_load(&document, path);
    if (err == CUP_OK) {
        err = checksum_document_find_expected(
            &document, asset_name, digest, digest_size);
    }
    checksum_document_free(&document);
    return err;
}


static void test_checksum_records(void) {
    char asset[256], sums[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char uppercase[CHECKSUM_SHA256_HEX_LENGTH + 1];
    int matches;
    FILE *file;
    size_t i;

    build_path(asset, sizeof(asset), "asset.bin");
    build_path(sums, sizeof(sums), "SHA256SUMS");
    write_bytes(asset, "abc", 3);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(asset, digest, sizeof(digest)));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  asset.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_verify_file(sums, "asset.bin", asset, &matches));
    TEST_ASSERT_TRUE(matches);

    for (i = 0; i < sizeof(uppercase); ++i) {
        char value = digest[i];
        uppercase[i] = value >= 'a' && value <= 'f' ? (char)(value - 'a' + 'A') : value;
    }
    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s    *asset.bin\n", uppercase);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          load_and_find_expected(sums, "asset.bin", digest, sizeof(digest)));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  asset.bin\n",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          load_and_find_expected(sums, "asset.bin", digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             digest);

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%064d  asset.bin\n", 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_verify_file(sums, "asset.bin", asset, &matches));
    TEST_ASSERT_FALSE(matches);

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  asset.bin\n%s  asset.bin\n", digest, digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          load_and_find_expected(sums, "asset.bin", digest, sizeof(digest)));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  other.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          load_and_find_expected(sums, "asset.bin", digest, sizeof(digest)));
}


static CupError load_checksum_count(const char *path, size_t *count) {
    ChecksumDocument document;
    CupError err;

    if (count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *count = 0;
    checksum_document_init(&document);
    err = checksum_document_load(&document, path);
    if (err == CUP_OK) {
        *count = document.count;
    }
    checksum_document_free(&document);
    return err;
}

static void test_checksum_validation(void) {
    char asset[256], sums[256], missing[256];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    const char *assets[2] = {"asset.bin", "other.bin"};
    const char *duplicate_assets[2] = {"asset.bin", "asset.bin"};
    const char *unsafe_assets[2] = {"asset.bin", "../other.bin"};
    size_t count;
    int matches;
    FILE *file;

    build_path(asset, sizeof(asset), "schema-asset.bin");
    build_path(sums, sizeof(sums), "schema-SHA256SUMS");
    build_path(missing, sizeof(missing), "missing");
    write_bytes(asset, "abc", 3);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(asset, digest, sizeof(digest)));

    /* File hashing rejects invalid destinations and missing input without stale output. */
    strcpy(digest, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          checksum_sha256_file(NULL, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("", digest);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, checksum_sha256_file("", digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, checksum_sha256_file(asset, NULL, sizeof(digest)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          checksum_sha256_file(asset, digest, CHECKSUM_SHA256_HEX_LENGTH));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          checksum_sha256_file(missing, digest, sizeof(digest)));

    /* Snapshot lookup validates the document, requested asset name, and output buffer. */
    {
        ChecksumDocument document;

        checksum_document_init(&document);
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            checksum_document_find_expected(NULL, "asset.bin", digest, sizeof(digest)));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            checksum_document_find_expected(&document, "../asset.bin", digest, sizeof(digest)));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            checksum_document_find_expected(&document, "asset.bin", NULL, sizeof(digest)));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_BUFFER_TOO_SMALL,
            checksum_document_find_expected(
                &document, "asset.bin", digest, CHECKSUM_SHA256_HEX_LENGTH));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              checksum_document_load(&document, missing));
        checksum_document_free(&document);
    }

    /* Verification validates its result and file arguments before reading either file. */
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          checksum_verify_file(sums, "asset.bin", asset, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          checksum_verify_file(sums, "asset.bin", "", &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          checksum_verify_file(missing, "asset.bin", asset, &matches));

    /* Whole-file validation resets outputs and rejects empty or missing checksum sets. */
    count = 99;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, load_checksum_count(NULL, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, load_checksum_count(sums, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, load_checksum_count(missing, &count));

    write_bytes(sums, "", 0);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    /* A canonical checksum set uses lowercase digests and exactly two spaces. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(asset, digest, sizeof(digest)));
    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  asset.bin\n", digest);
    fprintf(file, "%s  other.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, load_checksum_count(sums, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_validate_assets(sums, assets, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, checksum_validate_assets(sums, assets, 1));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s\tasset.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, " %s  asset.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  asset.bin \n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    /* Asset-set validation rejects incomplete, duplicate, and unsafe declarations. */
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, checksum_validate_assets(NULL, assets, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, checksum_validate_assets(sums, NULL, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, checksum_validate_assets(sums, assets, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          checksum_validate_assets(missing, duplicate_assets, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          checksum_validate_assets(missing, unsafe_assets, 2));

    /* Malformed separators, unsafe paths, and short records fail schema validation. */
    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%064dxasset.bin\n", 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "g%063d  asset.bin\n", 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%064d  ../asset.bin\n", 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "short\n");
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, load_checksum_count(sums, &count));
}


int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-checksum-test"));
    UNITY_BEGIN();
    RUN_TEST(test_digest_format);
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_padding_boundaries);
    RUN_TEST(test_sha256_million_a);
    RUN_TEST(test_hash_output_contracts);
    RUN_TEST(test_hashing_honors_interrupts);
    RUN_TEST(test_checksum_records);
    RUN_TEST(test_checksum_validation);
    return UNITY_END();
}
