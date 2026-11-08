// Track generation (closed loop highway), terrain heightfield and static scenery.
// The loop passes through scenery zones: countryside, forest, a village (Gaiaby),
// a mountain pass, a data-centre campus and farmland. Everything is procedural.
#include "world.h"
#include "noise.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRK_N       2048        // resampled centre line points (~2.5m apart)
#define TRK_RAW     16384       // raw samples used for arc-length parameterisation
#define TRACK_R0    760.0f      // base radius of the loop
#define PASS_HEIGHT 45.0f       // extra road elevation at the top of the mountain pass

static struct {
    float L;          // total length (m)
    float ds;         // distance between samples
    float dash;       // lane dash period (divides L exactly so texturing loops seamlessly)
    vec3 pos[TRK_N];
    vec3 tan[TRK_N];
    vec3 right[TRK_N];
} trk;

static const vec3 UP = { 0.0f, 1.0f, 0.0f };

// zones as fractions of the loop length (anything not listed is open countryside)
static const struct { float f0, f1; int zone; } ZONES[] = {
    { 0.120f, 0.290f, ZONE_FOREST },
    { 0.315f, 0.400f, ZONE_VILLAGE },
    { 0.440f, 0.730f, ZONE_MOUNTAIN },
    { 0.765f, 0.875f, ZONE_CAMPUS },
    { 0.885f, 0.985f, ZONE_FARM },
};
#define NUM_ZONES ((int)(sizeof(ZONES) / sizeof(ZONES[0])))
#define ZONE_BLEND 90.0f

static turbine_t g_turbines[MAX_TURBINES];
static int g_num_turbines = 0;

// ---------------------------------------------------------------------------
//  Height functions
// ---------------------------------------------------------------------------
static float natural_height(float x, float z) {
    return 16.0f * sinf(x * 0.0040f + 1.3f) * cosf(z * 0.0035f + 0.4f)
         + 7.0f * sinf(x * 0.0110f + z * 0.0080f)
         + 3.0f * sinf(z * 0.0230f - x * 0.0170f);
}

static float mountain_height(float x, float z) {
    float n = nz_fbm(x * 0.0015f + 11.3f, z * 0.0015f - 4.1f, 5, 0, 0, 77u);
    n = clampf((n - 0.36f) / 0.45f, 0.0f, 1.0f);
    return n * n * 400.0f + n * 50.0f;
}

vec3 sun_direction(void) {
    float th = SUN_U * 2.0f * PI_F;
    float el = SUN_ELEV_DEG * PI_F / 180.0f;
    return v3(cosf(th) * cosf(el), sinf(el), sinf(th) * cosf(el));
}

float zone_weight(float s, int zone) {
    float w = 0.0f;
    for (int i = 0; i < NUM_ZONES; i++) {
        if (ZONES[i].zone != zone) continue;
        float s0 = ZONES[i].f0 * trk.L, s1 = ZONES[i].f1 * trk.L;
        float v = smoothstepf(s0 - ZONE_BLEND, s0 + ZONE_BLEND, s) * (1.0f - smoothstepf(s1 - ZONE_BLEND, s1 + ZONE_BLEND, s));
        w = fmaxf(w, v);
    }
    return w;
}

static float zone_start(int zone) {
    for (int i = 0; i < NUM_ZONES; i++) if (ZONES[i].zone == zone) return ZONES[i].f0 * trk.L;
    return 0.0f;
}

static float zone_end(int zone) {
    for (int i = 0; i < NUM_ZONES; i++) if (ZONES[i].zone == zone) return ZONES[i].f1 * trk.L;
    return 0.0f;
}

// ---------------------------------------------------------------------------
//  Track
// ---------------------------------------------------------------------------
static void raw_point(float th, float* x, float* z) {
    float r = TRACK_R0 * (1.0f + 0.22f * sinf(2.0f * th + 0.5f)
                               + 0.10f * sinf(3.0f * th + 1.3f)
                               + 0.03f * sinf(5.0f * th + 2.1f));
    *x = r * cosf(th);
    *z = r * sinf(th);
}

void track_init(void) {
    static float rx[TRK_RAW + 1], rz[TRK_RAW + 1], cum[TRK_RAW + 1];
    for (int i = 0; i <= TRK_RAW; i++) {
        raw_point((float)i / (float)TRK_RAW * 2.0f * PI_F, &rx[i], &rz[i]);
    }
    cum[0] = 0.0f;
    for (int i = 1; i <= TRK_RAW; i++) {
        float dx = rx[i] - rx[i - 1], dz = rz[i] - rz[i - 1];
        cum[i] = cum[i - 1] + sqrtf(dx * dx + dz * dz);
    }
    trk.L = cum[TRK_RAW];
    trk.ds = trk.L / (float)TRK_N;

    // resample at equal arc length
    int j = 0;
    for (int i = 0; i < TRK_N; i++) {
        float s = (float)i * trk.ds;
        while (j < TRK_RAW - 1 && cum[j + 1] < s) {
            j++;
        }
        float t = (s - cum[j]) / (cum[j + 1] - cum[j]);
        trk.pos[i] = v3(lerpf(rx[j], rx[j + 1], t), 0.0f, lerpf(rz[j], rz[j + 1], t));
    }

    // road height: heavily smoothed natural terrain height along the loop ...
    static float h[TRK_N], tmp[TRK_N];
    for (int i = 0; i < TRK_N; i++) {
        h[i] = natural_height(trk.pos[i].x, trk.pos[i].z);
    }
    const int R = 40;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < TRK_N; i++) {
            float sum = 0.0f;
            for (int k = -R; k <= R; k++) {
                sum += h[(i + k + TRK_N) % TRK_N];
            }
            tmp[i] = sum / (float)(2 * R + 1);
        }
        memcpy(h, tmp, sizeof(h));
    }
    // ... plus a climb over the mountain pass (smooth cosine bump, ~9% max grade)
    float m0 = zone_start(ZONE_MOUNTAIN), m1 = zone_end(ZONE_MOUNTAIN);
    for (int i = 0; i < TRK_N; i++) {
        float s = (float)i * trk.ds;
        if (s > m0 && s < m1) {
            float u = (s - m0) / (m1 - m0);
            h[i] += PASS_HEIGHT * 0.5f * (1.0f - cosf(2.0f * PI_F * u));
        }
        trk.pos[i].y = h[i];
    }

    // tangents and right vectors
    for (int i = 0; i < TRK_N; i++) {
        vec3 a = trk.pos[(i + TRK_N - 1) % TRK_N];
        vec3 b = trk.pos[(i + 1) % TRK_N];
        trk.tan[i] = v3_norm(v3_sub(b, a));
        vec3 th = v3_norm(v3(trk.tan[i].x, 0.0f, trk.tan[i].z));
        trk.right[i] = v3_norm(v3_cross(th, UP));
    }

    int ndash = (int)(trk.L / 12.0f + 0.5f);
    trk.dash = trk.L / (float)ndash;
    printf("[highway] track length %.0f m\n", trk.L);
}

float track_length(void) { return trk.L; }
float track_dash_period(void) { return trk.dash; }

track_frame_t track_frame(float s) {
    s = wrapf(s, trk.L);
    float f = s / trk.ds;
    int i0 = (int)f;
    float t = f - (float)i0;
    i0 %= TRK_N;
    int i1 = (i0 + 1) % TRK_N;
    track_frame_t fr;
    fr.pos = v3_lerp(trk.pos[i0], trk.pos[i1], t);
    fr.tan = v3_norm(v3_lerp(trk.tan[i0], trk.tan[i1], t));
    fr.right = v3_norm(v3_lerp(trk.right[i0], trk.right[i1], t));
    fr.tanh = v3_norm(v3(fr.tan.x, 0.0f, fr.tan.z));
    return fr;
}

void track_nearest(float x, float z, float* out_dist, float* out_road_y, float* out_s, float* out_side) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < TRK_N; i += 8) {
        float dx = trk.pos[i].x - x, dz = trk.pos[i].z - z;
        float d2 = dx * dx + dz * dz;
        if (d2 < bd) { bd = d2; best = i; }
    }
    int coarse = best;
    for (int k = -8; k <= 8; k++) {
        int i = (coarse + k + TRK_N) % TRK_N;
        float dx = trk.pos[i].x - x, dz = trk.pos[i].z - z;
        float d2 = dx * dx + dz * dz;
        if (d2 < bd) { bd = d2; best = i; }
    }
    // project onto the two adjacent segments
    float res_d2 = 1e30f, res_y = trk.pos[best].y, res_s = (float)best * trk.ds;
    for (int sgm = -1; sgm <= 0; sgm++) {
        int ia = (best + sgm + TRK_N) % TRK_N;
        int ib = (ia + 1) % TRK_N;
        vec3 a = trk.pos[ia], b = trk.pos[ib];
        float abx = b.x - a.x, abz = b.z - a.z;
        float t = ((x - a.x) * abx + (z - a.z) * abz) / (abx * abx + abz * abz);
        t = clampf(t, 0.0f, 1.0f);
        float qx = a.x + abx * t, qz = a.z + abz * t;
        float d2 = (qx - x) * (qx - x) + (qz - z) * (qz - z);
        if (d2 < res_d2) {
            res_d2 = d2;
            res_y = lerpf(a.y, b.y, t);
            res_s = ((float)ia + t) * trk.ds;
        }
    }
    if (out_dist) *out_dist = sqrtf(res_d2);
    if (out_road_y) *out_road_y = res_y;
    if (out_s) *out_s = res_s;
    if (out_side) {
        float side = (x - trk.pos[best].x) * trk.right[best].x + (z - trk.pos[best].z) * trk.right[best].z;
        *out_side = side >= 0.0f ? 1.0f : -1.0f;
    }
}

typedef struct {
    float h, d, s, side, ry;
} terrain_info_t;

static terrain_info_t terrain_sample(float x, float z) {
    terrain_info_t ti;
    track_nearest(x, z, &ti.d, &ti.ry, &ti.s, &ti.side);
    float d = ti.d, ry = ti.ry;
    float nat = natural_height(x, z);
    float mf = smoothstepf(80.0f, 700.0f, d);
    if (mf > 0.0f) {
        nat += mf * mountain_height(x, z);
    }
    // mountain pass: rock wall on the right, a drop into a valley on the left, peaks beyond
    float wm = zone_weight(ti.s, ZONE_MOUNTAIN);
    if (wm > 0.0f) {
        float ridge = nz_fbm(x * 0.006f, z * 0.006f, 4, 0, 0, 91u);
        float m;
        if (ti.side > 0.0f) {
            m = ry + smoothstepf(12.0f, 110.0f, d) * (45.0f + 240.0f * ridge) + smoothstepf(110.0f, 600.0f, d) * 160.0f;
        } else {
            m = ry - smoothstepf(14.0f, 170.0f, d) * 160.0f + smoothstepf(260.0f, 750.0f, d) * (260.0f + 220.0f * ridge);
        }
        nat = lerpf(nat, m, wm);
    }
    // the village and the campus sit on flattened ground
    float flat = fmaxf(zone_weight(ti.s, ZONE_VILLAGE), zone_weight(ti.s, ZONE_CAMPUS)) * (1.0f - smoothstepf(130.0f, 230.0f, d));
    if (flat > 0.0f) {
        nat = lerpf(nat, ry - 0.4f + 1.2f * (nz_value(x * 0.02f, z * 0.02f, 0, 0, 5u) - 0.5f), flat);
    }
    float b = smoothstepf(ROAD_HALF + 2.5f, ROAD_HALF + lerpf(40.0f, 20.0f, wm), d);
    ti.h = lerpf(ry - 0.35f, nat, b);
    return ti;
}

float terrain_height(float x, float z, float* out_dist) {
    terrain_info_t ti = terrain_sample(x, z);
    if (out_dist) *out_dist = ti.d;
    return ti.h;
}

int world_turbines(const turbine_t** out) {
    *out = g_turbines;
    return g_num_turbines;
}

// ---------------------------------------------------------------------------
//  Geometry helpers
// ---------------------------------------------------------------------------
static uint32_t pack_rgb(vec3 c) {
    int r = (int)(clampf(c.x, 0.0f, 1.0f) * 255.0f + 0.5f);
    int g = (int)(clampf(c.y, 0.0f, 1.0f) * 255.0f + 0.5f);
    int b = (int)(clampf(c.z, 0.0f, 1.0f) * 255.0f + 0.5f);
    return RGB(r, g, b);
}

// box with arbitrary orthonormal axes
static void obox(mesh_t* m, vec3 c, vec3 ax, vec3 ay, vec3 az, float hx, float hy, float hz, uint32_t col) {
    vec3 corners[8];
    const float sx[4] = { -1, 1, 1, -1 };
    const float sz[4] = { -1, -1, 1, 1 };
    for (int k = 0; k < 4; k++) {
        vec3 p = v3_add(c, v3_add(v3_scale(ax, sx[k] * hx), v3_scale(az, sz[k] * hz)));
        corners[k] = v3_mad(p, ay, -hy);
        corners[k + 4] = v3_mad(p, ay, hy);
    }
    mesh_hexa(m, corners, col);
}

// box in the local frame of the track: lateral (right), along (tangent), y relative to road
static void frame_box(mesh_t* m, const track_frame_t* f, float lat0, float lat1, float a0, float a1,
                      float y0, float y1, uint32_t col) {
    vec3 c = f->pos;
    c = v3_mad(c, f->right, (lat0 + lat1) * 0.5f);
    c = v3_mad(c, f->tanh, (a0 + a1) * 0.5f);
    c = v3_mad(c, UP, (y0 + y1) * 0.5f);
    obox(m, c, f->right, UP, f->tanh,
         fabsf(lat1 - lat0) * 0.5f, fabsf(y1 - y0) * 0.5f, fabsf(a1 - a0) * 0.5f, col);
}

static void atlas_uv(int slot, float uv[4][2]) {
    float v0 = (float)slot / (float)ATLAS_SLOTS + 0.001f;
    float v1 = (float)(slot + 1) / (float)ATLAS_SLOTS - 0.001f;
    // corners: bottom-left, bottom-right, top-right, top-left
    uv[0][0] = 0.0f; uv[0][1] = v1;
    uv[1][0] = 1.0f; uv[1][1] = v1;
    uv[2][0] = 1.0f; uv[2][1] = v0;
    uv[3][0] = 0.0f; uv[3][1] = v0;
}

// textured panel facing direction 'facing', centred at c
static void decal_panel(mesh_t* m, vec3 c, vec3 facing, float hw, float hh, int slot) {
    vec3 vr = v3_norm(v3_cross(v3_scale(facing, -1.0f), UP));
    vec3 bl = v3_mad(v3_mad(c, vr, -hw), UP, -hh);
    vec3 br = v3_mad(v3_mad(c, vr, hw), UP, -hh);
    vec3 tr = v3_mad(v3_mad(c, vr, hw), UP, hh);
    vec3 tl = v3_mad(v3_mad(c, vr, -hw), UP, hh);
    float uv[4][2];
    atlas_uv(slot, uv);
    mesh_quad_uv(m, bl, br, tr, tl, uv, facing, RGB(255, 255, 255));
}

// local building frame: x along the facade, y up, z out of the front facade
typedef struct { vec3 o, ax, az; } lframe_t;

static lframe_t make_lframe(vec3 origin, vec3 facing) {
    lframe_t f;
    f.o = origin;
    f.az = v3_norm(v3(facing.x, 0.0f, facing.z));
    f.ax = v3_norm(v3_cross(UP, f.az));
    return f;
}

static vec3 lp(const lframe_t* f, float x, float y, float z) {
    return v3_add(f->o, v3_add(v3_scale(f->ax, x), v3_add(v3_scale(UP, y), v3_scale(f->az, z))));
}

static void lbox(mesh_t* m, const lframe_t* f, float x0, float x1, float y0, float y1, float z0, float z1, uint32_t col) {
    obox(m, lp(f, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f), f->ax, UP, f->az,
         (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f, col);
}

// gable roof with the ridge along x, eaves at y_eave, overhang o
static void gable_roof(mesh_t* m, const lframe_t* f, float hw, float hd, float ye, float rh, float o,
                       uint32_t roof, uint32_t gable) {
    vec3 a = lp(f, -hw - o, ye - o * 0.5f, -hd - o), b = lp(f, hw + o, ye - o * 0.5f, -hd - o);
    vec3 c = lp(f, hw + o, ye - o * 0.5f, hd + o), d = lp(f, -hw - o, ye - o * 0.5f, hd + o);
    vec3 r0 = lp(f, -hw - o, ye + rh, 0.0f), r1 = lp(f, hw + o, ye + rh, 0.0f);
    mesh_quad(m, d, c, r1, r0, v3_add(UP, f->az), roof);
    mesh_quad(m, a, b, r1, r0, v3_sub(UP, f->az), roof);
    mesh_tri(m, lp(f, -hw, ye, -hd), lp(f, -hw, ye, hd), lp(f, -hw, ye + rh * hd / (hd + o), 0.0f), v3_scale(f->ax, -1.0f), gable);
    mesh_tri(m, lp(f, hw, ye, -hd), lp(f, hw, ye, hd), lp(f, hw, ye + rh * hd / (hd + o), 0.0f), f->ax, gable);
}

// four-sided pyramid aligned with the frame
static void lpyramid(mesh_t* m, const lframe_t* f, float cx, float y, float cz, float half, float height, uint32_t col) {
    vec3 p[4] = {
        lp(f, cx - half, y, cz - half), lp(f, cx + half, y, cz - half),
        lp(f, cx + half, y, cz + half), lp(f, cx - half, y, cz + half),
    };
    vec3 apex = lp(f, cx, y + height, cz), centre = lp(f, cx, y, cz);
    for (int i = 0; i < 4; i++) {
        vec3 a = p[i], b = p[(i + 1) % 4];
        vec3 mid = v3_scale(v3_add(a, b), 0.5f);
        mesh_tri(m, a, b, apex, v3_add(v3_sub(mid, centre), v3_scale(UP, 0.3f)), col);
    }
}

static void frustum(mesh_t* m, vec3 base, float r0, float r1, float h, int sides, uint32_t col) {
    for (int i = 0; i < sides; i++) {
        float a0 = (float)i / (float)sides * 2.0f * PI_F, a1 = (float)(i + 1) / (float)sides * 2.0f * PI_F;
        vec3 d0 = v3(cosf(a0), 0, sinf(a0)), d1 = v3(cosf(a1), 0, sinf(a1));
        vec3 b0 = v3_mad(base, d0, r0), b1 = v3_mad(base, d1, r0);
        vec3 t0 = v3_mad(v3_mad(base, d0, r1), UP, h), t1 = v3_mad(v3_mad(base, d1, r1), UP, h);
        mesh_quad(m, b0, b1, t1, t0, v3_add(d0, d1), col);
    }
}

// ---------------------------------------------------------------------------
//  Terrain
// ---------------------------------------------------------------------------
static vec3 field_color(float x, float z) {
    float u = x * 0.878f + z * 0.479f, v = -x * 0.479f + z * 0.878f;
    int cu = (int)floorf(u / 150.0f), cv = (int)floorf(v / 105.0f);
    float hsh = nz_hash2(cu, cv, 7u);
    static const vec3 crops[5] = {
        { 0.86f, 0.78f, 0.18f },   // rapeseed
        { 0.78f, 0.66f, 0.36f },   // wheat
        { 0.36f, 0.62f, 0.22f },   // young crop
        { 0.42f, 0.30f, 0.20f },   // plowed
        { 0.45f, 0.62f, 0.28f },   // pasture
    };
    int k = (int)(hsh * 5.0f) % 5;
    return v3_scale(crops[k], 0.92f + 0.12f * nz_hash2(cu, cv, 8u));
}

static uint32_t terrain_color(float x, float z, const terrain_info_t* ti, vec3 n) {
    float h = ti->h, d = ti->d, s = ti->s;
    float g = nz_fbm(x * 0.012f, z * 0.012f, 3, 0, 0, 5u);
    vec3 c = v3_lerp(v3(0.30f, 0.50f, 0.17f), v3(0.50f, 0.58f, 0.22f), g);
    float wf = zone_weight(s, ZONE_FOREST), wv = zone_weight(s, ZONE_VILLAGE), wm = zone_weight(s, ZONE_MOUNTAIN);
    float wc = zone_weight(s, ZONE_CAMPUS), wa = zone_weight(s, ZONE_FARM);
    float off = smoothstepf(ROAD_HALF + 6.0f, ROAD_HALF + 14.0f, d);
    c = v3_lerp(c, v3_scale(v3(0.17f, 0.31f, 0.12f), 0.85f + 0.3f * g), wf * off);       // forest floor
    c = v3_lerp(c, v3(0.38f, 0.60f, 0.24f), wv * 0.7f);                                   // village lawns
    c = v3_lerp(c, v3(0.44f, 0.60f, 0.28f), wc * 0.6f);                                   // campus lawns
    if (wa > 0.0f) {
        c = v3_lerp(c, field_color(x, z), wa * off);                                      // patchwork fields
    }
    float slope = 1.0f - n.y;
    vec3 rock = v3(0.48f, 0.44f, 0.39f);
    // mountain pass: layered rock on the cliff side, rocky scree on the valley side
    if (wm > 0.0f) {
        vec3 strata = v3_scale(v3(0.50f, 0.46f, 0.42f), 0.82f + 0.18f * sinf(h * 0.55f + g * 3.0f));
        float k = wm * smoothstepf(ROAD_HALF + 5.0f, ROAD_HALF + 18.0f, d) * (ti->side > 0.0f ? 0.92f : 0.35f);
        c = v3_lerp(c, strata, k);
    }
    c = v3_lerp(c, rock, smoothstepf(lerpf(0.22f, 0.10f, wm), lerpf(0.45f, 0.30f, wm), slope));
    c = v3_lerp(c, v3_scale(rock, 0.92f), smoothstepf(120.0f - 50.0f * wm, 190.0f - 60.0f * wm, h));
    float snow_line = 250.0f - 130.0f * wm;
    c = v3_lerp(c, v3(0.94f, 0.95f, 0.98f), smoothstepf(snow_line, snow_line + 40.0f, h + 40.0f * (g - 0.5f)));
    // gravel verge next to the road
    c = v3_lerp(v3(0.52f, 0.48f, 0.40f), c, smoothstepf(ROAD_HALF + 1.0f, ROAD_HALF + 6.0f, d));
    return pack_rgb(c);
}

static void build_terrain(mesh_t* m) {
    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (int i = 0; i < TRK_N; i++) {
        minx = fminf(minx, trk.pos[i].x); maxx = fmaxf(maxx, trk.pos[i].x);
        minz = fminf(minz, trk.pos[i].z); maxz = fmaxf(maxz, trk.pos[i].z);
    }
    const float margin = 1500.0f, cell = 10.0f;
    float x0 = floorf((minx - margin) / cell) * cell;
    float z0 = floorf((minz - margin) / cell) * cell;
    int nx = (int)((maxx + margin - x0) / cell) + 1;
    int nz = (int)((maxz + margin - z0) / cell) + 1;
    terrain_info_t* T = (terrain_info_t*)malloc(sizeof(terrain_info_t) * (size_t)nx * (size_t)nz);
    for (int j = 0; j < nz; j++) {
        for (int i = 0; i < nx; i++) {
            T[j * nx + i] = terrain_sample(x0 + (float)i * cell, z0 + (float)j * cell);
        }
    }
    for (int j = 0; j < nz; j++) {
        for (int i = 0; i < nx; i++) {
            float x = x0 + (float)i * cell, z = z0 + (float)j * cell;
            float hl = T[j * nx + (i > 0 ? i - 1 : i)].h;
            float hr = T[j * nx + (i < nx - 1 ? i + 1 : i)].h;
            float hd = T[(j > 0 ? j - 1 : j) * nx + i].h;
            float hu = T[(j < nz - 1 ? j + 1 : j) * nx + i].h;
            vec3 n = v3_norm(v3(hl - hr, 2.0f * cell, hd - hu));
            const terrain_info_t* ti = &T[j * nx + i];
            mesh_vertex(m, v3(x, ti->h, z), n, x / 8.0f, z / 8.0f, terrain_color(x, z, ti, n));
        }
    }
    for (int j = 0; j < nz - 1; j++) {
        for (int i = 0; i < nx - 1; i++) {
            uint32_t a = (uint32_t)(j * nx + i), b = a + 1, c = a + (uint32_t)nx, d = c + 1;
            mesh_index(m, a); mesh_index(m, c); mesh_index(m, b);
            mesh_index(m, b); mesh_index(m, c); mesh_index(m, d);
        }
    }
    free(T);
    printf("[highway] terrain %dx%d vertices\n", nx, nz);
}

// ---------------------------------------------------------------------------
//  Road surface, barriers, rails, lamps
// ---------------------------------------------------------------------------
static void build_road(mesh_t* road, mesh_t* props) {
    const float lat_pts[5] = { 0.0f, 2.5f, 5.0f, 7.5f, ROAD_HALF };
    const uint32_t white = RGB(255, 255, 255);
    for (int i = 0; i < TRK_N; i++) {
        int i1 = (i + 1) % TRK_N;
        float v0 = (float)i * trk.ds / trk.dash;
        float v1 = (float)(i + 1) * trk.ds / trk.dash;
        vec3 n0 = v3_norm(v3_cross(trk.right[i], trk.tan[i]));
        for (int side = -1; side <= 1; side += 2) {
            for (int k = 0; k < 4; k++) {
                float la = lat_pts[k], lb = lat_pts[k + 1];
                vec3 p00 = v3_mad(trk.pos[i], trk.right[i], (float)side * la);
                vec3 p01 = v3_mad(trk.pos[i], trk.right[i], (float)side * lb);
                vec3 p10 = v3_mad(trk.pos[i1], trk.right[i1], (float)side * la);
                vec3 p11 = v3_mad(trk.pos[i1], trk.right[i1], (float)side * lb);
                float ua = la / ROAD_HALF, ub = lb / ROAD_HALF;
                float uv[4][2] = { { ua, v0 }, { ub, v0 }, { ub, v1 }, { ua, v1 } };
                mesh_quad_uv(road, p00, p01, p11, p10, uv, n0, white);
            }
            // embankment skirt below the road edge
            vec3 e0 = v3_mad(trk.pos[i], trk.right[i], (float)side * ROAD_HALF);
            vec3 e1 = v3_mad(trk.pos[i1], trk.right[i1], (float)side * ROAD_HALF);
            mesh_quad(props, e0, e1, v3_mad(e1, UP, -2.5f), v3_mad(e0, UP, -2.5f),
                      v3_scale(trk.right[i], (float)side), RGB(140, 128, 108));
        }
    }

    // concrete median barrier ("jersey" profile)
    const float prof[6][2] = {
        { -0.30f, 0.00f }, { -0.18f, 0.28f }, { -0.11f, 0.82f },
        {  0.11f, 0.82f }, {  0.18f, 0.28f }, {  0.30f, 0.00f },
    };
    for (int i = 0; i < TRK_N; i++) {
        int i1 = (i + 1) % TRK_N;
        for (int k = 0; k < 5; k++) {
            vec3 a = v3_mad(v3_mad(trk.pos[i], trk.right[i], prof[k][0]), UP, prof[k][1]);
            vec3 b = v3_mad(v3_mad(trk.pos[i], trk.right[i], prof[k + 1][0]), UP, prof[k + 1][1]);
            vec3 c = v3_mad(v3_mad(trk.pos[i1], trk.right[i1], prof[k + 1][0]), UP, prof[k + 1][1]);
            vec3 d = v3_mad(v3_mad(trk.pos[i1], trk.right[i1], prof[k][0]), UP, prof[k][1]);
            float ml = (prof[k][0] + prof[k + 1][0]) * 0.5f;
            float my = (prof[k][1] + prof[k + 1][1]) * 0.5f;
            vec3 hint = v3_add(v3_scale(trk.right[i], ml), v3_scale(UP, my - 0.35f));
            mesh_quad(props, a, b, c, d, hint, RGB(176, 174, 166));
        }
    }

    // guard rails + posts on both outer edges
    for (int i = 0; i < TRK_N; i++) {
        int i1 = (i + 1) % TRK_N;
        for (int side = -1; side <= 1; side += 2) {
            float lat = (float)side * (ROAD_HALF - 0.15f);
            vec3 a = v3_mad(trk.pos[i], trk.right[i], lat);
            vec3 b = v3_mad(trk.pos[i1], trk.right[i1], lat);
            mesh_quad(props, v3_mad(a, UP, 0.50f), v3_mad(b, UP, 0.50f), v3_mad(b, UP, 0.82f), v3_mad(a, UP, 0.82f),
                      v3_scale(trk.right[i], (float)-side), RGB(186, 190, 196));
            if ((i % 2) == 0) {
                track_frame_t f = track_frame((float)i * trk.ds);
                float pl = (float)side * (ROAD_HALF - 0.02f);
                frame_box(props, &f, pl - 0.08f, pl + 0.08f, -0.08f, 0.08f, -0.3f, 0.86f, RGB(120, 122, 128));
            }
        }
    }

    // street lamps on the median
    for (int i = 0; i < TRK_N; i += 22) {
        track_frame_t f = track_frame((float)i * trk.ds);
        frame_box(props, &f, -0.13f, 0.13f, -0.13f, 0.13f, 0.8f, 10.0f, RGB(150, 152, 158));
        frame_box(props, &f, -3.3f, 3.3f, -0.08f, 0.08f, 9.65f, 9.85f, RGB(150, 152, 158));
        frame_box(props, &f, 2.5f, 3.5f, -0.25f, 0.25f, 9.4f, 9.65f, RGB(255, 244, 196));
        frame_box(props, &f, -3.5f, -2.5f, -0.25f, 0.25f, 9.4f, 9.65f, RGB(255, 244, 196));
    }
}

// ---------------------------------------------------------------------------
//  Road furniture: bridges, gantries, billboards, town signs
// ---------------------------------------------------------------------------
static void place_bridge(mesh_t* props, float s) {
    const uint32_t concrete = RGB(176, 172, 162);
    track_frame_t f = track_frame(s);
    frame_box(props, &f, -70.0f, 70.0f, -5.0f, 5.0f, 7.0f, 8.2f, concrete);
    frame_box(props, &f, -70.0f, 70.0f, -5.0f, -4.6f, 8.2f, 9.1f, RGB(196, 192, 184));
    frame_box(props, &f, -70.0f, 70.0f, 4.6f, 5.0f, 8.2f, 9.1f, RGB(196, 192, 184));
    frame_box(props, &f, -70.0f, 70.0f, -5.05f, 5.05f, 6.6f, 7.0f, RGB(120, 60, 50));   // painted girder
    const float pillars[6] = { -14.0f, 14.0f, -40.0f, 40.0f, -64.0f, 64.0f };
    for (int k = 0; k < 6; k++) {
        frame_box(props, &f, pillars[k] - 0.7f, pillars[k] + 0.7f, -3.0f, 3.0f, -40.0f, 6.6f, concrete);
    }
    frame_box(props, &f, -0.45f, 0.45f, -2.5f, 2.5f, 0.0f, 6.6f, concrete);
}

// overhead sign gantry over the carriageway of traffic travelling in direction dir
static void place_gantry(mesh_t* props, mesh_t* decals, float s, float dir, int slot) {
    const uint32_t steel = RGB(110, 118, 130);
    track_frame_t f = track_frame(s);
    float lo = dir * 0.75f, hi = dir * (ROAD_HALF + 0.7f);
    frame_box(props, &f, lo - 0.18f, lo + 0.18f, -0.18f, 0.18f, 0.0f, 7.6f, steel);
    frame_box(props, &f, hi - 0.18f, hi + 0.18f, -0.18f, 0.18f, -1.0f, 7.6f, steel);
    frame_box(props, &f, dir * 0.5f, dir * (ROAD_HALF + 0.9f), -0.2f, 0.2f, 7.0f, 7.6f, steel);
    // the sign faces traffic approaching from behind (travelling along dir * tangent)
    float a_board = -dir * 0.3f;
    frame_box(props, &f, dir * 2.3f, dir * 9.7f, a_board - 0.08f, a_board + 0.08f, 4.3f, 8.0f, RGB(60, 64, 70));
    vec3 facing = v3_scale(f.tanh, -dir);
    vec3 c = v3_mad(v3_mad(v3_mad(f.pos, f.right, dir * 6.0f), f.tanh, a_board - dir * 0.1f), UP, 6.15f);
    decal_panel(decals, c, facing, 3.6f, 1.8f, slot);
}

// roadside billboard on the given side, facing the player's traffic (moving along +tangent)
static void place_billboard(mesh_t* props, mesh_t* decals, float s, float side, int slot) {
    track_frame_t f = track_frame(s);
    vec3 base = v3_mad(f.pos, f.right, side * 27.0f);
    base.y = terrain_height(base.x, base.z, NULL);
    vec3 facing = v3_norm(v3_add(v3_scale(f.tanh, -0.9f), v3_scale(f.right, -side * 0.45f)));
    vec3 vr = v3_norm(v3_cross(v3_scale(facing, -1.0f), UP));
    vec3 c = v3_mad(base, UP, 9.0f);
    obox(props, v3_mad(c, facing, -0.25f), vr, UP, facing, 6.3f, 3.3f, 0.2f, RGB(70, 70, 76));
    for (int p = -1; p <= 1; p += 2) {
        vec3 pc = v3_mad(v3_mad(base, vr, (float)p * 3.8f), facing, -0.3f);
        obox(props, v3_mad(pc, UP, 2.0f), vr, UP, facing, 0.22f, 4.5f, 0.22f, RGB(96, 96, 100));
    }
    decal_panel(decals, v3_mad(c, facing, -0.02f), facing, 6.0f, 3.0f, slot);
}

// small town name sign on two posts beside the road, facing traffic travelling along dir
static void place_town_sign(mesh_t* props, mesh_t* decals, float s, float dir, int slot) {
    track_frame_t f = track_frame(s);
    vec3 base = v3_mad(f.pos, f.right, dir * (ROAD_HALF + 2.4f));
    base.y = terrain_height(base.x, base.z, NULL);
    vec3 facing = v3_scale(f.tanh, -dir);
    vec3 vr = v3_norm(v3_cross(v3_scale(facing, -1.0f), UP));
    vec3 c = v3_mad(base, UP, 2.6f);
    obox(props, v3_mad(c, facing, -0.08f), vr, UP, facing, 2.5f, 1.3f, 0.05f, RGB(200, 200, 205));
    for (int p = -1; p <= 1; p += 2) {
        obox(props, v3_mad(v3_mad(v3_mad(base, vr, (float)p * 1.6f), facing, -0.15f), UP, 1.2f),
             vr, UP, facing, 0.06f, 1.5f, 0.06f, RGB(150, 152, 158));
    }
    decal_panel(decals, c, facing, 2.4f, 1.2f, slot);
}

static void build_structures(mesh_t* props, mesh_t* decals) {
    const float L = trk.L;
    place_bridge(props, 0.07f * L);
    place_bridge(props, 0.23f * L);

    // gantries over the player's carriageway (+ one for the oncoming traffic)
    place_gantry(props, decals, 0.09f * L, 1.0f, DECAL_SINGULARITY);
    place_gantry(props, decals, zone_start(ZONE_VILLAGE) - 280.0f, 1.0f, DECAL_EXIT_GAIABY);
    place_gantry(props, decals, zone_start(ZONE_MOUNTAIN) - 150.0f, 1.0f, DECAL_MOUNTAIN);
    place_gantry(props, decals, zone_start(ZONE_CAMPUS) - 130.0f, 1.0f, DECAL_CAMPUS_EXIT);
    place_gantry(props, decals, zone_start(ZONE_CAMPUS) + 250.0f, -1.0f, DECAL_SERVICE);

    // billboards in the open parts of the loop, all facing the player's traffic
    const struct { float s; float side; int slot; } boards[] = {
        { 0.045f * L,  1.0f, DECAL_SOKOL },
        { 0.100f * L,  1.0f, DECAL_GAIA },
        { 0.300f * L,  1.0f, DECAL_RSI },
        { 0.420f * L,  1.0f, DECAL_AGENTS },
        { 0.425f * L, -1.0f, DECAL_HORIZON },
        { 0.745f * L,  1.0f, DECAL_HORIZON },
        { 0.900f * L,  1.0f, DECAL_TOKENS },
        { 0.960f * L, -1.0f, DECAL_RSI },
        { 0.990f * L,  1.0f, DECAL_GAIA },
    };
    for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
        place_billboard(props, decals, boards[i].s, boards[i].side, boards[i].slot);
    }

    // Gaiaby town signs (entry, and the crossed-out version when leaving) for both directions
    float v0 = zone_start(ZONE_VILLAGE) + 15.0f, v1 = zone_end(ZONE_VILLAGE) - 15.0f;
    place_town_sign(props, decals, v0, 1.0f, DECAL_TOWN);
    place_town_sign(props, decals, v1, 1.0f, DECAL_TOWN_END);
    place_town_sign(props, decals, v1, -1.0f, DECAL_TOWN);
    place_town_sign(props, decals, v0, -1.0f, DECAL_TOWN_END);
}

// ---------------------------------------------------------------------------
//  Buildings
// ---------------------------------------------------------------------------
static const uint32_t TRIM_WHITE = RGB(236, 234, 226);
static const uint32_t FALU_RED = RGB(142, 32, 26);
static const uint32_t ROOF_DARK = RGB(56, 56, 64);
static const uint32_t GLASS_DARK = RGB(40, 52, 74);

static void build_house(mesh_t* m, vec3 base, vec3 facing, float w, float d, float h, uint32_t wall,
                        bool trim, uint32_t roof, uint32_t* rng) {
    lframe_t f = make_lframe(base, facing);
    float hw = w * 0.5f, hd = d * 0.5f;
    lbox(m, &f, -hw, hw, -2.0f, h, -hd, hd, wall);
    if (trim) {
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                float x = (float)sx * hw, z = (float)sz * hd;
                lbox(m, &f, x - 0.15f, x + 0.15f, 0.0f, h, z - 0.15f, z + 0.15f, TRIM_WHITE);
            }
        }
    }
    gable_roof(m, &f, hw, hd, h, d * 0.42f, 0.45f, roof, wall);
    // door and windows on the front facade
    float door_x = rng_range(rng, -hw + 1.5f, hw - 1.5f);
    lbox(m, &f, door_x - 0.5f, door_x + 0.5f, 0.0f, 2.1f, hd, hd + 0.06f, RGB(70, 50, 34));
    for (float x = -hw + 1.4f; x < hw - 1.0f; x += 2.6f) {
        if (fabsf(x - door_x) < 1.3f) continue;
        lbox(m, &f, x - 0.62f, x + 0.62f, 1.0f, 2.25f, hd, hd + 0.05f, TRIM_WHITE);
        lbox(m, &f, x - 0.44f, x + 0.44f, 1.15f, 2.08f, hd, hd + 0.08f, GLASS_DARK);
    }
    // chimney
    float cx = rng_range(rng, -hw * 0.5f, hw * 0.5f);
    lbox(m, &f, cx - 0.35f, cx + 0.35f, h + d * 0.15f, h + d * 0.42f + 0.9f, -0.35f, 0.35f, RGB(120, 70, 60));
}

static void build_church(mesh_t* m, vec3 base, vec3 facing) {
    lframe_t f = make_lframe(base, facing);
    const uint32_t white = RGB(238, 236, 230);
    lbox(m, &f, -12.0f, 12.0f, -2.0f, 8.0f, -5.5f, 5.5f, white);
    gable_roof(m, &f, 12.0f, 5.5f, 8.0f, 5.0f, 0.5f, RGB(44, 46, 52), white);
    for (float x = -8.0f; x <= 8.0f; x += 4.0f) {      // tall windows
        lbox(m, &f, x - 0.6f, x + 0.6f, 2.0f, 6.0f, 5.5f, 5.58f, GLASS_DARK);
    }
    lbox(m, &f, -18.0f, -12.0f, -2.0f, 20.0f, -3.0f, 3.0f, white);       // tower
    lbox(m, &f, -16.0f, -14.0f, 0.0f, 3.2f, 3.0f, 3.08f, RGB(70, 50, 34));  // door
    lbox(m, &f, -15.6f, -14.4f, 15.0f, 17.5f, 3.0f, 3.06f, RGB(30, 30, 34)); // belfry opening
    lpyramid(m, &f, -15.0f, 20.0f, 0.0f, 3.3f, 13.0f, RGB(40, 42, 48));
    lbox(m, &f, -15.1f, -14.9f, 33.0f, 35.6f, -0.1f, 0.1f, RGB(200, 170, 60));  // cross
    lbox(m, &f, -15.8f, -14.2f, 34.3f, 34.6f, -0.1f, 0.1f, RGB(200, 170, 60));
}

static void build_barn(mesh_t* m, vec3 base, vec3 facing) {
    lframe_t f = make_lframe(base, facing);
    lbox(m, &f, -10.0f, 10.0f, -2.0f, 6.0f, -6.5f, 6.5f, FALU_RED);
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sz = -1; sz <= 1; sz += 2) {
            lbox(m, &f, sx * 10.0f - 0.2f, sx * 10.0f + 0.2f, 0.0f, 6.0f, sz * 6.5f - 0.2f, sz * 6.5f + 0.2f, TRIM_WHITE);
        }
    }
    gable_roof(m, &f, 10.0f, 6.5f, 6.0f, 4.5f, 0.5f, ROOF_DARK, FALU_RED);
    // big double door with white frame
    lbox(m, &f, -3.0f, 3.0f, 0.0f, 4.6f, 6.5f, 6.55f, TRIM_WHITE);
    lbox(m, &f, -2.7f, 2.7f, 0.0f, 4.3f, 6.5f, 6.6f, RGB(120, 28, 22));
    lbox(m, &f, -0.08f, 0.08f, 0.0f, 4.3f, 6.5f, 6.65f, TRIM_WHITE);
}

static void build_hall(mesh_t* m, mesh_t* decals, vec3 base, vec3 facing, bool sign) {
    lframe_t f = make_lframe(base, facing);
    lbox(m, &f, -35.0f, 35.0f, -3.0f, 11.0f, -14.0f, 14.0f, RGB(198, 204, 212));
    lbox(m, &f, -35.1f, 35.1f, -3.0f, 1.2f, -14.1f, 14.1f, RGB(76, 82, 90));
    for (float x = -32.0f; x <= 32.0f; x += 4.0f) {           // vent stripes
        lbox(m, &f, x - 0.5f, x + 0.5f, 2.0f, 9.5f, 14.0f, 14.08f, RGB(140, 148, 158));
    }
    for (int i = 0; i < 10; i++) {                            // rooftop cooling units
        for (int j = -1; j <= 1; j++) {
            float x = -30.0f + (float)i * 6.6f, z = (float)j * 8.0f;
            lbox(m, &f, x - 1.8f, x + 1.8f, 11.0f, 12.8f, z - 2.0f, z + 2.0f, RGB(110, 116, 124));
            lbox(m, &f, x - 1.2f, x + 1.2f, 12.8f, 12.95f, z - 1.2f, z + 1.2f, RGB(40, 44, 50));
        }
    }
    if (sign) {
        decal_panel(decals, lp(&f, 0.0f, 6.5f, 14.15f), f.az, 8.0f, 4.0f, DECAL_CAMPUS_WALL);
    }
}

static void build_turbine(mesh_t* props, vec3 base, vec3 axis, uint32_t* rng) {
    if (g_num_turbines >= MAX_TURBINES) return;
    const float tower_h = 72.0f;
    frustum(props, v3_mad(base, UP, -4.0f), 2.3f, 1.2f, tower_h + 4.0f, 8, RGB(226, 228, 232));
    vec3 top = v3_mad(base, UP, tower_h);
    vec3 ax = v3_norm(v3_cross(UP, axis));
    obox(props, v3_add(v3_mad(top, axis, -1.2f), v3(0, 0.8f, 0)), ax, UP, axis, 1.4f, 1.5f, 3.6f, RGB(232, 234, 238));
    turbine_t* t = &g_turbines[g_num_turbines++];
    t->hub = v3_add(v3_mad(top, axis, 2.8f), v3(0, 0.8f, 0));
    t->axis = axis;
    t->phase = rng_range(rng, 0.0f, 6.28f);
}

// rotor in local space: rotation axis +z, three blades in the xy plane
static void build_blades(mesh_t* m) {
    const uint32_t white = RGB(234, 236, 240);
    mesh_box(m, v3(-1.0f, -1.0f, -0.6f), v3(1.0f, 1.0f, 1.0f), white);
    for (int k = 0; k < 3; k++) {
        float a = (float)k * 2.0f * PI_F / 3.0f;
        vec3 d = v3(cosf(a), sinf(a), 0.0f), p = v3(-sinf(a), cosf(a), 0.0f);
        vec3 r0 = v3_scale(d, 1.0f), r1 = v3_scale(d, 31.0f);
        mesh_quad(m, v3_mad(r0, p, -1.1f), v3_mad(r0, p, 1.2f), v3_mad(r1, p, 0.35f), v3_mad(r1, p, -0.25f),
                  v3(0, 0, 1), white);
    }
}

static void round_tree(mesh_t* m, vec3 p, float s, float gv) {
    mesh_box(m, v3(p.x - 0.25f * s, p.y - 0.5f, p.z - 0.25f * s), v3(p.x + 0.25f * s, p.y + 2.4f * s, p.z + 0.25f * s), RGB(96, 70, 44));
    mesh_blob(m, v3(p.x, p.y + 4.3f * s, p.z), 2.7f * s, 2.5f * s, 2.7f * s, 7, pack_rgb(v3(0.30f * gv, 0.50f * gv, 0.16f * gv)));
}

static void pine_tree(mesh_t* m, vec3 p, float s, float gv) {
    mesh_box(m, v3(p.x - 0.22f * s, p.y - 0.5f, p.z - 0.22f * s), v3(p.x + 0.22f * s, p.y + 1.6f * s, p.z + 0.22f * s), RGB(96, 70, 44));
    mesh_cone(m, v3(p.x, p.y + 1.1f * s, p.z), 2.3f * s, 4.4f * s, 6, pack_rgb(v3(0.13f * gv, 0.36f * gv, 0.16f * gv)));
    mesh_cone(m, v3(p.x, p.y + 3.4f * s, p.z), 1.7f * s, 3.8f * s, 6, pack_rgb(v3(0.17f * gv, 0.44f * gv, 0.19f * gv)));
}

static void build_village(mesh_t* props) {
    uint32_t rng = 0x5EED1u;
    float v0 = zone_start(ZONE_VILLAGE), v1 = zone_end(ZONE_VILLAGE), vmid = (v0 + v1) * 0.5f;
    static const uint32_t walls[] = {
        RGB(142, 32, 26), RGB(142, 32, 26), RGB(142, 32, 26), RGB(222, 190, 96), RGB(236, 234, 226), RGB(150, 182, 200),
    };
    int houses = 0;
    for (float s = v0 + 35.0f; s < v1 - 30.0f; s += rng_range(&rng, 18.0f, 26.0f)) {
        for (int side = -1; side <= 1; side += 2) {
            if (side < 0 && fabsf(s - vmid) < 45.0f) continue;       // the church plot
            track_frame_t f = track_frame(s);
            for (int row = 0; row < 2; row++) {
                if (row == 1 && rng_float(&rng) < 0.35f) continue;
                float lat = (row == 0) ? rng_range(&rng, 30.0f, 36.0f) : rng_range(&rng, 54.0f, 66.0f);
                vec3 p = v3_mad(v3_mad(f.pos, f.right, (float)side * lat), f.tanh, rng_range(&rng, -3.0f, 3.0f));
                p.y = terrain_height(p.x, p.z, NULL);
                vec3 facing = v3_scale(f.right, (float)-side);
                uint32_t wall = walls[rng_next(&rng) % 6];
                bool red = (wall == walls[0]);
                uint32_t roof = (rng_float(&rng) < 0.5f) ? ROOF_DARK : RGB(130, 56, 40);
                build_house(props, p, facing, rng_range(&rng, 8.0f, 12.0f), rng_range(&rng, 6.0f, 8.0f),
                            rng_range(&rng, 3.0f, 5.2f), wall, red || rng_float(&rng) < 0.5f, roof, &rng);
                houses++;
                if (rng_float(&rng) < 0.45f) {    // a tree in the garden
                    vec3 t = v3_mad(v3_mad(p, f.tanh, rng_range(&rng, 6.0f, 9.0f)), f.right, (float)side * 4.0f);
                    t.y = terrain_height(t.x, t.z, NULL);
                    round_tree(props, t, rng_range(&rng, 0.7f, 1.1f), rng_range(&rng, 0.85f, 1.1f));
                }
            }
        }
    }
    track_frame_t f = track_frame(vmid);
    vec3 cp = v3_mad(f.pos, f.right, -58.0f);
    cp.y = terrain_height(cp.x, cp.z, NULL);
    build_church(props, cp, f.right);
    printf("[highway] village: %d houses + church\n", houses);
}

static void build_campus(mesh_t* props, mesh_t* decals) {
    float c0 = zone_start(ZONE_CAMPUS);
    const struct { float ds, side, lat; bool sign; } halls[] = {
        { 90.0f, 1.0f, 66.0f, true }, { 200.0f, 1.0f, 66.0f, false }, { 310.0f, 1.0f, 66.0f, false },
        { 150.0f, -1.0f, 72.0f, false }, { 260.0f, -1.0f, 72.0f, false },
    };
    for (size_t i = 0; i < sizeof(halls) / sizeof(halls[0]); i++) {
        track_frame_t f = track_frame(c0 + halls[i].ds);
        vec3 p = v3_mad(f.pos, f.right, halls[i].side * halls[i].lat);
        p.y = terrain_height(p.x, p.z, NULL);
        build_hall(props, decals, p, v3_scale(f.right, -halls[i].side), halls[i].sign);
    }
}

static void build_farm_and_turbines(mesh_t* props) {
    uint32_t rng = 0xFA4Bu;
    float a0 = zone_start(ZONE_FARM), a1 = zone_end(ZONE_FARM);
    // red barns
    for (int i = 0; i < 4; i++) {
        float s = lerpf(a0 + 60.0f, a1 - 60.0f, ((float)i + rng_range(&rng, 0.2f, 0.8f)) / 4.0f);
        float side = (i % 2) ? 1.0f : -1.0f;
        track_frame_t f = track_frame(s);
        vec3 p = v3_mad(f.pos, f.right, side * rng_range(&rng, 48.0f, 110.0f));
        p.y = terrain_height(p.x, p.z, NULL);
        vec3 facing = v3_norm(v3_add(v3_scale(f.right, -side), v3_scale(f.tanh, rng_range(&rng, -0.4f, 0.4f))));
        build_barn(props, p, facing);
    }
    // wrapped white hay bales in clusters
    for (int k = 0; k < 9; k++) {
        float s = rng_range(&rng, a0, a1);
        float side = (rng_float(&rng) < 0.5f) ? -1.0f : 1.0f;
        track_frame_t f = track_frame(s);
        vec3 c = v3_mad(f.pos, f.right, side * rng_range(&rng, 30.0f, 140.0f));
        int n = 3 + (int)(rng_float(&rng) * 6.0f);
        for (int j = 0; j < n; j++) {
            vec3 p = v3(c.x + rng_range(&rng, -9.0f, 9.0f), 0.0f, c.z + rng_range(&rng, -9.0f, 9.0f));
            p.y = terrain_height(p.x, p.z, NULL) + 0.7f;
            mesh_cyl_x(props, p, 0.75f, 0.6f, 8, RGB(236, 238, 240));
        }
    }
    // wind turbines on the hills around the campus and the farmland
    const vec3 wind = v3_norm(v3(0.75f, 0.0f, 0.66f));
    float lo = zone_start(ZONE_CAMPUS) - 100.0f, hi = a1;
    int tries = 0;
    while (g_num_turbines < MAX_TURBINES - 4 && tries++ < 400) {
        float s = rng_range(&rng, lo, hi);
        float side = (rng_float(&rng) < 0.5f) ? -1.0f : 1.0f;
        track_frame_t f = track_frame(s);
        vec3 p = v3_mad(f.pos, f.right, side * rng_range(&rng, 170.0f, 480.0f));
        float d;
        p.y = terrain_height(p.x, p.z, &d);
        if (d < 150.0f || p.y > 140.0f) continue;
        bool ok = true;
        for (int i = 0; i < g_num_turbines && ok; i++) {
            float dx = g_turbines[i].hub.x - p.x, dz = g_turbines[i].hub.z - p.z;
            ok = dx * dx + dz * dz > 130.0f * 130.0f;
        }
        if (ok) build_turbine(props, p, wind, &rng);
    }
    printf("[highway] %d wind turbines\n", g_num_turbines);
}

// ---------------------------------------------------------------------------
//  Trees
// ---------------------------------------------------------------------------
static void build_trees(mesh_t* props) {
    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (int i = 0; i < TRK_N; i++) {
        minx = fminf(minx, trk.pos[i].x); maxx = fmaxf(maxx, trk.pos[i].x);
        minz = fminf(minz, trk.pos[i].z); maxz = fmaxf(maxz, trk.pos[i].z);
    }
    minx -= 700.0f; maxx += 700.0f; minz -= 700.0f; maxz += 700.0f;
    uint32_t rng = 0xC0FFEEu;
    int placed = 0, attempts = 0;
    // scattered trees, thinned out where buildings and fields are
    while (placed < 3600 && attempts < 400000) {
        attempts++;
        float x = rng_range(&rng, minx, maxx);
        float z = rng_range(&rng, minz, maxz);
        terrain_info_t ti = terrain_sample(x, z);
        if (ti.d < ROAD_HALF + 12.0f || ti.h > 175.0f) continue;
        if (ti.d > 350.0f && rng_float(&rng) < 0.75f) continue;
        float keep = 1.0f;
        keep *= 1.0f - 0.85f * zone_weight(ti.s, ZONE_VILLAGE) * (ti.d < 240.0f ? 1.0f : 0.0f);
        keep *= 1.0f - 0.95f * zone_weight(ti.s, ZONE_CAMPUS) * (ti.d < 350.0f ? 1.0f : 0.0f);
        keep *= 1.0f - 0.80f * zone_weight(ti.s, ZONE_FARM);
        keep *= 1.0f - 0.40f * zone_weight(ti.s, ZONE_MOUNTAIN);
        if (rng_float(&rng) > keep) continue;
        float s = rng_range(&rng, 0.75f, 1.6f);
        if (rng_float(&rng) < 0.6f + 0.3f * zone_weight(ti.s, ZONE_MOUNTAIN)) {
            pine_tree(props, v3(x, ti.h, z), s, rng_range(&rng, 0.75f, 1.1f));
        } else {
            round_tree(props, v3(x, ti.h, z), s, rng_range(&rng, 0.8f, 1.15f));
        }
        placed++;
    }
    // dense forest close to the road
    float f0 = zone_start(ZONE_FOREST) - 60.0f, f1 = zone_end(ZONE_FOREST) + 60.0f;
    int dense = 0;
    for (float s = f0; s < f1; s += 1.6f) {
        float w = zone_weight(s, ZONE_FOREST);
        track_frame_t f = track_frame(s);
        for (int side = -1; side <= 1; side += 2) {
            if (rng_float(&rng) > w * 0.85f) continue;
            float d = ROAD_HALF + 5.0f + powf(rng_float(&rng), 1.6f) * 150.0f;
            vec3 p = v3_mad(v3_mad(f.pos, f.right, (float)side * d), f.tanh, rng_range(&rng, -1.0f, 1.0f));
            p.y = terrain_height(p.x, p.z, NULL);
            float sc = rng_range(&rng, 0.9f, 1.8f);
            if (rng_float(&rng) < 0.85f) {
                pine_tree(props, p, sc, rng_range(&rng, 0.6f, 0.95f));
            } else {
                round_tree(props, p, sc, rng_range(&rng, 0.7f, 1.0f));
            }
            dense++;
        }
    }
    printf("[highway] %d trees (+%d forest)\n", placed, dense);
}

// ---------------------------------------------------------------------------
//  Sky cylinder
// ---------------------------------------------------------------------------
static void build_sky(mesh_t* m) {
    const int seg = 64;
    const vec3 n = v3(0, 1, 0);
    const uint32_t white = RGB(255, 255, 255);
    for (int i = 0; i < seg; i++) {
        float u0 = (float)i / (float)seg, u1 = (float)(i + 1) / (float)seg;
        float a0 = u0 * 2.0f * PI_F, a1 = u1 * 2.0f * PI_F;
        vec3 b0 = v3(cosf(a0) * SKY_RADIUS, SKY_BOTTOM, sinf(a0) * SKY_RADIUS);
        vec3 b1 = v3(cosf(a1) * SKY_RADIUS, SKY_BOTTOM, sinf(a1) * SKY_RADIUS);
        vec3 t0 = v3(b0.x, SKY_TOP, b0.z);
        vec3 t1 = v3(b1.x, SKY_TOP, b1.z);
        float uv[4][2] = { { u0, 1.0f }, { u1, 1.0f }, { u1, 0.0f }, { u0, 0.0f } };
        mesh_quad_uv(m, b0, b1, t1, t0, uv, n, white);
        mesh_vertex(m, v3(0, SKY_TOP, 0), n, (u0 + u1) * 0.5f, 0.002f, white);
        mesh_vertex(m, t0, n, u0, 0.002f, white);
        mesh_vertex(m, t1, n, u1, 0.002f, white);
    }
}

void world_build(world_meshes_t* w) {
    memset(w, 0, sizeof(*w));
    g_num_turbines = 0;
    build_terrain(&w->terrain);
    mesh_uv_scale = 0.5f;
    build_road(&w->road, &w->props);
    build_structures(&w->props, &w->decals);
    build_village(&w->props);
    build_campus(&w->props, &w->decals);
    build_farm_and_turbines(&w->props);
    build_trees(&w->props);
    build_sky(&w->sky);
    build_blades(&w->blades);
    printf("[highway] geometry: terrain %d verts / %d idx, road %d, props %d, decals %d\n",
           w->terrain.count, w->terrain.icount, w->road.count, w->props.count, w->decals.count);
}

void world_free(world_meshes_t* w) {
    mesh_free(&w->terrain);
    mesh_free(&w->road);
    mesh_free(&w->props);
    mesh_free(&w->decals);
    mesh_free(&w->sky);
    mesh_free(&w->blades);
}
