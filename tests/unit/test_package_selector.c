/*
 * Test focus: Exercises focused text, path and entry contracts shared by parsers, persistent
 * formats and command validation.
 */

#include "constants.h"
#include "package_selector.h"
#include "error.h"
#include "path.h"
#include "text.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
}

void tearDown(void) {
}

static FILE *open_bytes(const void *data, size_t size) {
    FILE *file = tmpfile();

    TEST_ASSERT_NOT_NULL(file);
    if (size > 0) {
        TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    }
    rewind(file);
    return file;
}

/* Test cases grouped by the public contract they exercise. */

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
    char input[] = "a.b";
    char first[8];
    char second[8];
    TextBuffer parts[2] = {
        {.data = first, .capacity = sizeof(first)},
        {.data = second, .capacity = sizeof(second)},
    };

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(NULL, '.', parts, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact("", '.', parts, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '\0', parts, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', NULL, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', parts, 0));

    parts[0].data = NULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', parts, 2));
    parts[0].data = first;
    parts[0].capacity = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_split_exact(input, '.', parts, 2));
}

static void test_line_formats(void) {
    static const char data[] = "  # comment\r\n"
                               "\t\r\n"
                               " key = value \r"
                               "last=value";
    FILE *file = open_bytes(data, sizeof(data) - 1);
    char line[32];
    size_t number = 0;
    int has_line = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_size_t(3, number);
    TEST_ASSERT_EQUAL_STRING("key = value", line);

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_size_t(4, number);
    TEST_ASSERT_EQUAL_STRING("last=value", line);

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_FALSE(has_line);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    number = 0;
    file = open_bytes("# trailing comment", 18);
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_FALSE(has_line);
    TEST_ASSERT_EQUAL_size_t(1, number);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    number = 0;
    file = open_bytes("value\r", 6);
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_TRUE(has_line);
    TEST_ASSERT_EQUAL_STRING("value", line);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_line_errors(void) {
    static const unsigned char control[] = {'a', 1, 'b', '\n'};
    static const unsigned char nul[] = {'a', 0, 'b', '\n'};
    static const char long_line[] = "123456789\n";
    char line[8];
    size_t number;
    int has_line;
    FILE *file;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_read_line(NULL, line, sizeof(line), &has_line, &number));
    file = open_bytes("a\n", 2);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_read_line(file, NULL, sizeof(line), &has_line, &number));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, text_read_line(file, line, 1, &has_line, &number));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_read_line(file, line, sizeof(line), NULL, &number));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_read_line(file, line, sizeof(line), &has_line, NULL));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    number = 0;
    file = open_bytes(control, sizeof(control));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_EQUAL_size_t(1, number);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    number = 0;
    file = open_bytes(nul, sizeof(nul));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    number = 0;
    file = open_bytes(long_line, sizeof(long_line) - 1);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          text_read_line(file, line, sizeof(line), &has_line, &number));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Key/value parsing preserves the selector grammar and destination bounds. */
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
    }
}

/* Symbolic and concrete values share one normalized selector representation. */
static void test_selector_values(void) {
    char tool[32];
    char release[32];
    char entry[64];

    TEST_ASSERT_FALSE(package_release_is_stable(NULL));
    TEST_ASSERT_FALSE(package_release_is_stable(""));
    TEST_ASSERT_TRUE(package_release_is_stable("stable"));
    TEST_ASSERT_FALSE(package_release_is_stable("22.1.5"));

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_parse_parts("clang@22.1.5", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_STRING("clang", tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", release);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_selector_format_parts(entry, sizeof(entry), "gcc", "16.1.0-rev1"));
    TEST_ASSERT_EQUAL_STRING("gcc@16.1.0-rev1", entry);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("@22.1.5", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang@", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang@1@2", tool, sizeof(tool), release, sizeof(release)));
    {
        char long_entry[MAX_SELECTOR_LEN + 2];
        memset(long_entry, 'x', sizeof(long_entry) - 1);
        long_entry[sizeof(long_entry) - 1] = '\0';
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_BUFFER_TOO_SMALL,
            package_selector_parse_parts(long_entry, tool, sizeof(tool), release, sizeof(release)));
    }
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang@22.1.5", tool, 3, release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_selector_format_parts(entry, 5, "gcc", "16"));
}

static void test_package_selectors(void) {
    PackageSelector selector;
    char text[MAX_SELECTOR_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_selector_init(&selector, "clang", "stable"));
    TEST_ASSERT_EQUAL_STRING("clang", selector.tool);
    TEST_ASSERT_EQUAL_STRING("stable", selector.release);
    TEST_ASSERT_TRUE(package_selector_is_symbolic(&selector));

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_selector_parse(&selector, "gcc@16.1.0-rev1"));
    TEST_ASSERT_EQUAL_STRING("gcc", selector.tool);
    TEST_ASSERT_EQUAL_STRING("16.1.0-rev1", selector.release);
    TEST_ASSERT_FALSE(package_selector_is_symbolic(&selector));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_selector_format(&selector, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("gcc@16.1.0-rev1", text);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_selector_init(NULL, "gcc", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL, package_selector_init(&selector, "../gcc", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, package_selector_init(&selector, "gcc", "../1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_selector_parse(NULL, "gcc@1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_selector_parse(&selector, "gcc"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_selector_format(NULL, text, sizeof(text)));
    TEST_ASSERT_FALSE(package_selector_is_symbolic(NULL));
}

static void test_selector_guards(void) {
    char tool[8];
    char release[8];
    char entry[16];

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts(NULL, tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("gcc@1", NULL, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_parse_parts("gcc@1", tool, 0, release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("gcc@1", tool, sizeof(tool), NULL, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_parse_parts("gcc@1", tool, sizeof(tool), release, 0));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(NULL, sizeof(entry), "gcc", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(entry, 0, "gcc", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(entry, sizeof(entry), "", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(entry, sizeof(entry), "gcc", NULL));
}

static void test_path_segments(void) {
    static const char *const reserved[] = {
        "CON", "prn.txt", "Aux", "nul", "COM1", "com9.log", "LPT1", "lpt9.tmp"};
    size_t i;

    TEST_ASSERT_TRUE(path_is_safe_segment("clang-22.1.5"));
    TEST_ASSERT_TRUE(path_is_safe_identifier("clang-format_22.1.5+rev1"));
    TEST_ASSERT_FALSE(path_is_safe_segment(NULL));
    TEST_ASSERT_FALSE(path_is_safe_segment(""));
    TEST_ASSERT_FALSE(path_is_safe_segment("."));
    TEST_ASSERT_FALSE(path_is_safe_segment(".."));
    TEST_ASSERT_FALSE(path_is_safe_segment("name."));
    TEST_ASSERT_FALSE(path_is_safe_segment("name "));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad/name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad\\name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad:name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad?name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad*name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad\"name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad<name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad>name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad|name"));
    {
        char control[] = {'a', 1, 'b', '\0'};
        char deleted[] = {'a', 127, 'b', '\0'};
        char utf8[] = {'a', (char)0xc3, (char)0xa9, '\0'};
        TEST_ASSERT_FALSE(path_is_safe_segment(control));
        TEST_ASSERT_FALSE(path_is_safe_segment(deleted));
        TEST_ASSERT_FALSE(path_is_safe_segment(utf8));
        TEST_ASSERT_FALSE(path_is_safe_segment("bad name"));
    }
    TEST_ASSERT_FALSE(path_is_safe_identifier("-starts"));
    TEST_ASSERT_FALSE(path_is_safe_identifier("bad name"));
    TEST_ASSERT_FALSE(path_is_safe_identifier("bad/name"));

    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        TEST_ASSERT_FALSE(path_is_safe_segment(reserved[i]));
    }
}

static void test_relative_paths(void) {
    char long_segment[257];

    memset(long_segment, 'a', sizeof(long_segment) - 1);
    long_segment[sizeof(long_segment) - 1] = '\0';

    TEST_ASSERT_TRUE(path_is_safe_relative("root/bin/clang"));
    TEST_ASSERT_TRUE(path_is_safe_relative("single"));
    TEST_ASSERT_FALSE(path_is_safe_relative(NULL));
    TEST_ASSERT_FALSE(path_is_safe_relative(""));
    TEST_ASSERT_FALSE(path_is_safe_relative("/absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("\\absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("C:/absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root:part"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root\\bin"));
    TEST_ASSERT_FALSE(path_is_safe_relative("../escape"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root//bin"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root/"));
    TEST_ASSERT_FALSE(path_is_safe_relative(long_segment));
}

static void test_path_building(void) {
    char path[64];

    TEST_ASSERT_EQUAL_STRING("name", path_last_segment("name"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root/bin/clang"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root\\bin\\clang"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root\\bin/clang"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root/bin\\clang"));
    TEST_ASSERT_EQUAL_STRING("", path_last_segment("root/bin/"));
    TEST_ASSERT_NULL(path_last_segment(NULL));

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), "/tmp/cup", "cache"));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup/cache", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join_safe_relative(path, sizeof(path), "/tmp/cup", "packages/gcc"));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup/packages/gcc", path);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(path, sizeof(path), "/tmp/cup", "../escape"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(NULL, sizeof(path), "/tmp/cup", "ok"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(path, 0, "/tmp/cup", "ok"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(path, sizeof(path), "", "ok"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(NULL, sizeof(path), "/tmp", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(path, 0, "/tmp", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(path, sizeof(path), "", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(path, sizeof(path), "/tmp", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, path_join(path, 8, "/tmp/cup", "cache"));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_text_basics);
    RUN_TEST(test_copy_format);
    RUN_TEST(test_split_values);
    RUN_TEST(test_split_guards);
    RUN_TEST(test_line_formats);
    RUN_TEST(test_line_errors);
    RUN_TEST(test_key_value);
    RUN_TEST(test_selector_values);
    RUN_TEST(test_package_selectors);
    RUN_TEST(test_selector_guards);
    RUN_TEST(test_path_segments);
    RUN_TEST(test_relative_paths);
    RUN_TEST(test_path_building);
    return UNITY_END();
}
