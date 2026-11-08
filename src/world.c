// Track generation (closed loop highway), terrain heightfield and static scenery.
#include "world.h"
#include "noise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRK_N       2048        // resampled centre line points (~2.4m apart)
#define TRK_RAW     16384       // raw samples used for arc-length parameterisation
#define TRACK_R0    760.0f      // base radius of the loop

static struct {
    float L;          // total length (m)
    float ds;         // distance between samples
    float dash;       // lane dash period (divides L exactly so texturing loops seamlessly)
    vec3 pos[TRK_N];
    vec3 tan[TRK_N];
    vec3 right[TRK_N];
} trk;

static const vec3 UP = { 0.0f, 1.0f, 0.0f };

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

    // road height: heavily smoothed natural terrain height along the loop
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
    for (int i = 0; i < TRK_N; i++) {
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

void track_nearest(float x, float z, float* out_dist, float* out_road_y, float* out_s) {
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
}

float terrain_height(float x, float z, float* out_dist) {
    float d, ry;
    track_nearest(x, z, &d, &ry, NULL);
    float nat = natural_height(x, z);
    float mf = smoothstepf(80.0f, 700.0f, d);
    if (mf > 0.0f) {
        nat += mf * mountain_height(x, z);
    }
    float b = smoothstepf(ROAD_HALF + 2.5f, ROAD_HALF + 40.0f, d);
    if (out_dist) *out_dist = d;
    return lerpf(ry - 0.35f, nat, b);
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
    float v0 = (float)slot / (float)ATLAS_SLOTS + 0.002f;
    float v1 = (float)(slot + 1) / (float)ATLAS_SLOTS - 0.002f;
    // corners: bottom-left, bottom-right, top-right, top-left
    uv[0][0] = 0.0f; uv[0][1] = v1;
    uv[1][0] = 1.0f; uv[1][1] = v1;
    uv[2][0] = 1.0f; uv[2][1] = v0;
    uv[3][0] = 0.0f; uv[3][1] = v0;
}

// textured panel facing direction 'facing', centred at c; viewer_right is the
// horizontal direction that is 'right' for someone looking at the panel
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

// ---------------------------------------------------------------------------
//  Terrain
// ---------------------------------------------------------------------------
static uint32_t terrain_color(float x, float z, float h, float d, vec3 n) {
    float g = nz_fbm(x * 0.012f, z * 0.012f, 3, 0, 0, 5u);
    vec3 c = v3_lerp(v3(0.30f, 0.50f, 0.17f), v3(0.50f, 0.58f, 0.22f), g);
    float slope = 1.0f - n.y;
    vec3 rock = v3(0.48f, 0.44f, 0.39f);
    c = v3_lerp(c, rock, smoothstepf(0.22f, 0.45f, slope));
    c = v3_lerp(c, v3_scale(rock, 0.92f), smoothstepf(120.0f, 190.0f, h));
    c = v3_lerp(c, v3(0.94f, 0.95f, 0.98f), smoothstepf(250.0f, 290.0f, h + 40.0f * (g - 0.5f)));
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
    float* H = (float*)malloc(sizeof(float) * (size_t)nx * (size_t)nz);
    float* D = (float*)malloc(sizeof(float) * (size_t)nx * (size_t)nz);
    for (int j = 0; j < nz; j++) {
        for (int i = 0; i < nx; i++) {
            float x = x0 + (float)i * cell, z = z0 + (float)j * cell;
            H[j * nx + i] = terrain_height(x, z, &D[j * nx + i]);
        }
    }
    for (int j = 0; j < nz; j++) {
        for (int i = 0; i < nx; i++) {
            float x = x0 + (float)i * cell, z = z0 + (float)j * cell;
            float hl = H[j * nx + (i > 0 ? i - 1 : i)];
            float hr = H[j * nx + (i < nx - 1 ? i + 1 : i)];
            float hd = H[(j > 0 ? j - 1 : j) * nx + i];
            float hu = H[(j < nz - 1 ? j + 1 : j) * nx + i];
            vec3 n = v3_norm(v3(hl - hr, 2.0f * cell, hd - hu));
            float h = H[j * nx + i];
            mesh_vertex(m, v3(x, h, z), n, x / 8.0f, z / 8.0f, terrain_color(x, z, h, D[j * nx + i], n));
        }
    }
    for (int j = 0; j < nz - 1; j++) {
        for (int i = 0; i < nx - 1; i++) {
            uint32_t a = (uint32_t)(j * nx + i), b = a + 1, c = a + (uint32_t)nx, d = c + 1;
            mesh_index(m, a); mesh_index(m, c); mesh_index(m, b);
            mesh_index(m, b); mesh_index(m, c); mesh_index(m, d);
        }
    }
    free(H);
    free(D);
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
//  Overpasses, gantries, billboards
// ---------------------------------------------------------------------------
static void build_structures(mesh_t* props, mesh_t* decals) {
    const uint32_t concrete = RGB(176, 172, 162);
    const uint32_t steel = RGB(110, 118, 130);

    // overpass bridges crossing the highway
    const float bridge_pos[2] = { 0.23f, 0.64f };
    for (int b = 0; b < 2; b++) {
        track_frame_t f = track_frame(bridge_pos[b] * trk.L);
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

    // overhead sign gantries (alternating carriageways)
    int k = 0;
    for (float s = 380.0f; s < trk.L - 200.0f; s += 1050.0f, k++) {
        track_frame_t f = track_frame(s);
        float dir = (k % 2 == 0) ? 1.0f : -1.0f;
        float lo = dir * 0.75f, hi = dir * (ROAD_HALF + 0.7f);
        frame_box(props, &f, lo - 0.18f, lo + 0.18f, -0.18f, 0.18f, 0.0f, 7.6f, steel);
        frame_box(props, &f, hi - 0.18f, hi + 0.18f, -0.18f, 0.18f, -1.0f, 7.6f, steel);
        frame_box(props, &f, dir * 0.5f, dir * (ROAD_HALF + 0.9f), -0.2f, 0.2f, 7.0f, 7.6f, steel);
        // sign board faces traffic approaching from behind (travelling along dir * tangent)
        float a_board = -dir * 0.3f;
        frame_box(props, &f, dir * 2.3f, dir * 9.7f, a_board - 0.08f, a_board + 0.08f, 4.3f, 8.0f, RGB(60, 64, 70));
        vec3 facing = v3_scale(f.tanh, -dir);
        vec3 c = v3_mad(v3_mad(v3_mad(f.pos, f.right, dir * 6.0f), f.tanh, a_board - dir * 0.1f), UP, 6.15f);
        decal_panel(decals, c, facing, 3.6f, 1.8f, 4 + (k % 3));
    }

    // roadside billboards
    k = 0;
    for (float s = 150.0f; s < trk.L - 100.0f; s += 470.0f, k++) {
        track_frame_t f = track_frame(s);
        float side = (k % 2 == 0) ? 1.0f : -1.0f;
        vec3 base = v3_mad(f.pos, f.right, side * 27.0f);
        base.y = terrain_height(base.x, base.z, NULL);
        // face the approaching traffic on that side, angled towards the road
        vec3 facing = v3_norm(v3_add(v3_scale(f.tanh, -side * 0.9f), v3_scale(f.right, -side * 0.45f)));
        vec3 vr = v3_norm(v3_cross(v3_scale(facing, -1.0f), UP));
        vec3 c = v3_mad(base, UP, 9.0f);
        obox(props, v3_mad(c, facing, -0.25f), vr, UP, facing, 6.3f, 3.3f, 0.2f, RGB(70, 70, 76));
        for (int p = -1; p <= 1; p += 2) {
            vec3 pc = v3_mad(v3_mad(base, vr, (float)p * 3.8f), facing, -0.3f);
            obox(props, v3_mad(pc, UP, 2.0f), vr, UP, facing, 0.22f, 4.5f, 0.22f, RGB(96, 96, 100));
        }
        decal_panel(decals, v3_mad(c, facing, -0.02f), facing, 6.0f, 3.0f, k % 4);
    }
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
    while (placed < 4200 && attempts < 400000) {
        attempts++;
        float x = rng_range(&rng, minx, maxx);
        float z = rng_range(&rng, minz, maxz);
        float d;
        float h = terrain_height(x, z, &d);
        if (d < ROAD_HALF + 12.0f || h > 175.0f) {
            continue;
        }
        if (d > 350.0f && rng_float(&rng) < 0.75f) {
            continue;
        }
        float s = rng_range(&rng, 0.75f, 1.6f);
        uint32_t trunk = RGB(96, 70, 44);
        if (rng_float(&rng) < 0.6f) {
            // pine
            float gv = rng_range(&rng, 0.75f, 1.1f);
            uint32_t g1 = pack_rgb(v3(0.13f * gv, 0.36f * gv, 0.16f * gv));
            uint32_t g2 = pack_rgb(v3(0.17f * gv, 0.44f * gv, 0.19f * gv));
            mesh_box(props, v3(x - 0.22f * s, h - 0.5f, z - 0.22f * s), v3(x + 0.22f * s, h + 1.6f * s, z + 0.22f * s), trunk);
            mesh_cone(props, v3(x, h + 1.1f * s, z), 2.3f * s, 4.4f * s, 6, g1);
            mesh_cone(props, v3(x, h + 3.4f * s, z), 1.7f * s, 3.8f * s, 6, g2);
        } else {
            // round deciduous tree
            float gv = rng_range(&rng, 0.8f, 1.15f);
            uint32_t g = pack_rgb(v3(0.30f * gv, 0.50f * gv, 0.16f * gv));
            mesh_box(props, v3(x - 0.25f * s, h - 0.5f, z - 0.25f * s), v3(x + 0.25f * s, h + 2.4f * s, z + 0.25f * s), trunk);
            mesh_blob(props, v3(x, h + 4.3f * s, z), 2.7f * s, 2.5f * s, 2.7f * s, 7, g);
        }
        placed++;
    }
    printf("[highway] %d trees\n", placed);
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
        // top cap
        mesh_vertex(m, v3(0, SKY_TOP, 0), n, (u0 + u1) * 0.5f, 0.002f, white);
        mesh_vertex(m, t0, n, u0, 0.002f, white);
        mesh_vertex(m, t1, n, u1, 0.002f, white);
    }
}

void world_build(world_meshes_t* w) {
    memset(w, 0, sizeof(*w));
    build_terrain(&w->terrain);
    mesh_uv_scale = 0.5f;
    build_road(&w->road, &w->props);
    build_structures(&w->props, &w->decals);
    build_trees(&w->props);
    build_sky(&w->sky);
    printf("[highway] geometry: terrain %d verts / %d idx, road %d, props %d, decals %d\n",
           w->terrain.count, w->terrain.icount, w->road.count, w->props.count, w->decals.count);
}

void world_free(world_meshes_t* w) {
    mesh_free(&w->terrain);
    mesh_free(&w->road);
    mesh_free(&w->props);
    mesh_free(&w->decals);
    mesh_free(&w->sky);
}
