#include <math.h>
#include <string.h>

#include "common.h"

static float *cov;
static int bw, bh;

static void clearc(void) { memset(cov, 0, (size_t)bw * bh * sizeof(float)); }

static void addpx(int x, int y, float v) {
    if (x < 0 || y < 0 || x >= bw || y >= bh) return;
    float *p = &cov[y * bw + x];
    *p += v;
    if (*p > 1.0f) *p = 1.0f;
}

static void frect(float x0, float y0, float x1, float y1) {
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++) {
        float yc = fminf((float)y + 1.0f, y1) - fmaxf((float)y, y0);
        if (yc <= 0) continue;
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            float xc = fminf((float)x + 1.0f, x1) - fmaxf((float)x, x0);
            if (xc > 0) addpx(x, y, xc * yc);
        }
    }
}

static void fillall(float v) {
    for (int i = 0; i < bw * bh; i++) cov[i] = v;
}

static void seg(float x0, float y0, float x1, float y1, float thick) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    dx /= len;
    dy /= len;
    float nx = -dy * thick * 0.5f, ny = dx * thick * 0.5f;
    float px[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx };
    float py[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            float acc = 0.0f;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float fx = (float)x + (sx + 0.5f) / 4.0f;
                    float fy = (float)y + (sy + 0.5f) / 4.0f;
                    bool pos = true, neg = true;
                    for (int e = 0; e < 4; e++) {
                        int n = (e + 1) & 3;
                        float ex = px[n] - px[e], ey = py[n] - py[e];
                        float cr = ex * (fy - py[e]) - ey * (fx - px[e]);
                        if (cr < 0.0f) pos = false;
                        if (cr > 0.0f) neg = false;
                    }
                    if (pos || neg) acc += 1.0f / 16.0f;
                }
            }
            if (acc > 0.0f) addpx(x, y, acc);
        }
    }
}

static void arc(float cx, float cy, float r, float a0, float a1, float thick) {
    int steps = 24;
    float pa = a0;
    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / steps;
        seg(cx + r * cosf(pa), cy + r * sinf(pa), cx + r * cosf(a), cy + r * sinf(a), thick);
        pa = a;
    }
}

static float light_thick(void) {
    float t = roundf((float)bh / 12.0f);
    return t < 1.0f ? 1.0f : t;
}

static void hline(float y, float x0, float x1, float t) { frect(x0, y - t / 2, x1, y + t / 2); }
static void vline(float x, float y0, float y1, float t) { frect(x - t / 2, y0, x + t / 2, y1); }

static void dashed(bool vert, int count, float w) {
    float t = w;
    float cx = bw / 2.0f, cy = bh / 2.0f;
    float span = vert ? (float)bh : (float)bw;
    float unit = span / (count * 2.0f - 1.0f);
    for (int i = 0; i < count; i++) {
        float a = i * 2.0f * unit;
        float b = a + unit;
        if (vert) vline(cx, a, b, t);
        else hline(cy, a, b, t);
    }
}

static bool draw_box(uint32_t cp) {
    float lt = light_thick();
    float ht = lt * 2.0f;
    float cx = bw / 2.0f, cy = bh / 2.0f;
    float gap = lt * 1.5f;

    static const uint8_t arms[0x4c][4] = {
        {0,1,0,1},{0,2,0,2},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},
        {0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},
        {0,1,1,0},{0,2,1,0},{0,1,2,0},{0,2,2,0},
        {0,0,1,1},{0,0,1,2},{0,0,2,1},{0,0,2,2},
        {1,1,0,0},{1,2,0,0},{2,1,0,0},{2,2,0,0},
        {1,0,0,1},{1,0,0,2},{2,0,0,1},{2,0,0,2},
        {1,1,1,0},{1,2,1,0},{2,1,1,0},{1,1,2,0},{2,1,2,0},{2,2,1,0},{1,2,2,0},{2,2,2,0},
        {1,0,1,1},{1,0,1,2},{2,0,1,1},{1,0,2,1},{2,0,2,1},{2,0,1,2},{1,0,2,2},{2,0,2,2},
        {0,1,1,1},{0,1,1,2},{0,2,1,1},{0,2,1,2},{0,1,2,1},{0,1,2,2},{0,2,2,1},{0,2,2,2},
        {1,1,0,1},{1,1,0,2},{1,2,0,1},{1,2,0,2},{2,1,0,1},{2,1,0,2},{2,2,0,1},{2,2,0,2},
        {1,1,1,1},{1,1,1,2},{1,2,1,1},{1,2,1,2},{2,1,1,1},{1,1,2,1},{2,1,2,1},
        {2,1,1,2},{2,2,1,1},{1,1,2,2},{1,2,2,1},{2,2,1,2},{1,2,2,2},{2,1,2,2},{2,2,2,1},{2,2,2,2}
    };

    if (cp >= 0x2504 && cp <= 0x250b) {
        static const struct { bool v; int n; bool heavy; } d[8] = {
            {false,3,false},{false,3,true},{true,3,false},{true,3,true},
            {false,4,false},{false,4,true},{true,4,false},{true,4,true}
        };
        int i = (int)(cp - 0x2504);
        dashed(d[i].v, d[i].n, d[i].heavy ? ht : lt);
        return true;
    }

    if (cp >= 0x2500 && cp <= 0x254b) {
        const uint8_t *a = arms[cp - 0x2500];
        int up = a[0], right = a[1], down = a[2], left = a[3];
        if (left) hline(cy, 0, cx + (left == 2 ? ht : lt) / 2, left == 2 ? ht : lt);
        if (right) hline(cy, cx - (right == 2 ? ht : lt) / 2, (float)bw, right == 2 ? ht : lt);
        if (up) vline(cx, 0, cy + (up == 2 ? ht : lt) / 2, up == 2 ? ht : lt);
        if (down) vline(cx, cy - (down == 2 ? ht : lt) / 2, (float)bh, down == 2 ? ht : lt);
        return true;
    }

    if (cp >= 0x2550 && cp <= 0x256c) {
        static const uint8_t d2[0x1d][4] = {
            {0,3,0,3},{3,0,3,0},
            {0,3,1,0},{0,1,3,0},{0,3,3,0},
            {0,0,1,3},{0,0,3,1},{0,0,3,3},
            {1,3,0,0},{3,1,0,0},{3,3,0,0},
            {1,0,0,3},{3,0,0,1},{3,0,0,3},
            {1,3,1,0},{3,1,3,0},{3,3,3,0},
            {1,0,1,3},{3,0,3,1},{3,0,3,3},
            {0,3,1,3},{0,1,3,1},{0,3,3,3},
            {1,3,0,3},{3,1,0,1},{3,3,0,3},
            {1,3,1,3},{3,1,3,1},{3,3,3,3}
        };
        const uint8_t *a = d2[cp - 0x2550];
        int up = a[0], right = a[1], down = a[2], left = a[3];

        if (up == 3 || down == 3) {
            if (up == 3 && down == 3) {
                vline(cx - gap, 0, (float)bh, lt);
                vline(cx + gap, 0, (float)bh, lt);
            } else if (up == 3) {
                vline(cx - gap, 0, cy + (left ? -gap : gap), lt);
                vline(cx + gap, 0, cy + (right ? -gap : gap), lt);
                if (!left && !right) { vline(cx - gap, 0, cy, lt); vline(cx + gap, 0, cy, lt); }
            } else {
                vline(cx - gap, cy + (left ? gap : -gap), (float)bh, lt);
                vline(cx + gap, cy + (right ? gap : -gap), (float)bh, lt);
            }
        } else if (up || down) {
            if (up) vline(cx, 0, cy, lt);
            if (down) vline(cx, cy, (float)bh, lt);
        }

        if (left == 3 || right == 3) {
            if (left == 3 && right == 3) {
                hline(cy - gap, 0, (float)bw, lt);
                hline(cy + gap, 0, (float)bw, lt);
            } else if (left == 3) {
                hline(cy - gap, 0, cx + (up ? -gap : gap), lt);
                hline(cy + gap, 0, cx + (down ? -gap : gap), lt);
            } else {
                hline(cy - gap, cx + (up ? gap : -gap), (float)bw, lt);
                hline(cy + gap, cx + (down ? gap : -gap), (float)bw, lt);
            }
        } else if (left || right) {
            if (left) hline(cy, 0, cx, lt);
            if (right) hline(cy, cx, (float)bw, lt);
        }
        return true;
    }

    if (cp >= 0x256d && cp <= 0x2570) {
        float r = fminf(cx, cy) * 0.9f;
        switch (cp) {
        case 0x256d: hline(cy, cx + r, (float)bw, lt); vline(cx, cy + r, (float)bh, lt);
                     arc(cx + r, cy + r, r, (float)M_PI, 1.5f * (float)M_PI, lt); break;
        case 0x256e: hline(cy, 0, cx - r, lt); vline(cx, cy + r, (float)bh, lt);
                     arc(cx - r, cy + r, r, -0.5f * (float)M_PI, 0.0f, lt); break;
        case 0x256f: hline(cy, 0, cx - r, lt); vline(cx, 0, cy - r, lt);
                     arc(cx - r, cy - r, r, 0.0f, 0.5f * (float)M_PI, lt); break;
        case 0x2570: hline(cy, cx + r, (float)bw, lt); vline(cx, 0, cy - r, lt);
                     arc(cx + r, cy - r, r, 0.5f * (float)M_PI, (float)M_PI, lt); break;
        }
        return true;
    }

    if (cp == 0x2571) { seg((float)bw, 0, 0, (float)bh, lt); return true; }
    if (cp == 0x2572) { seg(0, 0, (float)bw, (float)bh, lt); return true; }
    if (cp == 0x2573) { seg(0, 0, (float)bw, (float)bh, lt); seg((float)bw, 0, 0, (float)bh, lt); return true; }

    if (cp >= 0x2574 && cp <= 0x257f) {
        switch (cp) {
        case 0x2574: hline(cy, 0, cx, lt); break;
        case 0x2575: vline(cx, 0, cy, lt); break;
        case 0x2576: hline(cy, cx, (float)bw, lt); break;
        case 0x2577: vline(cx, cy, (float)bh, lt); break;
        case 0x2578: hline(cy, 0, cx, ht); break;
        case 0x2579: vline(cx, 0, cy, ht); break;
        case 0x257a: hline(cy, cx, (float)bw, ht); break;
        case 0x257b: vline(cx, cy, (float)bh, ht); break;
        case 0x257c: hline(cy, 0, cx, lt); hline(cy, cx, (float)bw, ht); break;
        case 0x257d: vline(cx, 0, cy, lt); vline(cx, cy, (float)bh, ht); break;
        case 0x257e: hline(cy, 0, cx, ht); hline(cy, cx, (float)bw, lt); break;
        case 0x257f: vline(cx, 0, cy, ht); vline(cx, cy, (float)bh, lt); break;
        }
        return true;
    }
    return false;
}

static bool draw_block(uint32_t cp) {
    float w = (float)bw, h = (float)bh;
    if (cp == 0x2580) { frect(0, 0, w, h / 2); return true; }
    if (cp >= 0x2581 && cp <= 0x2588) {
        float n = (float)(cp - 0x2580);
        frect(0, h - h * n / 8.0f, w, h);
        return true;
    }
    if (cp >= 0x2589 && cp <= 0x258f) {
        float n = (float)(0x2590 - cp);
        frect(0, 0, w * n / 8.0f, h);
        return true;
    }
    if (cp == 0x2590) { frect(w / 2, 0, w, h); return true; }
    if (cp == 0x2591) { fillall(0.25f); return true; }
    if (cp == 0x2592) { fillall(0.50f); return true; }
    if (cp == 0x2593) { fillall(0.75f); return true; }
    if (cp == 0x2594) { frect(0, 0, w, h / 8.0f); return true; }
    if (cp == 0x2595) { frect(w - w / 8.0f, 0, w, h); return true; }
    if (cp >= 0x2596 && cp <= 0x259f) {
        static const uint8_t q[10] = { 0x4, 0x8, 0x1, 0xd, 0x9, 0x7, 0xb, 0x2, 0x6, 0xe };
        uint8_t m = q[cp - 0x2596];
        if (m & 1) frect(0, 0, w / 2, h / 2);
        if (m & 2) frect(w / 2, 0, w, h / 2);
        if (m & 4) frect(0, h / 2, w / 2, h);
        if (m & 8) frect(w / 2, h / 2, w, h);
        return true;
    }
    if (cp >= 0x23ba && cp <= 0x23bd) {
        float t = light_thick();
        float y = h * (float)(cp - 0x23ba + 1) / 5.0f;
        hline(y, 0, w, t);
        return true;
    }
    return false;
}

static bool draw_braille(uint32_t cp) {
    uint32_t bits = cp - 0x2800;
    float w = (float)bw, h = (float)bh;
    float cellw = w / 2.0f, cellh = h / 4.0f;
    float r = fminf(cellw, cellh) * 0.40f;
    static const uint8_t pos[8][2] = {
        {0,0},{0,1},{0,2},{1,0},{1,1},{1,2},{0,3},{1,3}
    };
    for (int i = 0; i < 8; i++) {
        if (!(bits & (1u << i))) continue;
        float px = (pos[i][0] + 0.5f) * cellw;
        float py = (pos[i][1] + 0.5f) * cellh;
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                float acc = 0.0f;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = (float)x + (sx + 0.5f) / 4.0f - px;
                        float fy = (float)y + (sy + 0.5f) / 4.0f - py;
                        if (fx * fx + fy * fy <= r * r) acc += 1.0f / 16.0f;
                    }
                if (acc > 0.0f) addpx(x, y, acc);
            }
        }
    }
    return true;
}

bool boxdraw_supported(uint32_t cp) {
    return (cp >= 0x2500 && cp <= 0x259f) ||
           (cp >= 0x2800 && cp <= 0x28ff) ||
           (cp >= 0x23ba && cp <= 0x23bd);
}

bool boxdraw_render(uint32_t cp, int w, int h, float *out) {
    bw = w;
    bh = h;
    cov = out;
    clearc();
    if (cp >= 0x2800 && cp <= 0x28ff) return draw_braille(cp);
    if ((cp >= 0x2580 && cp <= 0x259f) || (cp >= 0x23ba && cp <= 0x23bd)) return draw_block(cp);
    return draw_box(cp);
}
