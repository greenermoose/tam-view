#ifndef VIEW_H
#define VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

enum view_result {
    VIEW_ERROR = -3,
    VIEW_WAIT = -2,
    VIEW_EOF = -1,
    VIEW_ROW = 0
};

#define VIEW_BLOCK (64 * 1024)

struct source {
    int input;
    int fd;
    FILE *spool;
    bool eof;
    off_t size;
    off_t block;
    size_t used;
    unsigned char data[VIEW_BLOCK];
};

int source_open(struct source *s, int input);
void source_close(struct source *s);
int source_pump(struct source *s);
int source_byte(struct source *s, off_t offset);
off_t source_size(struct source *s);
int source_refresh(struct source *s, off_t *new_size);

struct position {
    off_t byte;
    unsigned char tab_left;
    unsigned char tab_column;
};

struct view_input {
    const char *path;
    int fd;
};

enum view_format {
    VIEW_FORMAT_RAW = 0,
    VIEW_FORMAT_MARKDOWN,
    VIEW_FORMAT_HTML,
    VIEW_FORMAT_MERMAID
};

enum view_mode {
    VIEW_MODE_AUTO = 0,
    VIEW_MODE_RAW,
    VIEW_MODE_RENDER
};

enum view_color {
    VIEW_COLOR_AUTO = 0,
    VIEW_COLOR_ALWAYS,
    VIEW_COLOR_NEVER
};

enum view_style {
    VIEW_STYLE_NONE = 0,
    VIEW_STYLE_H1,
    VIEW_STYLE_H2,
    VIEW_STYLE_H3,
    VIEW_STYLE_BOLD,
    VIEW_STYLE_ITALIC,
    VIEW_STYLE_STRIKE,
    VIEW_STYLE_CODE,
    VIEW_STYLE_LINK,
    VIEW_STYLE_LINK_URL,
    VIEW_STYLE_QUOTE,
    VIEW_STYLE_RULE,
    VIEW_STYLE_TABLE_HDR,
    VIEW_STYLE_NODE,
    VIEW_STYLE_EDGE,
    VIEW_STYLE_SEARCH,
    VIEW_STYLE_SEARCH_CUR
};

struct view_span {
    off_t start;
    off_t end;
    unsigned char style;
};

struct view_styles {
    struct view_span *spans;
    size_t count;
    size_t cap;
};

struct view_row_style {
    const struct view_span *doc_spans;
    size_t doc_count;
    const struct view_span *search_spans;
    size_t search_count;
    bool linux_vt;
};

struct view_options {
    enum view_mode mode;
    enum view_color color_mode;
    bool color;
    bool ascii;
    bool linux_vt;
    bool quit_at_eof;
};

void view_styles_free(struct view_styles *st);
bool view_styles_push(struct view_styles *st, off_t start, off_t end,
                      unsigned char style);

bool position_equal(struct position a, struct position b);
int position_compare(struct position a, struct position b);
int view_row(struct source *s, struct position start, int width,
             char *out, struct position *next);
int view_row_h(struct source *s, struct position start, int width,
               int skip, bool chop, char *out, struct position *next);
int view_row_styled(struct source *s, struct position start, int width,
                    int skip, bool chop, const struct view_row_style *style,
                    char *out, struct position *next);
size_t view_row_capacity(int width);
size_t view_row_styled_capacity(int width);
void view_label(char *out, size_t cap, int width, const char *text);
int view_dump(struct source *s, int width);
int view_dump_styled(struct source *s, int width,
                     const struct view_styles *styles, bool linux_vt);
enum view_format view_detect_format(const char *path, struct source *s,
                                    bool force_render);
int view_render_source(struct source *raw, const char *path, int width,
                       bool ascii, bool force_render, struct source *out,
                       struct view_styles *styles_out);
int view_interactive(const struct view_input *inputs, size_t count, int tty,
                     struct view_options opts);

#endif
