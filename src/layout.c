#include "view.h"

#include <tam_uni.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define TAB_SIZE 8
#define MAX_MARKS 8

struct glyph {
    char bytes[4];
    size_t len;
    int width;
};

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

int view_row(struct source *s, struct position start, int width,
             char *out, struct position *next)
{
    struct position p = start;
    int cells = 0;
    unsigned marks = 0;
    size_t used = 0;

    for (;;) {
        uint32_t cp;
        int count = 0;
        bool tab = p.tab_left > 0;
        if (tab) {
            cp = ' ';
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
            memcpy(out + used, g.bytes, g.len);
            used += g.len;
        }
    }
    if (out)
        out[used] = '\0';
    *next = p;
    return VIEW_ROW;
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

int view_dump(struct source *s, int width)
{
    // A terminal without cursor controls can still safely show plain text.
    char buf[4 * (1 + MAX_MARKS) * 80 + 1];
    struct position p = {0}, next;
    for (;;) {
        int result = view_row(s, p, width, buf, &next);
        if (result == VIEW_WAIT) {
            struct pollfd fd = {.fd = s->input, .events = POLLIN};
            int ready = poll(&fd, 1, -1);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || source_pump(s) < 0)
                return -1;
        } else if (result == VIEW_ROW) {
            if (puts(buf) == EOF)
                return -1;
            p = next;
        } else {
            return result == VIEW_EOF ? fflush(stdout) : -1;
        }
    }
}
