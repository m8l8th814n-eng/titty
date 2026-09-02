#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include "common.h"

enum {
    ST_GROUND = 0,
    ST_ESC,
    ST_ESC_INTER,
    ST_CSI,
    ST_OSC,
    ST_STR_IGNORE,
    ST_CHARSET
};

static const uint16_t dec_graphics[] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
    0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
    0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
    0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
};

static bool g0_graphics = false;

static void reply(Term *t, const char *s) {
    if (t->ptyfd >= 0) {
        size_t n = strlen(s);
        ssize_t w = write(t->ptyfd, s, n);
        (void)w;
    }
}

static void erase_cells(Term *t, Cell *c, int n) {
    for (int i = 0; i < n; i++) {
        c[i].cp = ' ';
        c[i].fg = t->pen.fg;
        c[i].bg = t->pen.bg;
        c[i].attr = 0;
    }
}

static void sb_push(Term *t, const Cell *line) {
    if (t->sb_cap <= 0) return;
    memcpy(&t->sb[t->sb_head * t->cols], line, t->cols * sizeof(Cell));
    t->sb_head = (t->sb_head + 1) % t->sb_cap;
    if (t->sb_len < t->sb_cap) {
        t->sb_len++;
    } else {
        if (t->scroll_off > 0) t->scroll_off--;
        if (t->sel_mode) {
            t->sel_ay--;
            t->sel_by--;
            if (t->sel_ay < 0 || t->sel_by < 0) term_sel_clear(t);
        }
    }
}

int term_abs_row(Term *t, int view_y) {
    return t->sb_len + view_y - t->scroll_off;
}

const Cell *term_abs_line(Term *t, int abs_row) {
    if (abs_row < 0) return NULL;
    if (abs_row < t->sb_len) {
        if (t->sb_cap <= 0) return NULL;
        int idx = (t->sb_head - t->sb_len + abs_row) % t->sb_cap;
        if (idx < 0) idx += t->sb_cap;
        return &t->sb[idx * t->cols];
    }
    int y = abs_row - t->sb_len;
    if (y >= t->rows) return NULL;
    return &t->screen[y * t->cols];
}

void term_sel_clear(Term *t) {
    if (t->sel_mode) t->dirty = true;
    t->sel_mode = SEL_NONE;
    t->sel_dragging = false;
}

bool term_sel_active(Term *t) { return t->sel_mode != SEL_NONE; }

static bool is_word_char(uint32_t cp) {
    if (cp == 0 || cp == ' ') return false;
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
                          (cp >= 'a' && cp <= 'z') || cp == '_' || cp == '-' ||
                          cp == '.' || cp == '/' || cp == '~' || cp == ':';
    return true;
}

static void sel_snap(Term *t) {
    if (t->sel_mode == SEL_LINE) {
        t->sel_ax = 0;
        t->sel_bx = t->cols - 1;
        return;
    }
    if (t->sel_mode != SEL_WORD) return;

    const Cell *la = term_abs_line(t, t->sel_ay);
    const Cell *lb = term_abs_line(t, t->sel_by);
    if (!la || !lb) return;

    int ax = t->sel_ax, bx = t->sel_bx;
    if (t->sel_ay > t->sel_by || (t->sel_ay == t->sel_by && ax > bx)) {
        const Cell *tmp = la; la = lb; lb = tmp;
        int t2 = ax; ax = bx; bx = t2;
        int *pa = &t->sel_ax, *pb = &t->sel_bx;
        while (ax > 0 && is_word_char(la[ax - 1].cp)) ax--;
        while (bx < t->cols - 1 && is_word_char(lb[bx + 1].cp)) bx++;
        *pb = ax;
        *pa = bx;
        return;
    }
    while (ax > 0 && is_word_char(la[ax - 1].cp)) ax--;
    while (bx < t->cols - 1 && is_word_char(lb[bx + 1].cp)) bx++;
    t->sel_ax = ax;
    t->sel_bx = bx;
}

void term_sel_start(Term *t, int col, int row, int mode) {
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    t->sel_mode = mode;
    t->sel_dragging = true;
    t->sel_ax = t->sel_bx = col;
    t->sel_ay = t->sel_by = row;
    sel_snap(t);
    t->dirty = true;
}

void term_sel_extend(Term *t, int col, int row) {
    if (!t->sel_mode) return;
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    t->sel_bx = col;
    t->sel_by = row;
    sel_snap(t);
    t->dirty = true;
}

static void sel_ordered(Term *t, int *ax, int *ay, int *bx, int *by) {
    if (t->sel_ay < t->sel_by || (t->sel_ay == t->sel_by && t->sel_ax <= t->sel_bx)) {
        *ax = t->sel_ax; *ay = t->sel_ay; *bx = t->sel_bx; *by = t->sel_by;
    } else {
        *ax = t->sel_bx; *ay = t->sel_by; *bx = t->sel_ax; *by = t->sel_ay;
    }
}

bool term_sel_contains(Term *t, int col, int row) {
    if (!t->sel_mode) return false;
    int ax, ay, bx, by;
    sel_ordered(t, &ax, &ay, &bx, &by);
    if (row < ay || row > by) return false;
    if (row == ay && col < ax) return false;
    if (row == by && col > bx) return false;
    return true;
}

static void put_utf8(char **p, uint32_t cp) {
    char *o = *p;
    if (cp < 0x80) { *o++ = (char)cp; }
    else if (cp < 0x800) { *o++ = (char)(0xc0|(cp>>6)); *o++ = (char)(0x80|(cp&0x3f)); }
    else if (cp < 0x10000) { *o++ = (char)(0xe0|(cp>>12)); *o++ = (char)(0x80|((cp>>6)&0x3f)); *o++ = (char)(0x80|(cp&0x3f)); }
    else { *o++ = (char)(0xf0|(cp>>18)); *o++ = (char)(0x80|((cp>>12)&0x3f)); *o++ = (char)(0x80|((cp>>6)&0x3f)); *o++ = (char)(0x80|(cp&0x3f)); }
    *p = o;
}

char *term_sel_text(Term *t, size_t *out_len) {
    if (!t->sel_mode) return NULL;
    int ax, ay, bx, by;
    sel_ordered(t, &ax, &ay, &bx, &by);

    size_t cap = (size_t)(by - ay + 1) * (size_t)(t->cols + 1) * 4 + 8;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    char *p = buf;

    for (int row = ay; row <= by; row++) {
        const Cell *line = term_abs_line(t, row);
        if (!line) continue;
        int from = (row == ay) ? ax : 0;
        int to = (row == by) ? bx : t->cols - 1;
        if (from < 0) from = 0;
        if (to >= t->cols) to = t->cols - 1;

        int last = from - 1;
        for (int x = from; x <= to; x++)
            if (!(line[x].attr & ATTR_DUMMY) && line[x].cp && line[x].cp != ' ') last = x;

        for (int x = from; x <= last; x++) {
            if (line[x].attr & ATTR_DUMMY) continue;
            uint32_t cp = line[x].cp ? line[x].cp : ' ';
            put_utf8(&p, cp);
        }
        if (row != by) *p++ = '\n';
    }

    *p = 0;
    if (out_len) *out_len = (size_t)(p - buf);
    return buf;
}

const Cell *term_line(Term *t, int y) {
    int pos = t->sb_len + y - t->scroll_off;
    if (pos < t->sb_len) {
        int idx = (t->sb_head - t->sb_len + pos) % t->sb_cap;
        if (idx < 0) idx += t->sb_cap;
        return &t->sb[idx * t->cols];
    }
    return &t->screen[(pos - t->sb_len) * t->cols];
}

void term_scroll_view(Term *t, int delta) {
    if (t->alt) return;
    int off = t->scroll_off + delta;
    if (off < 0) off = 0;
    if (off > t->sb_len) off = t->sb_len;
    if (off != t->scroll_off) {
        t->scroll_off = off;
        t->dirty = true;
    }
}

static void scroll_up(Term *t, int n) {
    if (n <= 0) return;
    int top = t->top, bot = t->bot;
    if (n > bot - top + 1) n = bot - top + 1;
    if (!t->alt && top == 0)
        for (int i = 0; i < n; i++) sb_push(t, &t->screen[(top + i) * t->cols]);
    int move = bot - top + 1 - n;
    if (move > 0)
        memmove(&t->screen[top * t->cols], &t->screen[(top + n) * t->cols],
                move * t->cols * sizeof(Cell));
    erase_cells(t, &t->screen[(top + move) * t->cols], n * t->cols);
    t->dirty = true;
}

static void scroll_down(Term *t, int n) {
    if (n <= 0) return;
    int top = t->top, bot = t->bot;
    if (n > bot - top + 1) n = bot - top + 1;
    int move = bot - top + 1 - n;
    if (move > 0)
        memmove(&t->screen[(top + n) * t->cols], &t->screen[top * t->cols],
                move * t->cols * sizeof(Cell));
    erase_cells(t, &t->screen[top * t->cols], n * t->cols);
    t->dirty = true;
}

static void move_to(Term *t, int x, int y) {
    int ymin = t->origin_mode ? t->top : 0;
    int ymax = t->origin_mode ? t->bot : t->rows - 1;
    if (x < 0) x = 0;
    if (x >= t->cols) x = t->cols - 1;
    if (y < ymin) y = ymin;
    if (y > ymax) y = ymax;
    t->cx = x;
    t->cy = y;
    t->wrapnext = false;
    t->cursor_moved = true;
}

static void newline(Term *t) {
    if (t->cy == t->bot) scroll_up(t, 1);
    else if (t->cy < t->rows - 1) t->cy++;
    t->cursor_moved = true;
}

static void reverse_index(Term *t) {
    if (t->cy == t->top) scroll_down(t, 1);
    else if (t->cy > 0) t->cy--;
    t->cursor_moved = true;
}

static void put_char(Term *t, uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    if (w < 0) w = 1;
    if (w == 0) {
        if (t->cx > 0) {
            Cell *c = term_cell(t, t->cx - 1, t->cy);
            if (c->attr & ATTR_DUMMY && t->cx > 1) c = term_cell(t, t->cx - 2, t->cy);
            (void)c;
        }
        return;
    }

    if (t->wrapnext && t->autowrap) {
        newline(t);
        t->cx = 0;
        t->wrapnext = false;
    }
    if (t->cx + w > t->cols) {
        if (!t->autowrap) return;
        newline(t);
        t->cx = 0;
        if (t->cx + w > t->cols) return;
    }

    if (t->insert_mode) {
        Cell *row = &t->screen[t->cy * t->cols];
        int n = t->cols - t->cx - w;
        if (n > 0) memmove(&row[t->cx + w], &row[t->cx], n * sizeof(Cell));
    }

    Cell *c = term_cell(t, t->cx, t->cy);
    c->cp = cp;
    c->fg = t->pen.fg;
    c->bg = t->pen.bg;
    c->attr = t->pen.attr | (w == 2 ? ATTR_WIDE : 0);
    if (w == 2) {
        Cell *d = c + 1;
        d->cp = 0;
        d->fg = t->pen.fg;
        d->bg = t->pen.bg;
        d->attr = t->pen.attr | ATTR_DUMMY;
    }

    t->cx += w;
    if (t->cx >= t->cols) {
        t->cx = t->cols - 1;
        t->wrapnext = true;
    }
    t->dirty = true;
    t->cursor_moved = true;
}

static void set_mode(Term *t, bool priv, int m, bool on) {
    if (!priv) {
        switch (m) {
        case 4: t->insert_mode = on; break;
        case 20: break;
        }
        return;
    }
    switch (m) {
    case 1: t->app_cursor = on; break;
    case 5: t->reverse_video = on; t->dirty = true; break;
    case 6:
        t->origin_mode = on;
        move_to(t, 0, on ? t->top : 0);
        break;
    case 7: t->autowrap = on; break;
    case 12: break;
    case 25: t->cursor_visible = on; t->dirty = true; break;
    case 1004: t->focus_events = on; break;
    case 1049:
        if (on != t->alt) {
            if (on) {
                t->save_cx = t->cx; t->save_cy = t->cy; t->save_pen = t->pen;
                t->screen = t->alt_buf;
                t->alt = true;
                erase_cells(t, t->screen, t->cols * t->rows);
            } else {
                t->screen = t->main_buf;
                t->alt = false;
                t->cx = t->save_cx; t->cy = t->save_cy; t->pen = t->save_pen;
                if (t->cx >= t->cols) t->cx = t->cols - 1;
                if (t->cy >= t->rows) t->cy = t->rows - 1;
            }
            t->scroll_off = 0;
            t->dirty = true;
            t->cursor_moved = true;
        }
        break;
    case 2004: t->bracketed_paste = on; break;
    }
}

static int sub_count(Term *t, int i) {
    int n = 0;
    while (i + 1 + n < t->csi_nargs && t->csi_colon[i + 1 + n]) n++;
    return n;
}

static uint32_t sgr_color(Term *t, int *i) {
    int *a = t->csi_args;
    int n = t->csi_nargs;
    int start = *i;
    int subs = sub_count(t, start);

    if (*i + 1 >= n) { *i = n; return COL_DEF_FG; }
    int kind = a[*i + 1];

    if (kind == 5) {
        if (*i + 2 < n) {
            uint32_t c = (uint32_t)(a[*i + 2] & 0xff);
            *i = subs ? start + subs : *i + 2;
            return c;
        }
        *i = n;
        return COL_DEF_FG;
    }

    if (kind == 2) {
        int base = *i + 2;
        if (subs >= 4) base = *i + 3;
        if (base + 2 < n) {
            uint32_t r = (uint32_t)(a[base + 0] & 0xff);
            uint32_t g = (uint32_t)(a[base + 1] & 0xff);
            uint32_t b = (uint32_t)(a[base + 2] & 0xff);
            *i = subs ? start + subs : base + 2;
            return COL_RGB((r << 16) | (g << 8) | b);
        }
        *i = n;
        return COL_DEF_FG;
    }

    *i = subs ? start + subs : *i + 1;
    return COL_DEF_FG;
}

static void do_sgr(Term *t) {
    if (t->csi_nargs == 0) {
        t->pen.fg = COL_DEF_FG;
        t->pen.bg = COL_DEF_BG;
        t->pen.attr = 0;
        return;
    }
    for (int i = 0; i < t->csi_nargs; i++) {
        if (t->csi_colon[i]) continue;
        int v = t->csi_args[i];
        switch (v) {
        case 0: t->pen.fg = COL_DEF_FG; t->pen.bg = COL_DEF_BG; t->pen.attr = 0; break;
        case 1: t->pen.attr |= ATTR_BOLD; break;
        case 2: t->pen.attr |= ATTR_DIM; break;
        case 3: t->pen.attr |= ATTR_ITALIC; break;
        case 4:
            if (sub_count(t, i) && t->csi_args[i + 1] == 0) t->pen.attr &= ~ATTR_UNDERLINE;
            else t->pen.attr |= ATTR_UNDERLINE;
            i += sub_count(t, i);
            break;
        case 5: case 6: t->pen.attr |= ATTR_BLINK; break;
        case 7: t->pen.attr |= ATTR_REVERSE; break;
        case 8: t->pen.attr |= ATTR_HIDDEN; break;
        case 9: t->pen.attr |= ATTR_STRIKE; break;
        case 21: case 22: t->pen.attr &= ~(ATTR_BOLD | ATTR_DIM); break;
        case 23: t->pen.attr &= ~ATTR_ITALIC; break;
        case 24: t->pen.attr &= ~ATTR_UNDERLINE; break;
        case 25: t->pen.attr &= ~ATTR_BLINK; break;
        case 27: t->pen.attr &= ~ATTR_REVERSE; break;
        case 28: t->pen.attr &= ~ATTR_HIDDEN; break;
        case 29: t->pen.attr &= ~ATTR_STRIKE; break;
        case 38: t->pen.fg = sgr_color(t, &i); continue;
        case 39: t->pen.fg = COL_DEF_FG; break;
        case 48: t->pen.bg = sgr_color(t, &i); continue;
        case 49: t->pen.bg = COL_DEF_BG; break;
        case 58: sgr_color(t, &i); continue;
        case 59: break;
        default:
            if (v >= 30 && v <= 37) t->pen.fg = (uint32_t)(v - 30);
            else if (v >= 40 && v <= 47) t->pen.bg = (uint32_t)(v - 40);
            else if (v >= 90 && v <= 97) t->pen.fg = (uint32_t)(v - 90 + 8);
            else if (v >= 100 && v <= 107) t->pen.bg = (uint32_t)(v - 100 + 8);
            break;
        }
    }
}

static void erase_display(Term *t, int mode) {
    int total = t->cols * t->rows;
    int cur = t->cy * t->cols + t->cx;
    switch (mode) {
    case 0: erase_cells(t, &t->screen[cur], total - cur); break;
    case 1: erase_cells(t, t->screen, cur + 1); break;
    case 2:
    case 3:
        if (!t->alt && mode == 2)
            for (int y = 0; y < t->rows; y++) sb_push(t, &t->screen[y * t->cols]);
        erase_cells(t, t->screen, total);
        if (mode == 3) { t->sb_len = 0; t->sb_head = 0; t->scroll_off = 0; }
        break;
    }
    t->dirty = true;
}

static void erase_line(Term *t, int mode) {
    Cell *row = &t->screen[t->cy * t->cols];
    switch (mode) {
    case 0: erase_cells(t, &row[t->cx], t->cols - t->cx); break;
    case 1: erase_cells(t, row, t->cx + 1); break;
    case 2: erase_cells(t, row, t->cols); break;
    }
    t->dirty = true;
}

static void csi_dispatch(Term *t, char final) {
    int *a = t->csi_args;
    int n = t->csi_nargs;
    int p0 = n > 0 ? a[0] : 0;
    int arg1 = (n > 0 && a[0] > 0) ? a[0] : 1;
    char buf[64];

    if (t->csi_priv == '?') {
        switch (final) {
        case 'h': for (int i = 0; i < n; i++) set_mode(t, true, a[i], true); return;
        case 'l': for (int i = 0; i < n; i++) set_mode(t, true, a[i], false); return;
        case 'n':
            if (p0 == 6) {
                snprintf(buf, sizeof buf, "\033[?%d;%dR", t->cy + 1, t->cx + 1);
                reply(t, buf);
            }
            return;
        default: return;
        }
    }
    if (t->csi_priv == '>') {
        if (final == 'c') reply(t, "\033[>0;276;0c");
        return;
    }
    if (t->csi_priv) return;

    switch (final) {
    case '@': {
        Cell *row = &t->screen[t->cy * t->cols];
        int cnt = arg1 > t->cols - t->cx ? t->cols - t->cx : arg1;
        int move = t->cols - t->cx - cnt;
        if (move > 0) memmove(&row[t->cx + cnt], &row[t->cx], move * sizeof(Cell));
        erase_cells(t, &row[t->cx], cnt);
        t->dirty = true;
        break;
    }
    case 'A': move_to(t, t->cx, t->cy - arg1); break;
    case 'B': case 'e': move_to(t, t->cx, t->cy + arg1); break;
    case 'C': case 'a': move_to(t, t->cx + arg1, t->cy); break;
    case 'D': move_to(t, t->cx - arg1, t->cy); break;
    case 'E': move_to(t, 0, t->cy + arg1); break;
    case 'F': move_to(t, 0, t->cy - arg1); break;
    case 'G': case '`': move_to(t, arg1 - 1, t->cy); break;
    case 'H': case 'f': {
        int r = (n > 0 && a[0] > 0) ? a[0] : 1;
        int c = (n > 1 && a[1] > 0) ? a[1] : 1;
        move_to(t, c - 1, (t->origin_mode ? t->top : 0) + r - 1);
        break;
    }
    case 'I': {
        int x = t->cx;
        for (int i = 0; i < arg1; i++) x = (x / 8 + 1) * 8;
        move_to(t, x, t->cy);
        break;
    }
    case 'J': erase_display(t, p0); break;
    case 'K': erase_line(t, p0); break;
    case 'L': {
        if (t->cy < t->top || t->cy > t->bot) break;
        int save = t->top;
        t->top = t->cy;
        scroll_down(t, arg1);
        t->top = save;
        break;
    }
    case 'M': {
        if (t->cy < t->top || t->cy > t->bot) break;
        int save = t->top;
        t->top = t->cy;
        scroll_up(t, arg1);
        t->top = save;
        break;
    }
    case 'P': {
        Cell *row = &t->screen[t->cy * t->cols];
        int cnt = arg1 > t->cols - t->cx ? t->cols - t->cx : arg1;
        int move = t->cols - t->cx - cnt;
        if (move > 0) memmove(&row[t->cx], &row[t->cx + cnt], move * sizeof(Cell));
        erase_cells(t, &row[t->cols - cnt], cnt);
        t->dirty = true;
        break;
    }
    case 'S': scroll_up(t, arg1); break;
    case 'T': scroll_down(t, arg1); break;
    case 'X': {
        Cell *row = &t->screen[t->cy * t->cols];
        int cnt = arg1 > t->cols - t->cx ? t->cols - t->cx : arg1;
        erase_cells(t, &row[t->cx], cnt);
        t->dirty = true;
        break;
    }
    case 'Z': {
        int x = t->cx;
        for (int i = 0; i < arg1; i++) x = x > 0 ? ((x - 1) / 8) * 8 : 0;
        move_to(t, x, t->cy);
        break;
    }
    case 'b': break;
    case 'c': reply(t, "\033[?62;1;6;9;15;22c"); break;
    case 'd': move_to(t, t->cx, arg1 - 1); break;
    case 'h': for (int i = 0; i < n; i++) set_mode(t, false, a[i], true); break;
    case 'l': for (int i = 0; i < n; i++) set_mode(t, false, a[i], false); break;
    case 'm': do_sgr(t); break;
    case 'n':
        if (p0 == 6) {
            snprintf(buf, sizeof buf, "\033[%d;%dR", t->cy + 1, t->cx + 1);
            reply(t, buf);
        } else if (p0 == 5) {
            reply(t, "\033[0n");
        }
        break;
    case 'q':
        if (t->csi_inter == ' ') {
            int s = p0;
            t->cursor_shape = (s <= 2) ? CURSOR_BLOCK : (s <= 4) ? CURSOR_UNDERLINE : CURSOR_BEAM;
            t->dirty = true;
        }
        break;
    case 'r': {
        int top = (n > 0 && a[0] > 0) ? a[0] - 1 : 0;
        int bot = (n > 1 && a[1] > 0) ? a[1] - 1 : t->rows - 1;
        if (top < 0) top = 0;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) {
            t->top = top;
            t->bot = bot;
            move_to(t, 0, t->origin_mode ? top : 0);
        }
        break;
    }
    case 's': t->save_cx = t->cx; t->save_cy = t->cy; t->save_pen = t->pen; t->save_valid = true; break;
    case 'u':
        if (t->save_valid) { move_to(t, t->save_cx, t->save_cy); t->pen = t->save_pen; }
        break;
    case 't': break;
    default: break;
    }
}

static void osc_dispatch(Term *t) {
    char *s = t->osc;
    int cmd = 0;
    char *p = s;
    while (*p >= '0' && *p <= '9') { cmd = cmd * 10 + (*p - '0'); p++; }
    if (*p != ';') return;
    p++;
    switch (cmd) {
    case 0: case 1: case 2:
        snprintf(t->title, sizeof t->title, "%s", p);
        t->title_changed = true;
        break;
    case 10: case 11: {
        uint32_t c = (cmd == 10) ? (uint32_t)COLOR_FG : (uint32_t)COLOR_BG;
        unsigned r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
        char buf[64];
        snprintf(buf, sizeof buf, "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02x\033\\",
                 cmd, r, r, g, g, b, b);
        reply(t, buf);
        break;
    }
    default: break;
    }
}

static void exec_c0(Term *t, uint8_t c) {
    switch (c) {
    case 0x07: t->bell = true; break;
    case 0x08:
        if (t->wrapnext) t->wrapnext = false;
        else if (t->cx > 0) t->cx--;
        t->cursor_moved = true;
        break;
    case 0x09: {
        int x = (t->cx / 8 + 1) * 8;
        if (x >= t->cols) x = t->cols - 1;
        t->cx = x;
        t->wrapnext = false;
        t->cursor_moved = true;
        break;
    }
    case 0x0a: case 0x0b: case 0x0c:
        newline(t);
        t->wrapnext = false;
        break;
    case 0x0d: t->cx = 0; t->wrapnext = false; t->cursor_moved = true; break;
    case 0x0e: g0_graphics = true; break;
    case 0x0f: g0_graphics = false; break;
    }
}

static void esc_dispatch(Term *t, uint8_t c) {
    switch (c) {
    case 'D': newline(t); break;
    case 'E': newline(t); t->cx = 0; break;
    case 'M': reverse_index(t); break;
    case 'c':
        term_init(t, t->cols, t->rows);
        break;
    case '7': t->save_cx = t->cx; t->save_cy = t->cy; t->save_pen = t->pen; t->save_valid = true; break;
    case '8':
        if (t->save_valid) { move_to(t, t->save_cx, t->save_cy); t->pen = t->save_pen; }
        break;
    case '=': t->app_keypad = true; break;
    case '>': t->app_keypad = false; break;
    case 'H': break;
    default: break;
    }
}

void term_write(Term *t, const uint8_t *buf, size_t len) {
    if (t->scroll_off) { t->scroll_off = 0; t->dirty = true; }

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        switch (t->state) {
        case ST_GROUND:
            if (t->u_need) {
                if ((c & 0xc0) == 0x80) {
                    t->ucs = (t->ucs << 6) | (c & 0x3f);
                    if (++t->u_have == t->u_need) {
                        put_char(t, t->ucs);
                        t->u_need = 0;
                    }
                    continue;
                }
                t->u_need = 0;
                put_char(t, 0xfffd);
            }
            if (c == 0x1b) { t->state = ST_ESC; t->csi_inter = 0; continue; }
            if (c < 0x20 || c == 0x7f) { exec_c0(t, c); continue; }
            if (c < 0x80) {
                uint32_t cp = c;
                if (g0_graphics && c >= 0x60 && c <= 0x7e) cp = dec_graphics[c - 0x60];
                put_char(t, cp);
                continue;
            }
            if ((c & 0xe0) == 0xc0) { t->ucs = c & 0x1f; t->u_need = 1; t->u_have = 0; }
            else if ((c & 0xf0) == 0xe0) { t->ucs = c & 0x0f; t->u_need = 2; t->u_have = 0; }
            else if ((c & 0xf8) == 0xf0) { t->ucs = c & 0x07; t->u_need = 3; t->u_have = 0; }
            else put_char(t, 0xfffd);
            continue;

        case ST_ESC:
            if (c == '[') {
                t->state = ST_CSI;
                t->csi_nargs = 0;
                t->csi_priv = 0;
                t->csi_inter = 0;
                t->csi_empty = true;
                memset(t->csi_args, 0, sizeof t->csi_args);
                memset(t->csi_colon, 0, sizeof t->csi_colon);
                continue;
            }
            if (c == ']') { t->state = ST_OSC; t->osc_len = 0; continue; }
            if (c == 'P' || c == 'X' || c == '^' || c == '_') { t->state = ST_STR_IGNORE; continue; }
            if (c == '(' || c == ')' || c == '*' || c == '+') { t->state = ST_CHARSET; continue; }
            if (c == '#' || c == '%' || c == ' ') { t->state = ST_ESC_INTER; continue; }
            esc_dispatch(t, c);
            t->state = ST_GROUND;
            continue;

        case ST_ESC_INTER:
            t->state = ST_GROUND;
            continue;

        case ST_CHARSET:
            g0_graphics = (c == '0');
            t->state = ST_GROUND;
            continue;

        case ST_CSI:
            if (c >= '0' && c <= '9') {
                if (t->csi_nargs == 0) t->csi_nargs = 1;
                int *slot = &t->csi_args[t->csi_nargs - 1];
                if (*slot < 100000000) *slot = *slot * 10 + (c - '0');
                t->csi_empty = false;
                continue;
            }
            if (c == ';' || c == ':') {
                if (t->csi_nargs == 0) t->csi_nargs = 1;
                if (t->csi_nargs < CSI_MAX_ARGS) {
                    t->csi_colon[t->csi_nargs] = (c == ':');
                    t->csi_args[t->csi_nargs++] = 0;
                }
                continue;
            }
            if (c >= '<' && c <= '?') { t->csi_priv = (char)c; continue; }
            if (c >= 0x20 && c <= 0x2f) { t->csi_inter = (char)c; continue; }
            if (c >= 0x40 && c <= 0x7e) {
                csi_dispatch(t, (char)c);
                t->state = ST_GROUND;
                continue;
            }
            if (c == 0x1b) { t->state = ST_ESC; continue; }
            if (c < 0x20) { exec_c0(t, c); continue; }
            t->state = ST_GROUND;
            continue;

        case ST_OSC:
            if (c == 0x07) {
                t->osc[t->osc_len] = 0;
                osc_dispatch(t);
                t->state = ST_GROUND;
                continue;
            }
            if (c == 0x1b) {
                if (i + 1 < len && buf[i + 1] == '\\') i++;
                t->osc[t->osc_len] = 0;
                osc_dispatch(t);
                t->state = ST_GROUND;
                continue;
            }
            if (t->osc_len < OSC_MAX - 1) t->osc[t->osc_len++] = (char)c;
            continue;

        case ST_STR_IGNORE:
            if (c == 0x07) { t->state = ST_GROUND; continue; }
            if (c == 0x1b) {
                if (i + 1 < len && buf[i + 1] == '\\') i++;
                t->state = ST_GROUND;
            }
            continue;
        }
    }
}

void term_init(Term *t, int cols, int rows) {
    int ptyfd = t->ptyfd;
    Cell *mb = t->main_buf, *ab = t->alt_buf, *sb = t->sb;
    int sbcap = t->sb_cap;
    bool fresh = (mb == NULL);

    if (fresh) {
        mb = calloc((size_t)cols * rows, sizeof(Cell));
        ab = calloc((size_t)cols * rows, sizeof(Cell));
        sbcap = SCROLLBACK_LINES;
        sb = sbcap > 0 ? calloc((size_t)cols * sbcap, sizeof(Cell)) : NULL;
        if (!mb || !ab || (sbcap > 0 && !sb)) {
            fprintf(stderr, "titty: out of memory\n");
            exit(1);
        }
    }

    memset(t, 0, sizeof *t);
    t->ptyfd = ptyfd;
    t->cols = cols;
    t->rows = rows;
    t->main_buf = mb;
    t->alt_buf = ab;
    t->screen = mb;
    t->sb = sb;
    t->sb_cap = sbcap;
    t->top = 0;
    t->bot = rows - 1;
    t->pen.fg = COL_DEF_FG;
    t->pen.bg = COL_DEF_BG;
    t->cursor_visible = true;
    t->autowrap = true;
    t->cursor_shape = CURSOR_SHAPE;
    t->dirty = true;
    snprintf(t->title, sizeof t->title, "%s", WINDOW_TITLE);

    erase_cells(t, t->main_buf, cols * rows);
    erase_cells(t, t->alt_buf, cols * rows);
    g0_graphics = false;
}

void term_free(Term *t) {
    free(t->main_buf);
    free(t->alt_buf);
    free(t->sb);
    t->main_buf = t->alt_buf = t->sb = t->screen = NULL;
}

void term_resize(Term *t, int cols, int rows) {
    if (cols == t->cols && rows == t->rows) return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    Cell *nm = calloc((size_t)cols * rows, sizeof(Cell));
    Cell *na = calloc((size_t)cols * rows, sizeof(Cell));
    if (!nm || !na) { free(nm); free(na); return; }

    Pen save = t->pen;
    t->pen.fg = COL_DEF_FG;
    t->pen.bg = COL_DEF_BG;
    t->pen.attr = 0;

    int oc = t->cols, orow = t->rows;

    for (int i = 0; i < cols * rows; i++) {
        nm[i].cp = ' '; nm[i].fg = COL_DEF_FG; nm[i].bg = COL_DEF_BG;
        na[i].cp = ' '; na[i].fg = COL_DEF_FG; na[i].bg = COL_DEF_BG;
    }

    int shift = 0;
    if (t->cy >= rows) shift = t->cy - rows + 1;
    int copy_rows = orow - shift < rows ? orow - shift : rows;
    int copy_cols = oc < cols ? oc : cols;
    for (int y = 0; y < copy_rows; y++)
        memcpy(&nm[y * cols], &t->main_buf[(y + shift) * oc], copy_cols * sizeof(Cell));
    if (t->alt)
        for (int y = 0; y < copy_rows; y++)
            memcpy(&na[y * cols], &t->alt_buf[(y + shift) * oc], copy_cols * sizeof(Cell));

    free(t->main_buf);
    free(t->alt_buf);
    t->main_buf = nm;
    t->alt_buf = na;
    t->screen = t->alt ? na : nm;
    t->pen = save;

    term_sel_clear(t);

    if (t->sb) {
        free(t->sb);
        t->sb = t->sb_cap > 0 ? calloc((size_t)cols * t->sb_cap, sizeof(Cell)) : NULL;
        t->sb_len = 0;
        t->sb_head = 0;
        t->scroll_off = 0;
    }

    t->cols = cols;
    t->rows = rows;
    t->top = 0;
    t->bot = rows - 1;
    t->cy -= shift;
    if (t->cx >= cols) t->cx = cols - 1;
    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cy < 0) t->cy = 0;
    t->wrapnext = false;
    t->dirty = true;
    t->cursor_moved = true;
}
