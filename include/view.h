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

struct position {
    off_t byte;
    unsigned char tab_left;
    unsigned char tab_column;
};

bool position_equal(struct position a, struct position b);
int position_compare(struct position a, struct position b);
int view_row(struct source *s, struct position start, int width,
             char *out, struct position *next);
size_t view_row_capacity(int width);
void view_label(char *out, size_t cap, int width, const char *text);
int view_dump(struct source *s, int width);
int view_interactive(struct source *s, int tty, const char *label);

#endif
