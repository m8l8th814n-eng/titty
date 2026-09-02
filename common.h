#ifndef TITTY_COMMON_H
#define TITTY_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "titty.h"

#ifdef TITTY_PRESET
#include TITTY_PRESET
#endif

#define COL_DEF_FG   0xFFFFFFFFu
#define COL_DEF_BG   0xFFFFFFFEu
#define COL_RGB(v)   (0x01000000u | ((uint32_t)(v) & 0x00FFFFFFu))
#define COL_IS_RGB(c) (((c) & 0xFF000000u) == 0x01000000u)
#define COL_IS_IDX(c) ((c) < 256u)

enum {
    ATTR_BOLD      = 1u << 0,
    ATTR_DIM       = 1u << 1,
    ATTR_ITALIC    = 1u << 2,
    ATTR_UNDERLINE = 1u << 3,
    ATTR_BLINK     = 1u << 4,
    ATTR_REVERSE   = 1u << 5,
    ATTR_HIDDEN    = 1u << 6,
    ATTR_STRIKE    = 1u << 7,
    ATTR_WIDE      = 1u << 8,
    ATTR_DUMMY     = 1u << 9,
};

typedef struct {
    uint32_t cp;
    uint32_t fg, bg;
    uint16_t attr;
} Cell;

typedef struct {
    uint32_t fg, bg;
    uint16_t attr;
} Pen;

enum { CURSOR_BLOCK = 0, CURSOR_BEAM = 1, CURSOR_UNDERLINE = 2 };
enum { SEL_NONE = 0, SEL_CHAR, SEL_WORD, SEL_LINE };

#define CSI_MAX_ARGS 32
#define OSC_MAX      1024

typedef struct {
    int cols, rows;
    Cell *screen;
    Cell *main_buf, *alt_buf;
    int cx, cy;
    Pen pen;
    int top, bot;
    bool wrapnext;
    bool alt;

    int save_cx, save_cy;
    Pen save_pen;
    bool save_valid;

    bool cursor_visible;
    bool app_cursor;
    bool app_keypad;
    bool bracketed_paste;
    bool autowrap;
    bool insert_mode;
    bool origin_mode;
    bool reverse_video;
    bool focus_events;
    int cursor_shape;

    Cell *sb;
    int sb_cap;
    int sb_len;
    int sb_head;
    int scroll_off;

    int state;
    uint32_t ucs;
    int u_need, u_have;
    int csi_args[CSI_MAX_ARGS];
    bool csi_colon[CSI_MAX_ARGS];
    int csi_nargs;
    bool csi_empty;
    char csi_priv;
    char csi_inter;
    char osc[OSC_MAX];
    int osc_len;

    char title[512];
    bool title_changed;
    bool dirty;
    bool bell;
    bool cursor_moved;

    int sel_mode;
    bool sel_dragging;
    int sel_ax, sel_ay;
    int sel_bx, sel_by;

    int ptyfd;
} Term;

void term_init(Term *t, int cols, int rows);
void term_free(Term *t);
void term_resize(Term *t, int cols, int rows);
void term_write(Term *t, const uint8_t *buf, size_t len);
void term_scroll_view(Term *t, int delta);
int term_abs_row(Term *t, int view_y);
void term_sel_start(Term *t, int col, int row, int mode);
void term_sel_extend(Term *t, int col, int row);
void term_sel_clear(Term *t);
bool term_sel_active(Term *t);
bool term_sel_contains(Term *t, int col, int row);
char *term_sel_text(Term *t, size_t *out_len);
const Cell *term_abs_line(Term *t, int abs_row);
const Cell *term_line(Term *t, int y);
static inline Cell *term_cell(Term *t, int x, int y) { return &t->screen[y * t->cols + x]; }

int pty_spawn(int cols, int rows, char *const argv[]);
void pty_resize(int fd, int cols, int rows, int px, int py);

enum { STYLE_REGULAR = 0, STYLE_BOLD = 1, STYLE_ITALIC = 2, STYLE_BOLD_ITALIC = 3 };

typedef struct {
    float u0, v0, u1, v1;
    short w, h, bx, by;
    short adv;
    bool color;
    bool valid;
} Glyph;

bool font_init(void);
void font_fini(void);
const Glyph *font_glyph(uint32_t cp, int style);
int font_cell_w(void);
int font_glyph_advance(void);
int font_cell_h(void);
int font_baseline(void);
int font_underline_pos(void);
int font_underline_thickness(void);
unsigned font_atlas_tex(void);
bool font_subpixel(void);
bool font_set_size(double size);
double font_size(void);

bool boxdraw_supported(uint32_t cp);
bool boxdraw_render(uint32_t cp, int w, int h, float *out);

bool render_init(void);
void render_fini(void);
void render_resize(int w, int h);
void render_frame(Term *t, double now, bool focused);
bool render_animating(void);
void render_bell(double now);
void render_toggle_fx(void);
bool render_fx_on(void);

typedef struct {
    int width, height;
    int cols, rows;
    bool focused;
    bool running;
    bool need_draw;
    double now;
    Term *term;
    char *const *cmd;
} App;

extern App app;

void wl_run(App *a);
void wl_shutdown(void);
void wl_set_title(const char *title);
void wl_wake(void);

#endif
