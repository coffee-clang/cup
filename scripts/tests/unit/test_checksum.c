#include "checksum.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char temp_dir[] = "/tmp/cup-checksum-test-XXXXXX";

void setUp(void) {}
void tearDown(void) {}

static void build_path(char *out, size_t size, const char *name) {
    TEST_ASSERT_TRUE(snprintf(out, size, "%s/%s", temp_dir, name) > 0);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_sha256_vectors(void) {
    char path[256];
    char digest[SHA256_HEX_LENGTH + 1];
    unsigned char binary[] = {0x00, 0x01, 0x02, 0xff, 0x80};
    unsigned char blocks[20000];
    size_t i;

    build_path(path, sizeof(path), "empty");
    write_bytes(path, "", 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", digest);

    build_path(path, sizeof(path), "abc");
    write_bytes(path, "abc", 3);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest);

    build_path(path, sizeof(path), "binary");
    write_bytes(path, binary, sizeof(binary));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("e3a900fde4e52eea4ebedc82e7790ccf830552111fca7172c30561558d86d827", digest);

    for (i = 0; i < sizeof(blocks); ++i) blocks[i] = (unsigned char)(i * 31u + 7u);
    build_path(path, sizeof(path), "blocks");
    write_bytes(path, blocks, sizeof(blocks));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("f8aed270a592b255d90b04785c0b130a968ae204948fc2755db1861c810c6c83", digest);
}

static void test_sha256_padding_boundaries(void) {
    static const struct {
        size_t length;
        const char *digest;
    } vectors[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}
    };
    unsigned char data[65];
    char path[256];
    char digest[SHA256_HEX_LENGTH + 1];
    size_t i;

    memset(data, 'a', sizeof(data));
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/padding-%zu",
            temp_dir, vectors[i].length) > 0);
        write_bytes(path, data, vectors[i].length);
        TEST_ASSERT_EQUAL_INT(CUP_OK,
            checksum_sha256_file(path, digest, sizeof(digest)));
        TEST_ASSERT_EQUAL_STRING(vectors[i].digest, digest);
    }
}

static void test_sha256_million_a(void) {
    unsigned char block[4096];
    char path[256];
    char digest[SHA256_HEX_LENGTH + 1];
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
    TEST_ASSERT_EQUAL_INT(CUP_OK,
        checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING(
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        digest);
}

static void test_checksum_records(void) {
    char asset[256], sums[256];
    char digest[SHA256_HEX_LENGTH + 1];
    int matches;
    FILE *file;

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
        checksum_find_expected(sums, "asset.bin", digest, sizeof(digest)));

    file = fopen(sums, "w");
    TEST_ASSERT_NOT_NULL(file);
    fprintf(file, "%s  other.bin\n", digest);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
        checksum_find_expected(sums, "asset.bin", digest, sizeof(digest)));
}

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    UNITY_BEGIN();
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_sha256_padding_boundaries);
    RUN_TEST(test_sha256_million_a);
    RUN_TEST(test_checksum_records);
    return UNITY_END();
}
