#include "view.h"

#include <tam_key.h>
#include <tam_pager.h>
#include <tam_tty.h>
#include <tam_uni.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ROWS 512
#define MAX_COLUMNS 4096
#define SEARCH_SIZE 512
#define MARK_COUNT 53
#define MARK_PREV 52
#define LINE_BUF_MAX (16 * 1024 * 1024)
#define ROW_INDEX_MAX_BYTES (4 * 1024 * 1024)
#define HISTORY_MAX 256

enum job { IDLE, ADVANCE, GO_LINE, GO_END, SEARCH };
enum prompt {
    COMMAND,
    FIND_FORWARD,
    FIND_BACKWARD,
    MARK_SET,
    MARK_GOTO,
    COLON,
    HELP
};

struct frame {
    struct position rows[MAX_ROWS];
    char *text;
    size_t stride;
    int count;
    bool valid;
    bool document;
};

struct mark {
    bool set;
    size_t file;
    struct position pos;
    int h_offset;
};

struct nav_entry {
    size_t file_index;
    struct position top;
    struct position attn;
    int h_offset;
    bool render_mode;
};

struct file_entry {
    const char *path;
    bool own_path;
    struct source source;
    struct source rendered;
    struct view_styles rendered_styles;
    struct view_links rendered_links;
    enum view_format rendered_format;
    bool has_rendered;
    int rendered_width;
    bool render_mode;
    bool force_render;
    bool render_inited;
    int fd;
    bool opened;
    bool own_fd;
    struct position top;
    struct position attn;
    int h_offset;
    struct position *row_index;
    size_t row_count;
    size_t row_cap;
    bool row_index_valid;
    int row_index_width;
    int row_index_h_offset;
    bool row_index_chop;
    bool row_index_render;
};

struct search {
    regex_t regex;
    bool valid;
    bool highlight;
    int direction;
    int original_direction;
    off_t hit;
    off_t anchor;
    bool first_line;
    char *line;
    size_t line_cap;
};

struct pager {
    struct source *source;
    struct file_entry *files;
    size_t file_count;
    size_t file_cap;
    size_t cli_file_count;
    size_t file_index;
    tam_tty_t tty;
    tam_key_parser_t keys;
    struct frame current;
    struct frame next;
    struct position top;
    struct position attn;
    struct position cursor;
    ssize_t focused_link;
    int help_offset;
    struct nav_entry *history;
    size_t history_count;
    size_t history_cap;
    int height;
    int width;
    int h_offset;
    bool chop;
    bool following;
    bool hold_eof;
    struct view_options opts;
    off_t follow_size;
    enum job job;
    enum prompt prompt;
    uint64_t count;
    uint64_t remaining;
    struct search search;
    struct mark marks[MARK_COUNT];
    char query[SEARCH_SIZE + 1];
    size_t query_length;
    const char *label;
    const char *message;
    char message_buf[SEARCH_SIZE + 80];
    bool waiting;
    bool dirty;
    bool scroll;
    bool quit;
    int result;
};

// Only the signal handler writes these flags and the wakeup byte.
static volatile sig_atomic_t wake_fd = -1;
static volatile sig_atomic_t interrupted;
static volatile sig_atomic_t resized;
static volatile sig_atomic_t suspended;

static void on_signal(int sig)
{
    int error = errno;
    if (sig == SIGWINCH)
        resized = 1;
    else if (sig == SIGTSTP)
        suspended = 1;
    else
        interrupted = sig;
    if (wake_fd >= 0) {
        unsigned char byte = 0;
        (void)write((int)wake_fd, &byte, 1);
    }
    errno = error;
}

static int signal_pipe(int fd[2])
{
    if (pipe(fd) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        if (fcntl(fd[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0)
            return -1;
    }
    wake_fd = fd[1];
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGWINCH, SIGTSTP, SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) < 0)
            return -1;
    }
    return 0;
}

static int output(struct pager *p, const char *text)
{
    return tam_tty_write(&p->tty, text, strlen(text));
}

static int cursor_move(struct pager *p, int n, char direction)
{
    if (!n)
        return 0;
    char sequence[32];
    snprintf(sequence, sizeof(sequence), "\033[%d%c", n, direction);
    return output(p, sequence);
}

static off_t line_start(struct source *s, off_t p)
{
    while (p > 0) {
        int b = source_byte(s, p - 1);
        if (b == '\n')
            break;
        if (b < 0)
            return b == VIEW_ERROR ? -1 : p;
        p--;
    }
    return p;
}

// Walk only the physical lines needed for this movement. For a wrapped
// line, a ring keeps the last requested row starts in bounded memory.
static int back(struct pager *p, struct position end, int count,
                struct position *out)
{
    struct position starts[MAX_ROWS];
    while ((end.byte || end.tab_left) && count > 0) {
        off_t offset = line_start(p->source, end.byte);
        if (offset < 0)
            return -1;
        if (offset == end.byte && end.tab_left == 0 && offset > 0) {
            offset = line_start(p->source, offset - 1);
            if (offset < 0)
                return -1;
        }

        struct position row = {.byte = offset};
        int next_slot = 0;
        int saved = 0;
        while (position_compare(row, end) < 0) {
            struct position next;
            int result = view_row_h(p->source, row, p->width,
                                    p->h_offset, p->chop, NULL, &next);
            if (result == VIEW_ERROR)
                return -1;
            if (result != VIEW_ROW)
                break;
            starts[next_slot] = row;
            next_slot = (next_slot + 1) % count;
            if (saved < count)
                saved++;
            row = next;
        }
        if (saved == count) {
            *out = starts[next_slot];
            return 0;
        }
        count -= saved;
        end = (struct position){.byte = offset};
    }
    *out = end;
    return 0;
}

static int align(struct pager *p, struct position target, struct position *out)
{
    off_t offset = line_start(p->source, target.byte);
    if (offset < 0)
        return -1;
    struct position row = {.byte = offset};
    if (p->h_offset > 0 || p->chop) {
        *out = row;
        return 0;
    }
    while (position_compare(row, target) < 0) {
        struct position next;
        int result = view_row_h(p->source, row, p->width,
                                p->h_offset, p->chop, NULL, &next);
        if (result == VIEW_ERROR)
            return -1;
        if (result != VIEW_ROW || position_compare(next, target) > 0)
            break;
        row = next;
    }
    *out = row;
    return 0;
}

static int size_view(struct pager *p)
{
    int rows, cols;
    tam_tty_size(&p->tty, &rows, &cols);
    int height = rows > 1 ? rows - 1 : 1;
    p->height = height < MAX_ROWS ? height : MAX_ROWS;
    p->width = cols < MAX_COLUMNS ? cols : MAX_COLUMNS;
    size_t stride = view_row_styled_capacity(p->width);
    char *text = realloc(p->next.text, (size_t)p->height * stride);
    if (!text)
        return -1;
    p->next.text = text;
    p->next.stride = stride;
    return 0;
}

static int read_logical_line(struct pager *p, off_t start,
                             size_t *out_len, off_t *next_line);

static void collect_line_matches(struct pager *p, off_t ls,
                                 struct view_styles *out)
{
    out->count = 0;
    size_t len = 0;
    off_t next_line = 0;
    if (read_logical_line(p, ls, &len, &next_line) != VIEW_ROW)
        return;
    const char *line = p->search.line;
    size_t pos = 0;
    while (pos <= len) {
        regmatch_t m;
        int flags = pos > 0 ? REG_NOTBOL : 0;
        if (regexec(&p->search.regex, line + pos, 1, &m, flags) != 0)
            break;
        size_t ms = pos + (size_t)m.rm_so;
        size_t me = pos + (size_t)m.rm_eo;
        if (me > ms) {
            off_t abs_s = ls + (off_t)ms;
            off_t abs_e = ls + (off_t)me;
            unsigned char st = (p->search.hit >= 0 && abs_s == p->search.hit)
                               ? VIEW_STYLE_SEARCH_CUR : VIEW_STYLE_SEARCH;
            if (!view_styles_push(out, abs_s, abs_e, st))
                break;
            pos = me;
        } else {
            pos = ms + 1;
        }
    }
}

static bool has_attention_line(const struct pager *p)
{
    const struct file_entry *cur = &p->files[p->file_index];
    return cur->render_mode && cur->has_rendered &&
           (cur->rendered_format == VIEW_FORMAT_MARKDOWN ||
            cur->rendered_format == VIEW_FORMAT_HTML);
}

static const struct view_links *active_links(const struct pager *p)
{
    const struct file_entry *cur = &p->files[p->file_index];
    return (cur->render_mode && cur->has_rendered &&
            cur->rendered_links.count > 0)
           ? &cur->rendered_links : NULL;
}

static bool active_osc8(const struct pager *p)
{
    if (p->opts.hyperlinks_mode == VIEW_HYPERLINKS_ALWAYS)
        return true;
    if (p->opts.hyperlinks_mode == VIEW_HYPERLINKS_NEVER)
        return false;
    return p->opts.hyperlinks && p->opts.color;
}

static void invalidate_row_index(struct file_entry *f)
{
    f->row_index_valid = false;
}

static void ensure_row_index(struct pager *p)
{
    struct file_entry *cur = &p->files[p->file_index];
    if (cur->row_index_valid &&
        cur->row_index_width == p->width &&
        cur->row_index_h_offset == p->h_offset &&
        cur->row_index_chop == p->chop &&
        cur->row_index_render == cur->render_mode) {
        return;
    }
    cur->row_index_valid = false;
    if (!p->source->eof)
        return;
    off_t sz = source_size(p->source);
    if (sz < 0 || sz > ROW_INDEX_MAX_BYTES)
        return;

    cur->row_count = 0;
    struct position pos = {0};
    for (;;) {
        struct position next;
        int r = view_row_h(p->source, pos, p->width, p->h_offset, p->chop,
                           NULL, &next);
        if (r != VIEW_ROW)
            break;
        if (cur->row_count == cur->row_cap) {
            size_t ncap = cur->row_cap ? cur->row_cap * 2 : 128;
            struct position *grown = realloc(cur->row_index, ncap * sizeof(*grown));
            if (!grown)
                return;
            cur->row_index = grown;
            cur->row_cap = ncap;
        }
        cur->row_index[cur->row_count++] = pos;
        pos = next;
    }
    cur->row_index_width = p->width;
    cur->row_index_h_offset = p->h_offset;
    cur->row_index_chop = p->chop;
    cur->row_index_render = cur->render_mode;
    cur->row_index_valid = true;
}

static size_t row_index_lookup(const struct file_entry *cur, struct position pos)
{
    if (!cur->row_index_valid || cur->row_count == 0)
        return 0;
    size_t lo = 0, hi = cur->row_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (position_compare(cur->row_index[mid], pos) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < cur->row_count)
        return lo + 1;
    return cur->row_count;
}

static bool row_byte_range(struct pager *p, struct position row,
                           off_t *start_out, off_t *end_out)
{
    struct position next;
    if (view_row_h(p->source, row, p->width, p->h_offset, p->chop,
                   NULL, &next) != VIEW_ROW)
        return false;
    *start_out = row.byte;
    *end_out = next.byte;
    return true;
}

static ssize_t find_link_on_row(struct pager *p, struct position row, bool last)
{
    const struct view_links *lk = active_links(p);
    if (!lk || lk->span_count == 0)
        return -1;
    off_t rs = 0, re = 0;
    if (!row_byte_range(p, row, &rs, &re) || re <= rs)
        return -1;

    size_t lo = 0, hi = lk->span_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (lk->spans[mid].end <= rs)
            lo = mid + 1;
        else
            hi = mid;
    }
    ssize_t found = -1;
    for (size_t i = lo; i < lk->span_count && lk->spans[i].start < re; i++) {
        found = (ssize_t)lk->spans[i].link_index;
        if (!last)
            return found;
    }
    return found;
}

static bool step_link_on_row(struct pager *p, int dir)
{
    const struct view_links *lk = active_links(p);
    if (!lk || lk->span_count == 0)
        return false;
    off_t rs = 0, re = 0;
    if (!row_byte_range(p, p->attn, &rs, &re) || re <= rs)
        return false;

    ssize_t first = -1, last = -1, prev = -1, next = -1;
    bool on_row = false;
    size_t lo = 0, hi = lk->span_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (lk->spans[mid].end <= rs)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < lk->span_count && lk->spans[i].start < re; i++) {
        ssize_t idx = (ssize_t)lk->spans[i].link_index;
        if (first < 0)
            first = idx;
        last = idx;
        if (idx < p->focused_link)
            prev = idx;
        else if (idx == p->focused_link)
            on_row = true;
        else if (next < 0)
            next = idx;
    }
    if (first < 0)
        return false;
    if (!on_row) {
        p->focused_link = dir > 0 ? first : last;
        return true;
    }
    if (dir > 0) {
        if (next < 0)
            return false;
        p->focused_link = next;
        return true;
    }
    if (prev < 0)
        return false;
    p->focused_link = prev;
    return true;
}

static void catch_link_on_attn(struct pager *p, int dir)
{
    if (!has_attention_line(p)) {
        p->focused_link = -1;
        return;
    }
    p->focused_link = find_link_on_row(p, p->attn, dir < 0);
}

static const char *const HELP_LINES[] = {
    "tam-view keybindings guide (press g, h, or Esc to close)",
    "",
    "  Down / j               Next line (or attention line / link in Markup)",
    "  Up / k                 Previous line (or attention line / link in Markup)",
    "  Space / Page Down      Next page",
    "  b / Page Up            Previous page",
    "  d / u                  Half page down / up",
    "  Right / Enter / o      Follow highlighted link (Right scrolls if no link)",
    "  Left / Backspace       Return in link history (Left scrolls if shifted)",
    "  Tab / Shift-Tab        Jump to next / previous link in document",
    "  Shift-Left / -Right    Scroll horizontally",
    "  S                      Toggle chop long lines",
    "  Home / < / 1g          Start of file",
    "  G / End                End of file",
    "  Ng                     Go to file line N (e.g. 42g)",
    "  m<a-z> / '<a-z> / ''   Set mark / go to mark / previous position",
    "  /regex / ?regex        Find POSIX regex forward / backward",
    "  n / N / Esc            Repeat / reverse / clear highlight",
    "  :n / :p / :x / :f      Next / prev / first / current file",
    "  R / C                  Toggle rendered view / color",
    "  F                      Follow growing file or pipe (Ctrl-C stops)",
    "  g / h                  Toggle this keybindings guide",
    "  q                      Quit and keep the page"
};
#define HELP_LINE_COUNT (sizeof(HELP_LINES) / sizeof(HELP_LINES[0]))

static int build_page(struct pager *p, struct position *end)
{
    struct frame *f = &p->next;
    f->count = 0;
    f->document = p->prompt != HELP;
    f->valid = true;
    *end = p->top;
    if (!f->document) {
        int max_off = (int)HELP_LINE_COUNT > p->height
                      ? (int)HELP_LINE_COUNT - p->height : 0;
        if (p->help_offset < 0)
            p->help_offset = 0;
        if (p->help_offset > max_off)
            p->help_offset = max_off;
        for (size_t i = (size_t)p->help_offset;
             i < HELP_LINE_COUNT && f->count < p->height; i++, f->count++) {
            view_label(f->text + f->count * f->stride, f->stride,
                       p->width, HELP_LINES[i]);
        }
        return VIEW_ROW;
    }

    struct file_entry *cur = &p->files[p->file_index];
    const struct view_styles *doc_st =
        (p->opts.color && cur->render_mode && cur->has_rendered)
        ? &cur->rendered_styles : NULL;
    const struct view_links *lk = active_links(p);
    bool attn_mode = has_attention_line(p);
    bool want_osc8 = lk && active_osc8(p);
    bool want_search = p->opts.color && p->search.valid && p->search.highlight;
    struct view_styles line_matches = {0};
    off_t cached_ls = -1;
    int rc = VIEW_ROW;

    for (; f->count < p->height; f->count++) {
        struct position row = *end;
        f->rows[f->count] = row;
        if (want_search) {
            off_t ls = line_start(p->source, row.byte);
            if (ls >= 0 && ls != cached_ls) {
                collect_line_matches(p, ls, &line_matches);
                cached_ls = ls;
            }
        }
        bool is_attn = attn_mode && p->opts.color && position_equal(row, p->attn);
        struct view_row_style rs = {
            .doc_spans = doc_st ? doc_st->spans : NULL,
            .doc_count = doc_st ? doc_st->count : 0,
            .search_spans = want_search ? line_matches.spans : NULL,
            .search_count = want_search ? line_matches.count : 0,
            .links = lk,
            .focused_link = (p->opts.color && attn_mode) ? p->focused_link : -1,
            .attention = is_attn,
            .linux_vt = p->opts.linux_vt,
            .osc8 = want_osc8
        };
        const struct view_row_style *rsp =
            (rs.doc_count > 0 || rs.search_count > 0 || rs.attention ||
             (rs.links && (rs.focused_link >= 0 || rs.osc8))) ? &rs : NULL;
        int result = view_row_styled(p->source, row, p->width,
                                     p->h_offset, p->chop, rsp,
                                     f->text + f->count * f->stride, end);
        if (result != VIEW_ROW) {
            rc = result;
            goto done;
        }
    }
    if (end->tab_left) {
        rc = VIEW_ROW;
        goto done;
    }
    int b = source_byte(p->source, end->byte);
    rc = b < 0 ? b : VIEW_ROW;

done:
    view_styles_free(&line_matches);
    return rc;
}

static void status(struct pager *p, int page_state, char *out, size_t cap)
{
    const char *message = p->message;
    char text[SEARCH_SIZE + 80];
    if (p->prompt == HELP) {
        message = "Keybindings guide (g to close, q to quit)";
    } else if (p->prompt == MARK_SET) {
        message = "mark: ";
    } else if (p->prompt == MARK_GOTO) {
        message = "goto mark: ";
    } else if (p->prompt == COLON) {
        message = ":";
    } else if (p->prompt == FIND_FORWARD || p->prompt == FIND_BACKWARD) {
        snprintf(text, sizeof(text), "%c%s",
                 p->prompt == FIND_FORWARD ? '/' : '?', p->query);
        message = text;
    } else if (p->job == SEARCH) {
        message = "Searching... (Ctrl-C cancels)";
    } else if (!message && p->following) {
        message = "Waiting for data... (Ctrl-C to stop, q to quit)";
    } else if (!message && (page_state == VIEW_WAIT || p->job != IDLE)) {
        message = "Waiting for input... (q quits)";
    } else if (!message && p->count) {
        snprintf(text, sizeof(text), "%ju", (uintmax_t)p->count);
        message = text;
    } else if (!message) {
        struct file_entry *cur = &p->files[p->file_index];
        ensure_row_index(p);

        char label_buf[SEARCH_SIZE + 64];
        if (p->cli_file_count > 1 && p->file_index < p->cli_file_count) {
            snprintf(label_buf, sizeof(label_buf), "(file %zu of %zu) %s",
                     p->file_index + 1, p->cli_file_count, p->label);
        } else {
            snprintf(label_buf, sizeof(label_buf), "%s", p->label);
        }

        size_t top_line = 0, bot_line = 0, attn_line = 0, total_lines = 0;
        bool total_known = cur->row_index_valid;
        if (total_known) {
            total_lines = cur->row_count;
            if (p->next.count > 0 && total_lines > 0) {
                top_line = row_index_lookup(cur, p->next.rows[0]);
                bot_line = top_line + (size_t)p->next.count - 1;
                if (bot_line > total_lines)
                    bot_line = total_lines;
                attn_line = row_index_lookup(cur, p->attn);
                if (attn_line == 0)
                    attn_line = top_line;
            }
        } else if (p->next.count > 0) {
            top_line = 1;
            bot_line = (size_t)p->next.count;
            attn_line = 1;
            for (int i = 0; i < p->next.count; i++) {
                if (position_equal(p->next.rows[i], p->attn)) {
                    attn_line = (size_t)i + 1;
                    break;
                }
            }
        }

        const struct view_links *lk = active_links(p);
        const char *hint = (has_attention_line(p) && lk &&
                            p->focused_link >= 0 &&
                            (size_t)p->focused_link < lk->count)
                           ? lk->items[p->focused_link].url : NULL;

        tam_pager_footer_t footer = {
            .label = label_buf,
            .has_attn = has_attention_line(p),
            .attn_line = attn_line,
            .top_line = top_line,
            .bot_line = bot_line,
            .total_lines = total_lines,
            .total_known = total_known,
            .link_hint = hint,
            .version = VIEW_VERSION
        };
        tam_pager_format_footer(out, cap, p->width, &footer);
        return;
    }
    if (p->prompt == FIND_FORWARD || p->prompt == FIND_BACKWARD) {
        // Keep the editing cursor visible on narrow terminals.
        int width = p->width > 1 ? p->width - 1 : 1;
        size_t len = strlen(message);
        const char *tail = message + (len > (size_t)width ? len - (size_t)width : 0);
        while ((*tail & 0xc0) == 0x80)
            tail++;
        view_label(out, cap, width, tail);
    } else {
        view_label(out, cap, p->width, message);
    }
}

static int paint(struct pager *p)
{
    struct position end;
    int state = build_page(p, &end);
    if (state == VIEW_ERROR)
        return -1;
    bool at_last_file = p->file_index >= p->cli_file_count ||
                        p->file_index + 1 >= p->cli_file_count;
    bool eof_doc = state == VIEW_EOF && p->next.document;
    bool final = p->opts.quit_at_eof && eof_doc && !p->following &&
                 !p->hold_eof && at_last_file;
    if (eof_doc && !p->following && !final && !p->message &&
        !has_attention_line(p)) {
        p->message = at_last_file ? "End of file (q quit)"
                                  : "End of file (:n next, q quit)";
    }
    if (eof_doc && p->next.count < p->height && (p->top.byte || p->top.tab_left)) {
        if (p->next.count == 0 && p->top.byte > 0) {
            off_t size = source_size(p->source);
            if (size < 0)
                return -1;
            if (end.byte > size)
                end = (struct position){.byte = size};
        }
        if (back(p, end, p->height, &p->top) < 0 ||
            build_page(p, &end) == VIEW_ERROR)
            return -1;
    }

    if (has_attention_line(p) && p->next.document && p->next.count > 0) {
        bool in_view = false;
        for (int i = 0; i < p->next.count; i++) {
            if (position_equal(p->next.rows[i], p->attn)) {
                in_view = true;
                break;
            }
        }
        if (!in_view) {
            if (position_compare(p->attn, p->next.rows[0]) < 0)
                p->attn = p->next.rows[0];
            else
                p->attn = p->next.rows[p->next.count - 1];
            catch_link_on_attn(p, 1);
            if (build_page(p, &end) == VIEW_ERROR)
                return -1;
        }
    }

    struct frame *old = &p->current;
    struct frame *f = &p->next;
    int first = 0;
    bool scrolling = old->valid && old->document && f->document &&
                     old->count && f->count &&
                     (position_equal(f->rows[0], old->rows[0]) ||
                      (p->scroll && position_compare(f->rows[0], old->rows[0]) > 0));
    if (scrolling) {
        for (int i = 0; i < old->count; i++) {
            if (position_equal(old->rows[i], f->rows[0])) {
                first = old->count - i;
                break;
            }
        }
        if (first > f->count)
            scrolling = false;
    }
    if (!scrolling) {
        first = 0;
        if (old->valid && cursor_move(p, old->count, 'A') < 0)
            return -1;
    }

    for (int i = first; i < f->count; i++) {
        if (output(p, "\r\033[K") < 0 ||
            output(p, f->text + i * f->stride) < 0 ||
            output(p, "\r\n") < 0)
            return -1;
    }

    if (old->valid || f->count || !final) {
        if (output(p, "\r\033[K") < 0)
            return -1;
    }
    if (!scrolling && old->valid) {
        int extra = old->count - f->count;
        for (int i = 0; i < extra; i++) {
            if (output(p, "\033[B\r\033[K") < 0)
                return -1;
        }
        if (extra > 0 && cursor_move(p, extra, 'A') < 0)
            return -1;
    }

    if (!final && !p->quit) {
        char line[SEARCH_SIZE + 80];
        status(p, state, line, sizeof(line));
        if (output(p, line) < 0)
            return -1;
    }
    memcpy(old->rows, f->rows, (size_t)f->count * sizeof(f->rows[0]));
    old->count = f->count;
    old->valid = true;
    old->document = f->document;

    p->dirty = false;
    p->scroll = false;
    p->waiting = state == VIEW_WAIT;
    if (final) {
        old->valid = false; // The prompt line is already clear.
        p->quit = true;
    }
    return 0;
}

static void save_jump_mark(struct pager *p)
{
    p->marks[MARK_PREV] = (struct mark){
        .set = true,
        .file = p->file_index,
        .pos = p->top,
        .h_offset = p->h_offset
    };
}

static int mark_index(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= 'A' && c <= 'Z')
        return 26 + (c - 'A');
    if (c == '\'')
        return MARK_PREV;
    return -1;
}

static struct source *active_source(struct file_entry *f)
{
    return (f->render_mode && f->has_rendered) ? &f->rendered : &f->source;
}

static void ensure_rendered(struct pager *p, struct file_entry *f)
{
    if (!f->render_inited) {
        f->render_inited = true;
        if (p->opts.mode == VIEW_MODE_RAW) {
            f->render_mode = false;
            f->force_render = false;
        } else if (p->opts.mode == VIEW_MODE_RENDER) {
            f->render_mode = true;
            f->force_render = true;
        } else {
            enum view_format fmt = view_detect_format(f->path, &f->source, false);
            f->render_mode = (fmt != VIEW_FORMAT_RAW);
            f->force_render = false;
        }
    }
    if (!f->render_mode)
        return;
    if (f->has_rendered && f->rendered_width == p->width)
        return;
    if (f->has_rendered) {
        source_close(&f->rendered);
        view_styles_free(&f->rendered_styles);
        view_links_free(&f->rendered_links);
        f->has_rendered = false;
        f->rendered_format = VIEW_FORMAT_RAW;
    }
    if (view_render_source(&f->source, f->path, p->width, p->opts.ascii,
                           f->force_render, &f->rendered,
                           &f->rendered_styles, &f->rendered_links,
                           &f->rendered_format) == 0) {
        f->has_rendered = true;
        f->rendered_width = p->width;
        invalidate_row_index(f);
    } else {
        f->render_mode = false;
        snprintf(p->message_buf, sizeof(p->message_buf),
                 "Render failed: %s", strerror(errno));
        p->message = p->message_buf;
    }
}

static int switch_file(struct pager *p, size_t target)
{
    if (target >= p->file_count) {
        p->message = "No such file";
        return -1;
    }
    if (target == p->file_index)
        return 0;

    struct file_entry *cur = &p->files[p->file_index];
    cur->top = p->top;
    cur->attn = p->attn;
    cur->h_offset = p->h_offset;

    struct file_entry *dst = &p->files[target];
    if (!dst->opened) {
        if (!strcmp(dst->path, "-")) {
            p->message = "Cannot reopen standard input";
            return -1;
        }
        int fd = open(dst->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            snprintf(p->message_buf, sizeof(p->message_buf), "%s: %s",
                     dst->path, strerror(errno));
            p->message = p->message_buf;
            return -1;
        }
        struct stat st;
        if (fstat(fd, &st) < 0) {
            int err = errno;
            close(fd);
            snprintf(p->message_buf, sizeof(p->message_buf), "%s: %s",
                     dst->path, strerror(err));
            p->message = p->message_buf;
            return -1;
        }
        if (!S_ISREG(st.st_mode)) {
            close(fd);
            int err = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
            snprintf(p->message_buf, sizeof(p->message_buf), "%s: %s",
                     dst->path, strerror(err));
            p->message = p->message_buf;
            return -1;
        }
        if (source_open(&dst->source, fd) < 0) {
            int err = errno;
            close(fd);
            snprintf(p->message_buf, sizeof(p->message_buf), "%s: %s",
                     dst->path, strerror(err));
            p->message = p->message_buf;
            return -1;
        }
        dst->fd = fd;
        dst->own_fd = true;
        dst->opened = true;
    }

    ensure_rendered(p, dst);
    p->file_index = target;
    p->source = active_source(dst);
    p->top = dst->top;
    p->attn = dst->attn;
    p->h_offset = dst->h_offset;
    p->label = dst->path;
    p->message = NULL;
    p->search.hit = -1;
    p->focused_link = -1;
    p->following = false;
    p->hold_eof = false;
    p->job = IDLE;
    p->current.valid = false;
    p->dirty = true;
    if (align(p, p->top, &p->top) < 0)
        return -1;
    if (align(p, p->attn, &p->attn) < 0)
        return -1;
    catch_link_on_attn(p, 1);
    return 0;
}

static void push_history(struct pager *p)
{
    struct file_entry *cur = &p->files[p->file_index];
    if (p->history_count >= HISTORY_MAX) {
        memmove(&p->history[0], &p->history[1],
                (p->history_count - 1) * sizeof(p->history[0]));
        p->history_count--;
    } else if (p->history_count == p->history_cap) {
        size_t ncap = p->history_cap ? p->history_cap * 2 : 16;
        struct nav_entry *grown = realloc(p->history, ncap * sizeof(*grown));
        if (!grown)
            return;
        p->history = grown;
        p->history_cap = ncap;
    }
    p->history[p->history_count++] = (struct nav_entry){
        .file_index = p->file_index,
        .top = p->top,
        .attn = p->attn,
        .h_offset = p->h_offset,
        .render_mode = cur->render_mode
    };
}

static int jump_to_anchor(struct pager *p, const char *frag, bool record_history)
{
    struct file_entry *cur = &p->files[p->file_index];
    if (!cur->render_mode || !cur->has_rendered) {
        p->message = "Anchors require rendered view";
        return 0;
    }
    char decoded[256];
    tam_uri_percent_decode(frag, strlen(frag), decoded, sizeof(decoded));
    const struct view_links *lk = &cur->rendered_links;
    for (size_t i = 0; i < lk->anchor_count; i++) {
        if (strcasecmp(lk->anchors[i].id, decoded) == 0) {
            if (record_history)
                push_history(p);
            save_jump_mark(p);
            struct position target = {.byte = lk->anchors[i].byte};
            if (align(p, target, &p->top) < 0)
                return -1;
            p->attn = p->top;
            p->h_offset = 0;
            p->search.hit = -1;
            p->current.valid = false;
            catch_link_on_attn(p, 1);
            return 0;
        }
    }
    snprintf(p->message_buf, sizeof(p->message_buf),
             "Anchor not found: #%s", decoded);
    p->message = p->message_buf;
    return 0;
}

static ssize_t find_or_add_file(struct pager *p, const char *path)
{
    for (size_t i = 0; i < p->file_count; i++) {
        if (strcmp(p->files[i].path, path) == 0)
            return (ssize_t)i;
    }
    if (p->file_count == p->file_cap) {
        size_t ncap = p->file_cap ? p->file_cap * 2 : 8;
        struct file_entry *grown = realloc(p->files, ncap * sizeof(*grown));
        if (!grown) {
            p->message = "Out of memory";
            return -1;
        }
        p->files = grown;
        p->file_cap = ncap;
        p->source = active_source(&p->files[p->file_index]);
    }
    char *copy = strdup(path);
    if (!copy) {
        p->message = "Out of memory";
        return -1;
    }
    size_t idx = p->file_count++;
    p->files[idx] = (struct file_entry){
        .path = copy,
        .own_path = true,
        .fd = -1
    };
    return (ssize_t)idx;
}

static int follow_focused_link(struct pager *p)
{
    const struct view_links *lk = active_links(p);
    if (!lk || p->focused_link < 0 || (size_t)p->focused_link >= lk->count) {
        p->message = "No link highlighted";
        return 0;
    }
    const char *url = lk->items[p->focused_link].url;
    tam_uri_kind_t kind = tam_uri_classify(url);
    if (kind == TAM_URI_INVALID) {
        p->message = "Unsupported link scheme";
        return 0;
    }
    if (kind == TAM_URI_EXTERNAL) {
        if (tam_uri_open_external(url) < 0) {
            p->message = "Could not open external link";
            return 0;
        }
        char safe[SEARCH_SIZE];
        view_label(safe, sizeof(safe), p->width > 10 ? p->width - 10 : 40, url);
        snprintf(p->message_buf, sizeof(p->message_buf), "Opened: %s", safe);
        p->message = p->message_buf;
        return 0;
    }
    if (kind == TAM_URI_ANCHOR)
        return jump_to_anchor(p, url + 1, true);

    char target_path[4096];
    char frag[256];
    if (tam_uri_resolve_local(p->files[p->file_index].path, url,
                              target_path, sizeof(target_path),
                              frag, sizeof(frag)) < 0) {
        p->message = "Invalid link path";
        return 0;
    }
    if (target_path[0] == '\0')
        return frag[0] ? jump_to_anchor(p, frag, true) : 0;

    size_t prev_count = p->file_count;
    ssize_t target_idx = find_or_add_file(p, target_path);
    if (target_idx < 0)
        return 0;

    push_history(p);
    if (switch_file(p, (size_t)target_idx) < 0) {
        if (p->history_count > 0)
            p->history_count--;
        if ((size_t)target_idx == prev_count && p->file_count > prev_count) {
            if (p->files[target_idx].own_path)
                free((void *)p->files[target_idx].path);
            p->file_count = prev_count;
        }
        return 0;
    }
    if (frag[0])
        return jump_to_anchor(p, frag, false);
    return 0;
}

static int go_back(struct pager *p)
{
    if (p->history_count == 0) {
        p->message = "No link history";
        return 0;
    }
    struct nav_entry e = p->history[--p->history_count];
    if (e.file_index != p->file_index && switch_file(p, e.file_index) < 0)
        return 0;
    struct file_entry *cur = &p->files[p->file_index];
    if (cur->render_mode != e.render_mode) {
        cur->render_mode = e.render_mode;
        if (cur->render_mode)
            ensure_rendered(p, cur);
        p->source = active_source(cur);
        invalidate_row_index(cur);
    }
    p->top = e.top;
    p->attn = e.attn;
    p->h_offset = e.h_offset;
    p->search.hit = -1;
    p->current.valid = false;
    if (align(p, p->top, &p->top) < 0 || align(p, p->attn, &p->attn) < 0)
        return -1;
    catch_link_on_attn(p, 1);
    return 0;
}

static int move_attn_down(struct pager *p)
{
    if (step_link_on_row(p, 1)) {
        p->current.valid = false;
        return 0;
    }
    struct position next;
    int r = view_row_h(p->source, p->attn, p->width, p->h_offset, p->chop,
                       NULL, &next);
    if (r != VIEW_ROW)
        return r == VIEW_ERROR ? -1 : 0;
    if (next.tab_left == 0) {
        int b = source_byte(p->source, next.byte);
        if (b == VIEW_EOF)
            return 0;
        if (b < 0)
            return b == VIEW_ERROR ? -1 : 0;
    }
    p->attn = next;
    bool in_view = false;
    if (p->current.valid && p->current.document) {
        for (int i = 0; i < p->current.count; i++) {
            if (position_equal(p->current.rows[i], p->attn)) {
                in_view = true;
                break;
            }
        }
    }
    if (!in_view) {
        struct position new_top;
        if (view_row_h(p->source, p->top, p->width, p->h_offset, p->chop,
                       NULL, &new_top) == VIEW_ROW) {
            p->top = new_top;
        }
    }
    catch_link_on_attn(p, 1);
    p->current.valid = false;
    return 0;
}

static int move_attn_up(struct pager *p)
{
    if (step_link_on_row(p, -1)) {
        p->current.valid = false;
        return 0;
    }
    if (p->attn.byte == 0 && p->attn.tab_left == 0)
        return 0;
    struct position prev;
    if (back(p, p->attn, 1, &prev) < 0)
        return -1;
    p->attn = prev;
    if (position_compare(p->attn, p->top) < 0)
        p->top = p->attn;
    catch_link_on_attn(p, -1);
    p->current.valid = false;
    return 0;
}

static int cycle_link(struct pager *p, int dir)
{
    const struct view_links *lk = active_links(p);
    if (!lk || lk->count == 0) {
        p->message = "No links in document";
        return 0;
    }
    ssize_t target = -1;
    if (p->focused_link < 0) {
        if (dir > 0) {
            for (size_t i = 0; i < lk->count; i++) {
                if (lk->items[i].first_byte >= p->attn.byte) {
                    target = (ssize_t)i;
                    break;
                }
            }
            if (target < 0)
                target = 0;
        } else {
            for (size_t i = lk->count; i > 0; i--) {
                if (lk->items[i - 1].first_byte <= p->attn.byte) {
                    target = (ssize_t)(i - 1);
                    break;
                }
            }
            if (target < 0)
                target = (ssize_t)(lk->count - 1);
        }
    } else if (dir > 0) {
        target = (p->focused_link + 1) % (ssize_t)lk->count;
    } else {
        target = (p->focused_link + (ssize_t)lk->count - 1) % (ssize_t)lk->count;
    }

    p->focused_link = target;
    struct position lpos = {.byte = lk->items[target].first_byte};
    if (align(p, lpos, &p->attn) < 0)
        return -1;
    bool in_view = false;
    if (p->current.valid && p->current.document) {
        for (int i = 0; i < p->current.count; i++) {
            if (position_equal(p->current.rows[i], p->attn)) {
                in_view = true;
                break;
            }
        }
    }
    if (!in_view)
        p->top = p->attn;
    p->current.valid = false;
    return 0;
}

static bool compile_regex(struct pager *p, const char *pattern)
{
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NEWLINE);
    if (rc != 0) {
        char err[256];
        regerror(rc, &re, err, sizeof(err));
        snprintf(p->message_buf, sizeof(p->message_buf),
                 "Invalid pattern: %s", err);
        p->message = p->message_buf;
        return false;
    }
    if (p->search.valid)
        regfree(&p->search.regex);
    p->search.regex = re;
    p->search.valid = true;
    return true;
}

static void start_search(struct pager *p, int direction, bool repeat)
{
    struct search *s = &p->search;
    p->message = NULL;
    if (!s->valid) {
        p->message = "No previous search";
        return;
    }
    s->highlight = true;
    p->current.valid = false;
    s->direction = direction;

    off_t anchor = repeat && s->hit >= 0 ? s->hit : p->top.byte;
    if (repeat && s->hit >= 0 && direction > 0)
        anchor++;
    s->anchor = anchor;
    s->first_line = true;
    off_t start = line_start(p->source, anchor);
    if (start < 0)
        start = 0;
    p->cursor = (struct position){.byte = start};
    p->job = SEARCH;
}

static int read_logical_line(struct pager *p, off_t start,
                             size_t *out_len, off_t *next_line)
{
    struct search *find = &p->search;
    size_t len = 0;
    off_t pos = start;
    for (;;) {
        int b = source_byte(p->source, pos);
        if (b == VIEW_WAIT || b == VIEW_ERROR)
            return b;
        if (b == VIEW_EOF) {
            if (pos == start)
                return VIEW_EOF;
            *next_line = pos;
            break;
        }
        pos++;
        if (b == '\n') {
            if (len > 0 && find->line[len - 1] == '\r')
                len--;
            *next_line = pos;
            break;
        }
        if (len + 1 >= find->line_cap) {
            size_t new_cap = find->line_cap ? find->line_cap * 2 : 4096;
            if (new_cap > LINE_BUF_MAX)
                new_cap = LINE_BUF_MAX;
            if (len + 1 >= new_cap) {
                // Truncate very long binary lines for regex matching.
                continue;
            }
            char *grown = realloc(find->line, new_cap);
            if (!grown)
                return VIEW_ERROR;
            find->line = grown;
            find->line_cap = new_cap;
        }
        find->line[len++] = b == 0 ? 1 : (char)b;
    }
    if (!find->line) {
        find->line = malloc(1);
        if (!find->line)
            return VIEW_ERROR;
        find->line_cap = 1;
    }
    find->line[len] = '\0';
    *out_len = len;
    return VIEW_ROW;
}

static bool match_forward(regex_t *re, const char *line, size_t len,
                          size_t min_col, size_t *match_col)
{
    size_t pos = 0;
    while (pos <= len) {
        regmatch_t m;
        int flags = pos > 0 ? REG_NOTBOL : 0;
        if (regexec(re, line + pos, 1, &m, flags) != 0)
            return false;
        size_t start = pos + (size_t)m.rm_so;
        if (start >= min_col) {
            *match_col = start;
            return true;
        }
        pos = start + 1;
    }
    return false;
}

static bool match_backward(regex_t *re, const char *line, size_t len,
                           size_t max_col, size_t *match_col)
{
    bool found = false;
    size_t pos = 0;
    while (pos <= len && pos < max_col) {
        regmatch_t m;
        int flags = pos > 0 ? REG_NOTBOL : 0;
        if (regexec(re, line + pos, 1, &m, flags) != 0)
            break;
        size_t start = pos + (size_t)m.rm_so;
        if (start >= max_col)
            break;
        *match_col = start;
        found = true;
        pos = start + 1;
    }
    return found;
}

static int step_search(struct pager *p)
{
    struct search *find = &p->search;
    for (int lines = 0; lines < 64; lines++) {
        if (find->direction < 0 && find->first_line &&
            find->anchor == p->cursor.byte) {
            if (p->cursor.byte == 0) {
                p->message = "Pattern not found";
                p->job = IDLE;
                p->dirty = true;
                return VIEW_ROW;
            }
            off_t prev = line_start(p->source, p->cursor.byte - 1);
            if (prev < 0)
                return VIEW_ERROR;
            p->cursor.byte = prev;
            find->first_line = false;
            find->anchor = -1;
        }

        size_t len = 0;
        off_t next_line = 0;
        int rc = read_logical_line(p, p->cursor.byte, &len, &next_line);
        if (rc == VIEW_WAIT || rc == VIEW_ERROR)
            return rc;
        if (rc == VIEW_EOF) {
            p->message = "Pattern not found";
            p->job = IDLE;
            p->dirty = true;
            return VIEW_ROW;
        }

        size_t match_col = 0;
        bool matched = false;
        if (find->direction > 0) {
            size_t min_col = find->first_line && find->anchor > p->cursor.byte
                             ? (size_t)(find->anchor - p->cursor.byte) : 0;
            matched = match_forward(&find->regex, find->line, len,
                                    min_col, &match_col);
        } else {
            size_t max_col = find->first_line && find->anchor >= 0
                             ? (size_t)(find->anchor - p->cursor.byte)
                             : SIZE_MAX;
            matched = match_backward(&find->regex, find->line, len,
                                     max_col, &match_col);
        }

        if (matched) {
            save_jump_mark(p);
            find->hit = p->cursor.byte + (off_t)match_col;
            struct position hit = {.byte = find->hit};
            if (align(p, hit, &p->top) < 0)
                return VIEW_ERROR;
            p->attn = p->top;
            catch_link_on_attn(p, 1);
            p->job = IDLE;
            p->dirty = true;
            return VIEW_ROW;
        }

        find->first_line = false;
        if (find->direction > 0) {
            p->cursor.byte = next_line;
        } else if (p->cursor.byte == 0) {
            p->message = "Pattern not found";
            p->job = IDLE;
            p->dirty = true;
            return VIEW_ROW;
        } else {
            off_t prev = line_start(p->source, p->cursor.byte - 1);
            if (prev < 0)
                return VIEW_ERROR;
            p->cursor.byte = prev;
        }
    }
    return VIEW_ROW;
}

static int last_page(struct pager *p, struct position end)
{
    p->job = IDLE;
    p->dirty = true;
    if (back(p, end, p->height, &p->top) < 0)
        return -1;
    if (has_attention_line(p)) {
        if (back(p, end, 1, &p->attn) < 0)
            return -1;
        catch_link_on_attn(p, -1);
    }
    return 0;
}

static int step_job(struct pager *p)
{
    struct source *s = p->source;
    if (p->job == GO_END) {
        if (!s->eof)
            return VIEW_WAIT;
        off_t size = 0;
        if (source_refresh(s, &size) < 0)
            return VIEW_ERROR;
        p->follow_size = size;
        if (size || s->spool)
            return last_page(p, (struct position){.byte = size}) < 0
                       ? VIEW_ERROR : VIEW_ROW;
        // Some seekable pseudo-files report size zero despite having content.
    }

    if (p->job == ADVANCE) {
        for (; p->remaining; p->remaining--) {
            struct position next;
            int result = view_row_h(s, p->cursor, p->width,
                                    p->h_offset, p->chop, NULL, &next);
            if (result == VIEW_EOF)
                return last_page(p, p->cursor) < 0 ? VIEW_ERROR : VIEW_ROW;
            if (result != VIEW_ROW)
                return result;
            p->cursor = next;
        }
        if (p->cursor.tab_left == 0) {
            int next = source_byte(s, p->cursor.byte);
            if (next == VIEW_EOF)
                return last_page(p, p->cursor) < 0 ? VIEW_ERROR : VIEW_ROW;
            if (next < 0)
                return next;
        }
        p->top = p->cursor;
        if (has_attention_line(p)) {
            p->attn = p->top;
            catch_link_on_attn(p, 1);
            p->current.valid = false;
        }
        p->scroll = true;
        p->dirty = true;
        p->job = IDLE;
        return VIEW_ROW;
    }

    if (p->job == SEARCH)
        return step_search(p);

    for (int i = 0; i < VIEW_BLOCK; i++) {
        off_t offset = p->cursor.byte;
        int b = offset < 0 ? VIEW_EOF : source_byte(s, offset);
        if (b == VIEW_WAIT || b == VIEW_ERROR)
            return b;
        if (b == VIEW_EOF)
            return last_page(p, p->cursor) < 0 ? VIEW_ERROR : VIEW_ROW;
        p->cursor.byte++;
        if (p->job == GO_LINE && b == '\n' && --p->remaining == 0) {
            p->top = p->cursor;
            if (has_attention_line(p)) {
                p->attn = p->top;
                catch_link_on_attn(p, 1);
                p->current.valid = false;
            }
            break;
        }
        if (i == VIEW_BLOCK - 1)
            return VIEW_ROW;
    }
    p->job = IDLE;
    p->dirty = true;
    return VIEW_ROW;
}

static void edit_query(struct pager *p, tam_key_event_t key)
{
    if (key.type == TAM_KEY_ESC ||
        (key.type == TAM_KEY_CHAR && key.mods == TAM_MOD_CTRL &&
         key.codepoint == 'c')) {
        p->prompt = COMMAND;
    } else if (key.type == TAM_KEY_ENTER) {
        int direction = p->prompt == FIND_FORWARD ? 1 : -1;
        p->prompt = COMMAND;
        if (p->query_length && !compile_regex(p, p->query))
            return;
        p->search.original_direction = direction;
        p->search.hit = -1;
        start_search(p, direction, false);
    } else if (key.type == TAM_KEY_BACKSPACE && p->query_length) {
        do {
            p->query_length--;
        } while (p->query_length &&
                 (p->query[p->query_length] & 0xc0) == 0x80);
    } else if (key.type == TAM_KEY_CHAR && !key.mods && key.codepoint >= 32) {
        char bytes[4];
        size_t n = tam_utf8_encode(key.codepoint, bytes);
        if (n <= SEARCH_SIZE - p->query_length) {
            memcpy(p->query + p->query_length, bytes, n);
            p->query_length += n;
        }
    }
    p->query[p->query_length] = '\0';
}

static int handle_mark_prompt(struct pager *p, tam_key_event_t key, int c,
                              bool ctrl_c)
{
    enum prompt mode = p->prompt;
    p->prompt = COMMAND;
    if (ctrl_c || key.type == TAM_KEY_ESC)
        return 0;

    int idx = mark_index(c);
    if (mode == MARK_SET) {
        if (idx < 0 || idx == MARK_PREV) {
            p->message = "Invalid mark";
            return 0;
        }
        p->marks[idx] = (struct mark){
            .set = true,
            .file = p->file_index,
            .pos = p->top,
            .h_offset = p->h_offset
        };
        return 0;
    }

    if (idx < 0) {
        p->message = "Invalid mark";
        return 0;
    }
    struct mark target = p->marks[idx];
    if (!target.set) {
        p->message = "Mark not set";
        return 0;
    }
    save_jump_mark(p);
    if (target.file != p->file_index && switch_file(p, target.file) < 0)
        return 0;
    p->top = target.pos;
    p->h_offset = target.h_offset;
    p->current.valid = false;
    p->search.hit = -1;
    return align(p, p->top, &p->top);
}

static void handle_colon_prompt(struct pager *p, tam_key_event_t key, int c,
                                bool ctrl_c)
{
    uint64_t count = p->count;
    p->count = 0;
    p->prompt = COMMAND;
    if (ctrl_c || key.type == TAM_KEY_ESC)
        return;

    if (c == 'n') {
        size_t step = count ? (size_t)count : 1;
        if (p->file_index >= p->cli_file_count ||
            p->file_index + step >= p->cli_file_count || count > SIZE_MAX) {
            p->message = "No next file";
            return;
        }
        (void)switch_file(p, p->file_index + step);
    } else if (c == 'p') {
        size_t step = count ? (size_t)count : 1;
        if (p->file_index >= p->cli_file_count ||
            step > p->file_index || count > SIZE_MAX) {
            p->message = "No previous file";
            return;
        }
        (void)switch_file(p, p->file_index - step);
    } else if (c == 'x') {
        size_t target = count ? (size_t)(count - 1) : 0;
        if (target >= p->cli_file_count || count > SIZE_MAX) {
            p->message = "No such file";
            return;
        }
        (void)switch_file(p, target);
    } else if (c == 'f') {
        char name[SEARCH_SIZE + 1];
        view_label(name, sizeof(name), p->width, p->label);
        if (p->cli_file_count > 1 && p->file_index < p->cli_file_count) {
            snprintf(p->message_buf, sizeof(p->message_buf),
                     "file %zu of %zu: %s", p->file_index + 1,
                     p->cli_file_count, name);
        } else {
            snprintf(p->message_buf, sizeof(p->message_buf), "%s", name);
        }
        p->message = p->message_buf;
    } else if (c == 'q') {
        p->quit = true;
    } else {
        p->message = "Unknown command";
    }
}

static int handle_key(struct pager *p, tam_key_event_t key)
{
    p->dirty = true;
    int c = key.type == TAM_KEY_CHAR && !key.mods && key.codepoint < 128
            ? (int)key.codepoint : 0;
    bool ctrl_c = key.type == TAM_KEY_CHAR && key.mods == TAM_MOD_CTRL &&
                  key.codepoint == 'c';
    if (key.type == TAM_KEY_CHAR && key.mods == TAM_MOD_CTRL &&
        key.codepoint == 'z') {
        suspended = 1;
        return 0;
    }
    if (p->prompt == HELP) {
        if (c == 'q') {
            p->prompt = COMMAND;
            p->quit = true;
            return 0;
        }
        if (c == 'g' || c == 'h' || key.type == TAM_KEY_ESC || ctrl_c) {
            p->prompt = COMMAND;
            p->current.valid = false;
            return 0;
        }
        if (key.type == TAM_KEY_DOWN || c == 'j' || key.type == TAM_KEY_ENTER) {
            p->help_offset++;
            p->current.valid = false;
        } else if (key.type == TAM_KEY_UP || c == 'k') {
            if (p->help_offset > 0)
                p->help_offset--;
            p->current.valid = false;
        } else if (key.type == TAM_KEY_PAGE_DOWN || c == ' ') {
            p->help_offset += p->height;
            p->current.valid = false;
        } else if (key.type == TAM_KEY_PAGE_UP || c == 'b') {
            p->help_offset = p->help_offset > p->height ? p->help_offset - p->height : 0;
            p->current.valid = false;
        }
        return 0;
    }
    if (p->prompt == MARK_SET || p->prompt == MARK_GOTO)
        return handle_mark_prompt(p, key, c, ctrl_c);
    if (p->prompt == COLON) {
        handle_colon_prompt(p, key, c, ctrl_c);
        return 0;
    }
    if (p->prompt != COMMAND) {
        edit_query(p, key);
        return 0;
    }
    if (c == 'q') {
        p->quit = true;
        return 0;
    }
    if (ctrl_c || key.type == TAM_KEY_ESC) {
        if (p->following) {
            p->following = false;
            p->hold_eof = true;
            p->job = IDLE;
            p->message = "Follow stopped";
        } else if (p->job != IDLE) {
            p->job = IDLE;
            p->message = "Canceled";
        } else if (p->search.highlight) {
            p->search.highlight = false;
            p->search.hit = -1;
            p->current.valid = false;
            p->message = "Highlight cleared";
        }
        p->count = 0;
        return 0;
    }
    if (p->job != IDLE || p->following)
        return 0;

    p->hold_eof = false;
    p->message = NULL;
    if (c >= '0' && c <= '9') {
        if (p->count <= (UINT64_MAX - (unsigned)(c - '0')) / 10)
            p->count = p->count * 10 + (unsigned)(c - '0');
        return 0;
    }
    if (c == ':') {
        p->prompt = COLON;
        return 0;
    }

    uint64_t count = p->count;
    p->count = 0;
    int movement = 0;
    bool attn_mode = has_attention_line(p);

    if (key.type == TAM_KEY_TAB) {
        return cycle_link(p, (key.mods & TAM_MOD_SHIFT) ? -1 : 1);
    } else if (c == 'o') {
        return follow_focused_link(p);
    } else if (key.type == TAM_KEY_BACKSPACE) {
        return go_back(p);
    } else if (key.type == TAM_KEY_RIGHT && !(key.mods & TAM_MOD_SHIFT) &&
               attn_mode && p->focused_link >= 0) {
        return follow_focused_link(p);
    } else if (key.type == TAM_KEY_LEFT && !(key.mods & TAM_MOD_SHIFT) &&
               p->h_offset == 0 && (p->history_count > 0 || attn_mode)) {
        return go_back(p);
    } else if (key.type == TAM_KEY_ENTER && attn_mode && p->focused_link >= 0) {
        return follow_focused_link(p);
    } else if (key.type == TAM_KEY_DOWN || key.type == TAM_KEY_ENTER || c == 'j') {
        if (attn_mode)
            return move_attn_down(p);
        movement = 1;
    } else if (key.type == TAM_KEY_UP || c == 'k') {
        if (attn_mode)
            return move_attn_up(p);
        movement = -1;
    } else if (key.type == TAM_KEY_PAGE_DOWN || c == ' ') {
        movement = p->height;
    } else if (key.type == TAM_KEY_PAGE_UP || c == 'b') {
        movement = -p->height;
    } else if (c == 'd') {
        movement = (p->height + 1) / 2;
    } else if (c == 'u') {
        movement = -(p->height + 1) / 2;
    } else if (key.type == TAM_KEY_RIGHT || key.type == TAM_KEY_LEFT) {
        int step = count && count <= (uint64_t)INT_MAX
                   ? (int)count : (p->width > 1 ? p->width / 2 : 1);
        if (key.type == TAM_KEY_RIGHT) {
            p->h_offset = p->h_offset > INT_MAX - step
                          ? INT_MAX : p->h_offset + step;
        } else {
            p->h_offset = p->h_offset > step ? p->h_offset - step : 0;
        }
        p->current.valid = false;
        return align(p, p->top, &p->top);
    } else if (c == 'S') {
        p->chop = !p->chop;
        p->current.valid = false;
        p->message = p->chop ? "Chop long lines" : "Wrap long lines";
        return align(p, p->top, &p->top);
    } else if (c == 'R') {
        struct file_entry *cur = &p->files[p->file_index];
        cur->render_mode = !cur->render_mode;
        if (cur->render_mode) {
            cur->force_render = true;
            ensure_rendered(p, cur);
        }
        p->source = active_source(cur);
        invalidate_row_index(cur);
        p->top = (struct position){0};
        p->attn = (struct position){0};
        p->cursor = (struct position){0};
        p->h_offset = 0;
        p->search.hit = -1;
        p->focused_link = -1;
        p->current.valid = false;
        catch_link_on_attn(p, 1);
        p->message = cur->render_mode ? "Render mode" : "Raw mode";
        return 0;
    } else if (c == 'C') {
        p->opts.color = !p->opts.color;
        p->current.valid = false;
        p->message = p->opts.color ? "Color on" : "Color off";
        return 0;
    } else if (c == 'g' && count == 0) {
        p->prompt = HELP;
        p->help_offset = 0;
        p->current.valid = false;
        return 0;
    } else if ((c == 'g' && count > 0) || key.type == TAM_KEY_HOME || c == '<') {
        save_jump_mark(p);
        p->search.hit = -1;
        p->cursor = (struct position){0};
        if (count > 1 && c == 'g') {
            p->remaining = count - 1;
            p->job = GO_LINE;
        } else {
            p->top = p->cursor;
            p->attn = p->cursor;
            catch_link_on_attn(p, 1);
            p->current.valid = false;
        }
    } else if (c == 'G' || key.type == TAM_KEY_END) {
        save_jump_mark(p);
        p->cursor = (struct position){0};
        p->job = GO_END;
    } else if (c == 'F') {
        save_jump_mark(p);
        p->following = true;
        p->cursor = (struct position){0};
        p->job = GO_END;
    } else if (c == 'm') {
        p->prompt = MARK_SET;
    } else if (c == '\'') {
        p->prompt = MARK_GOTO;
    } else if (c == '/' || c == '?') {
        p->query_length = 0;
        p->query[0] = '\0';
        p->prompt = c == '/' ? FIND_FORWARD : FIND_BACKWARD;
    } else if (c == 'n' || c == 'N') {
        start_search(p, c == 'n' ? p->search.original_direction
                                 : -p->search.original_direction, true);
    } else if (c == 'h') {
        p->prompt = HELP;
        p->help_offset = 0;
        p->current.valid = false;
    }

    if (movement) {
        if (movement < 0) {
            if (back(p, p->top, -movement, &p->top) < 0)
                return -1;
            if (attn_mode) {
                p->attn = p->top;
                catch_link_on_attn(p, -1);
                p->current.valid = false;
            }
        }
        if (movement > 0) {
            p->cursor = p->top;
            p->remaining = (unsigned)movement;
            p->job = ADVANCE;
        }
        if (p->search.hit >= 0) {
            p->search.hit = -1;
            p->current.valid = false;
        }
    }
    return 0;
}

static int refresh_size(struct pager *p)
{
    if (p->current.valid && output(p, "\r\033[K\r\n") < 0)
        return -1;
    p->current.valid = false;
    p->dirty = true;
    if (p->job == ADVANCE)
        p->job = IDLE;
    if (size_view(p) < 0)
        return -1;
    struct file_entry *cur = &p->files[p->file_index];
    invalidate_row_index(cur);
    if (cur->render_mode) {
        ensure_rendered(p, cur);
        p->source = active_source(cur);
        off_t sz = source_size(p->source);
        if (sz >= 0 && p->top.byte >= sz)
            p->top = (struct position){0};
        if (sz >= 0 && p->attn.byte >= sz)
            p->attn = p->top;
    }
    if (align(p, p->top, &p->top) < 0)
        return -1;
    if (align(p, p->attn, &p->attn) < 0)
        return -1;
    catch_link_on_attn(p, 1);
    return 0;
}

static int suspend_view(struct pager *p)
{
    if (p->current.valid && output(p, "\r\033[K") < 0)
        return -1;
    if (tam_tty_raw(&p->tty, false) < 0)
        return -1;
    if (raise(SIGSTOP) != 0)
        return -1;
    if (interrupted)
        return 0;
    if (tam_tty_raw(&p->tty, true) < 0)
        return -1;
    tam_key_init(&p->keys);
    return refresh_size(p);
}

static int poll_keys(struct pager *p)
{
    unsigned char bytes[TAM_KEY_BUF_SIZE];
    size_t room = sizeof(bytes) - tam_key_pending(&p->keys);
    if (!room) {
        tam_key_init(&p->keys);
        room = sizeof(bytes);
    }
    ssize_t count = read(p->tty.in_fd, bytes, room);
    if (count < 0 && errno == EINTR)
        return 0;
    if (count == 0) {
        p->quit = true;
        return 0;
    }
    if (count < 0)
        return -1;
    if (tam_key_feed(&p->keys, bytes, (size_t)count) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int run(struct pager *p, int wake)
{
    while (!p->quit) {
        if (interrupted) {
            p->result = 128 + interrupted;
            break;
        }
        if (suspended) {
            suspended = 0;
            if (suspend_view(p) < 0)
                return -1;
            continue;
        }
        if (resized) {
            resized = 0;
            if (refresh_size(p) < 0)
                return -1;
        }

        int job_state = VIEW_ROW;
        if (p->job != IDLE) {
            job_state = step_job(p);
            if (job_state == VIEW_ERROR)
                return -1;
        }
        if (p->dirty && paint(p) < 0)
            return -1;
        if (p->quit)
            break;

        tam_key_event_t key;
        int ready = tam_key_next(&p->keys, &key);
        if (ready > 0) {
            if (handle_key(p, key) < 0)
                return -1;
            continue;
        }
        if (ready < 0)
            tam_key_init(&p->keys);

        bool pump = !p->source->eof &&
                    (p->waiting || job_state == VIEW_WAIT || p->following);
        struct pollfd fds[] = {
            {.fd = wake, .events = POLLIN},
            {.fd = p->tty.in_fd, .events = POLLIN},
            {.fd = pump ? p->source->input : -1, .events = POLLIN}
        };
        bool scanning = p->job != IDLE && job_state != VIEW_WAIT;
        int timeout = scanning ? 0
                    : tam_key_pending(&p->keys) ? 50
                    : (p->following && !p->source->spool) ? 250 : -1;
        int n = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0 && !scanning) {
            if (tam_key_pending(&p->keys)) {
                int got_key = tam_key_timeout(&p->keys, &key);
                tam_key_init(&p->keys);
                if (got_key > 0 && handle_key(p, key) < 0)
                    return -1;
            }
            if (p->following && !p->source->spool && p->job == IDLE) {
                off_t new_size = 0;
                if (source_refresh(p->source, &new_size) == 0 &&
                    new_size != p->follow_size) {
                    p->follow_size = new_size;
                    if (new_size < p->top.byte)
                        p->top = (struct position){0};
                    p->job = GO_END;
                }
            }
        }
        if (fds[0].revents) {
            char bytes[64];
            while (read(wake, bytes, sizeof(bytes)) > 0)
                continue;
            continue;
        }
        if (fds[1].revents) {
            if (poll_keys(p) < 0)
                return -1;
            continue;
        }
        if (fds[2].revents) {
            if (source_pump(p->source) < 0)
                return -1;
            if (p->following) {
                p->job = GO_END;
            } else if (p->waiting) {
                p->dirty = true;
                p->scroll = true;
            }
        }
    }

    if (p->current.valid && !p->current.document) {
        p->prompt = COMMAND;
        p->quit = true;
        if (paint(p) < 0)
            return -1;
    } else if (p->current.valid && output(p, "\r\033[K") < 0) {
        return -1;
    }
    return p->result;
}

static int dump_inputs(struct file_entry *files, size_t count)
{
    int result = view_dump(&files[0].source, 80);
    for (size_t i = 1; result == 0 && i < count; i++) {
        int fd = open(files[i].path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
            return -1;
        struct source s;
        if (source_open(&s, fd) < 0) {
            int err = errno;
            close(fd);
            errno = err;
            return -1;
        }
        result = view_dump(&s, 80);
        int err = errno;
        source_close(&s);
        close(fd);
        errno = err;
    }
    return result;
}

int view_interactive(const struct view_input *inputs, size_t count, int tty,
                     struct view_options opts)
{
    struct file_entry *files = calloc(count, sizeof(*files));
    if (!files)
        return -1;
    for (size_t i = 0; i < count; i++) {
        files[i].path = inputs[i].path;
        files[i].fd = inputs[i].fd;
    }
    if (source_open(&files[0].source, inputs[0].fd) < 0) {
        int err = errno;
        free(files);
        errno = err;
        return -1;
    }
    files[0].opened = true;

    struct pager p = {
        .source = &files[0].source,
        .files = files,
        .file_count = count,
        .file_cap = count,
        .cli_file_count = count,
        .file_index = 0,
        .focused_link = -1,
        .opts = opts,
        .label = files[0].path,
        .dirty = true
    };
    p.search.hit = -1;
    int wake[2] = {-1, -1};
    int result = -1;
    if (tam_tty_open(&p.tty, tty, STDOUT_FILENO) < 0 ||
        !p.tty.interactive) {
        result = dump_inputs(files, count);
        int err = errno;
        source_close(&files[0].source);
        free(files);
        errno = err;
        return result;
    }

    // Job control belongs to the shell. A background pager must stop before
    // changing the settings on its controlling terminal.
    for (;;) {
        pid_t foreground = tcgetpgrp(tty);
        if (foreground == getpgrp())
            break;
        if (foreground < 0)
            goto cleanup;
        if (raise(SIGTTIN) != 0)
            goto cleanup;
    }

    if (size_view(&p) == 0 && signal_pipe(wake) == 0 &&
        tam_tty_raw(&p.tty, true) == 0) {
        ensure_rendered(&p, &p.files[0]);
        p.source = active_source(&p.files[0]);
        catch_link_on_attn(&p, 1);
        result = run(&p, wake[0]);
        if (result < 0 && p.current.valid) {
            int error = errno;
            (void)output(&p, "\r\033[K");
            errno = error;
        }
    }

cleanup:;
    int error = errno;
    if (tam_tty_raw(&p.tty, false) < 0 && result == 0) {
        result = -1;
        error = errno;
    }
    tam_tty_close(&p.tty);
    wake_fd = -1;
    for (int i = 0; i < 2; i++)
        if (wake[i] >= 0)
            close(wake[i]);
    if (p.search.valid)
        regfree(&p.search.regex);
    free(p.search.line);
    free(p.next.text);
    free(p.history);
    for (size_t i = 0; i < p.file_count; i++) {
        if (p.files[i].has_rendered) {
            source_close(&p.files[i].rendered);
            view_styles_free(&p.files[i].rendered_styles);
            view_links_free(&p.files[i].rendered_links);
        }
        free(p.files[i].row_index);
        if (p.files[i].opened)
            source_close(&p.files[i].source);
        if (p.files[i].own_fd && p.files[i].fd >= 0)
            close(p.files[i].fd);
        if (p.files[i].own_path)
            free((void *)p.files[i].path);
    }
    free(p.files);
    errno = error;
    return result;
}
