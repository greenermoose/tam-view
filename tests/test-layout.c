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

static void rows_h(const char *input, size_t length, int width, int skip,
                   bool chop, const char *const *expected, size_t count)
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
        assert(view_row_h(&s, row, width, skip, chop, out, &next) == VIEW_ROW);
        assert(position_compare(row, next) < 0);
        if (strcmp(out, expected[i]))
            fprintf(stderr, "row_h %zu: got [%s], expected [%s]\n",
                    i + 1, out, expected[i]);
        assert(!strcmp(out, expected[i]));
        row = next;
    }
    struct position next;
    assert(view_row_h(&s, row, width, skip, chop, out, &next) == VIEW_EOF);
    source_close(&s);
    fclose(fp);
}

#define ROWS(input, width, ...) do { \
    const char *const expected[] = {__VA_ARGS__}; \
    rows((input), sizeof(input) - 1, width, expected, \
         sizeof(expected) / sizeof(expected[0])); \
} while (0)

#define ROWS_H(input, width, skip, chop, ...) do { \
    const char *const expected[] = {__VA_ARGS__}; \
    rows_h((input), sizeof(input) - 1, width, skip, chop, expected, \
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

    ROWS_H("abcdefgh\n1234\n", 4, 0, true, "abcd", "1234");
    ROWS_H("abcdefgh\n12\n", 4, 3, false, "defg", "");
    ROWS_H("\txyz\n", 5, 6, false, "  xyz");
    ROWS_H("a中bc\n", 4, 2, false, " bc");
    ROWS_H("ab中c\n", 3, 0, true, "ab");

    // Styled rows: document spans, search override, horizontal scroll, linux_vt, and ESC sanitization.
    {
        FILE *fp = tmpfile();
        assert(fp);
        const char *txt = "hello \033world\n";
        assert(fwrite(txt, 1, strlen(txt), fp) == strlen(txt));
        assert(fflush(fp) == 0);
        rewind(fp);
        struct source s;
        assert(source_open(&s, fileno(fp)) == 0);

        struct view_styles doc = {0};
        assert(view_styles_push(&doc, 0, 5, VIEW_STYLE_BOLD));
        assert(view_styles_push(&doc, 6, 12, VIEW_STYLE_ITALIC));
        struct view_styles srch = {0};
        assert(view_styles_push(&srch, 2, 4, VIEW_STYLE_SEARCH_CUR));

        struct view_row_style rs = {
            .doc_spans = doc.spans,
            .doc_count = doc.count,
            .search_spans = srch.spans,
            .search_count = srch.count,
            .linux_vt = false
        };
        char out[1024];
        struct position next;
        assert(view_row_styled(&s, (struct position){0}, 40, 0, false, &rs,
                               out, &next) == VIEW_ROW);
        // "he" bold, "ll" search_cur, "o" bold, " " none, "^[world" italic (ESC sanitized!).
        assert(!strcmp(out, "\033[1mhe\033[0m\033[1;30;43mll\033[0m\033[1mo\033[0m \033[3m^[world\033[0m"));

        // Linux VT maps italic to cyan (\033[36m) instead of \033[3m.
        rs.linux_vt = true;
        assert(view_row_styled(&s, (struct position){0}, 40, 0, false, &rs,
                               out, &next) == VIEW_ROW);
        assert(strstr(out, "\033[36m^[world\033[0m") != NULL);

        // Horizontal scroll preserves styles on visible slice.
        rs.linux_vt = false;
        assert(view_row_styled(&s, (struct position){0}, 6, 2, false, &rs,
                               out, &next) == VIEW_ROW);
        assert(!strcmp(out, "\033[1;30;43mll\033[0m\033[1mo\033[0m \033[3m^[\033[0m"));

        view_styles_free(&doc);
        view_styles_free(&srch);
        source_close(&s);
        fclose(fp);
    }

    puts("layout: ok");
    return 0;
}
