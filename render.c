#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>

#include "common.h"

typedef struct { float x, y, w, h, r, g, b, a; } RectInst;
typedef struct { float x, y, w, h, u0, v0, u1, v1, r, g, b, a, col, pad0, pad1, pad2; } GlyphInst;

static GLuint prog_rect, prog_glyph, prog_poly, prog_bright, prog_blur, prog_post;
static bool subpixel;
static GLuint vao_rect, vao_glyph, vao_poly, vao_empty;
static GLuint vbo_quad, vbo_rect, vbo_glyph, vbo_poly;
static GLuint fbo_scene, tex_scene;
static GLuint fbo_bloom[2], tex_bloom[2];
static GLuint fbo_ghost[2], tex_ghost[2];
static GLuint fbo_persist[2], tex_persist[2];
static int persist_cur;
static int fb_w, fb_h, bloom_w, bloom_h;

static RectInst *rects;
static int rect_n, rect_cap;
static GlyphInst *glyphs;
static int glyph_n, glyph_cap;
static float poly_pts[64];
static int poly_n;

static float palette[256][3];
static uint32_t palette_rgb[256];
static float trail_x[4], trail_y[4];
static bool trail_ready;
static double last_time = -1.0;
static bool trail_moving;
static double burnin_until;
static double bell_until;
static float fx_level = (CRT_START_ON || CRT_INTRO) ? 1.0f : 0.0f;
static float fx_target = (CRT_START_ON || CRT_INTRO) ? 1.0f : 0.0f;
static double crt_hold_until;
static bool crt_intro_done;

static const char *VS_RECT =
"#version 300 es\n"
"layout(location=0) in vec2 aCorner;\n"
"layout(location=1) in vec4 aRect;\n"
"layout(location=2) in vec4 aColor;\n"
"uniform vec2 uRes;\n"
"out vec4 vColor;\n"
"void main(){\n"
" vec2 p = aRect.xy + aCorner * aRect.zw;\n"
" vColor = aColor;\n"
" gl_Position = vec4(p.x/uRes.x*2.0-1.0, 1.0-p.y/uRes.y*2.0, 0.0, 1.0);\n"
"}\n";

static const char *FS_RECT =
"#version 300 es\n"
"precision highp float;\n"
"in vec4 vColor;\n"
"out vec4 oColor;\n"
"void main(){ oColor = vColor; }\n";

static const char *VS_GLYPH =
"#version 300 es\n"
"layout(location=0) in vec2 aCorner;\n"
"layout(location=1) in vec4 aRect;\n"
"layout(location=2) in vec4 aUV;\n"
"layout(location=3) in vec4 aColor;\n"
"layout(location=4) in float aIsColor;\n"
"uniform vec2 uRes;\n"
"out vec2 vUV;\n"
"out vec4 vColor;\n"
"out float vIsColor;\n"
"void main(){\n"
" vec2 p = aRect.xy + aCorner * aRect.zw;\n"
" vUV = mix(aUV.xy, aUV.zw, aCorner);\n"
" vColor = aColor;\n"
" vIsColor = aIsColor;\n"
" gl_Position = vec4(p.x/uRes.x*2.0-1.0, 1.0-p.y/uRes.y*2.0, 0.0, 1.0);\n"
"}\n";

static const char *FS_GLYPH_HEAD =
"#version 300 es\n"
"%s"
"precision highp float;\n"
"#define TEXT_GAMMA_C %f\n"
"#define TEXT_CONTRAST_C %f\n"
"in vec2 vUV;\n"
"in vec4 vColor;\n"
"in float vIsColor;\n"
"uniform sampler2D uAtlas;\n"
"%s"
"void main(){\n"
" vec4 t = texture(uAtlas, vUV);\n"
" float isCol = step(0.5, vIsColor);\n"
" vec3 srcRGB = mix(vColor.rgb, t.rgb / max(t.a, 0.0001), isCol);\n"
" vec3 cov = mix(t.rgb, vec3(t.a), isCol);\n"
" vec3 g = pow(clamp(cov, 0.0, 1.0), vec3(TEXT_GAMMA_C));\n"
" g = clamp(g + TEXT_CONTRAST_C * g * (1.0 - g), 0.0, 1.0);\n"
" cov = mix(g, cov, isCol);\n"
" float covA = mix(max(max(cov.r, cov.g), cov.b), t.a, isCol);\n"
" cov *= vColor.a;\n"
" covA *= vColor.a;\n"
"%s"
"}\n";

static const char *GLYPH_OUT_SINGLE = "out vec4 oColor;\n";
static const char *GLYPH_BODY_SINGLE = " oColor = vec4(srcRGB, covA);\n";
static const char *GLYPH_OUT_DUAL =
"layout(location = 0, index = 0) out vec4 oColor;\n"
"layout(location = 0, index = 1) out vec4 oBlend;\n";
static const char *GLYPH_BODY_DUAL =
" oColor = vec4(srcRGB, 1.0);\n"
" oBlend = vec4(cov, covA);\n";

static const char *VS_POLY =
"#version 300 es\n"
"layout(location=0) in vec2 aPos;\n"
"uniform vec2 uRes;\n"
"void main(){ gl_Position = vec4(aPos.x/uRes.x*2.0-1.0, 1.0-aPos.y/uRes.y*2.0, 0.0, 1.0); }\n";

static const char *FS_POLY =
"#version 300 es\n"
"precision highp float;\n"
"uniform vec4 uColor;\n"
"out vec4 oColor;\n"
"void main(){ oColor = uColor; }\n";

static const char *VS_FULL =
"#version 300 es\n"
"out vec2 vTex;\n"
"void main(){\n"
" vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0, (gl_VertexID == 1) ? 3.0 : -1.0);\n"
" vTex = p * 0.5 + 0.5;\n"
" gl_Position = vec4(p, 0.0, 1.0);\n"
"}\n";

static const char *FS_BRIGHT_FMT =
"#version 300 es\n"
"precision highp float;\n"
"const float GLOW_FALLOFF = %f;\n"
"const float GLOW_TINT = %f;\n"
"const vec3 GLOW_COLOR = vec3(%f, %f, %f);\n"
"in vec2 vTex;\n"
"uniform sampler2D uTex;\n"
"uniform float uThreshold;\n"
"out vec4 oColor;\n"
"void main(){\n"
" vec4 c = texture(uTex, vTex);\n"
" float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
" float k = clamp((l - uThreshold) / max(1.0 - uThreshold, 0.0001), 0.0, 1.0);\n"
" k = pow(k, GLOW_FALLOFF);\n"
" vec3 g = mix(c.rgb, GLOW_COLOR * max(l, 0.35), GLOW_TINT);\n"
" oColor = vec4(g * k, 1.0);\n"
"}\n";

static const char *FS_BLUR =
"#version 300 es\n"
"precision highp float;\n"
"in vec2 vTex;\n"
"uniform sampler2D uTex;\n"
"uniform vec2 uDir;\n"
"out vec4 oColor;\n"
"void main(){\n"
" vec3 s = texture(uTex, vTex).rgb * 0.227027;\n"
" s += texture(uTex, vTex + uDir * 1.3846).rgb * 0.316216;\n"
" s += texture(uTex, vTex - uDir * 1.3846).rgb * 0.316216;\n"
" s += texture(uTex, vTex + uDir * 3.2308).rgb * 0.070270;\n"
" s += texture(uTex, vTex - uDir * 3.2308).rgb * 0.070270;\n"
" oColor = vec4(s, 1.0);\n"
"}\n";

static const char *FS_PERSIST =
"#version 300 es\n"
"precision highp float;\n"
"in vec2 vTex;\n"
"uniform sampler2D uScene;\n"
"uniform sampler2D uPrev;\n"
"uniform float uDecay;\n"
"out vec4 oColor;\n"
"void main(){\n"
" vec4 s = texture(uScene, vTex);\n"
" vec4 p = texture(uPrev, vTex) * uDecay;\n"
" oColor = max(s, p);\n"
"}\n";

static const char *FS_POST_BODY =
"in vec2 vTex;\n"
"uniform sampler2D uScene;\n"
"uniform sampler2D uBloom;\n"
"uniform sampler2D uPersist;\n"
"uniform sampler2D uGhost;\n"
"uniform float uCell;\n"
"uniform vec2 uRes;\n"
"uniform float uTime;\n"
"uniform float uBell;\n"
"uniform float uFx;\n"
"out vec4 oColor;\n"
"float hash21(vec2 p){\n"
" p = fract(p * vec2(123.34, 456.21));\n"
" p += dot(p, p + 45.32);\n"
" return fract(p.x * p.y);\n"
"}\n"
"float vnoise(vec2 p){\n"
" vec2 i = floor(p), f = fract(p);\n"
" f = f * f * (3.0 - 2.0 * f);\n"
" float a = hash21(i), b = hash21(i + vec2(1.0, 0.0));\n"
" float c = hash21(i + vec2(0.0, 1.0)), d = hash21(i + vec2(1.0, 1.0));\n"
" return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
"}\n"
"vec2 curve(vec2 uv){\n"
" uv = uv * 2.0 - 1.0;\n"
" vec2 off = uv.yx * uv.yx * uv.xy * CURVE_AMOUNT;\n"
" return (uv + off) * 0.5 + 0.5;\n"
"}\n"
"vec2 warp(vec2 uv, float amt, float scale){\n"
" vec2 p = (uv - 0.5) * 2.0 / scale;\n"
" p += p.yx * p.yx * p.xy * amt;\n"
" return p * 0.5 + 0.5;\n"
"}\n"
"vec3 ghost(vec2 uv, float amt, float chromaCells, float scale, vec2 offCells){\n"
" vec2 g = warp(uv, amt, scale) + offCells * uCell / uRes;\n"
" if (g.x < 0.0 || g.x > 1.0 || g.y < 0.0 || g.y > 1.0) return vec3(0.0);\n"
" vec2 d = vec2(chromaCells * uCell, 0.0) / uRes;\n"
" return vec3(texture(uGhost, g + d).r, texture(uGhost, g).g, texture(uGhost, g - d).b);\n"
"}\n"
"void main(){\n"
" vec2 uv = vTex;\n"
" vec2 wuv = uv;\n"
"#if DO_CURVATURE && DISTORT_TEXT\n"
" wuv = curve(uv);\n"
" if (wuv.x < 0.0 || wuv.x > 1.0 || wuv.y < 0.0 || wuv.y > 1.0) { oColor = vec4(0.0); return; }\n"
" vec2 suv = wuv;\n"
"#else\n"
" vec2 suv = uv;\n"
"#endif\n"
" vec4 sc;\n"
"#if DO_CHROMATIC && DISTORT_TEXT\n"
" vec2 cdir = (suv - 0.5) * CHROMATIC_AMOUNT;\n"
" vec4 sr = texture(uScene, suv + cdir);\n"
" vec4 sg = texture(uScene, suv);\n"
" vec4 sb = texture(uScene, suv - cdir);\n"
" sc = vec4(sr.r, sg.g, sb.b, max(sg.a, max(sr.a, sb.a)));\n"
"#else\n"
" sc = texture(uScene, suv);\n"
"#endif\n"
" vec3 rgb = sc.rgb;\n"
" float alpha = sc.a;\n"
"#if DO_BURN_IN\n"
" vec4 pv = texture(uPersist, suv);\n"
" float pl = dot(pv.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
" float sl = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
" float burn = max(pl - sl, 0.0) * BURN_IN_AMOUNT;\n"
" rgb += pv.rgb * burn;\n"
" alpha = max(alpha, min(1.0, alpha + burn));\n"
"#endif\n"
"#if GHOST_LAYERS >= 1\n"
" float gdens = dot(texture(uGhost, uv).rgb, vec3(0.2126, 0.7152, 0.0722));\n"
" float gprot = 1.0 - GHOST_PROTECT * clamp(gdens * GHOST_DENSITY, 0.0, 1.0);\n"
" vec3 g1r = ghost(uv, G1_CURVE, G1_CHROMA, G1_SCALE, vec2(G1_OFFX, G1_OFFY)) * gprot;\n"
" vec3 gh1 = mix(g1r, max(g1r - rgb, 0.0), GHOST_UNDER) * G1_OPACITY;\n"
" rgb += gh1;\n"
" alpha = min(1.0, alpha + dot(gh1, vec3(0.2126, 0.7152, 0.0722)));\n"
"#endif\n"
"#if GHOST_LAYERS >= 2\n"
" vec3 g2r = ghost(uv, G2_CURVE, G2_CHROMA, G2_SCALE, vec2(G2_OFFX, G2_OFFY)) * gprot;\n"
" vec3 gh2 = mix(g2r, max(g2r - rgb, 0.0), GHOST_UNDER) * G2_OPACITY;\n"
" rgb += gh2;\n"
" alpha = min(1.0, alpha + dot(gh2, vec3(0.2126, 0.7152, 0.0722)));\n"
"#endif\n"
"#if DO_GLOW\n"
" vec3 bl;\n"
"#if DO_CHROMATIC\n"
" vec2 bdir = (suv - 0.5) * CHROMATIC_AMOUNT;\n"
" bl = vec3(texture(uBloom, suv + bdir).r, texture(uBloom, suv).g, texture(uBloom, suv - bdir).b);\n"
"#else\n"
" bl = texture(uBloom, suv).rgb;\n"
"#endif\n"
" vec3 gsrc = mix(bl, max(bl - rgb, 0.0), GLOW_UNDER);\n"
" vec3 glow = gsrc * GLOW_STRENGTH;\n"
" rgb += glow;\n"
" alpha = min(1.0, alpha + dot(glow, vec3(0.2126, 0.7152, 0.0722)));\n"
"#endif\n"
"#if DO_FROST\n"
" float bgmask = 1.0 - smoothstep(0.35, 0.92, alpha);\n"
" if (bgmask > 0.001) {\n"
"  float n1 = vnoise(gl_FragCoord.xy / FROST_SCALE);\n"
"  float n2 = vnoise(gl_FragCoord.xy / (FROST_SCALE * 4.7));\n"
"  float n3 = vnoise(gl_FragCoord.xy / (FROST_SCALE * 13.0));\n"
"  float grain = n1 * 0.55 + n2 * 0.30 + n3 * 0.15;\n"
"  rgb += (grain - 0.5) * FROST_GRAIN * 2.0 * bgmask;\n"
"  rgb = mix(rgb, FROST_TINT, FROST_TINT_AMT * bgmask);\n"
"  float sheen = smoothstep(1.5, -0.3, uv.x + uv.y);\n"
"  rgb += sheen * FROST_SHEEN * bgmask;\n"
"  float d = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));\n"
"  float rim = 1.0 - smoothstep(0.0, 0.03, d);\n"
"  rgb += rim * FROST_EDGE * 0.3 * bgmask;\n"
"  float milk = FROST_AMOUNT * 0.30 + (grain - 0.5) * FROST_GRAIN;\n"
"  alpha = mix(alpha, clamp(alpha + milk + rim * FROST_EDGE * 0.3, 0.0, 1.0), bgmask);\n"
" }\n"
"#endif\n"
"#if DO_SCANLINES\n"
" float sl2 = sin(gl_FragCoord.y * 3.14159265 / SCANLINE_PERIOD) * 0.5 + 0.5;\n"
" rgb *= 1.0 - SCANLINE_STRENGTH * sl2;\n"
"#endif\n"
"#if DO_NOISE\n"
"#if NOISE_ANIMATE\n"
" rgb += (hash21(gl_FragCoord.xy + fract(uTime) * 431.7) - 0.5) * NOISE_AMOUNT;\n"
"#else\n"
" rgb += (hash21(gl_FragCoord.xy) - 0.5) * NOISE_AMOUNT;\n"
"#endif\n"
"#endif\n"
"#if DO_FLICKER\n"
" rgb *= 1.0 - FLICKER_AMOUNT * (0.5 + 0.5 * sin(uTime * 47.3));\n"
"#endif\n"
"#if DO_VIGNETTE\n"
" vec2 vp = suv - 0.5;\n"
" float vig = 1.0 - dot(vp, vp) * VIGNETTE_AMOUNT * 2.4;\n"
" rgb *= clamp(vig, 0.0, 1.0);\n"
"#endif\n"
" rgb = rgb * (1.0 - LIFT) + LIFT;\n"
" rgb = pow(max(rgb, 0.0), vec3(1.0 / FXGAMMA));\n"
"#if DO_CURVATURE && DISTORT_TEXT\n"
" float be = min(min(wuv.x, 1.0 - wuv.x), min(wuv.y, 1.0 - wuv.y));\n"
" rgb *= smoothstep(0.0, 0.004, be);\n"
" alpha *= smoothstep(0.0, 0.004, be);\n"
"#endif\n"
"#if CRT_ON\n"
" if (uFx > 0.001) {\n"
"  vec3 L = vec3(0.2126, 0.7152, 0.0722);\n"
"  vec2 cd = (suv - 0.5) * CRT_CHROMA;\n"
"  vec3 cb = vec3(texture(uGhost, suv + cd).r, texture(uGhost, suv).g, texture(uGhost, suv - cd).b);\n"
"  vec3 cg = max(cb - rgb, 0.0) * CRT_GLOW_S * uFx;\n"
"  rgb += cg;\n"
"  alpha = min(1.0, alpha + dot(cg, L));\n"
"  vec4 cpv = texture(uPersist, suv);\n"
"  float cburn = max(dot(cpv.rgb, L) - dot(rgb, L), 0.0) * CRT_BURN * uFx;\n"
"  rgb += cpv.rgb * cburn;\n"
"  alpha = min(1.0, alpha + cburn);\n"
"  float cs = sin(gl_FragCoord.y * 3.14159265 / CRT_PERIOD) * 0.5 + 0.5;\n"
"  rgb *= 1.0 - CRT_SCAN * cs * uFx;\n"
"  rgb += (hash21(gl_FragCoord.xy + fract(uTime) * 431.7) - 0.5) * CRT_NOISE_A * uFx;\n"
"  rgb *= 1.0 - CRT_FLICK * (0.5 + 0.5 * sin(uTime * 47.3)) * uFx;\n"
"  vec2 cvp = suv - 0.5;\n"
"  rgb *= mix(1.0, clamp(1.0 - dot(cvp, cvp) * CRT_VIG * 2.4, 0.0, 1.0), uFx);\n"
"  rgb = mix(rgb, vec3(dot(rgb, L)) * CRT_TINT_RGB, CRT_TINT_A * uFx);\n"
" }\n"
"#endif\n"
" rgb += uBell * 0.35;\n"
" alpha = min(1.0, alpha + uBell * 0.35);\n"
" float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
" rgb = mix(vec3(lum), rgb, SATURATION);\n"
" rgb = (rgb - 0.5) * CONTRAST + 0.5;\n"
" rgb *= BRIGHTNESS;\n"
" rgb = clamp(rgb, 0.0, 1.0);\n"
" alpha = clamp(alpha, 0.0, 1.0);\n"
" oColor = vec4(rgb * alpha, alpha);\n"
"}\n";

static bool have_ext(const char *name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name)) return true;
    }
    return false;
}

static void die(const char *msg) {
    fprintf(stderr, "titty: %s\n", msg);
    exit(1);
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        FILE *f = fopen("/tmp/titty-shader-error.glsl", "w");
        if (f) {
            fputs(src, f);
            fclose(f);
        }
        fprintf(stderr, "titty: shader compile failed:\n%s\nkälla: /tmp/titty-shader-error.glsl\n", log);
        exit(1);
    }
    return s;
}

static GLuint link_prog(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "titty: program link failed:\n%s\n", log);
        exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static char *build_post_source(void) {
    static char buf[24576];
    snprintf(buf, sizeof buf,
        "#version 300 es\n"
        "precision highp float;\n"
        "#define DO_CURVATURE %d\n"
        "#define DO_CHROMATIC %d\n"
        "#define DO_GLOW %d\n"
        "#define DO_FROST %d\n"
        "#define DO_SCANLINES %d\n"
        "#define DO_NOISE %d\n"
        "#define DO_FLICKER %d\n"
        "#define DO_VIGNETTE %d\n"
        "#define DO_BURN_IN %d\n"
        "#define DISTORT_TEXT %d\n"
        "#define NOISE_ANIMATE %d\n"
        "#define GHOST_LAYERS %d\n"
        "#define CRT_ON %d\n"
        "const float CRT_SCAN = %f;\n"
        "const float CRT_PERIOD = %f;\n"
        "const float CRT_GLOW_S = %f;\n"
        "const float CRT_BURN = %f;\n"
        "const float CRT_CHROMA = %f;\n"
        "const float CRT_NOISE_A = %f;\n"
        "const float CRT_FLICK = %f;\n"
        "const float CRT_VIG = %f;\n"
        "const float CRT_TINT_A = %f;\n"
        "const vec3 CRT_TINT_RGB = vec3(%f, %f, %f);\n"
        "const float GHOST_PROTECT = %f;\n"
        "const float GHOST_DENSITY = %f;\n"
        "const float GHOST_UNDER = %f;\n"
        "const float GLOW_UNDER = %f;\n"
        "const float G1_CURVE = %f;\n"
        "const float G1_CHROMA = %f;\n"
        "const float G1_SCALE = %f;\n"
        "const float G1_OPACITY = %f;\n"
        "const float G1_OFFX = %f;\n"
        "const float G1_OFFY = %f;\n"
        "const float G2_CURVE = %f;\n"
        "const float G2_CHROMA = %f;\n"
        "const float G2_SCALE = %f;\n"
        "const float G2_OPACITY = %f;\n"
        "const float G2_OFFX = %f;\n"
        "const float G2_OFFY = %f;\n"
        "const float CURVE_AMOUNT = %f;\n"
        "const float CHROMATIC_AMOUNT = %f;\n"
        "const float GLOW_STRENGTH = %f;\n"
        "const float SCANLINE_STRENGTH = %f;\n"
        "const float SCANLINE_PERIOD = %f;\n"
        "const float NOISE_AMOUNT = %f;\n"
        "const float FLICKER_AMOUNT = %f;\n"
        "const float VIGNETTE_AMOUNT = %f;\n"
        "const float BURN_IN_AMOUNT = %f;\n"
        "const float FROST_AMOUNT = %f;\n"
        "const float FROST_GRAIN = %f;\n"
        "const float FROST_SCALE = %f;\n"
        "const float FROST_SHEEN = %f;\n"
        "const float FROST_EDGE = %f;\n"
        "const float FROST_TINT_AMT = %f;\n"
        "const vec3 FROST_TINT = vec3(%f, %f, %f);\n"
        "const float CONTRAST = %f;\n"
        "const float SATURATION = %f;\n"
        "const float BRIGHTNESS = %f;\n"
        "const float LIFT = %f;\n"
        "const float FXGAMMA = %f;\n"
        "%s",
        FX_CURVATURE, FX_CHROMATIC, FX_GLOW, FX_FROST, FX_SCANLINES,
        FX_NOISE, FX_FLICKER, FX_VIGNETTE, FX_BURN_IN, FX_DISTORT_TEXT, FX_NOISE_ANIMATE,
        FX_GHOST_LAYERS,
        CRT_OVERLAY, (double)CRT_SCANLINES, (double)CRT_SCANLINE_PERIOD,
        (double)CRT_GLOW, (double)CRT_BURN_IN, (double)CRT_CHROMATIC,
        (double)CRT_NOISE, (double)CRT_FLICKER, (double)CRT_VIGNETTE,
        (double)CRT_TINT_AMOUNT,
        ((CRT_TINT >> 16) & 0xff) / 255.0, ((CRT_TINT >> 8) & 0xff) / 255.0,
        (CRT_TINT & 0xff) / 255.0,
        (double)FX_GHOST_PROTECT, (double)FX_GHOST_DENSITY,
        (double)FX_GHOST_UNDER, (double)FX_GLOW_UNDER,
        (double)FX_GHOST1_CURVE, (double)FX_GHOST1_CHROMATIC,
        (double)FX_GHOST1_SCALE, (double)FX_GHOST1_OPACITY,
        (double)FX_GHOST1_OFFSET_X, (double)FX_GHOST1_OFFSET_Y,
        (double)FX_GHOST2_CURVE, (double)FX_GHOST2_CHROMATIC,
        (double)FX_GHOST2_SCALE, (double)FX_GHOST2_OPACITY,
        (double)FX_GHOST2_OFFSET_X, (double)FX_GHOST2_OFFSET_Y,
        (double)FX_CURVATURE_AMOUNT, (double)FX_CHROMATIC_AMOUNT,
        (double)FX_GLOW_STRENGTH, (double)FX_SCANLINE_STRENGTH,
        (double)FX_SCANLINE_PERIOD, (double)FX_NOISE_AMOUNT,
        (double)FX_FLICKER_AMOUNT, (double)FX_VIGNETTE_AMOUNT,
        (double)FX_BURN_IN_AMOUNT, (double)FX_FROST_AMOUNT,
        (double)FX_FROST_GRAIN, (double)FX_FROST_SCALE,
        (double)FX_FROST_SHEEN, (double)FX_FROST_EDGE,
        (double)FX_FROST_TINT_AMT,
        ((FX_FROST_TINT >> 16) & 0xff) / 255.0,
        ((FX_FROST_TINT >> 8) & 0xff) / 255.0,
        (FX_FROST_TINT & 0xff) / 255.0,
        (double)FX_CONTRAST, (double)FX_SATURATION, (double)FX_BRIGHTNESS,
        (double)FX_LIFT, (double)FX_GAMMA,
        FS_POST_BODY);
    return buf;
}

static void unpack(uint32_t rgb, float *out) {
    out[0] = ((rgb >> 16) & 0xff) / 255.0f;
    out[1] = ((rgb >> 8) & 0xff) / 255.0f;
    out[2] = (rgb & 0xff) / 255.0f;
}

static void init_palette(void) {
    static const uint32_t base[16] = {
        COLOR_0, COLOR_1, COLOR_2, COLOR_3, COLOR_4, COLOR_5, COLOR_6, COLOR_7,
        COLOR_8, COLOR_9, COLOR_10, COLOR_11, COLOR_12, COLOR_13, COLOR_14, COLOR_15
    };
    for (int i = 0; i < 16; i++) { unpack(base[i], palette[i]); palette_rgb[i] = base[i] & 0xffffffu; }
    static const int lvl[6] = { 0, 95, 135, 175, 215, 255 };
    for (int i = 0; i < 216; i++) {
        int r = lvl[(i / 36) % 6], g = lvl[(i / 6) % 6], b = lvl[i % 6];
        palette[16 + i][0] = r / 255.0f;
        palette[16 + i][1] = g / 255.0f;
        palette[16 + i][2] = b / 255.0f;
        palette_rgb[16 + i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    for (int i = 0; i < 24; i++) {
        int g = 8 + i * 10;
        palette[232 + i][0] = palette[232 + i][1] = palette[232 + i][2] = g / 255.0f;
        palette_rgb[232 + i] = ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
    }
}

static void resolve(uint32_t c, bool is_fg, float *out) {
    if (c == COL_DEF_FG) { unpack(COLOR_FG, out); out[3] = 1.0f; return; }
    if (c == COL_DEF_BG) {
        unpack(COLOR_BG, out);
        out[3] = is_fg ? 1.0f : (float)BG_OPACITY;
        return;
    }

    uint32_t rgb;
    if (COL_IS_RGB(c)) rgb = c & 0x00ffffffu;
    else if (COL_IS_IDX(c)) rgb = palette_rgb[c];
    else { unpack(COLOR_FG, out); out[3] = 1.0f; return; }

    unpack(rgb, out);
    out[3] = 1.0f;
    if (BG_OPACITY_MATCH && !is_fg && rgb == ((uint32_t)COLOR_BG & 0x00ffffffu))
        out[3] = (float)BG_OPACITY;
}

static float quick_lum(const float *c) {
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

static void min_contrast(float *fg, const float *bg) {
    if ((float)FX_MIN_CONTRAST <= 1.0f) return;
    float lb = quick_lum(bg);
    float lf = quick_lum(fg);
    float hi = lf > lb ? lf : lb, lo = lf > lb ? lb : lf;
    if ((hi + 0.05f) / (lo + 0.05f) >= (float)FX_MIN_CONTRAST) return;

    if (lb <= 0.45f) {
        float want = (float)FX_MIN_CONTRAST * (lb + 0.05f) - 0.05f;
        if (lf > 0.0001f && want <= 1.0f) {
            float k = want / lf;
            float peak = fg[0] > fg[1] ? fg[0] : fg[1];
            if (fg[2] > peak) peak = fg[2];
            if (peak * k <= 1.0f) {
                fg[0] *= k; fg[1] *= k; fg[2] *= k;
                return;
            }
            if (peak > 0.0001f) {
                float kmax = 1.0f / peak;
                fg[0] *= kmax; fg[1] *= kmax; fg[2] *= kmax;
                lf = quick_lum(fg);
                if ((lf + 0.05f) / (lb + 0.05f) >= (float)FX_MIN_CONTRAST) return;
            }
        }
    }

    float target = lb > 0.45f ? 0.0f : 1.0f;
    for (int step = 1; step <= 12; step++) {
        float t = (float)step / 12.0f;
        float cand[3];
        for (int k = 0; k < 3; k++) cand[k] = fg[k] + (target - fg[k]) * t;
        float lc = quick_lum(cand);
        float h = lc > lb ? lc : lb, l = lc > lb ? lb : lc;
        if ((h + 0.05f) / (l + 0.05f) >= (float)FX_MIN_CONTRAST) {
            fg[0] = cand[0]; fg[1] = cand[1]; fg[2] = cand[2];
            return;
        }
    }
    fg[0] = fg[1] = fg[2] = target;
}

static void push_rect(float x, float y, float w, float h, const float *c) {
    if (rect_n >= rect_cap) {
        rect_cap = rect_cap ? rect_cap * 2 : 4096;
        rects = realloc(rects, (size_t)rect_cap * sizeof(RectInst));
        if (!rects) die("out of memory");
    }
    RectInst *r = &rects[rect_n++];
    r->x = x; r->y = y; r->w = w; r->h = h;
    r->r = c[0]; r->g = c[1]; r->b = c[2]; r->a = c[3];
}

static void push_glyph(const Glyph *g, float px, float py, const float *c) {
    if (g->w == 0 || g->h == 0) return;
    if (glyph_n >= glyph_cap) {
        glyph_cap = glyph_cap ? glyph_cap * 2 : 4096;
        glyphs = realloc(glyphs, (size_t)glyph_cap * sizeof(GlyphInst));
        if (!glyphs) die("out of memory");
    }
    GlyphInst *gi = &glyphs[glyph_n++];
    gi->x = px;
    gi->y = py;
    gi->w = g->w;
    gi->h = g->h;
    gi->u0 = g->u0; gi->v0 = g->v0; gi->u1 = g->u1; gi->v1 = g->v1;
    gi->r = c[0]; gi->g = c[1]; gi->b = c[2]; gi->a = c[3];
    gi->col = g->color ? 1.0f : 0.0f;
    gi->pad0 = gi->pad1 = gi->pad2 = 0.0f;
}

static void make_fbo(GLuint *fbo, GLuint *tex, int w, int h) {
    if (!*fbo) glGenFramebuffers(1, fbo);
    if (!*tex) glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool render_init(void) {
    init_palette();

    subpixel = font_subpixel() && have_ext("GL_EXT_blend_func_extended");

    char fsg[4096];
    snprintf(fsg, sizeof fsg, FS_GLYPH_HEAD,
             subpixel ? "#extension GL_EXT_blend_func_extended : require\n" : "",
             (double)TEXT_GAMMA, (double)TEXT_CONTRAST,
             subpixel ? GLYPH_OUT_DUAL : GLYPH_OUT_SINGLE,
             subpixel ? GLYPH_BODY_DUAL : GLYPH_BODY_SINGLE);

    prog_rect = link_prog(VS_RECT, FS_RECT);
    prog_glyph = link_prog(VS_GLYPH, fsg);
    prog_poly = link_prog(VS_POLY, FS_POLY);
    char fsb[1536];
    snprintf(fsb, sizeof fsb, FS_BRIGHT_FMT,
             (double)FX_GLOW_FALLOFF, (double)FX_GLOW_TINT,
             ((FX_GLOW_COLOR >> 16) & 0xff) / 255.0,
             ((FX_GLOW_COLOR >> 8) & 0xff) / 255.0,
             (FX_GLOW_COLOR & 0xff) / 255.0);
    prog_bright = link_prog(VS_FULL, fsb);
    prog_blur = link_prog(VS_FULL, FS_BLUR);
    prog_post = link_prog(VS_FULL, build_post_source());

    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glGenBuffers(1, &vbo_quad);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    glGenBuffers(1, &vbo_rect);
    glGenBuffers(1, &vbo_glyph);
    glGenBuffers(1, &vbo_poly);

    glGenVertexArrays(1, &vao_rect);
    glBindVertexArray(vao_rect);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_rect);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(RectInst), (void *)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(RectInst), (void *)(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);

    glGenVertexArrays(1, &vao_glyph);
    glBindVertexArray(vao_glyph);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_glyph);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphInst), (void *)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphInst), (void *)(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphInst), (void *)(8 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GlyphInst), (void *)(12 * sizeof(float)));
    glVertexAttribDivisor(4, 1);

    glGenVertexArrays(1, &vao_poly);
    glBindVertexArray(vao_poly);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_poly);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glGenVertexArrays(1, &vao_empty);
    glBindVertexArray(0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    return true;
}

void render_fini(void) {
    free(rects);
    free(glyphs);
}

void render_resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == fb_w && h == fb_h) return;
    fb_w = w;
    fb_h = h;
    bloom_w = w / 4 > 1 ? w / 4 : 1;
    bloom_h = h / 4 > 1 ? h / 4 : 1;
    make_fbo(&fbo_scene, &tex_scene, fb_w, fb_h);
    make_fbo(&fbo_bloom[0], &tex_bloom[0], bloom_w, bloom_h);
    make_fbo(&fbo_bloom[1], &tex_bloom[1], bloom_w, bloom_h);
    if (FX_GHOST_LAYERS > 0) {
        make_fbo(&fbo_ghost[0], &tex_ghost[0], bloom_w, bloom_h);
        make_fbo(&fbo_ghost[1], &tex_ghost[1], bloom_w, bloom_h);
    }
    make_fbo(&fbo_persist[0], &tex_persist[0], fb_w, fb_h);
    make_fbo(&fbo_persist[1], &tex_persist[1], fb_w, fb_h);
    for (int i = 0; i < 2; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_persist[i]);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    trail_ready = false;
}

void render_bell(double now) {
    if (BELL_FLASH) bell_until = now + 0.12;
}

void render_toggle_fx(void) {
    crt_intro_done = true;
    fx_target = fx_target > 0.5f ? 0.0f : 1.0f;
}

bool render_fx_on(void) { return fx_target > 0.5f; }

bool render_animating(void) {
    if (CRT_OVERLAY && CRT_INTRO && !crt_intro_done) return true;
    if (fabsf(fx_level - fx_target) > 0.002f) return true;
    if (CRT_OVERLAY && fx_level > 0.001f &&
        ((double)CRT_NOISE > 0.0 || (double)CRT_FLICKER > 0.0 || (double)CRT_BURN_IN > 0.0))
        return true;
    if (FX_NOISE && FX_NOISE_ANIMATE) return true;
    if (FX_FLICKER) return true;
    return trail_moving || app.now < burnin_until || app.now < bell_until;
}

static float cross3(const float *px, const float *py, int o, int a, int b) {
    return (px[a] - px[o]) * (py[b] - py[o]) - (py[a] - py[o]) * (px[b] - px[o]);
}

static int hull(const float *px, const float *py, int n, float *out) {
    if (n < 3) return 0;
    int idx[16];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {
        int k = idx[i], j = i - 1;
        while (j >= 0 && (px[idx[j]] > px[k] || (px[idx[j]] == px[k] && py[idx[j]] > py[k]))) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = k;
    }

    int h[34], k = 0;
    for (int i = 0; i < n; i++) {
        int p = idx[i];
        while (k >= 2 && cross3(px, py, h[k - 2], h[k - 1], p) <= 0.0f) k--;
        h[k++] = p;
    }
    int lower = k + 1;
    for (int i = n - 2; i >= 0; i--) {
        int p = idx[i];
        while (k >= lower && cross3(px, py, h[k - 2], h[k - 1], p) <= 0.0f) k--;
        h[k++] = p;
    }
    k--;
    if (k < 3) return 0;
    for (int i = 0; i < k; i++) {
        out[i * 2 + 0] = px[h[i]];
        out[i * 2 + 1] = py[h[i]];
    }
    return k;
}

static void draw_rects(void) {
    if (!rect_n) return;
    glUseProgram(prog_rect);
    glUniform2f(glGetUniformLocation(prog_rect, "uRes"), (float)fb_w, (float)fb_h);
    glBindVertexArray(vao_rect);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_rect);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)rect_n * sizeof(RectInst), rects, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, rect_n);
    rect_n = 0;
}

#ifndef GL_SRC1_COLOR_EXT
#define GL_SRC1_COLOR_EXT 0x88F9
#define GL_SRC1_ALPHA_EXT 0x8589
#define GL_ONE_MINUS_SRC1_COLOR_EXT 0x88FA
#define GL_ONE_MINUS_SRC1_ALPHA_EXT 0x88FB
#endif

static void draw_glyphs(void) {
    if (!glyph_n) return;
    if (subpixel)
        glBlendFuncSeparate(GL_SRC1_COLOR_EXT, GL_ONE_MINUS_SRC1_COLOR_EXT,
                            GL_SRC1_ALPHA_EXT, GL_ONE_MINUS_SRC1_ALPHA_EXT);
    glUseProgram(prog_glyph);
    glUniform2f(glGetUniformLocation(prog_glyph, "uRes"), (float)fb_w, (float)fb_h);
    glUniform1i(glGetUniformLocation(prog_glyph, "uAtlas"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex());
    glBindVertexArray(vao_glyph);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_glyph);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)glyph_n * sizeof(GlyphInst), glyphs, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, glyph_n);
    glyph_n = 0;
    if (subpixel)
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void draw_poly(const float *c, float alpha) {
    if (poly_n < 3) return;
    glUseProgram(prog_poly);
    glUniform2f(glGetUniformLocation(prog_poly, "uRes"), (float)fb_w, (float)fb_h);
    glUniform4f(glGetUniformLocation(prog_poly, "uColor"), c[0], c[1], c[2], alpha);
    glBindVertexArray(vao_poly);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_poly);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)poly_n * 2 * sizeof(float), poly_pts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_FAN, 0, poly_n);
}

static void update_trail(float cx, float cy, float cw, float ch, double dt) {
    float tgx[4] = { cx, cx + cw, cx + cw, cx };
    float tgy[4] = { cy, cy, cy + ch, cy + ch };

    if (!trail_ready) {
        memcpy(trail_x, tgx, sizeof tgx);
        memcpy(trail_y, tgy, sizeof tgy);
        trail_ready = true;
        trail_moving = false;
        return;
    }

    float dx = tgx[0] - trail_x[0], dy = tgy[0] - trail_y[0];
    float dist = sqrtf(dx * dx + dy * dy);
    float thresh = (float)CURSOR_TRAIL_START * (float)font_cell_w();

    if (dist < 0.35f) {
        memcpy(trail_x, tgx, sizeof tgx);
        memcpy(trail_y, tgy, sizeof tgy);
        trail_moving = false;
        return;
    }
    if (!trail_moving && dist < thresh) {
        memcpy(trail_x, tgx, sizeof tgx);
        memcpy(trail_y, tgy, sizeof tgy);
        return;
    }

    float f = 1.0f - expf(-(float)dt / (float)CURSOR_TRAIL_DECAY);
    if (f > 1.0f) f = 1.0f;
    for (int i = 0; i < 4; i++) {
        trail_x[i] += (tgx[i] - trail_x[i]) * f;
        trail_y[i] += (tgy[i] - trail_y[i]) * f;
    }
    trail_moving = true;

    float px[8], py[8], out[64];
    for (int i = 0; i < 4; i++) {
        px[i] = trail_x[i]; py[i] = trail_y[i];
        px[4 + i] = tgx[i]; py[4 + i] = tgy[i];
    }
    poly_n = hull(px, py, 8, out);
    if (poly_n > 32) poly_n = 32;
    if (poly_n >= 3) memcpy(poly_pts, out, (size_t)poly_n * 2 * sizeof(float));
}

void render_frame(Term *t, double now, bool focused) {
    double dt = last_time < 0 ? 0.016 : now - last_time;
    if (dt > 0.25) dt = 0.25;
    last_time = now;

    if (CRT_OVERLAY && CRT_INTRO && !crt_intro_done) {
        if (crt_hold_until == 0.0)
            crt_hold_until = now + (double)CRT_INTRO_HOLD_MS / 1000.0;
        else if (now >= crt_hold_until) {
            fx_target = CRT_START_ON ? 1.0f : 0.0f;
            crt_intro_done = true;
        }
    }

    if (fx_level != fx_target) {
        float step = (float)dt / ((float)CRT_FADE_MS / 1000.0f);
        if (fx_level < fx_target) {
            fx_level += step;
            if (fx_level > fx_target) fx_level = fx_target;
        } else {
            fx_level -= step;
            if (fx_level < fx_target) fx_level = fx_target;
        }
    }

    int cw = font_cell_w(), chh = font_cell_h();
    int ox = INNER_BORDER + (app.width - 2 * INNER_BORDER - t->cols * cw) / 2;
    int oy = INNER_BORDER + (app.height - 2 * INNER_BORDER - t->rows * chh) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    float border[4], defbg[4];
    unpack(COLOR_BORDER, border);
    border[3] = (float)BG_OPACITY;
    unpack(COLOR_BG, defbg);
    defbg[3] = (float)BG_OPACITY;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_scene);
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_BLEND);
    glClearColor(border[0], border[1], border[2], border[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    push_rect((float)ox, (float)oy, (float)(t->cols * cw), (float)(t->rows * chh), defbg);

    bool have_sel = term_sel_active(t);
    float sel_bg[4], sel_fg[4];
    unpack(COLOR_SELECT_BG, sel_bg); sel_bg[3] = 1.0f;
    unpack(COLOR_SELECT_FG, sel_fg); sel_fg[3] = 1.0f;

    if (have_sel) {
        for (int y = 0; y < t->rows; y++) {
            int arow = term_abs_row(t, y);
            int run_start = -1;
            for (int x = 0; x <= t->cols; x++) {
                bool in = (x < t->cols) && term_sel_contains(t, x, arow);
                if (in && run_start < 0) run_start = x;
                else if (!in && run_start >= 0) {
                    push_rect((float)(ox + run_start * cw), (float)(oy + y * chh),
                              (float)((x - run_start) * cw), (float)chh, sel_bg);
                    run_start = -1;
                }
            }
        }
    }

    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_line(t, y);
        int x = 0;
        while (x < t->cols) {
            const Cell *c = &row[x];
            if (c->attr & ATTR_DUMMY) { x++; continue; }
            uint32_t fgc = c->fg, bgc = c->bg;
            if (((c->attr & ATTR_REVERSE) != 0) != t->reverse_video) {
                uint32_t tmp = fgc; fgc = bgc; bgc = tmp;
            }
            float bg[4];
            resolve(bgc, false, bg);
            int w = (c->attr & ATTR_WIDE) ? 2 : 1;
            int run = w;
            if (bg[3] > 0.0f) {
                while (x + run < t->cols) {
                    const Cell *n = &row[x + run];
                    if (n->attr & ATTR_DUMMY) { run++; continue; }
                    uint32_t nb = n->bg, nf = n->fg;
                    if (((n->attr & ATTR_REVERSE) != 0) != t->reverse_video) nb = nf;
                    if (nb != bgc) break;
                    run += (n->attr & ATTR_WIDE) ? 2 : 1;
                }
                if (bgc != COL_DEF_BG && !(have_sel && term_sel_contains(t, x, term_abs_row(t, y))))
                    push_rect((float)(ox + x * cw), (float)(oy + y * chh),
                              (float)(run * cw), (float)chh, bg);
            }
            x += run;
        }
    }

    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_line(t, y);
        for (int x = 0; x < t->cols; x++) {
            const Cell *c = &row[x];
            if (!(c->attr & (ATTR_UNDERLINE | ATTR_STRIKE))) continue;
            uint32_t fgc = c->fg, bgc = c->bg;
            if (((c->attr & ATTR_REVERSE) != 0) != t->reverse_video) {
                uint32_t tmp = fgc; fgc = bgc; bgc = tmp;
            }
            float fg[4], ubg[4];
            resolve(fgc, true, fg);
            if (c->attr & ATTR_DIM) { fg[0] *= 0.6f; fg[1] *= 0.6f; fg[2] *= 0.6f; }
            resolve(bgc, false, ubg);
            min_contrast(fg, ubg);
            if (c->attr & ATTR_UNDERLINE)
                push_rect((float)(ox + x * cw), (float)(oy + y * chh + font_underline_pos()),
                          (float)cw, (float)font_underline_thickness(), fg);
            if (c->attr & ATTR_STRIKE)
                push_rect((float)(ox + x * cw), (float)(oy + y * chh + chh * 0.55f),
                          (float)cw, (float)font_underline_thickness(), fg);
        }
    }
    draw_rects();

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    bool show_cursor = t->cursor_visible && t->scroll_off == 0;
    float curx = (float)(ox + t->cx * cw);
    float cury = (float)(oy + t->cy * chh);

    float ccol[4];
    unpack(CURSOR_TRAIL_COLOR, ccol);
    ccol[3] = 1.0f;

    if (show_cursor) {
        update_trail(curx, cury, (float)cw, (float)chh, dt);
        if (trail_moving) draw_poly(ccol, (float)CURSOR_TRAIL_OPACITY);
    } else {
        trail_ready = false;
        trail_moving = false;
    }

    float cur_rgb[4];
    unpack(COLOR_CURSOR, cur_rgb);
    cur_rgb[3] = 1.0f;
    bool block_cursor = false;
    if (show_cursor) {
        if (!focused) {
            float thin = 1.0f;
            push_rect(curx, cury, (float)cw, thin, cur_rgb);
            push_rect(curx, cury + chh - thin, (float)cw, thin, cur_rgb);
            push_rect(curx, cury, thin, (float)chh, cur_rgb);
            push_rect(curx + cw - thin, cury, thin, (float)chh, cur_rgb);
        } else if (t->cursor_shape == CURSOR_BLOCK) {
            push_rect(curx, cury, (float)cw, (float)chh, cur_rgb);
            block_cursor = true;
        } else if (t->cursor_shape == CURSOR_BEAM) {
            push_rect(curx, cury, 2.0f, (float)chh, cur_rgb);
        } else {
            push_rect(curx, cury + chh - 2.0f, (float)cw, 2.0f, cur_rgb);
        }
        draw_rects();
    }

    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_line(t, y);
        for (int x = 0; x < t->cols; x++) {
            const Cell *c = &row[x];
            if (c->attr & (ATTR_DUMMY | ATTR_HIDDEN)) continue;
            if (c->cp == 0 || c->cp == ' ') continue;

            uint32_t fgc = c->fg, bgc = c->bg;
            if (((c->attr & ATTR_REVERSE) != 0) != t->reverse_video) {
                uint32_t tmp = fgc; fgc = bgc; bgc = tmp;
            }
            if (BOLD_IS_BRIGHT && (c->attr & ATTR_BOLD) && COL_IS_IDX(fgc) && fgc < 8) fgc += 8;

            float fg[4];
            if (have_sel && term_sel_contains(t, x, term_abs_row(t, y))) {
                fg[0] = sel_fg[0]; fg[1] = sel_fg[1]; fg[2] = sel_fg[2]; fg[3] = 1.0f;
            } else if (block_cursor && t->scroll_off == 0 && x == t->cx && y == t->cy) {
                unpack(COLOR_CURSOR_TEXT, fg);
                fg[3] = 1.0f;
            } else {
                resolve(fgc, true, fg);
                if (c->attr & ATTR_DIM) { fg[0] *= 0.6f; fg[1] *= 0.6f; fg[2] *= 0.6f; }
                float cellbg[4];
                resolve(bgc, false, cellbg);
                min_contrast(fg, cellbg);
            }

            int style = STYLE_REGULAR;
            if ((c->attr & ATTR_BOLD) && (c->attr & ATTR_ITALIC)) style = STYLE_BOLD_ITALIC;
            else if (c->attr & ATTR_BOLD) style = STYLE_BOLD;
            else if (c->attr & ATTR_ITALIC) style = STYLE_ITALIC;

            const Glyph *g = font_glyph(c->cp, style);
            if (!g) continue;
            int span = (c->attr & ATTR_WIDE) ? 2 * cw : cw;
            int pad = (g->adv > 0 && g->adv < span) ? (span - g->adv) / 2 : 0;
            float gx = (float)(ox + x * cw + pad + g->bx + GLYPH_X_OFFSET);
            float gy = (float)(oy + y * chh + font_baseline() - g->by + GLYPH_Y_OFFSET);
            push_glyph(g, gx, gy, fg);
        }
    }
    draw_glyphs();

    glDisable(GL_BLEND);

    if (t->dirty || trail_moving) burnin_until = now + 0.9;

    GLuint persist_src = tex_persist[persist_cur];
    if (FX_BURN_IN) {
        static GLuint prog_persist;
        if (!prog_persist) prog_persist = link_prog(VS_FULL, FS_PERSIST);
        int dst = persist_cur ^ 1;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_persist[dst]);
        glViewport(0, 0, fb_w, fb_h);
        glUseProgram(prog_persist);
        glUniform1i(glGetUniformLocation(prog_persist, "uScene"), 0);
        glUniform1i(glGetUniformLocation(prog_persist, "uPrev"), 1);
        glUniform1f(glGetUniformLocation(prog_persist, "uDecay"), powf(0.0025f, (float)dt));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_scene);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_persist[persist_cur]);
        glBindVertexArray(vao_empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        persist_cur = dst;
        persist_src = tex_persist[dst];
    }

    if (FX_GHOST_LAYERS > 0) {
        glUseProgram(prog_blur);
        glUniform1i(glGetUniformLocation(prog_blur, "uTex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao_empty);
        glViewport(0, 0, bloom_w, bloom_h);

        float gscale = (float)font_cell_h() / 26.0f;
        if (gscale < 0.4f) gscale = 0.4f;
        if (gscale > 3.0f) gscale = 3.0f;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_ghost[0]);
        glUniform2f(glGetUniformLocation(prog_blur, "uDir"), gscale / bloom_w, 0.0f);
        glBindTexture(GL_TEXTURE_2D, tex_scene);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        for (int pass = 0; pass < FX_GHOST_SOFTNESS; pass++) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_ghost[1]);
            glUniform2f(glGetUniformLocation(prog_blur, "uDir"), 0.0f, gscale / bloom_h);
            glBindTexture(GL_TEXTURE_2D, tex_ghost[0]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_ghost[0]);
            glUniform2f(glGetUniformLocation(prog_blur, "uDir"), gscale / bloom_w, 0.0f);
            glBindTexture(GL_TEXTURE_2D, tex_ghost[1]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    if (FX_GLOW) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_bloom[0]);
        glViewport(0, 0, bloom_w, bloom_h);
        glUseProgram(prog_bright);
        glUniform1i(glGetUniformLocation(prog_bright, "uTex"), 0);
        glUniform1f(glGetUniformLocation(prog_bright, "uThreshold"), (float)FX_GLOW_THRESHOLD);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_scene);
        glBindVertexArray(vao_empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glUseProgram(prog_blur);
        glUniform1i(glGetUniformLocation(prog_blur, "uTex"), 0);
        for (int pass = 0; pass < FX_GLOW_RADIUS; pass++) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_bloom[1]);
            glUniform2f(glGetUniformLocation(prog_blur, "uDir"), 1.0f / bloom_w, 0.0f);
            glBindTexture(GL_TEXTURE_2D, tex_bloom[0]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_bloom[0]);
            glUniform2f(glGetUniformLocation(prog_blur, "uDir"), 0.0f, 1.0f / bloom_h);
            glBindTexture(GL_TEXTURE_2D, tex_bloom[1]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog_post);
    glUniform1i(glGetUniformLocation(prog_post, "uScene"), 0);
    glUniform1i(glGetUniformLocation(prog_post, "uBloom"), 1);
    glUniform1i(glGetUniformLocation(prog_post, "uPersist"), 2);
    glUniform1i(glGetUniformLocation(prog_post, "uGhost"), 3);
    glUniform2f(glGetUniformLocation(prog_post, "uRes"), (float)fb_w, (float)fb_h);
    glUniform1f(glGetUniformLocation(prog_post, "uTime"), (float)now);
    glUniform1f(glGetUniformLocation(prog_post, "uCell"), (float)font_cell_h());
    glUniform1f(glGetUniformLocation(prog_post, "uFx"), fx_level);
    glUniform1f(glGetUniformLocation(prog_post, "uBell"), now < bell_until ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_scene);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, FX_GLOW ? tex_bloom[0] : tex_scene);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, persist_src);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, FX_GHOST_LAYERS > 0 ? tex_ghost[0] : tex_scene);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    t->dirty = false;
    t->cursor_moved = false;
}
