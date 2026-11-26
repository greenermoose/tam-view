#ifndef VIEW_H
#define VIEW_H

/* tam-view's own interfaces. The document source, layout and renderers are
 * libtam-render (tam_render.h). */

#include <tam_render.h>

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

struct file_watch {
    struct stat stat;
    unsigned char *sample;
    size_t sample_size;
};

enum watch_change { WATCH_UNCHANGED, WATCH_APPEND, WATCH_REWRITE };

// On success, replacement is -1 or an open descriptor owned by the caller.
int watch_check(struct file_watch *watch, const char *path, int fd,
                int *replacement);
void watch_close(struct file_watch *watch);

struct view_input {
    const char *path;
    int fd;
    bool named;
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

enum view_hyperlinks {
    VIEW_HYPERLINKS_AUTO = 0,
    VIEW_HYPERLINKS_ALWAYS,
    VIEW_HYPERLINKS_NEVER
};

struct view_options {
    enum view_mode mode;
    unsigned long page_overlap;
    enum view_color color_mode;
    enum view_hyperlinks hyperlinks_mode;
    bool color;
    bool hyperlinks;
    bool ascii;
    bool linux_vt;
    bool quit_at_eof;
    bool no_watch;
};

int view_interactive(const struct view_input *inputs, size_t count, int tty,
                     struct view_options opts);

#endif
