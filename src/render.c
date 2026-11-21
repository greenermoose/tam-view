#include "render_internal.h"
#include "view.h"

#include <tam_uni.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct box_chars BOX_UNICODE = {
    .tl = "\xe2\x94\x8c",
    .tr = "\xe2\x94\x90",
    .bl = "\xe2\x94\x94",
    .br = "\xe2\x94\x98",
    .h = "\xe2\x94\x80",
    .v = "\xe2\x94\x82",
    .lt = "\xe2\x94\x9c",
    .rt = "\xe2\x94\xa4",
    .tt = "\xe2\x94\xac",
    .bt = "\xe2\x94\xb4",
    .cross = "\xe2\x94\xbc",
    .r_tl = "\xe2\x95\xad",
    .r_tr = "\xe2\x95\xae",
    .r_bl = "\xe2\x95\xb0",
    .r_br = "\xe2\x95\xaf",
    .arrow_r = "\xe2\x96\xb6",
    .arrow_d = "\xe2\x96\xbc",
    .arrow_l = "\xe2\x97\x80",
    .arrow_u = "\xe2\x96\xb2",
    .bullet = "\xe2\x80\xa2",
    .d_h = "\xe2\x95\x90"
};

static const struct box_chars BOX_ASCII = {
    .tl = "+",
    .tr = "+",
    .bl = "+",
    .br = "+",
    .h = "-",
    .v = "|",
    .lt = "+",
    .rt = "+",
    .tt = "+",
    .bt = "+",
    .cross = "+",
    .r_tl = "+",
    .r_tr = "+",
    .r_bl = "+",
    .r_br = "+",
    .arrow_r = ">",
    .arrow_d = "v",
    .arrow_l = "<",
    .arrow_u = "^",
    .bullet = "*",
    .d_h = "="
};

const struct box_chars *render_box_chars(bool ascii)
{
    return ascii ? &BOX_ASCII : &BOX_UNICODE;
}

void rbuf_init(struct rbuf *b)
{
    *b = (struct rbuf){0};
}

void rbuf_free(struct rbuf *b)
{
    free(b->data);
    *b = (struct rbuf){0};
}

static bool rbuf_reserve(struct rbuf *b, size_t extra)
{
    if (b->oom)
        return false;
    if (extra > SIZE_MAX - b->len - 1) {
        b->oom = true;
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return true;
    size_t next = b->cap ? b->cap : 4096;
    while (next < need) {
        if (next > SIZE_MAX / 2) {
            next = need;
            break;
        }
        next *= 2;
    }
    char *grown = realloc(b->data, next);
    if (!grown) {
        b->oom = true;
        return false;
    }
    b->data = grown;
    b->cap = next;
    return true;
}

void rbuf_putc(struct rbuf *b, char c)
{
    if (!rbuf_reserve(b, 1))
        return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void rbuf_putn(struct rbuf *b, const char *s, size_t n)
{
    if (!s || !n || !rbuf_reserve(b, n))
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void rbuf_puts(struct rbuf *b, const char *s)
{
    if (s)
        rbuf_putn(b, s, strlen(s));
}

void rbuf_printf(struct rbuf *b, const char *fmt, ...)
{
    if (b->oom)
        return;
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(copy);
        b->oom = true;
        return;
    }
    if (!rbuf_reserve(b, (size_t)need)) {
        va_end(copy);
        return;
    }
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, copy);
    va_end(copy);
    b->len += (size_t)need;
}

void rbuf_repeat(struct rbuf *b, const char *s, int count)
{
    if (!s || count <= 0)
        return;
    size_t slen = strlen(s);
    for (int i = 0; i < count; i++)
        rbuf_putn(b, s, slen);
}

void rbuf_newline(struct rbuf *b)
{
    if (b->len == 0 || b->data[b->len - 1] != '\n')
        rbuf_putc(b, '\n');
}

void rbuf_blank_line(struct rbuf *b)
{
    if (b->len == 0)
        return;
    rbuf_newline(b);
    if (b->len < 2 || b->data[b->len - 2] != '\n')
        rbuf_putc(b, '\n');
}

int render_str_width(const char *s, size_t len)
{
    return tam_uni_str_width(s, len);
}

void rbuf_wrap(struct rbuf *b, const char *text, int width,
               const char *first_prefix, const char *cont_prefix)
{
    if (!text)
        return;
    const char *fp = first_prefix ? first_prefix : "";
    const char *cp = cont_prefix ? cont_prefix : fp;
    int fp_w = render_str_width(fp, strlen(fp));
    int cp_w = render_str_width(cp, strlen(cp));
    if (width < 20)
        width = 20;

    const char *p = text;
    while (*p && isspace((unsigned char)*p))
        p++;

    bool first_line = true;
    int line_w = fp_w;
    rbuf_puts(b, fp);

    while (*p) {
        const char *ws = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - ws);
        int ww = render_str_width(ws, wlen);

        int max_w = width;
        const char *pfx = first_line ? fp : cp;
        int pfx_w = first_line ? fp_w : cp_w;
        (void)pfx;

        if (line_w > pfx_w && line_w + 1 + ww > max_w) {
            rbuf_putc(b, '\n');
            first_line = false;
            rbuf_puts(b, cp);
            line_w = cp_w;
        } else if (line_w > pfx_w) {
            rbuf_putc(b, ' ');
            line_w++;
        }

        rbuf_putn(b, ws, wlen);
        line_w += ww;

        while (*p && isspace((unsigned char)*p))
            p++;
    }
    rbuf_putc(b, '\n');
}

void rtable_free(struct rtable *t)
{
    for (size_t r = 0; r < t->nrows; r++) {
        for (size_t c = 0; c < t->rows[r].ncols; c++)
            free(t->rows[r].cells[c]);
        free(t->rows[r].cells);
    }
    free(t->rows);
    *t = (struct rtable){0};
}

static void render_table_border(struct rbuf *b, const int *col_w, size_t ncols,
                                const char *left, const char *mid,
                                const char *right, const char *h)
{
    rbuf_puts(b, left);
    for (size_t c = 0; c < ncols; c++) {
        rbuf_repeat(b, h, col_w[c] + 2);
        rbuf_puts(b, c + 1 < ncols ? mid : right);
    }
    rbuf_putc(b, '\n');
}

void render_table(struct rbuf *b, const struct rtable *t, int width, bool ascii)
{
    if (!t || t->nrows == 0 || t->max_cols == 0)
        return;
    const struct box_chars *box = render_box_chars(ascii);
    size_t ncols = t->max_cols;
    int *col_w = calloc(ncols, sizeof(*col_w));
    if (!col_w) {
        b->oom = true;
        return;
    }

    for (size_t r = 0; r < t->nrows; r++) {
        for (size_t c = 0; c < t->rows[r].ncols; c++) {
            const char *cell = t->rows[r].cells[c] ? t->rows[r].cells[c] : "";
            int w = render_str_width(cell, strlen(cell));
            if (w > col_w[c])
                col_w[c] = w;
        }
    }
    for (size_t c = 0; c < ncols; c++) {
        if (col_w[c] < 1)
            col_w[c] = 1;
    }

    int overhead = (int)(ncols * 3 + 1);
    int total = overhead;
    for (size_t c = 0; c < ncols; c++)
        total += col_w[c];

    if (width > overhead + (int)ncols && total > width) {
        int avail = width - overhead;
        while (total > width && avail >= (int)ncols) {
            size_t widest = 0;
            for (size_t c = 1; c < ncols; c++) {
                if (col_w[c] > col_w[widest])
                    widest = c;
            }
            if (col_w[widest] <= 3)
                break;
            col_w[widest]--;
            total--;
        }
    }

    render_table_border(b, col_w, ncols, box->tl, box->tt, box->tr, box->h);
    for (size_t r = 0; r < t->nrows; r++) {
        rbuf_puts(b, box->v);
        for (size_t c = 0; c < ncols; c++) {
            const char *cell = (c < t->rows[r].ncols && t->rows[r].cells[c])
                               ? t->rows[r].cells[c] : "";
            size_t clen = strlen(cell);
            int cw = render_str_width(cell, clen);
            rbuf_putc(b, ' ');
            if (cw <= col_w[c]) {
                rbuf_putn(b, cell, clen);
                rbuf_repeat(b, " ", col_w[c] - cw);
            } else {
                char *fit = malloc(clen + 1);
                if (!fit) {
                    free(col_w);
                    b->oom = true;
                    return;
                }
                int used = tam_uni_fit(cell, clen,
                                       col_w[c] > 1 ? col_w[c] - 1 : col_w[c],
                                       fit, clen + 1);
                rbuf_puts(b, fit);
                free(fit);
                if (col_w[c] > 1) {
                    rbuf_puts(b, ascii ? "~" : "\xe2\x80\xa6");
                    used++;
                }
                rbuf_repeat(b, " ", col_w[c] - used);
            }
            rbuf_putc(b, ' ');
            rbuf_puts(b, box->v);
        }
        rbuf_putc(b, '\n');
        if (r == 0 && t->has_header && t->nrows > 1)
            render_table_border(b, col_w, ncols, box->lt, box->cross, box->rt, box->h);
    }
    render_table_border(b, col_w, ncols, box->bl, box->bt, box->br, box->h);
    free(col_w);
}

static bool has_ext(const char *path, const char *ext)
{
    if (!path)
        return false;
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen)
        return false;
    const char *tail = path + plen - elen;
    for (size_t i = 0; i < elen; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static bool starts_with_ci(const char *s, size_t len, const char *pfx)
{
    size_t plen = strlen(pfx);
    if (len < plen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i]))
            return false;
    }
    return true;
}

static enum view_format sniff_content(const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len && isspace((unsigned char)buf[i]))
        i++;
    if (i >= len)
        return VIEW_FORMAT_RAW;

    const char *p = buf + i;
    size_t rem = len - i;

    if (starts_with_ci(p, rem, "<!doctype html") ||
        starts_with_ci(p, rem, "<html") ||
        starts_with_ci(p, rem, "<head") ||
        starts_with_ci(p, rem, "<body") ||
        starts_with_ci(p, rem, "<h1") ||
        starts_with_ci(p, rem, "<div") ||
        starts_with_ci(p, rem, "<p>") ||
        starts_with_ci(p, rem, "<table")) {
        return VIEW_FORMAT_HTML;
    }

    if (starts_with_ci(p, rem, "flowchart ") ||
        starts_with_ci(p, rem, "flowchart\t") ||
        starts_with_ci(p, rem, "graph ")) {
        return VIEW_FORMAT_MERMAID;
    }

    return VIEW_FORMAT_RAW;
}

static int read_source_prefix(struct source *s, char *buf, size_t cap,
                              size_t *out_len)
{
    size_t n = 0;
    while (n < cap) {
        int b = source_byte(s, (off_t)n);
        if (b == VIEW_EOF || b == VIEW_WAIT)
            break;
        if (b < 0)
            return -1;
        buf[n++] = (char)b;
    }
    *out_len = n;
    return 0;
}

enum view_format view_detect_format(const char *path, struct source *s,
                                    bool force_render)
{
    if (has_ext(path, ".md") || has_ext(path, ".markdown"))
        return VIEW_FORMAT_MARKDOWN;
    if (has_ext(path, ".html") || has_ext(path, ".htm") || has_ext(path, ".xhtml"))
        return VIEW_FORMAT_HTML;
    if (has_ext(path, ".mmd") || has_ext(path, ".mermaid"))
        return VIEW_FORMAT_MERMAID;

    if (s) {
        char head[512];
        size_t len = 0;
        if (read_source_prefix(s, head, sizeof(head), &len) == 0) {
            enum view_format sniffed = sniff_content(head, len);
            if (sniffed != VIEW_FORMAT_RAW)
                return sniffed;
        }
    }

    return force_render ? VIEW_FORMAT_MARKDOWN : VIEW_FORMAT_RAW;
}

static int read_all_source(struct source *s, char **out_buf, size_t *out_len)
{
    while (!s->eof) {
        if (source_pump(s) < 0)
            return -1;
        if (!s->eof && s->spool) {
            // Avoid spinning if a slow pipe has no more bytes yet; read what's available.
            break;
        }
    }
    off_t sz = source_size(s);
    if (sz < 0)
        return -1;
    if (sz > RENDER_MAX_INPUT) {
        errno = EFBIG;
        return -1;
    }
    size_t len = (size_t)sz;
    char *buf = malloc(len + 1);
    if (!buf)
        return -1;
    for (size_t i = 0; i < len; i++) {
        int b = source_byte(s, (off_t)i);
        if (b < 0) {
            free(buf);
            if (b == VIEW_EOF) {
                len = i;
                break;
            }
            errno = EIO;
            return -1;
        }
        buf[i] = (char)b;
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

int view_render_source(struct source *raw, const char *path, int width,
                       bool ascii, bool force_render, struct source *out)
{
    enum view_format fmt = view_detect_format(path, raw, force_render);
    if (fmt == VIEW_FORMAT_RAW) {
        errno = EINVAL;
        return -1;
    }

    char *src = NULL;
    size_t len = 0;
    if (read_all_source(raw, &src, &len) < 0)
        return -1;

    struct rbuf b;
    rbuf_init(&b);
    int w = width > 0 ? width : 80;
    if (fmt == VIEW_FORMAT_MARKDOWN)
        render_markdown(&b, src, len, w, ascii);
    else if (fmt == VIEW_FORMAT_HTML)
        render_html(&b, src, len, w, ascii);
    else
        render_mermaid(&b, src, len, w, ascii);

    free(src);
    if (b.oom) {
        rbuf_free(&b);
        errno = ENOMEM;
        return -1;
    }

    FILE *fp = tmpfile();
    if (!fp) {
        rbuf_free(&b);
        return -1;
    }
    if (b.len > 0 && fwrite(b.data, 1, b.len, fp) != b.len) {
        fclose(fp);
        rbuf_free(&b);
        errno = EIO;
        return -1;
    }
    rbuf_free(&b);
    if (fflush(fp) != 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);

    *out = (struct source){
        .input = -1,
        .fd = fileno(fp),
        .spool = fp,
        .eof = true,
        .block = -1
    };
    off_t end = lseek(out->fd, 0, SEEK_END);
    if (end < 0) {
        source_close(out);
        return -1;
    }
    out->size = end;
    return 0;
}
