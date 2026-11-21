#include "render_internal.h"

#include <tam_uni.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void decode_entity(struct rbuf *b, const char *s, size_t len,
                          size_t *consumed, bool ascii)
{
    size_t i = 1;
    while (i < len && i < 16 && s[i] != ';' && !isspace((unsigned char)s[i]))
        i++;
    if (i >= len || s[i] != ';') {
        rbuf_putc(b, '&');
        *consumed = 1;
        return;
    }
    *consumed = i + 1;
    const char *ent = s + 1;
    size_t elen = i - 1;

    if (elen > 0 && ent[0] == '#') {
        uint32_t cp = 0;
        if (elen > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
            for (size_t k = 2; k < elen; k++) {
                char c = ent[k];
                if (!isxdigit((unsigned char)c)) {
                    cp = 0;
                    break;
                }
                cp = cp * 16 + (uint32_t)(isdigit((unsigned char)c)
                     ? c - '0' : tolower((unsigned char)c) - 'a' + 10);
            }
        } else {
            for (size_t k = 1; k < elen; k++) {
                if (!isdigit((unsigned char)ent[k])) {
                    cp = 0;
                    break;
                }
                cp = cp * 10 + (uint32_t)(ent[k] - '0');
            }
        }
        if (cp > 0 && cp <= 0x10ffff) {
            if (ascii && cp >= 0x80) {
                rbuf_putc(b, cp == 0xa0 ? ' ' : '?');
            } else {
                char utf8[4];
                size_t n = tam_utf8_encode(cp, utf8);
                rbuf_putn(b, utf8, n);
            }
            return;
        }
    }

    struct {
        const char *name;
        const char *utf8;
        const char *ascii_alt;
    } map[] = {
        {"amp", "&", "&"},
        {"lt", "<", "<"},
        {"gt", ">", ">"},
        {"quot", "\"", "\""},
        {"apos", "'", "'"},
        {"nbsp", " ", " "},
        {"mdash", "\xe2\x80\x94", "--"},
        {"ndash", "\xe2\x80\x93", "-"},
        {"hellip", "\xe2\x80\xa6", "..."},
        {"bull", "\xe2\x80\xa2", "*"},
        {"copy", "\xc2\xa9", "(c)"}
    };
    for (size_t m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
        if (strlen(map[m].name) == elen &&
            memcmp(ent, map[m].name, elen) == 0) {
            rbuf_puts(b, ascii ? map[m].ascii_alt : map[m].utf8);
            return;
        }
    }
    rbuf_putn(b, s, *consumed);
}

static void extract_attr(const char *tag, size_t tlen, const char *attr,
                         struct rbuf *out)
{
    size_t alen = strlen(attr);
    for (size_t i = 0; i + alen + 1 < tlen; i++) {
        if ((i == 0 || isspace((unsigned char)tag[i - 1])) &&
            strncasecmp(tag + i, attr, alen) == 0) {
            size_t j = i + alen;
            while (j < tlen && isspace((unsigned char)tag[j]))
                j++;
            if (j >= tlen || tag[j] != '=')
                continue;
            j++;
            while (j < tlen && isspace((unsigned char)tag[j]))
                j++;
            if (j >= tlen)
                return;
            char q = (tag[j] == '"' || tag[j] == '\'') ? tag[j++] : 0;
            size_t start = j;
            while (j < tlen && (q ? tag[j] != q : !isspace((unsigned char)tag[j])))
                j++;
            rbuf_putn(out, tag + start, j - start);
            return;
        }
    }
}

static bool tag_eq(const char *name, const char *target)
{
    return strcasecmp(name, target) == 0;
}

void render_html(struct rbuf *b, const char *src, size_t len,
                 int width, bool ascii)
{
    const struct box_chars *box = render_box_chars(ascii);
    if (width < 20)
        width = 20;

    struct rbuf text;
    rbuf_init(&text);
    struct rbuf link_href;
    rbuf_init(&link_href);
    struct rbuf link_text;
    rbuf_init(&link_text);

    bool in_pre = false;
    bool in_link = false;
    bool in_quote = false;
    bool in_li = false;
    char li_fp[32] = {0};
    char li_cp[32] = {0};
    int heading = 0;
    int list_depth = 0;
    bool list_ordered[8] = {false};
    int list_index[8] = {0};

    bool in_table = false;
    bool in_cell = false;
    struct rtable table = {0};
    struct rbuf cell_buf;
    rbuf_init(&cell_buf);

    for (size_t i = 0; i < len;) {
        if (src[i] == '<') {
            if (i + 3 < len && memcmp(src + i, "<!--", 4) == 0) {
                size_t cend = i + 4;
                while (cend + 2 < len && memcmp(src + cend, "-->", 3) != 0)
                    cend++;
                i = (cend + 2 < len) ? cend + 3 : len;
                continue;
            }
            size_t j = i + 1;
            char quote = 0;
            while (j < len) {
                if (quote) {
                    if (src[j] == quote)
                        quote = 0;
                } else if (src[j] == '"' || src[j] == '\'') {
                    quote = src[j];
                } else if (src[j] == '>') {
                    break;
                }
                j++;
            }
            if (j >= len) {
                rbuf_putc(&text, src[i++]);
                continue;
            }

            const char *tag = src + i + 1;
            size_t tlen = j - (i + 1);
            i = j + 1;

            bool closing = false;
            size_t k = 0;
            while (k < tlen && isspace((unsigned char)tag[k]))
                k++;
            if (k < tlen && tag[k] == '/') {
                closing = true;
                k++;
            }
            char name[32];
            size_t nlen = 0;
            while (k < tlen && !isspace((unsigned char)tag[k]) && tag[k] != '/') {
                if (nlen + 1 < sizeof(name))
                    name[nlen++] = tag[k];
                k++;
            }
            name[nlen] = '\0';

            if (!closing && (tag_eq(name, "script") || tag_eq(name, "style") ||
                             tag_eq(name, "head"))) {
                char end_tag[40];
                snprintf(end_tag, sizeof(end_tag), "</%s", name);
                size_t elen = strlen(end_tag);
                while (i < len) {
                    if (src[i] == '<' && i + elen < len &&
                        strncasecmp(src + i, end_tag, elen) == 0) {
                        while (i < len && src[i] != '>')
                            i++;
                        if (i < len)
                            i++;
                        break;
                    }
                    i++;
                }
                continue;
            }

            // Flush accumulated paragraph/heading text on block boundaries.
            bool is_heading = (name[0] == 'h' || name[0] == 'H') &&
                              name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
            bool is_block = is_heading || tag_eq(name, "p") || tag_eq(name, "div") ||
                            tag_eq(name, "section") || tag_eq(name, "article") ||
                            tag_eq(name, "blockquote") || tag_eq(name, "pre") ||
                            tag_eq(name, "ul") || tag_eq(name, "ol") ||
                            tag_eq(name, "li") || tag_eq(name, "hr") ||
                            tag_eq(name, "table") || tag_eq(name, "tr");

            if (is_block && text.len > 0 && !in_table) {
                if (heading > 0) {
                    rbuf_blank_line(b);
                    rbuf_wrap(b, text.data, width, "", "");
                    if (heading <= 2) {
                        int w = render_str_width(text.data, text.len);
                        if (w > width)
                            w = width;
                        if (w < 3)
                            w = 3;
                        rbuf_repeat(b, heading == 1 ? box->d_h : box->h, w);
                        rbuf_putc(b, '\n');
                    }
                    heading = 0;
                } else if (in_quote) {
                    char pfx[8];
                    snprintf(pfx, sizeof(pfx), "%s ", box->v);
                    rbuf_blank_line(b);
                    rbuf_wrap(b, text.data, width, pfx, pfx);
                } else if (in_li) {
                    rbuf_newline(b);
                    rbuf_wrap(b, text.data, width, li_fp, li_cp);
                    in_li = false;
                } else {
                    rbuf_blank_line(b);
                    rbuf_wrap(b, text.data, width, "", "");
                }
                text.len = 0;
            }

            if (is_heading) {
                if (!closing) {
                    heading = name[1] - '0';
                }
            } else if (tag_eq(name, "br")) {
                if (in_cell)
                    rbuf_putc(&cell_buf, ' ');
                else if (in_pre)
                    rbuf_putc(b, '\n');
                else if (text.len > 0) {
                    rbuf_newline(b);
                    rbuf_wrap(b, text.data, width, "", "");
                    text.len = 0;
                }
            } else if (tag_eq(name, "hr")) {
                rbuf_blank_line(b);
                rbuf_repeat(b, box->h, width > 72 ? 72 : width);
                rbuf_putc(b, '\n');
            } else if (tag_eq(name, "pre")) {
                in_pre = !closing;
                if (!closing)
                    rbuf_blank_line(b);
                else
                    rbuf_newline(b);
            } else if (tag_eq(name, "blockquote")) {
                in_quote = !closing;
            } else if (tag_eq(name, "ul") || tag_eq(name, "ol")) {
                if (!closing && list_depth < 8) {
                    list_ordered[list_depth] = tag_eq(name, "ol");
                    list_index[list_depth] = 0;
                    list_depth++;
                    if (list_depth == 1)
                        rbuf_blank_line(b);
                } else if (closing && list_depth > 0) {
                    list_depth--;
                }
            } else if (tag_eq(name, "li")) {
                if (!closing) {
                    if (list_depth > 0 && list_ordered[list_depth - 1])
                        list_index[list_depth - 1]++;
                    int lvl = list_depth > 0 ? list_depth - 1 : 0;
                    int p = 0;
                    for (int s = 0; s < lvl * 2; s++)
                        li_fp[p++] = ' ';
                    if (list_depth > 0 && list_ordered[list_depth - 1]) {
                        int nw = snprintf(li_fp + p, sizeof(li_fp) - (size_t)p,
                                          "%d. ", list_index[list_depth - 1]);
                        int cw = p + (nw > 0 ? nw : 3);
                        memset(li_cp, ' ', (size_t)cw);
                        li_cp[cw] = '\0';
                    } else {
                        snprintf(li_fp + p, sizeof(li_fp) - (size_t)p, "%s ", box->bullet);
                        int cw = p + 2;
                        memset(li_cp, ' ', (size_t)cw);
                        li_cp[cw] = '\0';
                    }
                    in_li = true;
                } else if (in_li) {
                    if (text.len > 0) {
                        rbuf_newline(b);
                        rbuf_wrap(b, text.data, width, li_fp, li_cp);
                        text.len = 0;
                    }
                    in_li = false;
                }
            } else if (tag_eq(name, "a")) {
                if (!closing) {
                    in_link = true;
                    link_href.len = 0;
                    link_text.len = 0;
                    extract_attr(tag, tlen, "href", &link_href);
                } else if (in_link) {
                    in_link = false;
                    struct rbuf *dst = in_cell ? &cell_buf : &text;
                    if (link_text.len > 0)
                        rbuf_putn(dst, link_text.data, link_text.len);
                    if (link_href.len > 0 &&
                        !(link_href.len == link_text.len &&
                          memcmp(link_href.data, link_text.data, link_href.len) == 0)) {
                        rbuf_puts(dst, " (");
                        rbuf_putn(dst, link_href.data, link_href.len);
                        rbuf_putc(dst, ')');
                    }
                }
            } else if (tag_eq(name, "table")) {
                if (!closing) {
                    in_table = true;
                    table = (struct rtable){0};
                } else if (in_table) {
                    in_table = false;
                    rbuf_blank_line(b);
                    render_table(b, &table, width, ascii);
                    rtable_free(&table);
                }
            } else if (tag_eq(name, "tr") && in_table) {
                if (!closing) {
                    struct rtable_row *rows = realloc(
                        table.rows, (table.nrows + 1) * sizeof(*rows));
                    if (rows) {
                        table.rows = rows;
                        table.rows[table.nrows++] = (struct rtable_row){0};
                    }
                }
            } else if ((tag_eq(name, "th") || tag_eq(name, "td")) && in_table) {
                if (!closing) {
                    in_cell = true;
                    cell_buf.len = 0;
                    if (tag_eq(name, "th") && table.nrows == 1)
                        table.has_header = true;
                } else if (in_cell) {
                    in_cell = false;
                    if (table.nrows > 0) {
                        struct rtable_row *row = &table.rows[table.nrows - 1];
                        char **cells = realloc(row->cells,
                                               (row->ncols + 1) * sizeof(*cells));
                        if (cells) {
                            row->cells = cells;
                            const char *cs = cell_buf.data ? cell_buf.data : "";
                            size_t cl = cell_buf.len;
                            while (cl > 0 && isspace((unsigned char)*cs)) {
                                cs++;
                                cl--;
                            }
                            while (cl > 0 && isspace((unsigned char)cs[cl - 1]))
                                cl--;
                            char *copy = malloc(cl + 1);
                            if (copy) {
                                memcpy(copy, cs, cl);
                                copy[cl] = '\0';
                            }
                            row->cells[row->ncols++] = copy ? copy : strdup("");
                            if (row->ncols > table.max_cols)
                                table.max_cols = row->ncols;
                        }
                    }
                }
            }
            continue;
        }

        struct rbuf *dst = in_link ? &link_text
                         : in_cell ? &cell_buf
                         : in_pre  ? b
                                   : &text;
        if (src[i] == '&') {
            size_t used = 1;
            decode_entity(dst, src + i, len - i, &used, ascii);
            i += used;
        } else if (!in_pre && isspace((unsigned char)src[i])) {
            if (dst->len > 0 && dst->data[dst->len - 1] != ' ')
                rbuf_putc(dst, ' ');
            i++;
        } else {
            if (in_pre && src[i] == '\n' && dst == b) {
                rbuf_putc(b, '\n');
            } else {
                rbuf_putc(dst, src[i]);
            }
            i++;
        }
    }

    if (text.len > 0) {
        rbuf_blank_line(b);
        rbuf_wrap(b, text.data, width, "", "");
    }
    if (in_table) {
        rbuf_blank_line(b);
        render_table(b, &table, width, ascii);
        rtable_free(&table);
    }

    rbuf_free(&cell_buf);
    rbuf_free(&link_text);
    rbuf_free(&link_href);
    rbuf_free(&text);
}
