#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GLES3/gl3.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_OUTLINE_H
#include <fontconfig/fontconfig.h>

#include "common.h"

#define ATLAS_SIZE GLYPH_ATLAS_SIZE
#define CACHE_BITS 14
#define CACHE_SIZE (1 << CACHE_BITS)
#define MAX_FALLBACK 32

typedef struct {
    uint32_t key;
    Glyph g;
    bool used;
} CacheEnt;

static FT_Library ft;
static FT_Face faces[4];
static FT_Face fallback[MAX_FALLBACK];
static int fallback_n;
static FcPattern *fb_pat[MAX_FALLBACK];

static GLuint atlas;
static bool lcd_ok;
bool font_subpixel(void) { return FONT_AA == 2 && lcd_ok; }
static int shelf_x, shelf_y, shelf_h;
static CacheEnt cache[CACHE_SIZE];
static int cell_w, cell_h, baseline, ul_pos, ul_thick;
static double cur_size = FONT_SIZE;
static int glyph_adv;

static char *fc_resolve(const char *spec, int *index) {
    if (strchr(spec, '/')) {
        *index = 0;
        return strdup(spec);
    }
    FcPattern *pat = FcNameParse((const FcChar8 *)spec);
    if (!pat) return NULL;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!m) return NULL;
    FcChar8 *file = NULL;
    char *out = NULL;
    if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file)
        out = strdup((const char *)file);
    int idx = 0;
    FcPatternGetInteger(m, FC_INDEX, 0, &idx);
    *index = idx;
    FcPatternDestroy(m);
    return out;
}

static bool load_face(const char *spec, FT_Face *out) {
    int index = 0;
    char *path = fc_resolve(spec, &index);
    if (!path) return false;
    FT_Error err = FT_New_Face(ft, path, index, out);
    free(path);
    if (err) return false;
    FT_Set_Char_Size(*out, 0, (FT_F26Dot6)(cur_size * 64.0), (FT_UInt)FONT_DPI, (FT_UInt)FONT_DPI);
    return true;
}

static FT_Face fallback_for(uint32_t cp) {
    for (int i = 0; i < fallback_n; i++)
        if (FT_Get_Char_Index(fallback[i], cp)) return fallback[i];
    if (fallback_n >= MAX_FALLBACK) return NULL;

    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPattern *pat = FcPatternCreate();
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    if (!m) return NULL;

    FcChar8 *file = NULL;
    int index = 0;
    if (FcPatternGetString(m, FC_FILE, 0, &file) != FcResultMatch || !file) {
        FcPatternDestroy(m);
        return NULL;
    }
    FcPatternGetInteger(m, FC_INDEX, 0, &index);

    FT_Face face = NULL;
    if (FT_New_Face(ft, (const char *)file, index, &face)) {
        FcPatternDestroy(m);
        return NULL;
    }
    if (FT_Set_Char_Size(face, 0, (FT_F26Dot6)(cur_size * 64.0), (FT_UInt)FONT_DPI, (FT_UInt)FONT_DPI)) {
        if (face->num_fixed_sizes > 0) FT_Select_Size(face, 0);
    }
    fb_pat[fallback_n] = m;
    fallback[fallback_n] = face;
    return fallback[fallback_n++];
}

static void compute_metrics(void) {
    FT_Face f = faces[0];
    int adv = 0;
    FT_UInt gi = FT_Get_Char_Index(f, 'M');
    if (gi && !FT_Load_Glyph(f, gi, FT_LOAD_DEFAULT)) adv = (int)(f->glyph->metrics.horiAdvance >> 6);
    if (adv <= 0) adv = (int)(f->size->metrics.max_advance >> 6);
    if (adv <= 0) adv = (int)(cur_size * FONT_DPI / 96.0 * 0.6);

    int asc = (int)(f->size->metrics.ascender >> 6);
    int desc = (int)(-f->size->metrics.descender >> 6);
    int h = (int)(f->size->metrics.height >> 6);
#if CELL_HEIGHT_MODE == 0
    if (h < asc + desc) h = asc + desc;
#endif
    if (h < 1) h = asc + desc;

    cell_w = (int)lround(adv * (double)CELL_WIDTH);
    cell_h = (int)lround(h * (double)LINE_HEIGHT);
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;
    if ((double)CELL_ASPECT > 0.0) {
        int want = (int)lround((double)cell_h / (double)CELL_ASPECT);
        if (want > cell_w) cell_w = want;
    }
    glyph_adv = adv;
    baseline = asc + (cell_h - (asc + desc)) / 2;
    ul_thick = (int)(cur_size / 10.0) + 1;
    ul_pos = baseline + (desc > 2 ? 2 : 1);
    if (ul_pos + ul_thick > cell_h) ul_pos = cell_h - ul_thick;
}

static void atlas_reset(void) {
    memset(cache, 0, sizeof cache);
    shelf_x = shelf_y = shelf_h = 1;
    glBindTexture(GL_TEXTURE_2D, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uint8_t *zero = calloc(ATLAS_SIZE * 4, 1);
    if (zero) {
        for (int y = 0; y < ATLAS_SIZE; y++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, ATLAS_SIZE, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        free(zero);
    }
}

double font_size(void) { return cur_size; }

bool font_set_size(double size) {
    if (size < (double)FONT_SIZE_MIN) size = FONT_SIZE_MIN;
    if (size > (double)FONT_SIZE_MAX) size = FONT_SIZE_MAX;
    if (size == cur_size) return false;
    cur_size = size;

    FT_F26Dot6 px = (FT_F26Dot6)(cur_size * 64.0);
    for (int i = 0; i < 4; i++) {
        if (!faces[i]) continue;
        if (i > 0 && faces[i] == faces[0]) continue;
        FT_Set_Char_Size(faces[i], 0, px, (FT_UInt)FONT_DPI, (FT_UInt)FONT_DPI);
    }
    for (int i = 0; i < fallback_n; i++) {
        if (FT_Set_Char_Size(fallback[i], 0, px, (FT_UInt)FONT_DPI, (FT_UInt)FONT_DPI)) {
            if (fallback[i]->num_fixed_sizes > 0) FT_Select_Size(fallback[i], 0);
        }
    }
    compute_metrics();
    atlas_reset();
    return true;
}

bool font_init(void) {
    if (FT_Init_FreeType(&ft)) return false;
    if (!FcInit()) return false;

    if (FONT_AA == 2) {
        FT_LcdFilter lf = (FONT_LCD_FILTER == 2) ? FT_LCD_FILTER_LIGHT
                        : (FONT_LCD_FILTER == 0) ? FT_LCD_FILTER_NONE
                        : FT_LCD_FILTER_DEFAULT;
        lcd_ok = (FT_Library_SetLcdFilter(ft, lf) == 0);
    }

    static const char *specs[4] = { FONT_REGULAR, FONT_BOLD, FONT_ITALIC, FONT_BOLD_ITALIC };
    if (!load_face(specs[0], &faces[0])) {
        fprintf(stderr, "titty: cannot load font '%s'\n", specs[0]);
        return false;
    }
    for (int i = 1; i < 4; i++)
        if (!load_face(specs[i], &faces[i])) faces[i] = faces[0];

    compute_metrics();

    glGenTextures(1, &atlas);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    shelf_x = shelf_y = shelf_h = 1;
    atlas_reset();
    return true;
}

void font_fini(void) {
    for (int i = 0; i < fallback_n; i++) {
        FT_Done_Face(fallback[i]);
        if (fb_pat[i]) FcPatternDestroy(fb_pat[i]);
    }
    for (int i = 3; i >= 0; i--)
        if (faces[i] && (i == 0 || faces[i] != faces[0])) FT_Done_Face(faces[i]);
    if (ft) FT_Done_FreeType(ft);
    if (atlas) glDeleteTextures(1, &atlas);
}

static void atlas_reset(void);

static bool atlas_alloc(int w, int h, int *ox, int *oy) {
    if (w > ATLAS_SIZE || h > ATLAS_SIZE) return false;
    if (shelf_x + w + 1 > ATLAS_SIZE) {
        shelf_x = 1;
        shelf_y += shelf_h + 1;
        shelf_h = 0;
    }
    if (shelf_y + h + 1 > ATLAS_SIZE) return false;
    *ox = shelf_x;
    *oy = shelf_y;
    shelf_x += w + 1;
    if (h > shelf_h) shelf_h = h;
    return true;
}

static void upload(int x, int y, int w, int h, const uint8_t *rgba) {
    glBindTexture(GL_TEXTURE_2D, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static FT_Int32 load_flags(FT_Face face) {
    FT_Int32 f = FT_LOAD_DEFAULT;
#if FONT_HINTING == 0
    f |= FT_LOAD_NO_HINTING;
#elif FONT_HINTING == 1
    f |= FT_LOAD_TARGET_LIGHT;
#else
    f |= (FONT_AA == 2) ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL;
#endif
    if (FT_HAS_COLOR(face)) f |= FT_LOAD_COLOR;
    return f;
}

static bool rasterize_builtin(uint32_t cp, Glyph *out) {
    int w = cell_w, h = cell_h;
    float *c = malloc((size_t)w * h * sizeof(float));
    if (!c) return false;
    if (!boxdraw_render(cp, w, h, c)) { free(c); return false; }

    int ax, ay;
    if (!atlas_alloc(w, h, &ax, &ay)) {
        atlas_reset();
        if (!atlas_alloc(w, h, &ax, &ay)) { free(c); return false; }
    }

    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) { free(c); return false; }
    for (int i = 0; i < w * h; i++) {
        float v = c[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        uint8_t a = (uint8_t)lround(v * 255.0);
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = a;
        rgba[i * 4 + 3] = a;
    }
    upload(ax, ay, w, h, rgba);
    free(rgba);
    free(c);

    out->w = (short)w;
    out->h = (short)h;
    out->bx = 0;
    out->by = (short)baseline;
    out->adv = (short)cell_w;
    out->color = false;
    out->u0 = (float)ax / ATLAS_SIZE;
    out->v0 = (float)ay / ATLAS_SIZE;
    out->u1 = (float)(ax + w) / ATLAS_SIZE;
    out->v1 = (float)(ay + h) / ATLAS_SIZE;
    out->valid = true;
    return true;
}

static bool rasterize(uint32_t cp, int style, Glyph *out) {
    if (boxdraw_supported(cp) && rasterize_builtin(cp, out)) return true;

    FT_Face face = faces[style];
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi && style != STYLE_REGULAR) {
        face = faces[STYLE_REGULAR];
        gi = FT_Get_Char_Index(face, cp);
    }
    if (!gi) {
        FT_Face fb = fallback_for(cp);
        if (fb) {
            face = fb;
            gi = FT_Get_Char_Index(face, cp);
        }
    }
    if (!gi) return false;

    if (FT_Load_Glyph(face, gi, load_flags(face))) return false;

    FT_GlyphSlot slot = face->glyph;
    if (FONT_EMBOLDEN > 0.0 && slot->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_EmboldenXY(&slot->outline, (FT_Pos)(FONT_EMBOLDEN * 64.0), 0);

    if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
        FT_Render_Mode rm = (FONT_AA == 0) ? FT_RENDER_MODE_MONO
                          : (FONT_AA == 2 && lcd_ok) ? FT_RENDER_MODE_LCD
                          : FT_RENDER_MODE_NORMAL;
        if (FT_Render_Glyph(slot, rm)) return false;
    }

    FT_Bitmap *bm = &slot->bitmap;
    bool lcd = (bm->pixel_mode == FT_PIXEL_MODE_LCD);
    int w = lcd ? (int)bm->width / 3 : (int)bm->width;
    int h = (int)bm->rows;
    out->bx = (short)slot->bitmap_left;
    out->by = (short)slot->bitmap_top;
    out->adv = (short)(slot->advance.x >> 6);
    out->color = false;
    out->w = (short)w;
    out->h = (short)h;

    if (w == 0 || h == 0) {
        out->u0 = out->v0 = out->u1 = out->v1 = 0.0f;
        out->valid = true;
        return true;
    }

    int ax, ay;
    if (!atlas_alloc(w, h, &ax, &ay)) {
        atlas_reset();
        if (!atlas_alloc(w, h, &ax, &ay)) return false;
    }

    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return false;

    if (lcd) {
        for (int y = 0; y < h; y++) {
            const uint8_t *src = bm->buffer + (ptrdiff_t)y * bm->pitch;
            uint8_t *dst = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                uint8_t c0 = src[x * 3 + 0], c1 = src[x * 3 + 1], c2 = src[x * 3 + 2];
                uint8_t r = FONT_SUBPIXEL_BGR ? c2 : c0;
                uint8_t b = FONT_SUBPIXEL_BGR ? c0 : c2;
                uint8_t mx = r > c1 ? r : c1;
                if (b > mx) mx = b;
                dst[x * 4 + 0] = r;
                dst[x * 4 + 1] = c1;
                dst[x * 4 + 2] = b;
                dst[x * 4 + 3] = mx;
            }
        }
    } else if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
        out->color = true;
        for (int y = 0; y < h; y++) {
            const uint8_t *src = bm->buffer + (ptrdiff_t)y * bm->pitch;
            uint8_t *dst = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
    } else if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
        for (int y = 0; y < h; y++) {
            const uint8_t *src = bm->buffer + (ptrdiff_t)y * bm->pitch;
            uint8_t *dst = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                uint8_t a = (src[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
                dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = a;
                dst[x * 4 + 3] = a;
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            const uint8_t *src = bm->buffer + (ptrdiff_t)y * bm->pitch;
            uint8_t *dst = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = src[x];
                dst[x * 4 + 3] = src[x];
            }
        }
    }

    upload(ax, ay, w, h, rgba);
    free(rgba);

    if (out->color) {
        double sc = (double)cell_h * 0.92 / (double)h;
        double maxw = (double)cell_w * 2.0;
        if (w * sc > maxw) sc = maxw / w;
        out->w = (short)lround(w * sc);
        out->h = (short)lround(h * sc);
        out->bx = (short)lround(out->bx * sc);
        out->by = (short)lround(out->by * sc);
        if (out->by <= 0) out->by = (short)(out->h);
    }

    out->u0 = (float)ax / ATLAS_SIZE;
    out->v0 = (float)ay / ATLAS_SIZE;
    out->u1 = (float)(ax + w) / ATLAS_SIZE;
    out->v1 = (float)(ay + h) / ATLAS_SIZE;
    out->valid = true;
    return true;
}

const Glyph *font_glyph(uint32_t cp, int style) {
    uint32_t key = (cp & 0x1fffffu) | ((uint32_t)style << 21);
    uint32_t h = key * 2654435761u;
    uint32_t idx = h & (CACHE_SIZE - 1);

    for (int probe = 0; probe < 64; probe++) {
        CacheEnt *e = &cache[(idx + probe) & (CACHE_SIZE - 1)];
        if (e->used && e->key == key) return e->g.valid ? &e->g : NULL;
        if (!e->used) {
            e->used = true;
            e->key = key;
            memset(&e->g, 0, sizeof e->g);
            bool ok = rasterize(cp, style, &e->g);
            e->used = true;
            e->key = key;
            e->g.valid = ok;
            return ok ? &e->g : NULL;
        }
    }
    return NULL;
}

int font_cell_w(void) { return cell_w; }
int font_glyph_advance(void) { return glyph_adv; }
int font_cell_h(void) { return cell_h; }
int font_baseline(void) { return baseline; }
int font_underline_pos(void) { return ul_pos; }
int font_underline_thickness(void) { return ul_thick; }
unsigned font_atlas_tex(void) { return atlas; }
