#include "view.h"

#include <tam_uni.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAB_SIZE 8
#define MAX_MARKS 8

struct glyph {
    char bytes[4];
    size_t len;
    int width;
};

void view_styles_free(struct view_styles *st)
{
    if (!st)
        return;
    free(st->spans);
    *st = (struct view_styles){0};
}

bool view_styles_push(struct view_styles *st, off_t start, off_t end,
                      unsigned char style)
{
    if (!st || style == VIEW_STYLE_NONE || end <= start)
        return true;
    if (st->count > 0) {
        struct view_span *prev = &st->spans[st->count - 1];
        if (start < prev->end) {
            start = prev->end;
            if (end <= start)
                return true;
        }
        if (prev->end == start && prev->style == style) {
            prev->end = end;
            return true;
        }
    }
    if (st->count == st->cap) {
        size_t ncap = st->cap ? st->cap * 2 : 64;
        struct view_span *grown = realloc(st->spans, ncap * sizeof(*grown));
        if (!grown)
            return false;
        st->spans = grown;
        st->cap = ncap;
    }
    st->spans[st->count++] = (struct view_span){
        .start = start,
        .end = end,
        .style = style
    };
    return true;
}

static const char *style_sgr(unsigned char style, bool linux_vt)
{
    switch (style) {
    case VIEW_STYLE_H1:         return "\033[1;36m";
    case VIEW_STYLE_H2:         return "\033[1;34m";
    case VIEW_STYLE_H3:         return "\033[1;33m";
    case VIEW_STYLE_BOLD:       return "\033[1m";
    case VIEW_STYLE_ITALIC:     return linux_vt ? "\033[36m" : "\033[3m";
    case VIEW_STYLE_STRIKE:     return linux_vt ? "\033[2m" : "\033[9;2m";
    case VIEW_STYLE_CODE:       return "\033[33m";
    case VIEW_STYLE_LINK:       return "\033[4;34m";
    case VIEW_STYLE_LINK_URL:   return "\033[2m";
    case VIEW_STYLE_QUOTE:      return linux_vt ? "\033[32m" : "\033[2;32m";
    case VIEW_STYLE_RULE:       return "\033[2m";
    case VIEW_STYLE_TABLE_HDR:  return "\033[1m";
    case VIEW_STYLE_NODE:       return "\033[1;36m";
    case VIEW_STYLE_EDGE:       return "\033[33m";
    case VIEW_STYLE_SEARCH:     return "\033[7m";
    case VIEW_STYLE_SEARCH_CUR: return "\033[1;30;43m";
    default:                    return "";
    }
}

static size_t span_lower_bound(const struct view_span *spans, size_t count,
                               off_t byte)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (spans[mid].end <= byte)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static unsigned char span_style_at(const struct view_span *spans, size_t count,
                                   size_t *idx, off_t byte)
{
    while (*idx < count && spans[*idx].end <= byte)
        (*idx)++;
    if (*idx < count && spans[*idx].start <= byte && byte < spans[*idx].end)
        return spans[*idx].style;
    return VIEW_STYLE_NONE;
}

static void emit_style(char *out, size_t *used, unsigned char *active,
                       unsigned char want, bool linux_vt)
{
    if (*active == want)
        return;
    if (out) {
        if (*active != VIEW_STYLE_NONE) {
            memcpy(out + *used, "\033[0m", 4);
            *used += 4;
        }
        if (want != VIEW_STYLE_NONE) {
            const char *seq = style_sgr(want, linux_vt);
            size_t slen = strlen(seq);
            memcpy(out + *used, seq, slen);
            *used += slen;
        }
    }
    *active = want;
}

bool position_equal(struct position a, struct position b)
{
    return a.byte == b.byte && a.tab_left == b.tab_left;
}

int position_compare(struct position a, struct position b)
{
    if (a.byte != b.byte)
        return a.byte < b.byte ? -1 : 1;
    return (a.tab_left < b.tab_left) - (a.tab_left > b.tab_left);
}

static struct glyph glyph(uint32_t cp, int width)
{
    struct glyph result = {0};
    if (cp < 0x20 || cp == 0x7f) {
        result.bytes[0] = '^';
        result.bytes[1] = cp == 0x7f ? '?' : (char)(cp + '@');
        result.len = 2;
        result.width = 2;
    } else if ((cp >= 0x80 && cp <= 0x9f) ||
               (cp >= 0x202a && cp <= 0x202e) ||
               (cp >= 0x2066 && cp <= 0x2069)) {
        result.bytes[0] = '?';
        result.len = 1;
        result.width = 1;
    } else {
        result.len = tam_utf8_encode(cp, result.bytes);
        result.width = tam_uni_width(cp);
    }
    if (result.width > width)
        return (struct glyph){.bytes = "?", .len = 1, .width = 1};
    return result;
}

static int decode(struct source *s, off_t offset, int first, uint32_t *cp)
{
    char bytes[4] = {(char)first};
    size_t count = 1;
    size_t need = tam_utf8_seq_len((unsigned char)first);
    for (; count < need; count++) {
        int c = source_byte(s, offset + (off_t)count);
        if (c == VIEW_ERROR || c == VIEW_WAIT)
            return c;
        if (c < 0 || (c & 0xc0) != 0x80)
            break;
        bytes[count] = (char)c;
    }
    return (int)tam_utf8_decode(bytes, count, cp);
}

size_t view_row_capacity(int width)
{
    return (size_t)width * 4 * (1 + MAX_MARKS) + 1;
}

size_t view_row_styled_capacity(int width)
{
    return view_row_capacity(width) + (size_t)width * 16 + 8;
}

static unsigned char resolve_style(const struct view_row_style *st,
                                   size_t *doc_idx, size_t *search_idx,
                                   off_t byte)
{
    if (!st)
        return VIEW_STYLE_NONE;
    if (st->search_count > 0) {
        unsigned char s = span_style_at(st->search_spans, st->search_count,
                                        search_idx, byte);
        if (s != VIEW_STYLE_NONE)
            return s;
    }
    if (st->doc_count > 0)
        return span_style_at(st->doc_spans, st->doc_count, doc_idx, byte);
    return VIEW_STYLE_NONE;
}

int view_row_styled(struct source *s, struct position start, int width,
                    int skip, bool chop, const struct view_row_style *style,
                    char *out, struct position *next)
{
    bool has_style = style && (style->doc_count > 0 || style->search_count > 0);
    const struct view_row_style *st = has_style ? style : NULL;
    size_t doc_idx = st && st->doc_count
                     ? span_lower_bound(st->doc_spans, st->doc_count, start.byte)
                     : 0;
    size_t search_idx = st && st->search_count
                        ? span_lower_bound(st->search_spans, st->search_count, start.byte)
                        : 0;
    bool linux_vt = st ? st->linux_vt : false;
    unsigned char active = VIEW_STYLE_NONE;

    if (skip == 0 && !chop) {
        struct position p = start;
        int cells = 0;
        unsigned marks = 0;
        size_t used = 0;

        for (;;) {
            uint32_t cp;
            int count = 0;
            bool tab = p.tab_left > 0;
            off_t glyph_byte = p.byte;
            if (tab) {
                cp = ' ';
                if (glyph_byte > 0)
                    glyph_byte--;
            } else {
                int b = source_byte(s, p.byte);
                if (b == VIEW_EOF) {
                    if (position_equal(p, start))
                        return VIEW_EOF;
                    break;
                }
                if (b < 0)
                    return b;

                if (b == '\r') {
                    int after = source_byte(s, p.byte + 1);
                    if (after == VIEW_ERROR || after == VIEW_WAIT)
                        return after;
                    if (after == '\n') {
                        b = '\n';
                        p.byte++;
                    }
                }
                if (b == '\n') {
                    p.byte++;
                    p.tab_column = 0;
                    break;
                }
                if (b == '\t') {
                    p.byte++;
                    p.tab_left = TAB_SIZE - p.tab_column;
                    continue;
                }
                count = decode(s, p.byte, b, &cp);
                if (count < 0)
                    return count;
            }

            struct glyph g = glyph(cp, width);
            if (g.width == 0 && (cells == 0 || marks >= MAX_MARKS))
                g = (struct glyph){.bytes = "?", .len = 1, .width = 1};
            if (g.width > width - cells)
                break;
            p.byte += count;
            if (tab)
                p.tab_left--;
            p.tab_column = (unsigned char)((p.tab_column + g.width) % TAB_SIZE);
            if (g.width != 0)
                marks = 0;
            else
                marks++;
            cells += g.width;
            if (out) {
                if (st) {
                    unsigned char want = resolve_style(st, &doc_idx, &search_idx,
                                                       glyph_byte);
                    emit_style(out, &used, &active, want, linux_vt);
                }
                memcpy(out + used, g.bytes, g.len);
                used += g.len;
            }
        }
        if (out) {
            emit_style(out, &used, &active, VIEW_STYLE_NONE, linux_vt);
            out[used] = '\0';
        }
        *next = p;
        return VIEW_ROW;
    }

    struct position p = {.byte = start.byte};
    int col = 0;
    int cells = 0;
    unsigned marks = 0;
    size_t used = 0;

    for (;;) {
        int b = source_byte(s, p.byte);
        if (b == VIEW_EOF) {
            if (p.byte == start.byte)
                return VIEW_EOF;
            break;
        }
        if (b < 0)
            return b;

        if (b == '\r') {
            int after = source_byte(s, p.byte + 1);
            if (after == VIEW_ERROR || after == VIEW_WAIT)
                return after;
            if (after == '\n') {
                b = '\n';
                p.byte++;
            }
        }
        if (b == '\n') {
            p.byte++;
            break;
        }
        if (b == '\t') {
            off_t glyph_byte = p.byte;
            p.byte++;
            int tab_w = TAB_SIZE - (col % TAB_SIZE);
            int start_c = col;
            int end_c = col + tab_w;
            col = end_c;
            if (end_c > skip && cells < width) {
                int vis_start = start_c > skip ? start_c : skip;
                int vis_end = end_c < skip + width ? end_c : skip + width;
                int spaces = vis_end - vis_start;
                if (spaces > width - cells)
                    spaces = width - cells;
                if (out) {
                    if (st) {
                        unsigned char want = resolve_style(st, &doc_idx, &search_idx,
                                                           glyph_byte);
                        emit_style(out, &used, &active, want, linux_vt);
                    }
                    memset(out + used, ' ', (size_t)spaces);
                    used += (size_t)spaces;
                }
                cells += spaces;
                marks = 0;
            }
            continue;
        }

        off_t glyph_byte = p.byte;
        uint32_t cp;
        int count = decode(s, p.byte, b, &cp);
        if (count < 0)
            return count;
        p.byte += count;

        struct glyph g = glyph(cp, width);
        if (g.width == 0) {
            if (col < skip || cells >= width)
                continue;
            if (cells == 0 || marks >= MAX_MARKS)
                g = (struct glyph){.bytes = "?", .len = 1, .width = 1};
        }

        int start_c = col;
        int end_c = col + g.width;
        col = end_c;

        if (cells >= width)
            continue;
        if (end_c <= skip)
            continue;
        if (start_c < skip && end_c > skip) {
            int spaces = end_c - skip;
            if (spaces > width - cells)
                spaces = width - cells;
            if (out) {
                if (st) {
                    unsigned char want = resolve_style(st, &doc_idx, &search_idx,
                                                       glyph_byte);
                    emit_style(out, &used, &active, want, linux_vt);
                }
                memset(out + used, ' ', (size_t)spaces);
                used += (size_t)spaces;
            }
            cells += spaces;
            marks = 0;
            continue;
        }
        if (g.width > width - cells) {
            cells = width;
            continue;
        }
        if (out) {
            if (st) {
                unsigned char want = resolve_style(st, &doc_idx, &search_idx,
                                                   glyph_byte);
                emit_style(out, &used, &active, want, linux_vt);
            }
            memcpy(out + used, g.bytes, g.len);
            used += g.len;
        }
        cells += g.width;
        if (g.width != 0)
            marks = 0;
        else
            marks++;
    }
    if (out) {
        emit_style(out, &used, &active, VIEW_STYLE_NONE, linux_vt);
        out[used] = '\0';
    }
    *next = p;
    return VIEW_ROW;
}

int view_row_h(struct source *s, struct position start, int width,
               int skip, bool chop, char *out, struct position *next)
{
    return view_row_styled(s, start, width, skip, chop, NULL, out, next);
}

int view_row(struct source *s, struct position start, int width,
             char *out, struct position *next)
{
    return view_row_styled(s, start, width, 0, false, NULL, out, next);
}

void view_label(char *out, size_t cap, int width, const char *text)
{
    size_t used = 0;
    size_t remaining = strlen(text);
    bool have_base = false;
    while (remaining) {
        uint32_t cp;
        size_t read = tam_utf8_decode(text, remaining, &cp);
        struct glyph g = glyph(cp, width > 1 ? width : 1);
        if (!have_base && g.width == 0)
            g = (struct glyph){.bytes = "?", .len = 1, .width = 1};
        if (g.width > width)
            break;
        if (g.len >= cap - used)
            break;
        memcpy(out + used, g.bytes, g.len);
        used += g.len;
        width -= g.width;
        have_base = true;
        text += read;
        remaining -= read;
    }
    out[used] = '\0';
}

int view_dump_styled(struct source *s, int width,
                     const struct view_styles *styles, bool linux_vt)
{
    // A terminal without cursor controls can still safely show plain or styled text.
    char *buf = malloc(view_row_styled_capacity(width));
    if (!buf)
        return -1;
    struct view_row_style rs = {
        .doc_spans = styles ? styles->spans : NULL,
        .doc_count = styles ? styles->count : 0,
        .linux_vt = linux_vt
    };
    const struct view_row_style *rsp = rs.doc_count > 0 ? &rs : NULL;
    struct position p = {0}, next;
    int rc = 0;
    for (;;) {
        int result = view_row_styled(s, p, width, 0, false, rsp, buf, &next);
        if (result == VIEW_WAIT) {
            struct pollfd fd = {.fd = s->input, .events = POLLIN};
            int ready = poll(&fd, 1, -1);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || source_pump(s) < 0) {
                rc = -1;
                break;
            }
        } else if (result == VIEW_ROW) {
            if (puts(buf) == EOF) {
                rc = -1;
                break;
            }
            p = next;
        } else {
            rc = result == VIEW_EOF ? fflush(stdout) : -1;
            break;
        }
    }
    free(buf);
    return rc;
}

int view_dump(struct source *s, int width)
{
    return view_dump_styled(s, width, NULL, false);
}
