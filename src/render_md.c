#include "render_internal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void inline_md(struct rbuf *out, const char *s, size_t len)
{
    for (size_t i = 0; i < len;) {
        if (s[i] == '\\' && i + 1 < len && ispunct((unsigned char)s[i + 1])) {
            rbuf_putc(out, s[i + 1]);
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            size_t ticks = 0;
            while (i + ticks < len && s[i + ticks] == '`')
                ticks++;
            size_t j = i + ticks;
            bool found = false;
            while (j < len) {
                if (s[j] == '`') {
                    size_t close_t = 0;
                    while (j + close_t < len && s[j + close_t] == '`')
                        close_t++;
                    if (close_t == ticks) {
                        size_t s0 = out->len;
                        rbuf_putn(out, s + i + ticks, j - (i + ticks));
                        rbuf_push_span(out, s0, out->len, VIEW_STYLE_CODE);
                        i = j + close_t;
                        found = true;
                        break;
                    }
                    j += close_t;
                } else {
                    j++;
                }
            }
            if (!found) {
                rbuf_putn(out, s + i, ticks);
                i += ticks;
            }
            continue;
        }
        if ((s[i] == '!' && i + 1 < len && s[i + 1] == '[') || s[i] == '[') {
            bool is_img = (s[i] == '!');
            size_t start = is_img ? i + 2 : i + 1;
            size_t close_b = start;
            int depth = 1;
            while (close_b < len && depth > 0) {
                if (s[close_b] == '\\' && close_b + 1 < len)
                    close_b += 2;
                else if (s[close_b] == '[') {
                    depth++;
                    close_b++;
                } else if (s[close_b] == ']') {
                    depth--;
                    if (depth == 0)
                        break;
                    close_b++;
                } else {
                    close_b++;
                }
            }
            if (close_b < len && s[close_b] == ']' &&
                close_b + 1 < len && s[close_b + 1] == '(') {
                size_t url_start = close_b + 2;
                size_t url_end = url_start;
                int pdepth = 1;
                while (url_end < len && pdepth > 0) {
                    if (s[url_end] == '(')
                        pdepth++;
                    else if (s[url_end] == ')') {
                        pdepth--;
                        if (pdepth == 0)
                            break;
                    }
                    url_end++;
                }
                if (url_end < len && s[url_end] == ')') {
                    size_t lbl_len = close_b - start;
                    size_t url_len = url_end - url_start;
                    while (url_len > 0 && isspace((unsigned char)s[url_start])) {
                        url_start++;
                        url_len--;
                    }
                    while (url_len > 0 && isspace((unsigned char)s[url_start + url_len - 1]))
                        url_len--;
                    /* Strip optional angle brackets <url> and optional title after whitespace. */
                    if (url_len >= 2 && s[url_start] == '<') {
                        const char *gt = memchr(s + url_start + 1, '>', url_len - 1);
                        if (gt) {
                            url_start++;
                            url_len = (size_t)(gt - (s + url_start));
                        }
                    } else {
                        for (size_t u = 0; u < url_len; u++) {
                            if (isspace((unsigned char)s[url_start + u])) {
                                url_len = u;
                                break;
                            }
                        }
                    }
                    if (is_img) {
                        size_t s0 = out->len;
                        rbuf_puts(out, "[image: ");
                        inline_md(out, s + start, lbl_len);
                        rbuf_putc(out, ']');
                        rbuf_fill_style(out, s0, out->len, VIEW_STYLE_LINK_URL);
                    } else {
                        size_t s0 = out->len;
                        inline_md(out, s + start, lbl_len);
                        size_t s1 = out->len;
                        rbuf_fill_style(out, s0, s1, VIEW_STYLE_LINK);
                        if (url_len > 0 && s1 > s0)
                            rbuf_add_link(out, s0, s1, s + url_start, url_len);
                        if (url_len > 0 &&
                            !(url_len == lbl_len &&
                              memcmp(s + start, s + url_start, url_len) == 0)) {
                            size_t u0 = out->len;
                            rbuf_puts(out, " (");
                            rbuf_putn(out, s + url_start, url_len);
                            rbuf_putc(out, ')');
                            rbuf_push_span(out, u0, out->len, VIEW_STYLE_LINK_URL);
                            if (s1 == s0)
                                rbuf_add_link(out, u0 + 2, out->len - 1,
                                              s + url_start, url_len);
                        }
                    }
                    i = url_end + 1;
                    continue;
                }
            }
        }
        if (s[i] == '<') {
            size_t gt = i + 1;
            while (gt < len && s[gt] != '>' && !isspace((unsigned char)s[gt]))
                gt++;
            if (gt < len && s[gt] == '>') {
                size_t ulen = gt - (i + 1);
                const char *u = s + i + 1;
                if ((ulen > 7 && strncasecmp(u, "http://", 7) == 0) ||
                    (ulen > 8 && strncasecmp(u, "https://", 8) == 0) ||
                    (ulen > 7 && strncasecmp(u, "mailto:", 7) == 0)) {
                    size_t s0 = out->len;
                    rbuf_putn(out, u, ulen);
                    rbuf_push_span(out, s0, out->len, VIEW_STYLE_LINK);
                    rbuf_add_link(out, s0, out->len, u, ulen);
                    i = gt + 1;
                    continue;
                }
            }
        }
        if ((s[i] == '*' || s[i] == '_' || s[i] == '~') &&
            i + 1 < len && s[i + 1] == s[i]) {
            char mark = s[i];
            size_t j = i + 2;
            bool closed = false;
            while (j + 1 < len) {
                if (s[j] == mark && s[j + 1] == mark) {
                    closed = true;
                    break;
                }
                j++;
            }
            if (closed) {
                size_t s0 = out->len;
                inline_md(out, s + i + 2, j - (i + 2));
                rbuf_fill_style(out, s0, out->len,
                                mark == '~' ? VIEW_STYLE_STRIKE : VIEW_STYLE_BOLD);
                i = j + 2;
                continue;
            }
        }
        if ((s[i] == '*' || s[i] == '_') && i + 1 < len &&
            !isspace((unsigned char)s[i + 1])) {
            char mark = s[i];
            size_t j = i + 1;
            bool closed = false;
            while (j < len) {
                if (s[j] == mark && !isspace((unsigned char)s[j - 1])) {
                    closed = true;
                    break;
                }
                j++;
            }
            if (closed && (mark == '*' || i == 0 || !isalnum((unsigned char)s[i - 1]))) {
                size_t s0 = out->len;
                inline_md(out, s + i + 1, j - (i + 1));
                rbuf_fill_style(out, s0, out->len, VIEW_STYLE_ITALIC);
                i = j + 1;
                continue;
            }
        }
        rbuf_putc(out, s[i++]);
    }
}

static size_t next_line(const char *src, size_t len, size_t pos,
                        const char **line_out, size_t *line_len)
{
    *line_out = src + pos;
    size_t i = pos;
    while (i < len && src[i] != '\n')
        i++;
    size_t n = i - pos;
    if (n > 0 && src[pos + n - 1] == '\r')
        n--;
    *line_len = n;
    return i < len ? i + 1 : len;
}

static bool is_hrule(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && line[i] == ' ')
        i++;
    if (i >= len)
        return false;
    char ch = line[i];
    if (ch != '-' && ch != '*' && ch != '_')
        return false;
    int count = 0;
    for (; i < len; i++) {
        if (line[i] == ch)
            count++;
        else if (line[i] != ' ' && line[i] != '\t')
            return false;
    }
    return count >= 3;
}

static bool is_table_sep(const char *line, size_t len)
{
    bool has_dash = false;
    bool has_pipe = false;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (c == '-')
            has_dash = true;
        else if (c == '|')
            has_pipe = true;
        else if (c != ':' && c != ' ' && c != '\t')
            return false;
    }
    return has_dash && has_pipe;
}

static bool parse_table_row(const char *line, size_t len, struct rtable *t)
{
    size_t i = 0;
    while (i < len && isspace((unsigned char)line[i]))
        i++;
    if (i >= len || line[i] != '|')
        return false;

    size_t end = len;
    while (end > i && isspace((unsigned char)line[end - 1]))
        end--;
    if (end > i && line[end - 1] == '|')
        end--;
    i++;

    size_t cap = 4;
    size_t ncols = 0;
    struct rbuf *cells = calloc(cap, sizeof(*cells));
    if (!cells)
        return false;

    struct rbuf cell;
    while (i <= end) {
        size_t start = i;
        while (i < end && !(line[i] == '|' && (i == 0 || line[i - 1] != '\\')))
            i++;
        size_t cs = start;
        size_t ce = i;
        while (cs < ce && isspace((unsigned char)line[cs]))
            cs++;
        while (ce > cs && isspace((unsigned char)line[ce - 1]))
            ce--;
        rbuf_init(&cell);
        inline_md(&cell, line + cs, ce - cs);
        if (ncols == cap) {
            cap *= 2;
            struct rbuf *grown = realloc(cells, cap * sizeof(*cells));
            if (!grown) {
                rbuf_free(&cell);
                for (size_t k = 0; k < ncols; k++)
                    rbuf_free(&cells[k]);
                free(cells);
                return false;
            }
            cells = grown;
        }
        cells[ncols++] = cell;
        if (i >= end)
            break;
        i++;
    }

    struct rtable_row *rows = realloc(t->rows, (t->nrows + 1) * sizeof(*rows));
    if (!rows) {
        for (size_t k = 0; k < ncols; k++)
            rbuf_free(&cells[k]);
        free(cells);
        return false;
    }
    t->rows = rows;
    t->rows[t->nrows++] = (struct rtable_row){.cells = cells, .ncols = ncols};
    if (ncols > t->max_cols)
        t->max_cols = ncols;
    return true;
}

static bool parse_list_item(const char *line, size_t len, int *indent_out,
                            bool *ordered_out, char *num_buf, size_t num_cap,
                            size_t *content_offset)
{
    size_t i = 0;
    int indent = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        indent += (line[i] == '\t') ? 4 : 1;
        i++;
    }
    if (i < len && (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
        i + 1 < len && line[i + 1] == ' ') {
        *indent_out = indent;
        *ordered_out = false;
        *content_offset = i + 2;
        return true;
    }
    size_t d = i;
    while (d < len && isdigit((unsigned char)line[d]))
        d++;
    if (d > i && d - i < num_cap - 2 && d + 1 < len &&
        (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ') {
        memcpy(num_buf, line + i, d - i + 1);
        num_buf[d - i + 1] = '\0';
        *indent_out = indent;
        *ordered_out = true;
        *content_offset = d + 2;
        return true;
    }
    return false;
}

static void emit_heading(struct rbuf *b, int level, const char *text,
                         size_t len, int width, bool ascii)
{
    const struct box_chars *box = render_box_chars(ascii);
    unsigned char hstyle = level == 1 ? VIEW_STYLE_H1
                         : level == 2 ? VIEW_STYLE_H2
                                      : VIEW_STYLE_H3;
    rbuf_blank_line(b);
    size_t anchor_byte = b->len;
    struct rbuf tmp;
    rbuf_init(&tmp);
    inline_md(&tmp, text, len);
    rbuf_fill_style(&tmp, 0, tmp.len, hstyle);
    const char *s = tmp.data ? tmp.data : "";
    rbuf_add_heading_anchor(b, s, tmp.len, anchor_byte);

    if (level <= 2) {
        rbuf_wrap_styled(b, &tmp, width, "", "", VIEW_STYLE_NONE);
        int w = render_str_width(s, strlen(s));
        if (w > width)
            w = width;
        if (w < 3)
            w = 3;
        size_t rs = b->len;
        rbuf_repeat(b, level == 1 ? box->d_h : box->h, w);
        rbuf_push_span(b, rs, b->len, VIEW_STYLE_RULE);
        rbuf_putc(b, '\n');
    } else {
        char pfx[16];
        int n = level > 6 ? 6 : level;
        memset(pfx, '#', (size_t)n);
        pfx[n] = ' ';
        pfx[n + 1] = '\0';
        rbuf_wrap_styled(b, &tmp, width, pfx, "  ", hstyle);
    }
    rbuf_free(&tmp);
}

void render_markdown(struct rbuf *b, const char *src, size_t len,
                     int width, bool ascii)
{
    const struct box_chars *box = render_box_chars(ascii);
    if (width < 20)
        width = 20;

    struct rbuf para;
    rbuf_init(&para);

    size_t pos = 0;
    while (pos < len) {
        const char *line = NULL;
        size_t llen = 0;
        size_t next = next_line(src, len, pos, &line, &llen);

        // Blank line flushes any pending paragraph.
        size_t ws = 0;
        while (ws < llen && isspace((unsigned char)line[ws]))
            ws++;
        if (ws == llen) {
            if (para.len > 0) {
                rbuf_blank_line(b);
                rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                rbuf_clear(&para);
            }
            pos = next;
            continue;
        }

        // Fenced code block (``` or ~~~).
        if (llen >= 3 && ((line[0] == '`' && line[1] == '`' && line[2] == '`') ||
                          (line[0] == '~' && line[1] == '~' && line[2] == '~'))) {
            if (para.len > 0) {
                rbuf_blank_line(b);
                rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                rbuf_clear(&para);
            }
            char fence = line[0];
            size_t flen = 0;
            while (flen < llen && line[flen] == fence)
                flen++;
            size_t ls = flen;
            while (ls < llen && isspace((unsigned char)line[ls]))
                ls++;
            size_t le = llen;
            while (le > ls && isspace((unsigned char)line[le - 1]))
                le--;
            bool is_mermaid = (le - ls == 7 &&
                               strncasecmp(line + ls, "mermaid", 7) == 0);

            struct rbuf code;
            rbuf_init(&code);
            pos = next;
            while (pos < len) {
                const char *cl = NULL;
                size_t clen = 0;
                size_t cnext = next_line(src, len, pos, &cl, &clen);
                size_t cf = 0;
                while (cf < clen && cl[cf] == fence)
                    cf++;
                bool only_ws = true;
                for (size_t k = cf; k < clen; k++) {
                    if (!isspace((unsigned char)cl[k])) {
                        only_ws = false;
                        break;
                    }
                }
                pos = cnext;
                if (cf >= flen && only_ws)
                    break;
                rbuf_putn(&code, cl, clen);
                rbuf_putc(&code, '\n');
            }

            rbuf_blank_line(b);
            if (is_mermaid) {
                render_mermaid(b, code.data ? code.data : "", code.len,
                               width, ascii);
            } else if (code.data) {
                size_t cp = 0;
                while (cp < code.len) {
                    const char *cl = NULL;
                    size_t clen = 0;
                    cp = next_line(code.data, code.len, cp, &cl, &clen);
                    size_t cs = b->len;
                    rbuf_puts(b, "    ");
                    rbuf_putn(b, cl, clen);
                    rbuf_push_span(b, cs, b->len, VIEW_STYLE_CODE);
                    rbuf_putc(b, '\n');
                }
            }
            rbuf_free(&code);
            continue;
        }

        // ATX heading.
        if (line[0] == '#') {
            size_t h = 0;
            while (h < llen && h < 6 && line[h] == '#')
                h++;
            if (h < llen && (line[h] == ' ' || line[h] == '\t')) {
                if (para.len > 0) {
                    rbuf_blank_line(b);
                    rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                    rbuf_clear(&para);
                }
                size_t ts = h;
                while (ts < llen && isspace((unsigned char)line[ts]))
                    ts++;
                size_t te = llen;
                while (te > ts && isspace((unsigned char)line[te - 1]))
                    te--;
                while (te > ts && line[te - 1] == '#')
                    te--;
                while (te > ts && isspace((unsigned char)line[te - 1]))
                    te--;
                emit_heading(b, (int)h, line + ts, te - ts, width, ascii);
                pos = next;
                continue;
            }
        }

        // Horizontal rule.
        if (is_hrule(line, llen)) {
            if (para.len > 0) {
                rbuf_blank_line(b);
                rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                rbuf_clear(&para);
            }
            rbuf_blank_line(b);
            int rw = width > 72 ? 72 : width;
            size_t rs = b->len;
            rbuf_repeat(b, box->h, rw);
            rbuf_push_span(b, rs, b->len, VIEW_STYLE_RULE);
            rbuf_putc(b, '\n');
            pos = next;
            continue;
        }

        // GFM Pipe Table.
        if (line[ws] == '|' && next < len) {
            const char *l2 = NULL;
            size_t l2_len = 0;
            size_t n2 = next_line(src, len, next, &l2, &l2_len);
            if (is_table_sep(l2, l2_len)) {
                if (para.len > 0) {
                    rbuf_blank_line(b);
                    rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                    rbuf_clear(&para);
                }
                struct rtable t = {.has_header = true};
                parse_table_row(line, llen, &t);
                pos = n2;
                while (pos < len) {
                    const char *rl = NULL;
                    size_t rlen = 0;
                    size_t rnext = next_line(src, len, pos, &rl, &rlen);
                    size_t rws = 0;
                    while (rws < rlen && isspace((unsigned char)rl[rws]))
                        rws++;
                    if (rws >= rlen || rl[rws] != '|')
                        break;
                    if (!parse_table_row(rl, rlen, &t))
                        break;
                    pos = rnext;
                }
                rbuf_blank_line(b);
                render_table(b, &t, width, ascii);
                rtable_free(&t);
                continue;
            }
        }

        // Blockquote.
        if (line[ws] == '>') {
            if (para.len > 0) {
                rbuf_blank_line(b);
                rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                rbuf_clear(&para);
            }
            rbuf_blank_line(b);
            struct rbuf bq;
            rbuf_init(&bq);
            while (pos < len) {
                const char *ql = NULL;
                size_t qlen = 0;
                size_t qnext = next_line(src, len, pos, &ql, &qlen);
                size_t qws = 0;
                while (qws < qlen && isspace((unsigned char)ql[qws]))
                    qws++;
                if (qws >= qlen || ql[qws] != '>')
                    break;
                size_t qs = qws + 1;
                if (qs < qlen && ql[qs] == ' ')
                    qs++;
                if (bq.len > 0)
                    rbuf_putc(&bq, ' ');
                inline_md(&bq, ql + qs, qlen - qs);
                pos = qnext;
            }
            char pfx[8];
            snprintf(pfx, sizeof(pfx), "%s ", box->v);
            rbuf_fill_style(&bq, 0, bq.len, VIEW_STYLE_QUOTE);
            rbuf_wrap_styled(b, &bq, width, pfx, pfx, VIEW_STYLE_QUOTE);
            rbuf_free(&bq);
            continue;
        }

        // List item.
        int indent = 0;
        bool ordered = false;
        char num_buf[16] = {0};
        size_t off = 0;
        if (parse_list_item(line, llen, &indent, &ordered, num_buf,
                            sizeof(num_buf), &off)) {
            if (para.len > 0) {
                rbuf_blank_line(b);
                rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
                rbuf_clear(&para);
            } else if (b->len > 0) {
                rbuf_newline(b);
            }
            struct rbuf item;
            rbuf_init(&item);
            inline_md(&item, line + off, llen - off);
            int level = indent / 2;
            if (level > 4)
                level = 4;
            char fpfx[64], cpfx[64];
            int p = 0;
            for (int k = 0; k < level * 2; k++)
                fpfx[p++] = ' ';
            if (ordered) {
                int nw = snprintf(fpfx + p, sizeof(fpfx) - (size_t)p, "%s ", num_buf);
                int cw = p + (nw > 0 ? nw : 2);
                memset(cpfx, ' ', (size_t)cw);
                cpfx[cw] = '\0';
            } else {
                int bw = snprintf(fpfx + p, sizeof(fpfx) - (size_t)p, "%s ", box->bullet);
                (void)bw;
                int cw = p + 2;
                memset(cpfx, ' ', (size_t)cw);
                cpfx[cw] = '\0';
            }
            rbuf_wrap_styled(b, &item, width, fpfx, cpfx, VIEW_STYLE_BOLD);
            rbuf_free(&item);
            pos = next;
            continue;
        }

        // Setext heading (=== or --- on next line).
        if (para.len == 0 && next < len) {
            const char *l2 = NULL;
            size_t l2_len = 0;
            size_t n2 = next_line(src, len, next, &l2, &l2_len);
            if (l2_len >= 3) {
                char mark = l2[0];
                if (mark == '=' || mark == '-') {
                    bool all_mark = true;
                    for (size_t k = 0; k < l2_len; k++) {
                        if (l2[k] != mark && l2[k] != ' ') {
                            all_mark = false;
                            break;
                        }
                    }
                    if (all_mark) {
                        emit_heading(b, mark == '=' ? 1 : 2, line, llen,
                                     width, ascii);
                        pos = n2;
                        continue;
                    }
                }
            }
        }

        // Accumulate paragraph text.
        if (para.len > 0)
            rbuf_putc(&para, ' ');
        inline_md(&para, line + ws, llen - ws);
        pos = next;
    }

    if (para.len > 0) {
        rbuf_blank_line(b);
        rbuf_wrap_styled(b, &para, width, "", "", VIEW_STYLE_NONE);
    }
    rbuf_free(&para);
}
