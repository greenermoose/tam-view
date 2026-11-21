#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#define RENDER_MAX_INPUT (16 * 1024 * 1024)

struct rbuf {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
};

struct box_chars {
    const char *tl;
    const char *tr;
    const char *bl;
    const char *br;
    const char *h;
    const char *v;
    const char *lt;
    const char *rt;
    const char *tt;
    const char *bt;
    const char *cross;
    const char *r_tl;
    const char *r_tr;
    const char *r_bl;
    const char *r_br;
    const char *arrow_r;
    const char *arrow_d;
    const char *arrow_l;
    const char *arrow_u;
    const char *bullet;
    const char *d_h;
};

struct rtable_row {
    char **cells;
    size_t ncols;
};

struct rtable {
    struct rtable_row *rows;
    size_t nrows;
    size_t max_cols;
    bool has_header;
};

void rbuf_init(struct rbuf *b);
void rbuf_free(struct rbuf *b);
void rbuf_putc(struct rbuf *b, char c);
void rbuf_putn(struct rbuf *b, const char *s, size_t n);
void rbuf_puts(struct rbuf *b, const char *s);
void rbuf_printf(struct rbuf *b, const char *fmt, ...);
void rbuf_repeat(struct rbuf *b, const char *s, int count);
void rbuf_newline(struct rbuf *b);
void rbuf_blank_line(struct rbuf *b);
void rbuf_wrap(struct rbuf *b, const char *text, int width,
               const char *first_prefix, const char *cont_prefix);

const struct box_chars *render_box_chars(bool ascii);
int render_str_width(const char *s, size_t len);
void render_table(struct rbuf *b, const struct rtable *t, int width, bool ascii);
void rtable_free(struct rtable *t);

void render_markdown(struct rbuf *b, const char *src, size_t len,
                     int width, bool ascii);
void render_html(struct rbuf *b, const char *src, size_t len,
                 int width, bool ascii);
void render_mermaid(struct rbuf *b, const char *src, size_t len,
                    int width, bool ascii);

#endif
