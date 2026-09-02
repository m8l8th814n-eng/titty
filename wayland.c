#include <errno.h>
#include <limits.h>
#include <math.h>
#include <strings.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#if HAVE_EXTBG
#include "ext-background-effect-client-protocol.h"
#endif
#if HAVE_KBLUR
#include "blur-client-protocol.h"
#endif
#if HAVE_PRIMSEL
#include "primary-selection-client-protocol.h"
#endif

#include "common.h"

static struct wl_display *dpy;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct zxdg_toplevel_decoration_v1 *deco;
#if HAVE_EXTBG
static struct ext_background_effect_manager_v1 *bg_mgr;
static struct ext_background_effect_surface_v1 *bg_surf;
static uint32_t bg_caps;
#endif
#if HAVE_KBLUR
static struct org_kde_kwin_blur_manager *kde_blur_mgr;
static struct org_kde_kwin_blur *kde_blur;
#endif

static struct wl_surface *surface;
static struct xdg_surface *xdg_surf;
static struct xdg_toplevel *toplevel;
static struct wl_egl_window *egl_win;
static struct wl_callback *frame_cb;

static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static EGLSurface egl_surf = EGL_NO_SURFACE;
static EGLConfig egl_cfg;

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;
static xkb_mod_index_t mod_ctrl, mod_alt, mod_shift, mod_logo;

static int repeat_fd = -1;
static int32_t repeat_rate = 25, repeat_delay = 600;
static uint32_t repeat_key;
static bool repeat_armed;

enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4, MOD_LOGO = 8 };

#define MAX_BIND_ALTS 6
typedef struct {
    struct { uint32_t mods; xkb_keysym_t sym; } alt[MAX_BIND_ALTS];
    int n;
} BindSet;

static BindSet bind_font_inc, bind_font_dec, bind_new_window, bind_copy, bind_paste, bind_fx;

static bool configured;
static int pending_w, pending_h;
static bool frame_pending;
static bool blur_applied;
static bool gl_ready;

static struct wl_data_device_manager *ddm;
static struct wl_data_device *ddev;
static struct wl_data_source *dsrc;
static struct wl_data_offer *doffer;
static char *clip_text;
static size_t clip_len;
#if HAVE_PRIMSEL
static struct zwp_primary_selection_device_manager_v1 *psm;
static struct zwp_primary_selection_device_v1 *psdev;
static struct zwp_primary_selection_source_v1 *pssrc;
static struct zwp_primary_selection_offer_v1 *psoffer;
static char *prim_text;
static size_t prim_len;
#endif

static uint32_t last_serial;
static int paste_pending;
static double last_click_time;
static int click_count;
static int last_click_cell_x = -1, last_click_cell_y = -1;
static bool ptr_down;
static double ptr_x, ptr_y;

static const char *MIME_UTF8 = "text/plain;charset=utf-8";
static const char *MIME_PLAIN = "text/plain";

App app;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void ptywrite(const char *s, size_t n) {
    int fd = app.term->ptyfd;
    if (fd < 0) return;
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                poll(&p, 1, 100);
                continue;
            }
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

void wl_set_title(const char *title) {
    if (toplevel) xdg_toplevel_set_title(toplevel, title);
}

void wl_shutdown(void) { app.running = false; }
void wl_wake(void) { app.need_draw = true; }

static void apply_blur(void) {
    if (!FX_BLUR || blur_applied || !surface) return;
    struct wl_region *reg = NULL;
    if (FX_BLUR_REGION && compositor) {
        reg = wl_compositor_create_region(compositor);
        wl_region_add(reg, 0, 0, app.width > 0 ? app.width : 1 << 20,
                      app.height > 0 ? app.height : 1 << 20);
    }
#if HAVE_EXTBG
    if (bg_mgr && (bg_caps & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR)) {
        if (!bg_surf) bg_surf = ext_background_effect_manager_v1_get_background_effect(bg_mgr, surface);
        ext_background_effect_surface_v1_set_blur_region(bg_surf, reg);
        blur_applied = true;
    }
#endif
#if HAVE_KBLUR
    if (!blur_applied && kde_blur_mgr) {
        if (!kde_blur) kde_blur = org_kde_kwin_blur_manager_create(kde_blur_mgr, surface);
        org_kde_kwin_blur_set_region(kde_blur, reg);
        org_kde_kwin_blur_commit(kde_blur);
        blur_applied = true;
    }
#endif
    if (reg) wl_region_destroy(reg);
}

static uint32_t mod_from_name(const char *n) {
    if (!strcasecmp(n, "ctrl") || !strcasecmp(n, "control")) return MOD_CTRL;
    if (!strcasecmp(n, "alt") || !strcasecmp(n, "mod1")) return MOD_ALT;
    if (!strcasecmp(n, "shift")) return MOD_SHIFT;
    if (!strcasecmp(n, "super") || !strcasecmp(n, "logo") || !strcasecmp(n, "mod4")) return MOD_LOGO;
    return 0;
}

static void parse_binds(const char *spec, BindSet *out) {
    out->n = 0;
    const char *p = spec;
    while (*p && out->n < MAX_BIND_ALTS) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char item[128];
        if (len >= sizeof item) len = sizeof item - 1;
        memcpy(item, p, len);
        item[len] = 0;

        uint32_t mods = 0;
        char *key = item;
        for (;;) {
            char *plus = strchr(key, '+');
            if (!plus) break;
            *plus = 0;
            uint32_t m = mod_from_name(key);
            if (!m) {
                *plus = '+';
                break;
            }
            mods |= m;
            key = plus + 1;
        }

        xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
        if (sym == XKB_KEY_NoSymbol)
            sym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
        if (sym != XKB_KEY_NoSymbol) {
            out->alt[out->n].mods = mods;
            out->alt[out->n].sym = sym;
            out->n++;
        } else {
            fprintf(stderr, "titty: okänd tangent i bindning: %s\n", key);
        }

        if (!comma) break;
        p = comma + 1;
    }
}

static xkb_keysym_t base_keysym(xkb_keycode_t code) {
    if (!xkb_map || !xkb_st) return XKB_KEY_NoSymbol;
    xkb_layout_index_t layout = xkb_state_key_get_layout(xkb_st, code);
    const xkb_keysym_t *syms = NULL;
    int n = xkb_keymap_key_get_syms_by_level(xkb_map, code, layout, 0, &syms);
    return n > 0 ? syms[0] : XKB_KEY_NoSymbol;
}

static bool bind_match(const BindSet *b, xkb_keysym_t sym, xkb_keysym_t base,
                       bool ctrl, bool alt, bool shift, bool logo) {
    for (int i = 0; i < b->n; i++) {
        uint32_t m = b->alt[i].mods;
        if (b->alt[i].sym != sym && b->alt[i].sym != base) continue;
        if (!!(m & MOD_CTRL) != ctrl) continue;
        if (!!(m & MOD_ALT) != alt) continue;
        if (!!(m & MOD_LOGO) != logo) continue;
        if ((m & MOD_SHIFT) && !shift) continue;
        return true;
    }
    return false;
}

static void spawn_new_window(void) {
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
    for (int fd = 3; fd < 64; fd++) close(fd);
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        char *argv[] = { self, NULL };
        execv(self, argv);
    }
    execlp("titty", "titty", (char *)NULL);
    _exit(127);
}

static void regrid(void) {
    int cw = font_cell_w(), ch = font_cell_h();
    int cols = (app.width - 2 * INNER_BORDER) / cw;
    int rows = (app.height - 2 * INNER_BORDER) / ch;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols != app.cols || rows != app.rows) {
        app.cols = cols;
        app.rows = rows;
        term_resize(app.term, cols, rows);
    }
    pty_resize(app.term->ptyfd, cols, rows, cols * cw, rows * ch);
    app.term->dirty = true;
    app.need_draw = true;
}

static void change_font_size(double delta) {
    if (!font_set_size(font_size() + delta)) return;
    regrid();
}

static void write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void src_send(void *d, struct wl_data_source *src, const char *mime, int32_t fd) {
    if (clip_text) write_all(fd, clip_text, clip_len);
    close(fd);
}
static void src_cancelled(void *d, struct wl_data_source *src) {
    wl_data_source_destroy(src);
    if (src == dsrc) dsrc = NULL;
}
static void src_target(void *d, struct wl_data_source *s2, const char *m) {}
static void src_dnd_drop(void *d, struct wl_data_source *s2) {}
static void src_dnd_finished(void *d, struct wl_data_source *s2) {}
static void src_action(void *d, struct wl_data_source *s2, uint32_t a) {}
static const struct wl_data_source_listener src_listener = {
    .target = src_target,
    .send = src_send,
    .cancelled = src_cancelled,
    .dnd_drop_performed = src_dnd_drop,
    .dnd_finished = src_dnd_finished,
    .action = src_action,
};

static void copy_clipboard(const char *text, size_t len) {
    if (!ddm || !ddev || !text || !len) return;
    free(clip_text);
    clip_text = malloc(len + 1);
    if (!clip_text) return;
    memcpy(clip_text, text, len);
    clip_text[len] = 0;
    clip_len = len;

    if (dsrc) wl_data_source_destroy(dsrc);
    dsrc = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(dsrc, &src_listener, NULL);
    wl_data_source_offer(dsrc, MIME_UTF8);
    wl_data_source_offer(dsrc, MIME_PLAIN);
    wl_data_source_offer(dsrc, "UTF8_STRING");
    wl_data_source_offer(dsrc, "TEXT");
    wl_data_device_set_selection(ddev, dsrc, last_serial);
}

#if HAVE_PRIMSEL
static void psrc_send(void *d, struct zwp_primary_selection_source_v1 *s2,
                      const char *mime, int32_t fd) {
    if (prim_text) write_all(fd, prim_text, prim_len);
    close(fd);
}
static void psrc_cancelled(void *d, struct zwp_primary_selection_source_v1 *s2) {
    zwp_primary_selection_source_v1_destroy(s2);
    if (s2 == pssrc) pssrc = NULL;
}
static const struct zwp_primary_selection_source_v1_listener psrc_listener = {
    .send = psrc_send,
    .cancelled = psrc_cancelled,
};

static void copy_primary(const char *text, size_t len) {
    if (!psm || !psdev || !text || !len) return;
    free(prim_text);
    prim_text = malloc(len + 1);
    if (!prim_text) return;
    memcpy(prim_text, text, len);
    prim_text[len] = 0;
    prim_len = len;

    if (pssrc) zwp_primary_selection_source_v1_destroy(pssrc);
    pssrc = zwp_primary_selection_device_manager_v1_create_source(psm);
    zwp_primary_selection_source_v1_add_listener(pssrc, &psrc_listener, NULL);
    zwp_primary_selection_source_v1_offer(pssrc, MIME_UTF8);
    zwp_primary_selection_source_v1_offer(pssrc, MIME_PLAIN);
    zwp_primary_selection_source_v1_offer(pssrc, "UTF8_STRING");
    zwp_primary_selection_device_v1_set_selection(psdev, pssrc, last_serial);
}
#endif

static void paste_from_fd(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    char buf[8192];
    char *acc = NULL;
    size_t n = 0;
    double deadline = now_sec() + 2.0;

    for (;;) {
        wl_display_flush(dpy);
        int timeout = (int)((deadline - now_sec()) * 1000.0);
        if (timeout <= 0) break;

        struct pollfd fds[2];
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[1].fd = wl_display_get_fd(dpy);
        fds[1].events = POLLIN;

        int pr = poll(fds, 2, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) break;

        if (fds[1].revents & POLLIN) {
            if (wl_display_dispatch(dpy) < 0) break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r > 0) {
                char *na = realloc(acc, n + (size_t)r + 1);
                if (!na) break;
                acc = na;
                memcpy(acc + n, buf, (size_t)r);
                n += (size_t)r;
                continue;
            }
            if (r == 0) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
    }
    close(fd);

    if (!acc || !n) { free(acc); return; }
    acc[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (acc[i] == '\n') acc[i] = '\r';

    if (app.term->bracketed_paste) ptywrite("\033[200~", 6);
    ptywrite(acc, n);
    if (app.term->bracketed_paste) ptywrite("\033[201~", 6);
    free(acc);
}

static void do_paste_now(bool primary) {
    int fds[2];
#if HAVE_PRIMSEL
    if (primary) {
        if (!psoffer) return;
        if (pipe(fds) < 0) return;
        zwp_primary_selection_offer_v1_receive(psoffer, MIME_UTF8, fds[1]);
        close(fds[1]);
        wl_display_flush(dpy);
        paste_from_fd(fds[0]);
        return;
    }
#else
    (void)primary;
#endif
    if (!doffer) return;
    if (pipe(fds) < 0) return;
    wl_data_offer_receive(doffer, MIME_UTF8, fds[1]);
    close(fds[1]);
    wl_display_flush(dpy);
    paste_from_fd(fds[0]);
}

static void do_paste(bool primary) { paste_pending = primary ? 2 : 1; }

static void copy_selection(bool also_clipboard) {
    size_t n = 0;
    char *txt = term_sel_text(app.term, &n);
    if (!txt) return;
    if (n) {
#if HAVE_PRIMSEL
        copy_primary(txt, n);
#endif
        if (also_clipboard) copy_clipboard(txt, n);
    }
    free(txt);
}

static void resize_to(int w, int h) {
    if (w < 1 || h < 1) return;
    if (w == app.width && h == app.height) return;
    app.width = w;
    app.height = h;
    if (egl_win) wl_egl_window_resize(egl_win, w, h, 0, 0);
    render_resize(w, h);

    regrid();
    blur_applied = false;
    apply_blur();
    app.need_draw = true;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t) {
    wl_callback_destroy(cb);
    frame_cb = NULL;
    frame_pending = false;
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void draw(bool force) {
    if (!configured || !gl_ready) return;
    if (frame_pending && !force) return;
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);
    app.now = now_sec();
    render_frame(app.term, app.now, app.focused);

    if (frame_cb) wl_callback_destroy(frame_cb);
    frame_cb = wl_surface_frame(surface);
    wl_callback_add_listener(frame_cb, &frame_listener, NULL);
    frame_pending = true;

    if (BG_OPACITY >= 1.0 && !FX_FROST) {
        struct wl_region *op = wl_compositor_create_region(compositor);
        wl_region_add(op, 0, 0, app.width, app.height);
        wl_surface_set_opaque_region(surface, op);
        wl_region_destroy(op);
    } else {
        wl_surface_set_opaque_region(surface, NULL);
    }

    eglSwapBuffers(egl_dpy, egl_surf);
    app.need_draw = false;
}

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    if (pending_w > 0 && pending_h > 0) resize_to(pending_w, pending_h);
    configured = true;
    app.need_draw = true;
    draw(true);
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };

static void toplevel_configure(void *d, struct xdg_toplevel *tl, int32_t w, int32_t h,
                               struct wl_array *states) {
    bool act = false;
    uint32_t *st;
    wl_array_for_each(st, states)
        if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) act = true;
    if (act != app.focused) {
        app.focused = act;
        app.need_draw = true;
    }
    if (w > 0 && h > 0) {
        pending_w = w;
        pending_h = h;
    }
}

static void toplevel_close(void *d, struct xdg_toplevel *tl) { app.running = false; }

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *dd, uint32_t mode) {}
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = { .configure = deco_configure };

#if HAVE_EXTBG
static void bg_capabilities(void *d, struct ext_background_effect_manager_v1 *m, uint32_t flags) {
    bg_caps = flags;
    blur_applied = false;
    apply_blur();
}
static const struct ext_background_effect_manager_v1_listener bg_listener = {
    .capabilities = bg_capabilities
};
#endif

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    struct xkb_keymap *nm = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!nm) return;
    if (xkb_st) xkb_state_unref(xkb_st);
    if (xkb_map) xkb_keymap_unref(xkb_map);
    xkb_map = nm;
    xkb_st = xkb_state_new(xkb_map);
    mod_ctrl = xkb_keymap_mod_get_index(xkb_map, XKB_MOD_NAME_CTRL);
    mod_alt = xkb_keymap_mod_get_index(xkb_map, XKB_MOD_NAME_ALT);
    mod_shift = xkb_keymap_mod_get_index(xkb_map, XKB_MOD_NAME_SHIFT);
    mod_logo = xkb_keymap_mod_get_index(xkb_map, XKB_MOD_NAME_LOGO);
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                     struct wl_array *keys) {
    last_serial = serial;
    app.focused = true;
    app.need_draw = true;
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    app.focused = false;
    repeat_armed = false;
    if (repeat_fd >= 0) {
        struct itimerspec its = { 0 };
        timerfd_settime(repeat_fd, 0, &its, NULL);
    }
    app.need_draw = true;
}

static void send_key(xkb_keysym_t sym, uint32_t code, bool ctrl, bool alt, bool shift);

static void arm_repeat(uint32_t key) {
    if (repeat_fd < 0 || repeat_rate <= 0) return;
    repeat_key = key;
    repeat_armed = true;
    struct itimerspec its = {
        .it_value = { .tv_sec = repeat_delay / 1000,
                      .tv_nsec = (long)(repeat_delay % 1000) * 1000000L },
        .it_interval = { .tv_sec = 0, .tv_nsec = 1000000000L / repeat_rate }
    };
    timerfd_settime(repeat_fd, 0, &its, NULL);
}

static void disarm_repeat(void) {
    repeat_armed = false;
    if (repeat_fd >= 0) {
        struct itimerspec its = { 0 };
        timerfd_settime(repeat_fd, 0, &its, NULL);
    }
}

static bool is_modifier_sym(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L: case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L: case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L: case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L: case XKB_KEY_Hyper_R:
    case XKB_KEY_Caps_Lock: case XKB_KEY_Shift_Lock:
    case XKB_KEY_Num_Lock: case XKB_KEY_Scroll_Lock:
    case XKB_KEY_ISO_Level3_Shift: case XKB_KEY_ISO_Level5_Shift:
        return true;
    default:
        return false;
    }
}

static void handle_key(uint32_t key) {
    if (!xkb_st) return;
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, code);
    if (is_modifier_sym(sym)) return;
    bool ctrl = xkb_state_mod_index_is_active(xkb_st, mod_ctrl, XKB_STATE_MODS_EFFECTIVE);
    bool alt = xkb_state_mod_index_is_active(xkb_st, mod_alt, XKB_STATE_MODS_EFFECTIVE);
    bool shift = xkb_state_mod_index_is_active(xkb_st, mod_shift, XKB_STATE_MODS_EFFECTIVE);
    bool logo = xkb_state_mod_index_is_active(xkb_st, mod_logo, XKB_STATE_MODS_EFFECTIVE);
    xkb_keysym_t base = base_keysym(code);

    if (bind_match(&bind_font_inc, sym, base, ctrl, alt, shift, logo)) {
        change_font_size(FONT_SIZE_STEP);
        return;
    }
    if (bind_match(&bind_font_dec, sym, base, ctrl, alt, shift, logo)) {
        change_font_size(-(double)FONT_SIZE_STEP);
        return;
    }
    if (bind_match(&bind_new_window, sym, base, ctrl, alt, shift, logo)) {
        spawn_new_window();
        return;
    }
    if (bind_match(&bind_copy, sym, base, ctrl, alt, shift, logo)) {
        if (term_sel_active(app.term)) copy_selection(true);
        return;
    }
    if (bind_match(&bind_paste, sym, base, ctrl, alt, shift, logo)) {
        do_paste(false);
        return;
    }
    if (bind_match(&bind_fx, sym, base, ctrl, alt, shift, logo)) {
        render_toggle_fx();
        app.need_draw = true;
        return;
    }
    if (shift && sym == XKB_KEY_Insert) {
        do_paste(true);
        return;
    }

    send_key(sym, code, ctrl, alt, shift);
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state) {
    last_serial = serial;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (repeat_armed && repeat_key == key) disarm_repeat();
        return;
    }
    handle_key(key);
    if (xkb_map && xkb_keymap_key_repeats(xkb_map, key + 8)) arm_repeat(key);
    else disarm_repeat();
}

static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                         uint32_t lat, uint32_t lock, uint32_t group) {
    last_serial = serial;
    if (xkb_st) xkb_state_update_mask(xkb_st, dep, lat, lock, 0, 0, group);
}

static void kb_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    repeat_rate = rate;
    repeat_delay = delay;
}

static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

static bool pixel_to_cell(double px, double py, int *cx, int *cy) {
    int cw = font_cell_w(), ch = font_cell_h();
    int ox = INNER_BORDER + (app.width - 2 * INNER_BORDER - app.cols * cw) / 2;
    int oy = INNER_BORDER + (app.height - 2 * INNER_BORDER - app.rows * ch) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    double fx = (px - ox) / cw;
    double fy = (py - oy) / ch;
    int x = (int)floor(fx);
    int y = (int)floor(fy);
    if (x < 0) x = 0;
    if (x >= app.cols) x = app.cols - 1;
    if (y < 0) y = 0;
    if (y >= app.rows) y = app.rows - 1;
    *cx = x;
    *cy = y;
    return true;
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                      wl_fixed_t x, wl_fixed_t y) {
    last_serial = serial;
    ptr_x = wl_fixed_to_double(x);
    ptr_y = wl_fixed_to_double(y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    last_serial = serial;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    ptr_x = wl_fixed_to_double(x);
    ptr_y = wl_fixed_to_double(y);
    if (!ptr_down || !app.term->sel_dragging) return;
    int cx, cy;
    pixel_to_cell(ptr_x, ptr_y, &cx, &cy);
    term_sel_extend(app.term, cx, term_abs_row(app.term, cy));
    app.need_draw = true;
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
                       uint32_t button, uint32_t state) {
    last_serial = serial;
    bool press = (state == WL_POINTER_BUTTON_STATE_PRESSED);

    if (button == 0x110) {
        int cx, cy;
        pixel_to_cell(ptr_x, ptr_y, &cx, &cy);
        if (press) {
            ptr_down = true;
            double now = now_sec();
            if (now - last_click_time < 0.4 && cx == last_click_cell_x && cy == last_click_cell_y)
                click_count = click_count % 3 + 1;
            else
                click_count = 1;
            last_click_time = now;
            last_click_cell_x = cx;
            last_click_cell_y = cy;

            int mode = click_count == 1 ? SEL_CHAR : click_count == 2 ? SEL_WORD : SEL_LINE;
            term_sel_start(app.term, cx, term_abs_row(app.term, cy), mode);
        } else {
            ptr_down = false;
            app.term->sel_dragging = false;
            if (term_sel_active(app.term)) copy_selection(false);
        }
        app.need_draw = true;
        return;
    }

    if (button == 0x112 && press) {
        do_paste(true);
        return;
    }

    if (button == 0x111 && press) {
        int cx, cy;
        pixel_to_cell(ptr_x, ptr_y, &cx, &cy);
        if (term_sel_active(app.term)) {
            term_sel_extend(app.term, cx, term_abs_row(app.term, cy));
            copy_selection(false);
            app.need_draw = true;
        }
    }
}

static void data_offer_offer(void *d, struct wl_data_offer *o, const char *mime) {}
static void data_offer_source_actions(void *d, struct wl_data_offer *o, uint32_t a) {}
static void data_offer_action(void *d, struct wl_data_offer *o, uint32_t a) {}
static const struct wl_data_offer_listener offer_listener = {
    .offer = data_offer_offer,
    .source_actions = data_offer_source_actions,
    .action = data_offer_action,
};

static void dd_data_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o) {
    wl_data_offer_add_listener(o, &offer_listener, NULL);
}
static void dd_selection(void *d, struct wl_data_device *dev, struct wl_data_offer *o) {
    if (doffer) wl_data_offer_destroy(doffer);
    doffer = o;
}
static void dd_enter(void *d, struct wl_data_device *dev, uint32_t s2, struct wl_surface *sf,
                     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {}
static void dd_leave(void *d, struct wl_data_device *dev) {}
static void dd_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void dd_drop(void *d, struct wl_data_device *dev) {}
static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

#if HAVE_PRIMSEL
static void ps_offer_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *m) {}
static const struct zwp_primary_selection_offer_v1_listener ps_offer_listener = {
    .offer = ps_offer_offer,
};
static void psd_data_offer(void *d, struct zwp_primary_selection_device_v1 *dev,
                           struct zwp_primary_selection_offer_v1 *o) {
    zwp_primary_selection_offer_v1_add_listener(o, &ps_offer_listener, NULL);
}
static void psd_selection(void *d, struct zwp_primary_selection_device_v1 *dev,
                          struct zwp_primary_selection_offer_v1 *o) {
    if (psoffer) zwp_primary_selection_offer_v1_destroy(psoffer);
    psoffer = o;
}
static const struct zwp_primary_selection_device_v1_listener psd_listener = {
    .data_offer = psd_data_offer,
    .selection = psd_selection,
};
#endif

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    double v = wl_fixed_to_double(value);
    int lines = (int)(v / 10.0);
    if (lines == 0) lines = v > 0 ? 1 : -1;
    if (app.term->alt) {
        const char *seq = lines < 0 ? (app.term->app_cursor ? "\033OA" : "\033[A")
                                    : (app.term->app_cursor ? "\033OB" : "\033[B");
        int n = lines < 0 ? -lines : lines;
        if (n > 10) n = 10;
        for (int i = 0; i < n * 3; i++) ptywrite(seq, 3);
    } else {
        term_scroll_view(app.term, -lines * 3);
        app.need_draw = true;
    }
}

static void ptr_frame(void *d, struct wl_pointer *p) {}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src) {}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis) {}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t disc) {}

static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

static void setup_data_devices(void) {
    if (!seat) return;
    if (ddm && !ddev) {
        ddev = wl_data_device_manager_get_data_device(ddm, seat);
        wl_data_device_add_listener(ddev, &dd_listener, NULL);
    }
#if HAVE_PRIMSEL
    if (psm && !psdev) {
        psdev = zwp_primary_selection_device_manager_v1_get_device(psm, seat);
        zwp_primary_selection_device_v1_add_listener(psdev, &psd_listener, NULL);
    }
#endif
}

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &ptr_listener, NULL);
    }
    setup_data_devices();
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 3 ? ver : 3);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 7 ? ver : 7);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, ver < 3 ? ver : 3);
#if HAVE_PRIMSEL
    } else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) {
        psm = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
#endif
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
#if HAVE_EXTBG
    } else if (!strcmp(iface, ext_background_effect_manager_v1_interface.name)) {
        bg_mgr = wl_registry_bind(r, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(bg_mgr, &bg_listener, NULL);
#endif
#if HAVE_KBLUR
    } else if (!strcmp(iface, org_kde_kwin_blur_manager_interface.name)) {
        kde_blur_mgr = wl_registry_bind(r, name, &org_kde_kwin_blur_manager_interface, 1);
#endif
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

static void keywrite(const char *b, size_t n) {
    Term *t = app.term;
    if (t->scroll_off) { t->scroll_off = 0; t->dirty = true; app.need_draw = true; }
    if (term_sel_active(t)) { term_sel_clear(t); app.need_draw = true; }
    ptywrite(b, n);
}

static void send_key(xkb_keysym_t sym, uint32_t code, bool ctrl, bool alt, bool shift) {
    Term *t = app.term;
    char buf[64];
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);

    if (shift && sym == XKB_KEY_Page_Up) { term_scroll_view(t, t->rows - 1); app.need_draw = true; return; }
    if (shift && sym == XKB_KEY_Page_Down) { term_scroll_view(t, -(t->rows - 1)); app.need_draw = true; return; }
    if (shift && ctrl && (sym == XKB_KEY_Up || sym == XKB_KEY_K || sym == XKB_KEY_k)) {
        term_scroll_view(t, 1); app.need_draw = true; return;
    }
    if (shift && ctrl && (sym == XKB_KEY_Down || sym == XKB_KEY_J || sym == XKB_KEY_j)) {
        term_scroll_view(t, -1); app.need_draw = true; return;
    }

    const char *cur = NULL;
    switch (sym) {
    case XKB_KEY_Up: cur = "A"; break;
    case XKB_KEY_Down: cur = "B"; break;
    case XKB_KEY_Right: cur = "C"; break;
    case XKB_KEY_Left: cur = "D"; break;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: cur = "H"; break;
    case XKB_KEY_End: case XKB_KEY_KP_End: cur = "F"; break;
    default: break;
    }
    if (cur) {
        if (mod > 1) snprintf(buf, sizeof buf, "\033[1;%d%s", mod, cur);
        else if (t->app_cursor) snprintf(buf, sizeof buf, "\033O%s", cur);
        else snprintf(buf, sizeof buf, "\033[%s", cur);
        keywrite(buf, strlen(buf));
        return;
    }

    int tilde = 0;
    switch (sym) {
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: tilde = 2; break;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: tilde = 3; break;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: tilde = 5; break;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: tilde = 6; break;
    case XKB_KEY_F5: tilde = 15; break;
    case XKB_KEY_F6: tilde = 17; break;
    case XKB_KEY_F7: tilde = 18; break;
    case XKB_KEY_F8: tilde = 19; break;
    case XKB_KEY_F9: tilde = 20; break;
    case XKB_KEY_F10: tilde = 21; break;
    case XKB_KEY_F11: tilde = 23; break;
    case XKB_KEY_F12: tilde = 24; break;
    default: break;
    }
    if (tilde) {
        if (mod > 1) snprintf(buf, sizeof buf, "\033[%d;%d~", tilde, mod);
        else snprintf(buf, sizeof buf, "\033[%d~", tilde);
        keywrite(buf, strlen(buf));
        return;
    }

    const char *fk = NULL;
    switch (sym) {
    case XKB_KEY_F1: fk = "P"; break;
    case XKB_KEY_F2: fk = "Q"; break;
    case XKB_KEY_F3: fk = "R"; break;
    case XKB_KEY_F4: fk = "S"; break;
    default: break;
    }
    if (fk) {
        if (mod > 1) snprintf(buf, sizeof buf, "\033[1;%d%s", mod, fk);
        else snprintf(buf, sizeof buf, "\033O%s", fk);
        keywrite(buf, strlen(buf));
        return;
    }

    switch (sym) {
    case XKB_KEY_BackSpace:
        if (alt) keywrite(ctrl ? "\033\010" : "\033\177", 2);
        else keywrite(ctrl ? "\010" : "\177", 1);
        return;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
        if (alt) keywrite("\033\r", 2);
        else keywrite("\r", 1);
        return;
    case XKB_KEY_Tab:
        keywrite("\t", 1);
        return;
    case XKB_KEY_ISO_Left_Tab:
        keywrite("\033[Z", 3);
        return;
    case XKB_KEY_Escape:
        keywrite("\033", 1);
        return;
    default: break;
    }

    char utf[16];
    int n = xkb_state_key_get_utf8(xkb_st, code, utf, sizeof utf);
    if (n <= 0) return;

    if (ctrl && n == 1) {
        unsigned char c = (unsigned char)utf[0];
        if (c >= 'a' && c <= 'z') utf[0] = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') utf[0] = (char)(c - 'A' + 1);
        else if (c == ' ' || c == '@') utf[0] = 0;
        else if (c == '[') utf[0] = 27;
        else if (c == '\\') utf[0] = 28;
        else if (c == ']') utf[0] = 29;
        else if (c == '^') utf[0] = 30;
        else if (c == '_' || c == '/') utf[0] = 31;
        else if (c == '?') utf[0] = 127;
    }

    if (alt) {
        keywrite("\033", 1);
        keywrite(utf, (size_t)n);
    } else {
        keywrite(utf, (size_t)n);
    }
}

static bool egl_setup(void) {
    egl_dpy = eglGetDisplay((EGLNativeDisplayType)dpy);
    if (egl_dpy == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(egl_dpy, NULL, NULL)) return false;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n = 0;
    EGLConfig cfgs[64];
    if (!eglChooseConfig(egl_dpy, cfg_attr, cfgs, 64, &n) || n == 0) return false;

    egl_cfg = cfgs[0];
    for (EGLint i = 0; i < n; i++) {
        EGLint a = 0, r = 0, g = 0, b = 0;
        eglGetConfigAttrib(egl_dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(egl_dpy, cfgs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(egl_dpy, cfgs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(egl_dpy, cfgs[i], EGL_BLUE_SIZE, &b);
        if (a == 8 && r == 8 && g == 8 && b == 8) {
            egl_cfg = cfgs[i];
            break;
        }
    }

    const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT) return false;
    return true;
}

void wl_run(App *a) {
    dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "titty: cannot connect to a wayland compositor\n");
        exit(1);
    }
    registry = wl_display_get_registry(dpy);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    if (!compositor || !wm_base) {
        fprintf(stderr, "titty: compositor lacks wl_compositor or xdg_wm_base\n");
        exit(1);
    }

    setup_data_devices();

    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    parse_binds(BIND_FONT_INC, &bind_font_inc);
    parse_binds(BIND_FONT_DEC, &bind_font_dec);
    parse_binds(BIND_NEW_WINDOW, &bind_new_window);
    parse_binds(BIND_COPY, &bind_copy);
    parse_binds(BIND_PASTE, &bind_paste);
    parse_binds(BIND_TOGGLE_CRT, &bind_fx);
    repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (!egl_setup()) {
        fprintf(stderr, "titty: EGL initialisation failed\n");
        exit(1);
    }

    surface = wl_compositor_create_surface(compositor);
    xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, WINDOW_TITLE);
    xdg_toplevel_set_app_id(toplevel, WINDOW_APP_ID);

    if (deco_mgr && SERVER_DECORATION) {
        deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr, toplevel);
        zxdg_toplevel_decoration_v1_add_listener(deco, &deco_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    int w = a->cols * font_cell_w() + 2 * INNER_BORDER;
    int h = a->rows * font_cell_h() + 2 * INNER_BORDER;

    wl_surface_commit(surface);
    wl_display_roundtrip(dpy);

    egl_win = wl_egl_window_create(surface, w, h);
    egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg, (EGLNativeWindowType)egl_win, NULL);
    if (egl_surf == EGL_NO_SURFACE) {
        fprintf(stderr, "titty: cannot create EGL window surface\n");
        exit(1);
    }
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);
    eglSwapInterval(egl_dpy, 0);

    if (!font_init()) exit(1);
    render_init();

    app.term->ptyfd = pty_spawn(app.cols, app.rows, app.cmd);
    if (app.term->ptyfd < 0) exit(1);

    app.width = w;
    app.height = h;
    render_resize(w, h);
    gl_ready = true;
    apply_blur();

    if (pending_w > 0 && pending_h > 0) resize_to(pending_w, pending_h);
    app.need_draw = true;

    uint8_t rbuf[65536];
    char last_title[512] = { 0 };

    while (app.running) {
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);

        struct pollfd fds[3];
        int nfds = 0;
        fds[nfds].fd = wl_display_get_fd(dpy);
        fds[nfds].events = POLLIN;
        int wl_i = nfds++;
        int pty_i = -1, rep_i = -1;
        if (app.term->ptyfd >= 0) {
            fds[nfds].fd = app.term->ptyfd;
            fds[nfds].events = POLLIN;
            pty_i = nfds++;
        }
        if (repeat_fd >= 0) {
            fds[nfds].fd = repeat_fd;
            fds[nfds].events = POLLIN;
            rep_i = nfds++;
        }

        bool want = app.need_draw || app.term->dirty || render_animating();
        int timeout = want ? 0 : -1;
        if (want && frame_pending) timeout = 4;

        int pr = poll(fds, nfds, timeout);
        if (pr < 0 && errno != EINTR) {
            wl_display_cancel_read(dpy);
            break;
        }

        if (fds[wl_i].revents & POLLIN) wl_display_read_events(dpy);
        else wl_display_cancel_read(dpy);
        if (wl_display_dispatch_pending(dpy) < 0) break;

        if (fds[wl_i].revents & (POLLERR | POLLHUP)) break;

        if (pty_i >= 0 && (fds[pty_i].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (int iter = 0; iter < 64; iter++) {
                ssize_t rn = read(app.term->ptyfd, rbuf, sizeof rbuf);
                if (rn > 0) {
                    term_write(app.term, rbuf, (size_t)rn);
                    if ((size_t)rn < sizeof rbuf) break;
                } else if (rn == 0) {
                    app.running = false;
                    break;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN) break;
                    app.running = false;
                    break;
                }
            }
        }

        if (rep_i >= 0 && (fds[rep_i].revents & POLLIN)) {
            uint64_t exp;
            if (read(repeat_fd, &exp, sizeof exp) == (ssize_t)sizeof exp && repeat_armed)
                for (uint64_t i = 0; i < exp && i < 8; i++) handle_key(repeat_key);
        }

        if (paste_pending) {
            int which = paste_pending;
            paste_pending = 0;
            do_paste_now(which == 2);
        }

        if (app.term->bell) {
            app.term->bell = false;
            if (BELL_FLASH) {
                render_bell(now_sec());
                app.need_draw = true;
            }
        }

        if (app.term->title_changed) {
            app.term->title_changed = false;
            if (strcmp(last_title, app.term->title)) {
                snprintf(last_title, sizeof last_title, "%s", app.term->title);
                wl_set_title(last_title);
            }
        }

        app.now = now_sec();
        if (app.need_draw || app.term->dirty || render_animating()) draw(false);
    }

    if (frame_cb) wl_callback_destroy(frame_cb);
    render_fini();
    font_fini();
    if (egl_surf != EGL_NO_SURFACE) eglDestroySurface(egl_dpy, egl_surf);
    if (egl_win) wl_egl_window_destroy(egl_win);
    if (egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy, egl_ctx);
    if (egl_dpy != EGL_NO_DISPLAY) eglTerminate(egl_dpy);
    if (xkb_st) xkb_state_unref(xkb_st);
    if (xkb_map) xkb_keymap_unref(xkb_map);
    if (xkb_ctx) xkb_context_unref(xkb_ctx);
    wl_display_disconnect(dpy);
}
