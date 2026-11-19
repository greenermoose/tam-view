#include "view.h"

#include <assert.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>

static void rows(const char *input, size_t length, int width,
                 const char *const *expected, size_t count)
{
    FILE *fp = tmpfile();
    assert(fp);
    assert(fwrite(input, 1, length, fp) == length);
    assert(fflush(fp) == 0);
    rewind(fp);
    struct source s;
    assert(source_open(&s, fileno(fp)) == 0);

    struct position row = {0};
    char out[1024];
    for (size_t i = 0; i < count; i++) {
        struct position next;
        assert(view_row(&s, row, width, out, &next) == VIEW_ROW);
        assert(position_compare(row, next) < 0);
        if (strcmp(out, expected[i]))
            fprintf(stderr, "row %zu: got [%s], expected [%s]\n",
                    i + 1, out, expected[i]);
        assert(!strcmp(out, expected[i]));
        row = next;
    }
    struct position next;
    assert(view_row(&s, row, width, out, &next) == VIEW_EOF);
    source_close(&s);
    fclose(fp);
}

#define ROWS(input, width, ...) do { \
    const char *const expected[] = {__VA_ARGS__}; \
    rows((input), sizeof(input) - 1, width, expected, \
         sizeof(expected) / sizeof(expected[0])); \
} while (0)

int main(void)
{
    assert(setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, ""));

    rows("", 0, 5, NULL, 0);
    ROWS("12345\r\n12345\nx\n\nend", 5, "12345", "12345", "x", "", "end");
    ROWS("\ta\tb", 5, "     ", "   a ", "     ", " b");
    ROWS("\t\t", 1, " ", " ", " ", " ", " ", " ", " ", " ",
                     " ", " ", " ", " ", " ", " ", " ", " ");
    ROWS("é中a\n", 2, "é", "中", "a");
    ROWS("x\033\x9b\0\177\xc2\x9b\xc3\r", 24,
         "x^[�^@^??�^M");
    ROWS("a\xcc\x81" "bc\n", 2, "a\xcc\x81" "b", "c");
    ROWS("a\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81"
         "\xcc\x81\xcc\x81\xcc\x81\xcc\x81", 1,
         "a\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81"
         "\xcc\x81\xcc\x81\xcc\x81", "?");

    char label[32];
    view_label(label, sizeof(label), 12, "é\033\nxyz");
    assert(!strcmp(label, "é^[^Jxyz"));
    puts("layout: ok");
    return 0;
}
