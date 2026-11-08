#include "textures.h"
#include "noise.h"
#include "mathx.h"
#include "world.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

static image_t image_new(int w, int h) {
    image_t img = { w, h, (uint32_t*)calloc((size_t)w * (size_t)h, sizeof(uint32_t)) };
    return img;
}

void image_free(image_t* img) {
    free(img->px);
    img->px = 0;
}

static uint32_t rgbf(float r, float g, float b) {
    int ir = (int)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    int ig = (int)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    int ib = (int)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    return RGB(ir, ig, ib);
}
static uint32_t rgbv(vec3 c) { return rgbf(c.x, c.y, c.z); }

static void put(image_t* img, int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < img->w && y < img->h) {
        img->px[y * img->w + x] = c;
    }
}

static void fill_rect(image_t* img, int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            put(img, i, j, c);
        }
    }
}

// ---------------------------------------------------------------------------
//  5x7 pixel font
// ---------------------------------------------------------------------------
static const char* FONT_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.!";
static const char* FONT[][7] = {
    { " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" }, // A
    { "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### " }, // B
    { " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### " }, // C
    { "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### " }, // D
    { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####" }, // E
    { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    " }, // F
    { " ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####" }, // G
    { "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" }, // H
    { " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " }, // I
    { "  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  " }, // J
    { "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #" }, // K
    { "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####" }, // L
    { "#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #" }, // M
    { "#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #" }, // N
    { " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " }, // O
    { "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    " }, // P
    { " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #" }, // Q
    { "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #" }, // R
    { " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### " }, // S
    { "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  " }, // T
    { "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " }, // U
    { "#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  " }, // V
    { "#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # " }, // W
    { "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #" }, // X
    { "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  " }, // Y
    { "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####" }, // Z
    { " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### " }, // 0
    { "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " }, // 1
    { " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####" }, // 2
    { "#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### " }, // 3
    { "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # " }, // 4
    { "#####", "#    ", "#### ", "    #", "    #", "#   #", " ### " }, // 5
    { "  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### " }, // 6
    { "#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   " }, // 7
    { " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### " }, // 8
    { " ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  " }, // 9
    { "     ", "     ", "     ", "#####", "     ", "     ", "     " }, // -
    { "     ", "     ", "     ", "     ", "     ", " ##  ", " ##  " }, // .
    { "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  " }, // !
};

static int text_width(const char* s, int scale) {
    return ((int)strlen(s) * 6 - 1) * scale;
}

static void draw_text(image_t* img, int x, int y, int scale, uint32_t col, const char* s) {
    for (int ci = 0; s[ci]; ci++) {
        const char* p = strchr(FONT_CHARS, s[ci]);
        if (s[ci] != ' ' && p) {
            int g = (int)(p - FONT_CHARS);
            for (int r = 0; r < 7; r++) {
                for (int c = 0; c < 5; c++) {
                    if (FONT[g][r][c] == '#') {
                        fill_rect(img, x + (ci * 6 + c) * scale, y + r * scale, scale, scale, col);
                    }
                }
            }
        }
    }
}

static void draw_text_centered(image_t* img, int cx, int y, int scale, uint32_t col, uint32_t shadow, const char* s) {
    int x = cx - text_width(s, scale) / 2;
    if (shadow) {
        draw_text(img, x + scale / 2 + 1, y + scale / 2 + 1, scale, shadow, s);
    }
    draw_text(img, x, y, scale, col, s);
}

// ---------------------------------------------------------------------------
//  Basic textures
// ---------------------------------------------------------------------------
image_t tex_gen_white(void) {
    image_t img = image_new(4, 4);
    for (int i = 0; i < 16; i++) {
        img.px[i] = RGB(255, 255, 255);
    }
    return img;
}

image_t tex_gen_detail(void) {
    const int N = 64;
    image_t img = image_new(N, N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float n = nz_fbm((float)x / 4.0f, (float)y / 4.0f, 4, N / 4, N / 4, 3u);
            float g = 0.78f + 0.24f * n + (nz_hash2(x, y, 9u) - 0.5f) * 0.08f;
            img.px[y * N + x] = rgbf(g, g, g);
        }
    }
    return img;
}

image_t tex_gen_road(float dash_period) {
    const int W = 128, H = 256;
    image_t img = image_new(W, H);
    for (int y = 0; y < H; y++) {
        float along = ((float)y + 0.5f) / (float)H * dash_period;
        for (int x = 0; x < W; x++) {
            float lat = ((float)x + 0.5f) / (float)W * ROAD_HALF;
            float big = nz_fbm((float)x / 8.0f, (float)y / 8.0f, 3, W / 8, H / 8, 17u);
            float grain = nz_hash2(x, y, 23u) - 0.5f;
            float a = 0.29f + 0.10f * (big - 0.5f) + grain * 0.07f;
            if (lat < MEDIAN_HALF) {
                a += 0.05f;
            }
            if (lat > 8.36f) {
                a += 0.08f + grain * 0.05f;
            }
            // darker wheel tracks in both lanes
            const float tracks[4] = { 2.0f, 3.6f, 5.6f, 7.2f };
            for (int t = 0; t < 4; t++) {
                float d = (lat - tracks[t]) / 0.35f;
                a -= 0.035f * expf(-d * d);
            }
            vec3 c = v3(a, a, a * 1.04f);
            float wear = 0.82f + 0.18f * nz_value((float)x / 3.0f, (float)y / 3.0f, W / 3, 0, 31u);
            if (lat > 0.76f && lat < 0.92f) {
                c = v3(0.88f * wear, 0.72f * wear, 0.16f * wear);              // yellow inner edge line
            } else if (fabsf(lat - 4.6f) < 0.08f && along < 3.0f) {
                c = v3(0.92f * wear, 0.92f * wear, 0.90f * wear);              // lane dashes
            } else if (lat > 8.2f && lat < 8.36f) {
                c = v3(0.92f * wear, 0.92f * wear, 0.90f * wear);              // outer edge line
            }
            img.px[y * W + x] = rgbv(c);
        }
    }
    return img;
}

// The sky is composited from precomputed layers (clouds, mountain ranges, a city skyline)
// so it can be re-lit cheaply while the sun sets in the future-city phase.
#define SKY_W 1024
#define SKY_H 256
static struct {
    bool ready;
    float far_m[SKY_W], near_m[SKY_W], city_m[SKY_W];
    float cos_th[SKY_W], sin_th[SKY_W];
    float elev[SKY_H];
    float* cover;    // cloud coverage per pixel
    float* bright;   // cloud brightness per pixel
    uint8_t* lit;    // skyline window lights
} SK;

static void sky_layers_init(void) {
    if (SK.ready) return;
    SK.ready = true;
    for (int x = 0; x < SKY_W; x++) {
        float u = ((float)x + 0.5f) / (float)SKY_W;
        float f = clampf((nz_fbm(u * 12.0f, 3.7f, 5, 12, 0, 11u) - 0.30f) / 0.42f, 0.0f, 1.0f);
        SK.far_m[x] = 0.6f + 6.0f * f * f;
        float g = clampf((nz_fbm(u * 20.0f, 9.1f, 4, 20, 0, 12u) - 0.30f) / 0.45f, 0.0f, 1.0f);
        SK.near_m[x] = 0.3f + 2.6f * g * g;
        SK.cos_th[x] = cosf(u * 2.0f * PI_F);
        SK.sin_th[x] = sinf(u * 2.0f * PI_F);
    }
    // distant megacity: blocky towers of random widths, taller in a few clusters
    for (int x = 0; x < SKY_W;) {
        int w = 2 + (int)(nz_hash2(x, 1, 61u) * 9.0f);
        float u = (float)x / (float)SKY_W;
        float cluster = nz_fbm(u * 6.0f, 1.3f, 3, 6, 0, 62u);
        float h = 0.5f + nz_hash2(x, 2, 63u) * (1.2f + 6.0f * cluster * cluster);
        if (nz_hash2(x, 3, 64u) > 0.93f) h += 2.5f;   // the odd needle-like spire
        for (int k = 0; k < w && x < SKY_W; k++, x++) {
            SK.city_m[x] = h;
        }
    }
    SK.cover = (float*)calloc((size_t)SKY_W * SKY_H, sizeof(float));
    SK.bright = (float*)calloc((size_t)SKY_W * SKY_H, sizeof(float));
    SK.lit = (uint8_t*)calloc((size_t)SKY_W * SKY_H, 1);
    for (int y = 0; y < SKY_H; y++) {
        float yw = SKY_TOP - ((float)y + 0.5f) / (float)SKY_H * (SKY_TOP - SKY_BOTTOM);
        SK.elev[y] = atan2f(yw, SKY_RADIUS);
        float ce = cosf(SK.elev[y]), se = sinf(SK.elev[y]);
        for (int x = 0; x < SKY_W; x++) {
            int i = y * SKY_W + x;
            vec3 dir = v3(SK.cos_th[x] * ce, se, SK.sin_th[x] * ce);
            if (dir.y > 0.01f) {
                float px = dir.x / dir.y, pz = dir.z / dir.y;
                float n = nz_fbm(px * 0.9f + 3.0f, pz * 0.9f - 7.0f, 5, 0, 0, 21u);
                SK.cover[i] = smoothstepf(0.52f, 0.72f, n) * smoothstepf(0.02f, 0.15f, dir.y);
                SK.bright[i] = smoothstepf(0.55f, 0.8f, n);
            }
            SK.lit[i] = (nz_hash2(x / 2, y, 65u) > 0.80f && (y & 1) == 0) ? 1 : 0;
        }
    }
}

image_t tex_gen_sky_ex(const sky_params_t* sp) {
    sky_layers_init();
    image_t img = image_new(SKY_W, SKY_H);
    const float k = clampf(sp->sunset, 0.0f, 1.0f), city = clampf(sp->city, 0.0f, 1.0f);
    const vec3 fog = sp->fog;
    const vec3 zenith = v3_lerp(v3(0.15f, 0.34f, 0.80f), v3(0.12f, 0.13f, 0.36f), k);
    const vec3 hz_day = v3(FOG_R, FOG_G, FOG_B);
    const vec3 hz_sun = v3(1.0f, 0.62f, 0.30f), hz_away = v3(0.60f, 0.43f, 0.60f);
    const float el = sp->sun_elev_deg * PI_F / 180.0f;
    const float sth = SUN_U * 2.0f * PI_F;
    const vec3 sun = v3(cosf(sth) * cosf(el), sinf(el), sinf(sth) * cosf(el));
    const vec3 glow_col = v3_lerp(v3(1.0f, 0.85f, 0.6f), v3(1.0f, 0.52f, 0.22f), k);
    const vec3 disk_col = v3_lerp(v3(1.0f, 0.98f, 0.88f), v3(1.0f, 0.86f, 0.55f), k);
    const float disk_cos = cosf((2.2f + 0.8f * k) * PI_F / 180.0f);
    const float wide = 0.10f + 0.30f * k;
    for (int y = 0; y < SKY_H; y++) {
        float elev = SK.elev[y];
        float ed = elev * 180.0f / PI_F;
        float ce = cosf(elev), se = sinf(elev);
        for (int x = 0; x < SKY_W; x++) {
            int i = y * SKY_W + x;
            vec3 dir = v3(SK.cos_th[x] * ce, se, SK.sin_th[x] * ce);
            // horizon colour: plain fog by day, warm towards the sun and violet away from it at dusk
            float toward = 0.5f + 0.5f * (SK.cos_th[x] * cosf(sth) + SK.sin_th[x] * sinf(sth));
            vec3 hz = v3_lerp(hz_day, v3_lerp(hz_away, hz_sun, toward * toward), k);
            vec3 c = fog;
            if (ed > 0.0f) {
                c = v3_lerp(hz, zenith, powf(clampf(ed / 55.0f, 0.0f, 1.0f), 0.5f));
                c = v3_lerp(fog, c, smoothstepf(-0.5f, 5.0f, ed) * k + (1.0f - k));
            }
            // sun glow + disk
            float cs = fmaxf(v3_dot(dir, sun), 0.0f);
            float c2 = cs * cs, c4 = c2 * c2, c8 = c4 * c4, c16 = c8 * c8, c64 = c16 * c16 * c16 * c16;
            float glow = c64 * 0.35f + c8 * wide;
            c = v3_add(c, v3_scale(glow_col, glow));
            if (cs > disk_cos) {
                c = disk_col;
            }
            // clouds (lit pink and orange from below at dusk)
            float cover = SK.cover[i];
            if (cover > 0.0f) {
                vec3 cc = v3_lerp(v3(0.78f, 0.80f, 0.88f), v3(1.0f, 1.0f, 1.0f), SK.bright[i]);
                cc = v3_lerp(cc, v3_lerp(v3(0.55f, 0.40f, 0.55f), v3(1.0f, 0.62f, 0.45f), SK.bright[i] * 0.6f + toward * 0.4f), k);
                cc = v3_add(cc, v3_scale(glow_col, glow * 0.5f));
                c = v3_lerp(c, cc, cover * 0.9f);
            }
            // distant mountain ranges, replaced by a megacity skyline in the future-city phase
            if (ed > 0.0f) {
                vec3 m = c;
                if (ed < SK.far_m[x] || ed < SK.near_m[x]) {
                    vec3 mc;
                    if (ed < SK.near_m[x]) {
                        mc = v3_lerp(fog, v3_lerp(v3(0.30f, 0.42f, 0.44f), v3(0.32f, 0.24f, 0.36f), k), 0.55f);
                    } else {
                        mc = v3_lerp(fog, v3_lerp(v3(0.42f, 0.50f, 0.64f), v3(0.46f, 0.34f, 0.48f), k), 0.55f);
                        if (SK.far_m[x] > 3.5f && ed > SK.far_m[x] - 0.8f) {
                            mc = v3_lerp(v3(0.86f, 0.89f, 0.95f), v3(1.0f, 0.74f, 0.62f), k);
                        }
                    }
                    m = v3_lerp(fog, mc, 0.3f + 0.7f * smoothstepf(0.0f, 1.6f, ed));
                }
                vec3 s = c;
                if (city > 0.0f && ed < SK.city_m[x]) {
                    vec3 sc = v3_lerp(fog, v3_lerp(v3(0.36f, 0.42f, 0.52f), v3(0.20f, 0.17f, 0.30f), k), 0.65f);
                    if (SK.lit[i]) {
                        sc = v3_lerp(sc, v3(1.0f, 0.86f, 0.55f), 0.25f + 0.65f * k);
                    }
                    s = sc;
                }
                c = v3_lerp(m, s, city);
            }
            img.px[i] = rgbv(c);
        }
    }
    return img;
}

image_t tex_gen_sky(void) {
    sky_params_t sp = { SUN_ELEV_DEG, 0.0f, 0.0f, { FOG_R, FOG_G, FOG_B } };
    return tex_gen_sky_ex(&sp);
}

// ---------------------------------------------------------------------------
//  Future city: tower facades and street blocks
// ---------------------------------------------------------------------------
// 128x128 texels = 32 x 32 m of facade: 8 floors x 8 window bays of 4 m.
// Lit windows have alpha 0 (they glow at dusk); everything else is opaque.
image_t tex_gen_facade(void) {
    const int N = 128;
    image_t img = image_new(N, N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int cx = x / 16, cy = y / 16, lx = x % 16, ly = y % 16;
            float grain = (nz_hash2(x, y, 71u) - 0.5f) * 0.05f;
            uint32_t col;
            if (ly < 3) {
                col = rgbf(0.66f + grain, 0.67f + grain, 0.70f + grain);          // floor slab
            } else if (lx < 2) {
                col = rgbf(0.52f + grain, 0.53f + grain, 0.56f + grain);          // mullion
            } else {
                float h = nz_hash2(cx, cy, 72u);
                if (h > 0.70f) {   // lit window: warm or cool light
                    vec3 l = (nz_hash2(cx, cy, 73u) > 0.3f) ? v3(1.0f, 0.82f, 0.50f) : v3(0.70f, 0.92f, 1.0f);
                    float v = 0.85f + 0.15f * (float)(ly - 3) / 12.0f;
                    col = rgbf(l.x * v, l.y * v, l.z * v) & 0x00FFFFFFu;
                } else {           // glass, brighter towards the top of the pane
                    float v = 0.30f + 0.25f * (1.0f - (float)(ly - 3) / 12.0f) + grain;
                    col = rgbf(v * 0.75f, v * 0.88f, v * 1.1f);
                }
            }
            img.px[y * N + x] = col;
        }
    }
    return img;
}

// 128x128 texels = one 64 x 64 m city block: street with dashed centre line around the
// edge, glowing kerbs (alpha 0), pavement and a tiled plaza in the middle
image_t tex_gen_city_ground(void) {
    const int N = 128;
    image_t img = image_new(N, N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int ex = x < N / 2 ? x : N - 1 - x, ey = y < N / 2 ? y : N - 1 - y;
            int e = ex < ey ? ex : ey;    // distance (texels) from the block edge = street centre
            float grain = (nz_hash2(x, y, 81u) - 0.5f) * 0.05f;
            uint32_t col;
            if (e == 0 && (((ex == 0 ? y : x) / 6) & 1)) {
                col = rgbf(0.80f, 0.68f, 0.25f);                                   // centre line
            } else if (e < 10) {
                col = rgbf(0.15f + grain, 0.16f + grain, 0.20f + grain);           // asphalt
            } else if (e == 10) {
                col = rgbf(0.30f, 0.95f, 1.0f) & 0x00FFFFFFu;                      // glowing kerb
            } else if (e < 18) {
                col = rgbf(0.40f + grain, 0.41f + grain, 0.45f + grain);           // pavement
            } else {
                bool joint = ((x % 8) == 0) || ((y % 8) == 0);
                float v = joint ? 0.26f : 0.33f + grain;
                col = rgbf(v, v, v * 1.08f);                                        // plaza tiles
            }
            img.px[y * N + x] = col;
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
//  Atlas: billboards (slots 0..3) and highway signs (slots 4..6)
// ---------------------------------------------------------------------------
static void ad_sokol(image_t* img, int oy) {
    const int hz = 88;
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 256; x++) {
            vec3 c;
            if (y < hz) {
                float t = (float)y / (float)hz;
                c = (t < 0.65f) ? v3_lerp(v3(0.14f, 0.03f, 0.30f), v3(0.78f, 0.16f, 0.46f), t / 0.65f)
                                : v3_lerp(v3(0.78f, 0.16f, 0.46f), v3(1.0f, 0.56f, 0.22f), (t - 0.65f) / 0.35f);
                float dx = (float)x - 128.0f, dy = (float)y - (float)hz;
                if (dx * dx + dy * dy < 40.0f * 40.0f) {
                    float st = (float)(hz - y);
                    bool gap = (y > 62) && (fmodf(st, 7.0f) < (1.0f + (24.0f - st) * 0.12f));
                    if (!gap) {
                        c = v3_lerp(v3(1.0f, 0.92f, 0.30f), v3(1.0f, 0.30f, 0.52f), clampf(((float)y - 48.0f) / 40.0f, 0.0f, 1.0f));
                    }
                }
            } else {
                c = v3(0.10f, 0.02f, 0.18f);
                float gy = (float)(y - hz);
                float gx = ((float)x - 128.0f) / (gy + 2.0f) * 1.6f;
                bool line = fabsf(gx - floorf(gx + 0.5f)) < 0.07f;
                for (int k = 1; k < 10; k++) {
                    if (fabsf(gy - (float)(k * k) * 0.55f) < 0.6f) {
                        line = true;
                    }
                }
                if (line) {
                    c = v3(1.0f, 0.25f, 0.85f);
                }
            }
            put(img, x, oy + y, rgbv(c));
        }
    }
    draw_text_centered(img, 128, oy + 10, 5, rgbf(0.65f, 1.0f, 1.0f), rgbf(0.9f, 0.1f, 0.6f), "SOKOL");
}

static void ad_gaia(image_t* img, int oy) {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 256; x++) {
            vec3 c = v3_lerp(v3(0.02f, 0.03f, 0.12f), v3(0.08f, 0.10f, 0.30f), (float)y / 127.0f);
            if (nz_hash2(x, y, 41u) > 0.985f) {
                c = v3(0.9f, 0.9f, 1.0f);
            }
            float dx = (float)x - 52.0f, dy = (float)y - 64.0f;
            float r2 = dx * dx + dy * dy;
            if (r2 < 40.0f * 40.0f) {
                float n = nz_fbm((float)x / 14.0f, (float)y / 14.0f, 4, 0, 0, 55u);
                c = (n > 0.53f) ? v3(0.20f, 0.62f, 0.28f) : v3(0.10f, 0.35f, 0.82f);
                float shade = 1.15f - 0.55f * sqrtf(fmaxf(0.0f, (dx + 14.0f) * (dx + 14.0f) + (dy + 14.0f) * (dy + 14.0f))) / 60.0f;
                c = v3_scale(c, clampf(shade, 0.35f, 1.2f));
            }
            put(img, x, oy + y, rgbv(c));
        }
    }
    draw_text(img, 112, oy + 20, 5, rgbf(1, 1, 1), "GAIA");
    draw_text(img, 120, oy + 76, 4, rgbf(1.0f, 0.65f, 0.15f), "2026");
}

static void thick_line(image_t* img, float x0, float y0, float x1, float y1, float w, uint32_t col) {
    float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    for (float t = 0.0f; t <= len; t += 0.5f) {
        float x = x0 + (x1 - x0) * t / len, y = y0 + (y1 - y0) * t / len;
        fill_rect(img, (int)(x - w * 0.5f), (int)(y - w * 0.5f), (int)w, (int)w, col);
    }
}

static void fill_circle(image_t* img, int cx, int cy, int r, uint32_t col) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) put(img, cx + x, cy + y, col);
        }
    }
}

static void rect_outline(image_t* img, int x, int y, int w, int h, int t, uint32_t col) {
    fill_rect(img, x, y, w, t, col);
    fill_rect(img, x, y + h - t, w, t, col);
    fill_rect(img, x, y, t, h, col);
    fill_rect(img, x + w - t, y, t, h, col);
}

// "RSI - improve yourself ... recursively" with nested frames
static void ad_rsi(image_t* img, int oy) {
    for (int y = 0; y < 128; y++) {
        uint32_t c = rgbv(v3_lerp(v3(0.18f, 0.04f, 0.34f), v3(0.55f, 0.10f, 0.45f), (float)y / 127.0f));
        fill_rect(img, 0, oy + y, 256, 1, c);
    }
    float w = 250.0f, h = 122.0f;
    for (int k = 0; k < 9; k++) {
        uint32_t c = (k & 1) ? rgbf(0.95f, 0.35f, 0.75f) : rgbf(0.35f, 0.85f, 1.0f);
        rect_outline(img, (int)(128.0f - w * 0.5f), oy + (int)(64.0f - h * 0.5f), (int)w, (int)h, 1, c);
        w *= 0.82f; h *= 0.82f;
    }
    draw_text_centered(img, 128, oy + 6, 6, rgbf(1, 1, 1), rgbf(0.3f, 0.0f, 0.3f), "RSI");
    draw_text_centered(img, 128, oy + 60, 2, rgbf(1, 1, 1), rgbf(0.2f, 0.0f, 0.2f), "IMPROVE YOURSELF");
    draw_text_centered(img, 128, oy + 84, 3, rgbf(1.0f, 0.86f, 0.25f), rgbf(0.3f, 0.0f, 0.2f), "RECURSIVELY");
}

// "time horizon - x2 every 4 months" with an exponential curve
static void ad_horizon(image_t* img, int oy) {
    fill_rect(img, 0, oy, 256, 128, rgbf(0.03f, 0.06f, 0.18f));
    for (int x = 16; x < 256; x += 24) fill_rect(img, x, oy + 34, 1, 70, rgbf(0.12f, 0.20f, 0.42f));
    for (int y = 40; y < 104; y += 16) fill_rect(img, 16, oy + y, 224, 1, rgbf(0.12f, 0.20f, 0.42f));
    float px = -1, py = -1;
    for (int x = 18; x <= 238; x++) {
        float t = (float)(x - 18) / 220.0f;
        float y = 100.0f - 62.0f * (expf(3.2f * t) - 1.0f) / (expf(3.2f) - 1.0f);
        if (px >= 0) thick_line(img, px, (float)oy + py, (float)x, (float)oy + y, 3.0f, rgbf(0.30f, 0.95f, 0.40f));
        px = (float)x; py = y;
        if ((x - 18) % 36 == 0) fill_rect(img, x - 3, oy + (int)y - 3, 7, 7, rgbf(1.0f, 0.86f, 0.25f));
    }
    draw_text_centered(img, 128, oy + 8, 3, rgbf(1, 1, 1), rgbf(0, 0, 0), "TIME HORIZON");
    draw_text_centered(img, 128, oy + 110, 2, rgbf(0.6f, 1.0f, 0.6f), 0, "X2 EVERY 4 MONTHS");
}

// "agents - working nonstop" with a little robot
static void ad_agents(image_t* img, int oy) {
    fill_rect(img, 0, oy, 256, 128, rgbf(0.95f, 0.55f, 0.10f));
    fill_rect(img, 0, oy + 118, 256, 10, rgbf(0.15f, 0.10f, 0.08f));
    fill_rect(img, 40, oy + 16, 4, 12, rgbf(0.3f, 0.3f, 0.35f));      // antenna
    fill_circle(img, 42, oy + 14, 4, rgbf(1.0f, 0.2f, 0.2f));
    fill_rect(img, 14, oy + 28, 58, 50, rgbf(0.78f, 0.80f, 0.85f));   // head
    fill_rect(img, 24, oy + 40, 12, 12, rgbf(0.2f, 0.9f, 1.0f));      // eyes
    fill_rect(img, 50, oy + 40, 12, 12, rgbf(0.2f, 0.9f, 1.0f));
    fill_rect(img, 26, oy + 62, 34, 6, rgbf(0.25f, 0.25f, 0.3f));     // mouth
    fill_rect(img, 22, oy + 80, 42, 30, rgbf(0.60f, 0.62f, 0.68f));   // body
    draw_text(img, 90, oy + 18, 4, rgbf(0.08f, 0.05f, 0.03f), "AGENTS");
    draw_text(img, 92, oy + 60, 3, rgbf(1, 1, 1), "WORKING");
    draw_text(img, 92, oy + 88, 3, rgbf(1, 1, 1), "NONSTOP");
}

// "fresh tokens - farm shop" over a little field scene
static void ad_tokens(image_t* img, int oy) {
    for (int y = 0; y < 128; y++) {
        vec3 c = (y < 64) ? v3_lerp(v3(0.45f, 0.70f, 0.95f), v3(0.75f, 0.88f, 1.0f), (float)y / 64.0f)
                          : ((y / 6) % 2 ? v3(0.35f, 0.62f, 0.22f) : v3(0.30f, 0.55f, 0.18f));
        fill_rect(img, 0, oy + y, 256, 1, rgbv(c));
    }
    for (int i = 0; i < 5; i++) {
        int cx = 48 + i * 40;
        fill_circle(img, cx, oy + 72, 13, rgbf(0.55f, 0.40f, 0.05f));
        fill_circle(img, cx, oy + 71, 12, rgbf(1.0f, 0.82f, 0.20f));
        draw_text(img, cx - 4, oy + 64, 2, rgbf(0.55f, 0.40f, 0.05f), "T");
    }
    draw_text_centered(img, 128, oy + 12, 3, rgbf(1, 1, 1), rgbf(0.1f, 0.25f, 0.1f), "FRESH TOKENS");
    draw_text_centered(img, 128, oy + 100, 2, rgbf(1.0f, 0.9f, 0.3f), rgbf(0.1f, 0.2f, 0.05f), "FARM SHOP 1 KM");
}

static void campus_wall(image_t* img, int oy) {
    fill_rect(img, 0, oy, 256, 128, rgbf(0.12f, 0.13f, 0.17f));
    for (int y = 6; y < 128; y += 8) {
        for (int x = 6; x < 256; x += 8) {
            if (nz_hash2(x, y, 3u) > 0.55f) fill_rect(img, x, oy + y, 2, 2, rgbf(0.15f, 0.55f, 0.25f));
        }
    }
    draw_text_centered(img, 128, oy + 22, 4, rgbf(0.35f, 0.92f, 1.0f), rgbf(0, 0, 0), "GPU CAMPUS");
    draw_text_centered(img, 128, oy + 80, 2, rgbf(1, 1, 1), 0, "COMPUTE INSIDE");
}

static void sign_frame(image_t* img, int oy, vec3 bg) {
    fill_rect(img, 0, oy, 256, 128, rgbv(bg));
    uint32_t w = rgbf(0.95f, 0.95f, 0.95f);
    fill_rect(img, 5, oy + 5, 246, 3, w);
    fill_rect(img, 5, oy + 120, 246, 3, w);
    fill_rect(img, 5, oy + 5, 3, 118, w);
    fill_rect(img, 248, oy + 5, 3, 118, w);
}

static void sign_arrow_up(image_t* img, int cx, int top, uint32_t col) {
    fill_rect(img, cx - 4, top + 14, 8, 26, col);
    for (int r = 0; r < 14; r++) {
        fill_rect(img, cx - r - 1, top + r, 2 * r + 2, 1, col);
    }
}

image_t tex_gen_atlas(void) {
    image_t img = image_new(256, 128 * ATLAS_SLOTS);
    fill_rect(&img, 0, 0, 256, 128 * ATLAS_SLOTS, rgbf(0.5f, 0.5f, 0.5f));
    const uint32_t white = rgbf(0.96f, 0.96f, 0.96f);
    const vec3 green = v3(0.0f, 0.42f, 0.22f), blue = v3(0.07f, 0.26f, 0.62f);
    #define SLOT(k) ((k) * 128)

    ad_sokol(&img, SLOT(DECAL_SOKOL));
    ad_rsi(&img, SLOT(DECAL_RSI));
    ad_gaia(&img, SLOT(DECAL_GAIA));
    ad_horizon(&img, SLOT(DECAL_HORIZON));
    ad_agents(&img, SLOT(DECAL_AGENTS));
    ad_tokens(&img, SLOT(DECAL_TOKENS));
    campus_wall(&img, SLOT(DECAL_CAMPUS_WALL));

    sign_frame(&img, SLOT(DECAL_EXIT_GAIABY), green);
    draw_text_centered(&img, 128, SLOT(DECAL_EXIT_GAIABY) + 18, 5, white, 0, "GAIABY");
    draw_text(&img, 40, SLOT(DECAL_EXIT_GAIABY) + 78, 3, white, "EXIT 26");
    sign_arrow_up(&img, 200, SLOT(DECAL_EXIT_GAIABY) + 66, white);

    sign_frame(&img, SLOT(DECAL_CAMPUS_EXIT), green);
    draw_text_centered(&img, 128, SLOT(DECAL_CAMPUS_EXIT) + 24, 3, white, 0, "GPU CAMPUS");
    draw_text_centered(&img, 128, SLOT(DECAL_CAMPUS_EXIT) + 70, 3, white, 0, "NEXT EXIT");

    sign_frame(&img, SLOT(DECAL_SERVICE), v3(0.05f, 0.25f, 0.65f));
    draw_text_centered(&img, 128, SLOT(DECAL_SERVICE) + 20, 4, white, 0, "SERVICE");
    draw_text_centered(&img, 128, SLOT(DECAL_SERVICE) + 74, 3, white, 0, "AREA 3 KM");

    sign_frame(&img, SLOT(DECAL_SINGULARITY), green);
    draw_text_centered(&img, 120, SLOT(DECAL_SINGULARITY) + 22, 3, white, 0, "SINGULARITY");
    draw_text_centered(&img, 110, SLOT(DECAL_SINGULARITY) + 68, 4, white, 0, "42 KM");
    sign_arrow_up(&img, 214, SLOT(DECAL_SINGULARITY) + 62, white);

    sign_frame(&img, SLOT(DECAL_MOUNTAIN), v3(0.42f, 0.25f, 0.10f));
    draw_text_centered(&img, 128, SLOT(DECAL_MOUNTAIN) + 14, 3, white, 0, "MOUNTAIN PASS");
    for (int y = 0; y < 50; y++) {   // two little peaks with snow caps
        int oy = SLOT(DECAL_MOUNTAIN) + 60 + y;
        fill_rect(&img, 60 - y, oy, 2 * y, 1, y < 12 ? white : rgbf(0.75f, 0.65f, 0.50f));
        if (y > 14) fill_rect(&img, 100 - (y - 14), oy, 2 * (y - 14), 1, y < 24 ? white : rgbf(0.65f, 0.55f, 0.42f));
    }
    draw_text(&img, 132, SLOT(DECAL_MOUNTAIN) + 72, 2, white, "SNOW");
    draw_text(&img, 132, SLOT(DECAL_MOUNTAIN) + 92, 2, white, "AHEAD");

    for (int k = 0; k < 2; k++) {     // Swedish-style blue town sign; the exit version is crossed out
        int slot = (k == 0) ? DECAL_TOWN : DECAL_TOWN_END;
        fill_rect(&img, 0, SLOT(slot), 256, 128, rgbv(blue));
        rect_outline(&img, 6, SLOT(slot) + 6, 244, 116, 5, white);
        draw_text_centered(&img, 128, SLOT(slot) + 46, 5, white, 0, "GAIABY");
        if (k == 1) {
            thick_line(&img, 26.0f, (float)SLOT(slot) + 106.0f, 230.0f, (float)SLOT(slot) + 22.0f, 9.0f, rgbf(0.85f, 0.12f, 0.12f));
        }
    }
    // future city: welcome hologram and the gantry that says you have arrived
    fill_rect(&img, 0, SLOT(DECAL_CITY), 256, 128, rgbf(0.05f, 0.02f, 0.12f));
    for (int y = 0; y < 128; y += 4) {
        fill_rect(&img, 0, SLOT(DECAL_CITY) + y, 256, 1, rgbf(0.10f, 0.04f, 0.22f));
    }
    rect_outline(&img, 4, SLOT(DECAL_CITY) + 4, 248, 120, 3, rgbf(1.0f, 0.25f, 0.75f));
    draw_text_centered(&img, 128, SLOT(DECAL_CITY) + 16, 4, rgbf(0.35f, 0.95f, 1.0f), rgbf(0.1f, 0.2f, 0.5f), "GAIA CITY");
    draw_text_centered(&img, 128, SLOT(DECAL_CITY) + 62, 2, rgbf(1, 1, 1), 0, "POPULATION");
    draw_text_centered(&img, 128, SLOT(DECAL_CITY) + 84, 3, rgbf(1.0f, 0.45f, 0.85f), 0, "8 BN AGENTS");

    sign_frame(&img, SLOT(DECAL_SING_HERE), v3(0.30f, 0.05f, 0.42f));
    draw_text_centered(&img, 120, SLOT(DECAL_SING_HERE) + 22, 3, white, 0, "SINGULARITY");
    draw_text_centered(&img, 110, SLOT(DECAL_SING_HERE) + 68, 4, rgbf(0.35f, 0.95f, 1.0f), 0, "0 KM");
    sign_arrow_up(&img, 214, SLOT(DECAL_SING_HERE) + 62, white);
    #undef SLOT
    return img;
}
