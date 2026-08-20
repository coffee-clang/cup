/* Exercises bounded text parsing, formatting and line-reading contracts. */

#include "error.h"
#include "text.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_text_basics(void) {
    char left[] = "  cup\t ";
    char spaces[] = " \t  ";
    char plain[] = "cup";

    TEST_ASSERT_TRUE(text_is_empty(NULL));
    TEST_ASSERT_TRUE(text_is_empty(""));
    TEST_ASSERT_FALSE(text_is_empty("cup"));
    TEST_ASSERT_NULL(text_trim(NULL));
    TEST_ASSERT_EQUAL_STRING("cup", text_trim(left));
    TEST_ASSERT_EQUAL_STRING("", text_trim(spaces));
    TEST_ASSERT_EQUAL_STRING("cup", text_trim(plain));
}

static void test_copy_format(void) {
    char buffer[8];

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(buffer, sizeof(buffer), "cup"));
    TEST_ASSERT_EQUAL_STRING("cup", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(buffer, sizeof(buffer), "1234567"));
    TEST_ASSERT_EQUAL_STRING("1234567", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_copy(NULL, sizeof(buffer), "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_copy(buffer, 0, "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_copy(buffer, sizeof(buffer), NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, text_copy(buffer, sizeof(buffer), "too-long"));

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy_lower_ascii(buffer, sizeof(buffer), "CuP"));
    TEST_ASSERT_EQUAL_STRING("cup", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy_lower_ascii(buffer, sizeof(buffer), buffer));
    TEST_ASSERT_EQUAL_STRING("cup", buffer);

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_format(buffer, sizeof(buffer), "%s", "0.2.0"));
    TEST_ASSERT_EQUAL_STRING("0.2.0", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_format(buffer, sizeof(buffer), "%s", "1234567"));
    TEST_ASSERT_EQUAL_STRING("1234567", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_format(NULL, sizeof(buffer), "%s", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_format(buffer, 0, "%s", "cup"));
    {
        CupError (*format_call)(char *, size_t, const char *, ...) = text_format;
        const char *empty = "";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, format_call(buffer, sizeof(buffer), empty));
    }
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          text_format(buffer, sizeof(buffer), "%s", "0.2.0-dev"));
}

static void test_split_values(void) {
    char input[] = "  compiler . linux-x64 . windows-x64  ";
    char component[32];
    char host[32];
    char target[32];
    TextBuffer parts[3] = {
        {.data = component, .capacity = sizeof(component)},
        {.data = host, .capacity = sizeof(host)},
        {.data = target, .capacity = sizeof(target)},
    };

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_split_exact(input, '.', parts, 3));
    TEST_ASSERT_EQUAL_STRING("compiler", component);
    TEST_ASSERT_EQUAL_STRING("linux-x64", host);
    TEST_ASSERT_EQUAL_STRING("windows-x64", target);

    {
        char too_few[] = "compiler.linux-x64";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(too_few, '.', parts, 3));
    }
    {
        char too_many[] = "a.b.c.d";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(too_many, '.', parts, 3));
    }
    {
        char empty_part[] = "compiler..linux-x64";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(empty_part, '.', parts, 3));
    }
    {
        char small[] = "long.value";
        char first[4];
        char second[8];
        TextBuffer small_parts[2] = {
            {.data = first, .capacity = sizeof(first)},
            {.data = second, .capacity = sizeof(second)},
        };
        TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                              text_split_exact(small, '.', small_parts, 2));
    }
}

static void test_split_guards(void) {
    char first[8];
    char second[8];
    TextBuffer parts[2] = {
        {.data = first, .capacity = sizeof(first)},
        {.data = second, .capacity = sizeof(second)},
    };

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(NULL, '.', parts, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact("", '.', parts, 2));
    {
        char input[] = "a.b";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '\0', parts, 2));
    }
    {
        char input[] = "a.b";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', NULL, 2));
    }
    {
        char input[] = "a.b";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', parts, 0));
    }

    parts[0].data = NULL;
    {
        char input[] = "a.b";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', parts, 2));
    }
    parts[0].data = first;
    parts[0].capacity = 0;
    {
        char input[] = "a.b";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', parts, 2));
    }
}

static void test_parse_uint(void) {
    unsigned value = 999u;

    TEST_ASSERT_TRUE(text_parse_uint("0", 255u, &value));
    TEST_ASSERT_EQUAL_UINT(0u, value);
    TEST_ASSERT_TRUE(text_parse_uint("255", 255u, &value));
    TEST_ASSERT_EQUAL_UINT(255u, value);
    TEST_ASSERT_TRUE(text_parse_uint("0", 0u, &value));
    TEST_ASSERT_EQUAL_UINT(0u, value);
    TEST_ASSERT_FALSE(text_parse_uint("1", 0u, &value));

    TEST_ASSERT_FALSE(text_parse_uint(NULL, 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("+1", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("-1", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint(" 1", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("1 ", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("01", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("256", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("1x", 255u, &value));
    TEST_ASSERT_FALSE(text_parse_uint("1", 255u, NULL));
}

static void test_document_reader(void) {
    static const unsigned char valid[] =
        "# comment\n\n key = value \nlast=value\n";
    static const unsigned char missing_lf[] = "key=value";
    static const unsigned char carriage[] = "key=value\r\n";
    static const unsigned char tab[] = "key=\tvalue\n";
    static const unsigned char nul[] = {'k', 'e', 'y', '=', 'v', 0, '\n'};
    TextDocumentReader reader;
    char line[32];
    int has_line = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_reader_init(&reader, valid, sizeof(valid) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_read_raw_line(&reader, line, sizeof(line), &has_line));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_STRING("# comment", line);
    TEST_ASSERT_EQUAL_size_t(1, reader.line_number);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_read_raw_line(&reader, line, sizeof(line), &has_line));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_STRING("", line);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_read_raw_line(&reader, line, sizeof(line), &has_line));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_STRING(" key = value ", line);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_reader_init(&reader, valid, sizeof(valid) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_read_line(&reader, line, sizeof(line), &has_line));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_STRING("key = value", line);
    TEST_ASSERT_EQUAL_size_t(3, reader.line_number);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_read_line(&reader, line, sizeof(line), &has_line));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_STRING("last=value", line);
    TEST_ASSERT_EQUAL_size_t(4, reader.line_number);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_document_read_line(&reader, line, sizeof(line), &has_line));
    TEST_ASSERT_FALSE(has_line);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_document_reader_init(NULL, valid, sizeof(valid) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_document_reader_init(&reader, NULL, sizeof(valid) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_document_reader_init(&reader, valid, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          text_document_reader_init(&reader, missing_lf, sizeof(missing_lf) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          text_document_reader_init(&reader, carriage, sizeof(carriage) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          text_document_reader_init(&reader, tab, sizeof(tab) - 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          text_document_reader_init(&reader, nul, sizeof(nul)));
}

static void test_key_value(void) {
    char line[] = "  key.name = value text  ";
    char key[16];
    char value[16];

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_parse_key_value(line, key, sizeof(key), value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("key.name", key);
    TEST_ASSERT_EQUAL_STRING("value text", value);

    {
        char no_separator[] = "key";
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            text_parse_key_value(no_separator, key, sizeof(key), value, sizeof(value)));
    }
    {
        char empty_key[] = " =value";
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            text_parse_key_value(empty_key, key, sizeof(key), value, sizeof(value)));
    }
    {
        char empty_value[] = "key=  ";
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            text_parse_key_value(empty_value, key, sizeof(key), value, sizeof(value)));
    }
    {
        char small_key[] = "long-key=value";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                              text_parse_key_value(small_key, key, 4, value, sizeof(value)));
    }
    {
        char small_value[] = "key=long-value";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                              text_parse_key_value(small_value, key, sizeof(key), value, 4));
    }
    {
        char valid[] = "key=value";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              text_parse_key_value(NULL, key, sizeof(key), value, sizeof(value)));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              text_parse_key_value(valid, NULL, sizeof(key), value, sizeof(value)));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              text_parse_key_value(valid, key, 0, value, sizeof(value)));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              text_parse_key_value(valid, key, sizeof(key), NULL, sizeof(value)));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              text_parse_key_value(valid, key, sizeof(key), value, 0));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_text_basics);
    RUN_TEST(test_copy_format);
    RUN_TEST(test_split_values);
    RUN_TEST(test_split_guards);
    RUN_TEST(test_parse_uint);
    RUN_TEST(test_document_reader);
    RUN_TEST(test_key_value);
    return UNITY_END();
}
