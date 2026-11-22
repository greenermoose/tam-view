#include "render_internal.h"
#include "view.h"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *render_str_styles(enum view_format fmt, const char *input,
                               int width, bool ascii,
                               struct view_styles *styles_out)
{
    struct rbuf b;
    rbuf_init(&b);
    size_t len = strlen(input);
    if (fmt == VIEW_FORMAT_MARKDOWN)
        render_markdown(&b, input, len, width, ascii);
    else if (fmt == VIEW_FORMAT_HTML)
        render_html(&b, input, len, width, ascii);
    else
        render_mermaid(&b, input, len, width, ascii);
    assert(!b.oom);
    // Rendered text must never contain raw ESC bytes.
    if (b.data)
        assert(memchr(b.data, '\033', b.len) == NULL);
    // Verify spans are sorted and non-overlapping.
    for (size_t i = 0; i < b.styles.count; i++) {
        assert(b.styles.spans[i].start < b.styles.spans[i].end);
        assert((size_t)b.styles.spans[i].end <= b.len);
        if (i > 0)
            assert(b.styles.spans[i - 1].end <= b.styles.spans[i].start);
    }
    if (styles_out) {
        *styles_out = b.styles;
        b.styles = (struct view_styles){0};
    } else {
        view_styles_free(&b.styles);
    }
    return b.data ? b.data : strdup("");
}

static char *render_str(enum view_format fmt, const char *input, int width, bool ascii)
{
    return render_str_styles(fmt, input, width, ascii, NULL);
}

static bool has_style(const struct view_styles *st, unsigned char style)
{
    for (size_t i = 0; i < st->count; i++) {
        if (st->spans[i].style == style)
            return true;
    }
    return false;
}

static void test_detect(void)
{
    assert(view_detect_format("doc.md", NULL, false) == VIEW_FORMAT_MARKDOWN);
    assert(view_detect_format("page.HTML", NULL, false) == VIEW_FORMAT_HTML);
    assert(view_detect_format("flow.mmd", NULL, false) == VIEW_FORMAT_MERMAID);
    assert(view_detect_format("notes.txt", NULL, false) == VIEW_FORMAT_RAW);
    assert(view_detect_format("notes.txt", NULL, true) == VIEW_FORMAT_MARKDOWN);

    FILE *fp = tmpfile();
    assert(fp);
    const char *html = "<!DOCTYPE html><html><body><p>Hi</p></body></html>";
    assert(fwrite(html, 1, strlen(html), fp) == strlen(html));
    assert(fflush(fp) == 0);
    rewind(fp);
    struct source s;
    assert(source_open(&s, fileno(fp)) == 0);
    assert(view_detect_format("-", &s, false) == VIEW_FORMAT_HTML);
    source_close(&s);
    fclose(fp);
}

static void test_markdown(void)
{
    const char *md =
        "# Title\n\n"
        "A **bold** and `code` [link](https://example.com) paragraph that wraps nicely.\n\n"
        "- item one\n"
        "- item two\n\n"
        "> quoted line\n\n"
        "| Col1 | Col2 |\n"
        "| --- | --- |\n"
        "| a | b |\n\n"
        "```mermaid\n"
        "flowchart TD\n"
        "  A[Start] --> B[End]\n"
        "```\n";

    struct view_styles st = {0};
    char *uni = render_str_styles(VIEW_FORMAT_MARKDOWN, md, 60, false, &st);
    assert(strstr(uni, "Title\n"));
    assert(strstr(uni, "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"));
    assert(strstr(uni, "bold and code link (https://example.com)"));
    assert(strstr(uni, "\xe2\x80\xa2 item one"));
    assert(strstr(uni, "\xe2\x94\x82 quoted line"));
    assert(strstr(uni, "\xe2\x94\x8c"));
    assert(strstr(uni, "Start"));
    assert(strstr(uni, "End"));
    assert(has_style(&st, VIEW_STYLE_H1));
    assert(has_style(&st, VIEW_STYLE_BOLD));
    assert(has_style(&st, VIEW_STYLE_CODE));
    assert(has_style(&st, VIEW_STYLE_LINK));
    assert(has_style(&st, VIEW_STYLE_LINK_URL));
    assert(has_style(&st, VIEW_STYLE_QUOTE));
    assert(has_style(&st, VIEW_STYLE_RULE));
    assert(has_style(&st, VIEW_STYLE_TABLE_HDR));
    assert(has_style(&st, VIEW_STYLE_NODE));
    view_styles_free(&st);
    free(uni);

    char *asc = render_str(VIEW_FORMAT_MARKDOWN, md, 60, true);
    assert(strstr(asc, "====="));
    assert(strstr(asc, "* item one"));
    assert(strstr(asc, "| quoted line"));
    assert(strstr(asc, "+------"));
    free(asc);
}

static void test_html(void)
{
    const char *html =
        "<html><head><title>Ignored</title><style>body{}</style></head>"
        "<body><h1>Hello &amp; Welcome</h1>"
        "<script>alert(1)</script>"
        "<p>Visit <a href=\"https://tamlinux.org\">Tamlinux</a> &mdash; &ldquo;ok&rdquo; &lsquo;y&rsquo;.</p>"
        "<ul><li>One</li><li>Two</li></ul>"
        "<table><tr><th>K</th><th>V</th></tr><tr><td>x</td><td>1</td></tr></table>"
        "</body></html>";

    struct view_styles st = {0};
    char *out = render_str_styles(VIEW_FORMAT_HTML, html, 72, true, &st);
    assert(!strstr(out, "Ignored"));
    assert(!strstr(out, "alert(1)"));
    assert(strstr(out, "Hello & Welcome"));
    assert(strstr(out, "Tamlinux (https://tamlinux.org) -- \"ok\" 'y'."));
    assert(strstr(out, "* One"));
    assert(strstr(out, "* Two"));
    assert(strstr(out, "+---+---+"));
    assert(has_style(&st, VIEW_STYLE_H1));
    assert(has_style(&st, VIEW_STYLE_LINK));
    assert(has_style(&st, VIEW_STYLE_LINK_URL));
    assert(has_style(&st, VIEW_STYLE_TABLE_HDR));
    assert(has_style(&st, VIEW_STYLE_RULE));
    view_styles_free(&st);
    free(out);

    char *out_u = render_str(VIEW_FORMAT_HTML, "<p>&ldquo;hi&rdquo;</p>", 72, false);
    assert(strstr(out_u, "\xe2\x80\x9c" "hi" "\xe2\x80\x9d"));
    free(out_u);
}

static void test_mermaid(void)
{
    const char *td =
        "flowchart TD\n"
        "  A[Input] -->|ok| B(Process)\n"
        "  B --> C{Valid?}\n"
        "  C -->|yes| D[Done]\n"
        "  C -->|no| A\n";

    struct view_styles st = {0};
    char *out_td = render_str_styles(VIEW_FORMAT_MERMAID, td, 72, true, &st);
    assert(strstr(out_td, "+-------+"));
    assert(strstr(out_td, "| Input |"));
    assert(strstr(out_td, "< Valid? >"));
    assert(strstr(out_td, "(ok)"));
    assert(strstr(out_td, "(yes) > [Done]"));
    assert(strstr(out_td, "(no) > [Input]"));
    // Cycle back-edge A <- C must not push Input below Valid?.
    assert(strstr(out_td, "| Input |") < strstr(out_td, "< Valid? >"));
    assert(strstr(out_td, "< Valid? >") < strstr(out_td, "| Done |"));
    assert(has_style(&st, VIEW_STYLE_RULE));
    assert(has_style(&st, VIEW_STYLE_NODE));
    assert(has_style(&st, VIEW_STYLE_EDGE));
    view_styles_free(&st);
    free(out_td);

    const char *lr =
        "graph LR\n"
        "  S([Begin]) --> M[Middle] --> E[Finish]\n";
    char *out_lr = render_str(VIEW_FORMAT_MERMAID, lr, 80, false);
    assert(strstr(out_lr, "\xe2\x95\xad"));
    assert(strstr(out_lr, "\xe2\x96\xb6"));
    free(out_lr);

    // Narrow width triggers TD fallback for LR without overflowing.
    char *out_narrow = render_str(VIEW_FORMAT_MERMAID, lr, 24, true);
    assert(strstr(out_narrow, "Begin"));
    assert(strstr(out_narrow, "Finish"));
    free(out_narrow);

    // Unsupported diagram emits diagnostic + raw fallback.
    char *bad = render_str(VIEW_FORMAT_MERMAID, "sequenceDiagram\nA->>B: Hi\n", 80, true);
    assert(strstr(bad, "unsupported or invalid mermaid diagram"));
    assert(strstr(bad, "A->>B: Hi"));
    free(bad);
}

int main(void)
{
    assert(setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, ""));
    test_detect();
    test_markdown();
    test_html();
    test_mermaid();
    puts("render: ok");
    return 0;
}
