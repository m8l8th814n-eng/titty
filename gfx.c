#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

#if HAVE_PNG
#include <png.h>
#endif
#if HAVE_ZLIB
#include <zlib.h>
#endif

#define APC_MAX   (256u << 20)
#define GFX_QUOTA ((size_t)320 << 20)
#define IMG_MAX   10000

typedef struct {
    char a, t, o, d;
    int q, f, m, C, z;
    uint32_t i, I, p;
    int s, v, S, O;
    int x, y, w, h, X, Y, c, r;
} Cmd;

static GfxImage **imgs;
static int nimg, capimg;
static GfxPlace *pls;
static int npl, cappl;
static uint32_t next_id = 0x80000000u;
static uint64_t seq;
static size_t total_bytes;

static char *apc;
static size_t apc_len, apc_cap;
static bool apc_over;

static bool chunking;
static Cmd pending;
static char *b64;
static size_t b64_len, b64_cap;

static bool append(char **buf, size_t *len, size_t *cap, const void *src, size_t n) {
    if (*len + n > APC_MAX) return false;
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap : 4096;
        while (nc < *len + n) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return false;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return true;
}

void gfx_apc_begin(void) {
    apc_len = 0;
    apc_over = false;
}

void gfx_apc_put(const uint8_t *p, size_t n) {
    if (apc_over || !n) return;
    if (!append(&apc, &apc_len, &apc_cap, p, n)) apc_over = true;
}

static void reply(Term *t, const char *s) {
    if (t->ptyfd < 0) return;
    ssize_t w = write(t->ptyfd, s, strlen(s));
    (void)w;
}

static void respond(Term *t, const Cmd *c, uint32_t id, const char *err) {
    if (!c->i && !c->I) return;
    if (!err && c->q >= 1) return;
    if (err && c->q >= 2) return;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "\033_Gi=%u", c->i ? c->i : id);
    if (c->I) n += snprintf(buf + n, sizeof buf - n, ",I=%u", c->I);
    if (c->p) n += snprintf(buf + n, sizeof buf - n, ",p=%u", c->p);
    snprintf(buf + n, sizeof buf - n, ";%s\033\\", err ? err : "OK");
    reply(t, buf);
}

static const char *parse_cmd(const char *s, size_t n, Cmd *c, size_t *payload) {
    memset(c, 0, sizeof *c);
    c->a = 't'; c->t = 'd'; c->d = 'a'; c->f = 32;
    size_t k = 0;
    while (k < n && s[k] != ';') {
        char key = s[k++];
        if (k >= n || s[k] != '=') return "EINVAL:bad key";
        k++;
        size_t vs = k;
        while (k < n && s[k] != ',' && s[k] != ';') k++;
        char val[24];
        size_t vl = k - vs < sizeof val - 1 ? k - vs : sizeof val - 1;
        memcpy(val, s + vs, vl);
        val[vl] = 0;
        long iv = strtol(val, NULL, 10);
        uint32_t uv = (uint32_t)strtoul(val, NULL, 10);
        switch (key) {
        case 'a': c->a = val[0]; break;
        case 't': c->t = val[0]; break;
        case 'o': c->o = val[0]; break;
        case 'd': c->d = val[0]; break;
        case 'q': c->q = (int)iv; break;
        case 'f': c->f = (int)iv; break;
        case 'm': c->m = (int)iv; break;
        case 'C': c->C = (int)iv; break;
        case 'z': c->z = (int)iv; break;
        case 'i': c->i = uv; break;
        case 'I': c->I = uv; break;
        case 'p': c->p = uv; break;
        case 's': c->s = (int)iv; break;
        case 'v': c->v = (int)iv; break;
        case 'S': c->S = (int)iv; break;
        case 'O': c->O = (int)iv; break;
        case 'x': c->x = (int)iv; break;
        case 'y': c->y = (int)iv; break;
        case 'w': c->w = (int)iv; break;
        case 'h': c->h = (int)iv; break;
        case 'X': c->X = (int)iv; break;
        case 'Y': c->Y = (int)iv; break;
        case 'c': c->c = (int)iv; break;
        case 'r': c->r = (int)iv; break;
        default: break;
        }
        if (k < n && s[k] == ',') k++;
    }
    *payload = k < n ? k + 1 : n;
    return NULL;
}

static uint8_t *b64_decode(const char *s, size_t n, size_t *out_len) {
    static signed char tab[256];
    if (!tab['B']) {
        memset(tab, -1, sizeof tab);
        const char *al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) tab[(uint8_t)al[i]] = (signed char)i;
    }
    uint8_t *o = malloc(n / 4 * 3 + 4);
    if (!o) return NULL;
    size_t w = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = tab[(uint8_t)s[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o[w++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = w;
    return o;
}

#if HAVE_ZLIB
static uint8_t *inflate_all(const uint8_t *src, size_t n, size_t *out_len) {
    size_t cap = n * 4 + 4096, len = 0;
    uint8_t *o = malloc(cap);
    if (!o) return NULL;
    z_stream zs = {0};
    if (inflateInit(&zs) != Z_OK) { free(o); return NULL; }
    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)n;
    int rc;
    do {
        if (len == cap) {
            if (cap * 2 > APC_MAX * 4) { rc = Z_MEM_ERROR; break; }
            uint8_t *no = realloc(o, cap * 2);
            if (!no) { rc = Z_MEM_ERROR; break; }
            o = no;
            cap *= 2;
        }
        zs.next_out = o + len;
        zs.avail_out = (uInt)(cap - len);
        rc = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
    } while (rc == Z_OK);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) { free(o); return NULL; }
    *out_len = len;
    return o;
}
#endif

static uint8_t *read_path(const Cmd *c, const uint8_t *path_b, size_t plen, size_t *out_len) {
    char path[4096];
    if (plen == 0 || plen >= sizeof path) return NULL;
    memcpy(path, path_b, plen);
    path[plen] = 0;

    int fd = c->t == 's' ? shm_open(path, O_RDONLY, 0) : open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *data = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        size_t off = c->O > 0 ? (size_t)c->O : 0;
        size_t sz = (size_t)st.st_size;
        if (off < sz) {
            size_t want = sz - off;
            if (c->S > 0 && (size_t)c->S < want) want = (size_t)c->S;
            if (want <= APC_MAX * 4 && (data = malloc(want))) {
                size_t got = 0;
                while (got < want) {
                    ssize_t r = pread(fd, data + got, want - got, (off_t)(off + got));
                    if (r <= 0) break;
                    got += (size_t)r;
                }
                if (got != want) { free(data); data = NULL; }
                else *out_len = want;
            }
        }
    }
    close(fd);
    if (c->t == 's') shm_unlink(path);
    else if (c->t == 't' && strstr(path, "tty-graphics-protocol")) unlink(path);
    return data;
}

static const char *load(const Cmd *c, const char *b, size_t blen, uint8_t **out, int *ow, int *oh) {
    *out = NULL;
    size_t n = 0;
    uint8_t *raw = b64_decode(b, blen, &n);
    if (!raw) return "ENOMEM:out of memory";

    if (c->t == 'f' || c->t == 't' || c->t == 's') {
        size_t fl = 0;
        uint8_t *fd = read_path(c, raw, n, &fl);
        free(raw);
        if (!fd) return "EBADF:cannot read file";
        raw = fd;
        n = fl;
    } else if (c->t != 'd') {
        free(raw);
        return "EINVAL:unsupported transmission medium";
    }

    if (c->o == 'z') {
#if HAVE_ZLIB
        size_t zl = 0;
        uint8_t *z = inflate_all(raw, n, &zl);
        free(raw);
        if (!z) return "EINVAL:zlib decompression failed";
        raw = z;
        n = zl;
#else
        free(raw);
        return "EINVAL:built without zlib";
#endif
    }

    if (c->f == 100) {
#if HAVE_PNG
        png_image pi;
        memset(&pi, 0, sizeof pi);
        pi.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&pi, raw, n)) { free(raw); return "EBADPNG:bad png"; }
        pi.format = PNG_FORMAT_RGBA;
        if (pi.width == 0 || pi.height == 0 || pi.width > 16384 || pi.height > 16384) {
            png_image_free(&pi);
            free(raw);
            return "EINVAL:bad image size";
        }
        uint8_t *px = malloc(PNG_IMAGE_SIZE(pi));
        if (!px || !png_image_finish_read(&pi, NULL, px, 0, NULL)) {
            png_image_free(&pi);
            free(px);
            free(raw);
            return "EBADPNG:bad png";
        }
        free(raw);
        *out = px;
        *ow = (int)pi.width;
        *oh = (int)pi.height;
        return NULL;
#else
        free(raw);
        return "EINVAL:built without libpng";
#endif
    }

    if (c->f != 24 && c->f != 32) { free(raw); return "EINVAL:unsupported format"; }
    if (c->s <= 0 || c->v <= 0 || c->s > 16384 || c->v > 16384) { free(raw); return "EINVAL:bad image size"; }
    int bpp = c->f / 8;
    size_t need = (size_t)c->s * c->v * bpp;
    if (n < need) { free(raw); return "ENODATA:insufficient image data"; }

    uint8_t *px = malloc((size_t)c->s * c->v * 4);
    if (!px) { free(raw); return "ENOMEM:out of memory"; }
    if (bpp == 4) {
        memcpy(px, raw, need);
    } else {
        size_t cnt = (size_t)c->s * c->v;
        for (size_t k = 0; k < cnt; k++) {
            px[k * 4 + 0] = raw[k * 3 + 0];
            px[k * 4 + 1] = raw[k * 3 + 1];
            px[k * 4 + 2] = raw[k * 3 + 2];
            px[k * 4 + 3] = 0xff;
        }
    }
    free(raw);
    *out = px;
    *ow = c->s;
    *oh = c->v;
    return NULL;
}

GfxImage *gfx_image(uint32_t id) {
    for (int k = 0; k < nimg; k++)
        if (imgs[k]->id == id) return imgs[k];
    return NULL;
}

static GfxImage *image_by_number(uint32_t number) {
    GfxImage *best = NULL;
    for (int k = 0; k < nimg; k++)
        if (imgs[k]->number == number && (!best || imgs[k]->seq > best->seq)) best = imgs[k];
    return best;
}

int gfx_placements(const GfxPlace **out) {
    *out = pls;
    return npl;
}

static bool has_placement(uint32_t id) {
    for (int k = 0; k < npl; k++)
        if (pls[k].img == id) return true;
    return false;
}

static void remove_placement(int k) {
    pls[k] = pls[--npl];
}

static void free_image(uint32_t id) {
    for (int k = 0; k < nimg; k++) {
        if (imgs[k]->id != id) continue;
        GfxImage *im = imgs[k];
        for (int j = npl - 1; j >= 0; j--)
            if (pls[j].img == id) remove_placement(j);
        if (im->tex) render_free_tex(im->tex);
        total_bytes -= im->bytes;
        free(im->rgba);
        free(im);
        imgs[k] = imgs[--nimg];
        return;
    }
}

static void enforce_quota(uint32_t keep) {
    while ((total_bytes > GFX_QUOTA || nimg > IMG_MAX) && nimg > 1) {
        GfxImage *victim = NULL;
        for (int pass = 0; pass < 2 && !victim; pass++)
            for (int k = 0; k < nimg; k++) {
                GfxImage *im = imgs[k];
                if (im->id == keep || (pass == 0 && has_placement(im->id))) continue;
                if (!victim || im->seq < victim->seq) victim = im;
            }
        if (!victim) return;
        free_image(victim->id);
    }
}

static void place(Term *t, const Cmd *c, GfxImage *im) {
    if (c->p)
        for (int k = npl - 1; k >= 0; k--)
            if (pls[k].img == im->id && pls[k].pid == c->p) remove_placement(k);

    GfxPlace pl = {0};
    pl.img = im->id;
    pl.pid = c->p;
    pl.sx = c->x > 0 && c->x < im->w ? c->x : 0;
    pl.sy = c->y > 0 && c->y < im->h ? c->y : 0;
    pl.sw = c->w > 0 && c->w <= im->w - pl.sx ? c->w : im->w - pl.sx;
    pl.sh = c->h > 0 && c->h <= im->h - pl.sy ? c->h : im->h - pl.sy;

    int cw = font_cell_w(), ch = font_cell_h();
    pl.ox = c->X > 0 && c->X < cw ? c->X : 0;
    pl.oy = c->Y > 0 && c->Y < ch ? c->Y : 0;
    pl.col = t->cx;
    pl.row = t->cy;
    pl.alt = t->alt;
    pl.z = c->z;

    if (c->c > 0 || c->r > 0) {
        pl.fit = true;
        pl.cols = c->c;
        pl.rows = c->r;
        if (!pl.cols) pl.cols = (int)(((double)pl.rows * ch * pl.sw / pl.sh + cw - 1) / cw);
        if (!pl.rows) pl.rows = (int)(((double)pl.cols * cw * pl.sh / pl.sw + ch - 1) / ch);
    } else {
        pl.cols = (pl.sw + pl.ox + cw - 1) / cw;
        pl.rows = (pl.sh + pl.oy + ch - 1) / ch;
    }
    if (pl.cols < 1) pl.cols = 1;
    if (pl.rows < 1) pl.rows = 1;

    if (npl == cappl) {
        int nc = cappl ? cappl * 2 : 16;
        GfxPlace *np = realloc(pls, (size_t)nc * sizeof *np);
        if (!np) return;
        pls = np;
        cappl = nc;
    }
    pls[npl++] = pl;
    t->dirty = true;

    if (!c->C) term_image_advance(t, pl.cols, pl.rows);
}

static void transmit(Term *t, const Cmd *c, const char *data, size_t len) {
    uint8_t *px;
    int w = 0, h = 0;
    const char *err = load(c, data, len, &px, &w, &h);
    if (err) { respond(t, c, 0, err); return; }

    if (c->i) free_image(c->i);
    if (nimg == capimg) {
        int nc = capimg ? capimg * 2 : 16;
        GfxImage **ni = realloc(imgs, (size_t)nc * sizeof *ni);
        if (!ni) { free(px); respond(t, c, 0, "ENOMEM:out of memory"); return; }
        imgs = ni;
        capimg = nc;
    }
    GfxImage *im = calloc(1, sizeof *im);
    if (!im) { free(px); respond(t, c, 0, "ENOMEM:out of memory"); return; }
    im->id = c->i ? c->i : next_id++;
    if (next_id == 0) next_id = 0x80000000u;
    im->number = c->I;
    im->w = w;
    im->h = h;
    im->rgba = px;
    im->bytes = (size_t)w * h * 4;
    im->seq = ++seq;
    imgs[nimg++] = im;
    total_bytes += im->bytes;
    enforce_quota(im->id);

    if (c->a == 'T') place(t, c, im);
    respond(t, c, im->id, NULL);
}

static bool hits_cell(const GfxPlace *p, int x, int y) {
    return x >= p->col && x < p->col + p->cols && y >= p->row && y < p->row + p->rows;
}

static void delete_cmd(Term *t, const Cmd *c) {
    char d = c->d ? c->d : 'a';
    bool free_data = isupper((unsigned char)d);
    d = (char)tolower((unsigned char)d);

    GfxImage *target = NULL;
    if (d == 'i') target = gfx_image(c->i);
    else if (d == 'n') target = image_by_number(c->I);

    uint32_t touched[64];
    int ntouched = 0;
    for (int k = npl - 1; k >= 0; k--) {
        GfxPlace *p = &pls[k];
        if (p->alt != t->alt && d != 'i' && d != 'n' && d != 'r') continue;
        bool hit = false;
        switch (d) {
        case 'a': hit = p->row + p->rows > 0 && p->row < t->rows; break;
        case 'i': case 'n': hit = target && p->img == target->id && (!c->p || p->pid == c->p); break;
        case 'c': hit = hits_cell(p, t->cx, t->cy); break;
        case 'p': hit = hits_cell(p, c->x - 1, c->y - 1); break;
        case 'q': hit = hits_cell(p, c->x - 1, c->y - 1) && p->z == c->z; break;
        case 'x': hit = c->x - 1 >= p->col && c->x - 1 < p->col + p->cols; break;
        case 'y': hit = c->y - 1 >= p->row && c->y - 1 < p->row + p->rows; break;
        case 'z': hit = p->z == c->z; break;
        case 'r': hit = p->img >= (uint32_t)c->x && p->img <= (uint32_t)c->y; break;
        default: break;
        }
        if (!hit) continue;
        if (ntouched < 64) touched[ntouched++] = p->img;
        remove_placement(k);
    }
    if (free_data) {
        if (target && !has_placement(target->id)) free_image(target->id);
        for (int k = 0; k < ntouched; k++)
            if (!has_placement(touched[k])) free_image(touched[k]);
    }
    t->dirty = true;
}

static void run(Term *t, const Cmd *c, const char *data, size_t len) {
    switch (c->a) {
    case 'q': {
        uint8_t *px;
        int w, h;
        const char *err = load(c, data, len, &px, &w, &h);
        free(px);
        respond(t, c, c->i, err);
        break;
    }
    case 't': case 'T':
        transmit(t, c, data, len);
        break;
    case 'p': {
        GfxImage *im = c->i ? gfx_image(c->i) : image_by_number(c->I);
        if (!im) { respond(t, c, 0, "ENOENT:image not found"); break; }
        place(t, c, im);
        respond(t, c, im->id, NULL);
        break;
    }
    case 'd':
        delete_cmd(t, c);
        break;
    default:
        respond(t, c, 0, "EINVAL:unsupported action");
        break;
    }
}

void gfx_apc_end(Term *t) {
    if (apc_len < 1 || apc[0] != 'G') return;
    if (apc_over) {
        chunking = false;
        b64_len = 0;
        return;
    }

    Cmd c;
    size_t pay;
    const char *err = parse_cmd(apc + 1, apc_len - 1, &c, &pay);
    pay += 1;
    if (err) { respond(t, &c, 0, err); return; }

    if (chunking) {
        if (!append(&b64, &b64_len, &b64_cap, apc + pay, apc_len - pay)) {
            chunking = false;
            b64_len = 0;
            respond(t, &pending, 0, "EFBIG:image too large");
            return;
        }
        if (c.m) return;
        chunking = false;
        if (c.q) pending.q = c.q;
        run(t, &pending, b64, b64_len);
        b64_len = 0;
        return;
    }

    if (c.m && (c.a == 't' || c.a == 'T' || c.a == 'q')) {
        pending = c;
        chunking = true;
        b64_len = 0;
        append(&b64, &b64_len, &b64_cap, apc + pay, apc_len - pay);
        return;
    }
    run(t, &c, apc + pay, apc_len - pay);
}

void gfx_scroll(Term *t, int top, int bot, int n) {
    bool to_sb = !t->alt && top == 0 && n > 0;
    for (int k = npl - 1; k >= 0; k--) {
        GfxPlace *p = &pls[k];
        if (p->alt != t->alt) continue;
        if (p->row > bot || (p->row < top && !to_sb)) continue;
        p->row -= n;
        bool gone = to_sb ? p->row + p->rows <= -t->sb_cap
                          : (n > 0 ? p->row + p->rows <= top : p->row > bot);
        if (gone) remove_placement(k);
    }
}

void gfx_clear(Term *t, bool alt) {
    for (int k = npl - 1; k >= 0; k--)
        if (pls[k].alt == alt) remove_placement(k);
    t->dirty = true;
}

void gfx_resize(Term *t, int shift) {
    for (int k = npl - 1; k >= 0; k--) {
        GfxPlace *p = &pls[k];
        p->row -= shift;
        if (p->row + p->rows <= 0 || p->row >= t->rows) remove_placement(k);
    }
}

void gfx_reset(void) {
    while (nimg) free_image(imgs[0]->id);
    npl = 0;
    chunking = false;
    b64_len = 0;
}
