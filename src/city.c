// Future city generation. Everything is procedural and deterministic.
// Emissive surfaces (neon, lamps, holograms) are marked with vertex alpha 0; lit windows
// come from the facade texture's alpha channel.
#include "city.h"
#include "world.h"
#include "noise.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CITY_TRK_N 2048          // samples along the loop (matches the old road's resolution)

static const vec3 UP = { 0.0f, 1.0f, 0.0f };
static float g_ground = 0.0f;
static vec3 g_spire;
static city_pod_t g_pods[CITY_MAX_PODS];
static int g_num_pods = 0;

float city_ground_y(void) { return g_ground; }
vec3 city_spire_pos(void) { return g_spire; }
int city_pods(const city_pod_t** out) { *out = g_pods; return g_num_pods; }

// neon colours (alpha 0 = emissive)
static const uint32_t NEON[] = {
    RGBA(40, 230, 255, 0), RGBA(255, 50, 200, 0), RGBA(255, 170, 40, 0), RGBA(150, 90, 255, 0),
};
#define NUM_NEON ((int)(sizeof(NEON) / sizeof(NEON[0])))

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
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
static void fbox(mesh_t* m, const track_frame_t* f, float lat0, float lat1, float a0, float a1,
                 float y0, float y1, uint32_t col) {
    vec3 c = f->pos;
    c = v3_mad(c, f->right, (lat0 + lat1) * 0.5f);
    c = v3_mad(c, f->tanh, (a0 + a1) * 0.5f);
    c = v3_mad(c, UP, (y0 + y1) * 0.5f);
    obox(m, c, f->right, UP, f->tanh, fabsf(lat1 - lat0) * 0.5f, fabsf(y1 - y0) * 0.5f, fabsf(a1 - a0) * 0.5f, col);
}

// vertical box from y0 to y1, rotated by angle a around the vertical axis
static void tbox(mesh_t* m, float x, float z, float a, float hx, float hz, float y0, float y1, uint32_t col) {
    vec3 ax = v3(cosf(a), 0.0f, sinf(a)), az = v3(-sinf(a), 0.0f, cosf(a));
    obox(m, v3(x, (y0 + y1) * 0.5f, z), ax, UP, az, hx, (y1 - y0) * 0.5f, hz, col);
}

// n-sided frustum (sides only, plus an optional top cap)
static void prism(mesh_t* m, float x, float z, float r0, float r1, float y0, float y1, int sides, float a0,
                  bool cap, uint32_t col) {
    for (int i = 0; i < sides; i++) {
        float t0 = a0 + (float)i / (float)sides * 2.0f * PI_F, t1 = a0 + (float)(i + 1) / (float)sides * 2.0f * PI_F;
        vec3 d0 = v3(cosf(t0), 0, sinf(t0)), d1 = v3(cosf(t1), 0, sinf(t1));
        vec3 b0 = v3(x + d0.x * r0, y0, z + d0.z * r0), b1 = v3(x + d1.x * r0, y0, z + d1.z * r0);
        vec3 u0 = v3(x + d0.x * r1, y1, z + d0.z * r1), u1 = v3(x + d1.x * r1, y1, z + d1.z * r1);
        mesh_quad(m, b0, b1, u1, u0, v3_add(d0, d1), col);
        if (cap) {
            mesh_tri(m, v3(x, y1, z), u0, u1, UP, col);
        }
    }
}

// emissive ring of boxes around a vertical axis
static void ring(mesh_t* m, float x, float z, float r, float y, float th, float hh, int segs, uint32_t col) {
    for (int i = 0; i < segs; i++) {
        float a = ((float)i + 0.5f) / (float)segs * 2.0f * PI_F;
        float half = r * tanf(PI_F / (float)segs) + 0.2f;
        tbox(m, x + cosf(a) * r, z + sinf(a) * r, a, th * 0.5f, half, y - hh, y + hh, col);
    }
}

static void panel(mesh_t* m, vec3 c, vec3 facing, float hw, float hh, int slot, uint32_t col) {
    vec3 vr = v3_norm(v3_cross(v3_scale(facing, -1.0f), UP));
    vec3 bl = v3_mad(v3_mad(c, vr, -hw), UP, -hh);
    vec3 br = v3_mad(v3_mad(c, vr, hw), UP, -hh);
    vec3 tr = v3_mad(v3_mad(c, vr, hw), UP, hh);
    vec3 tl = v3_mad(v3_mad(c, vr, -hw), UP, hh);
    float v0 = (float)slot / (float)ATLAS_SLOTS + 0.001f, v1 = (float)(slot + 1) / (float)ATLAS_SLOTS - 0.001f;
    float uv[4][2] = { { 0.0f, v1 }, { 1.0f, v1 }, { 1.0f, v0 }, { 0.0f, v0 } };
    mesh_quad_uv(m, bl, br, tr, tl, uv, facing, col);
}

// shifts the facade texture of the vertices [first, count) so towers don't share window patterns
static void offset_uv(mesh_t* m, int first, float du, float dv) {
    for (int i = first; i < m->count; i++) {
        m->v[i].uv[0] += du;
        m->v[i].uv[1] += dv;
    }
}

static void track_bounds(float* minx, float* maxx, float* minz, float* maxz, float* miny) {
    *minx = 1e9f; *maxx = -1e9f; *minz = 1e9f; *maxz = -1e9f; *miny = 1e9f;
    const float L = track_length();
    for (int i = 0; i < CITY_TRK_N; i++) {
        vec3 p = track_frame((float)i * L / (float)CITY_TRK_N).pos;
        *minx = fminf(*minx, p.x); *maxx = fmaxf(*maxx, p.x);
        *minz = fminf(*minz, p.z); *maxz = fmaxf(*maxz, p.z);
        *miny = fminf(*miny, p.y);
    }
}

// ---------------------------------------------------------------------------
//  Ground: one flat street grid
// ---------------------------------------------------------------------------
static void build_ground(mesh_t* m, float minx, float maxx, float minz, float maxz) {
    const float margin = 1500.0f, cell = 32.0f;
    float x0 = floorf((minx - margin) / CITY_BLOCK) * CITY_BLOCK;
    float z0 = floorf((minz - margin) / CITY_BLOCK) * CITY_BLOCK;
    int nx = (int)((maxx + margin - x0) / cell) + 2;
    int nz = (int)((maxz + margin - z0) / cell) + 2;
    for (int j = 0; j < nz; j++) {
        for (int i = 0; i < nx; i++) {
            float x = x0 + (float)i * cell, z = z0 + (float)j * cell;
            mesh_vertex(m, v3(x, g_ground, z), UP, x / CITY_BLOCK, z / CITY_BLOCK, RGB(225, 225, 235));
        }
    }
    for (int j = 0; j < nz - 1; j++) {
        for (int i = 0; i < nx - 1; i++) {
            uint32_t a = (uint32_t)(j * nx + i), b = a + 1, c = a + (uint32_t)nx, d = c + 1;
            mesh_index(m, a); mesh_index(m, c); mesh_index(m, b);
            mesh_index(m, b); mesh_index(m, c); mesh_index(m, d);
        }
    }
}

// ---------------------------------------------------------------------------
//  Skyway: deck, pylons, barriers with neon rails, lamps, gantries
// ---------------------------------------------------------------------------
static void build_skyway(city_meshes_t* c) {
    const float L = track_length(), ds = L / (float)CITY_TRK_N, dash = track_dash_period();
    const float lat_pts[5] = { 0.0f, 2.5f, 5.0f, 7.5f, ROAD_HALF };
    const uint32_t surface = RGB(150, 156, 180), deck = RGB(70, 74, 90), glass = RGB(80, 120, 150);
    const uint32_t steel = RGB(150, 156, 172), cyan = NEON[0], magenta = NEON[1];
    for (int i = 0; i < CITY_TRK_N; i++) {
        track_frame_t f0 = track_frame((float)i * ds), f1 = track_frame((float)(i + 1) * ds);
        float v0 = (float)i * ds / dash, v1 = (float)(i + 1) * ds / dash;
        vec3 n0 = v3_norm(v3_cross(f0.right, f0.tan));
        for (int side = -1; side <= 1; side += 2) {
            for (int k = 0; k < 4; k++) {
                float la = lat_pts[k], lb = lat_pts[k + 1];
                vec3 p00 = v3_mad(f0.pos, f0.right, (float)side * la), p01 = v3_mad(f0.pos, f0.right, (float)side * lb);
                vec3 p10 = v3_mad(f1.pos, f1.right, (float)side * la), p11 = v3_mad(f1.pos, f1.right, (float)side * lb);
                float ua = la / ROAD_HALF, ub = lb / ROAD_HALF;
                float uv[4][2] = { { ua, v0 }, { ub, v0 }, { ub, v1 }, { ua, v1 } };
                mesh_quad_uv(&c->road, p00, p01, p11, p10, uv, n0, surface);
            }
            // deck edge (fascia) with a neon strip, barrier glass and the glowing hand rail
            float e = (float)side * (ROAD_HALF + 0.3f);
            vec3 e0 = v3_mad(f0.pos, f0.right, e), e1 = v3_mad(f1.pos, f1.right, e);
            vec3 out = v3_scale(f0.right, (float)side);
            mesh_quad(&c->deck, v3_mad(e0, UP, 0.2f), v3_mad(e1, UP, 0.2f), v3_mad(e1, UP, -1.6f), v3_mad(e0, UP, -1.6f), out, deck);
            mesh_quad(&c->deck, v3_mad(e0, UP, -0.75f), v3_mad(e1, UP, -0.75f), v3_mad(e1, UP, -0.45f), v3_mad(e0, UP, -0.45f),
                      out, side > 0 ? cyan : magenta);
            float bl = (float)side * (ROAD_HALF - 0.1f);
            vec3 b0 = v3_mad(f0.pos, f0.right, bl), b1 = v3_mad(f1.pos, f1.right, bl);
            mesh_quad(&c->deck, v3_mad(b0, UP, 0.15f), v3_mad(b1, UP, 0.15f), v3_mad(b1, UP, 0.95f), v3_mad(b0, UP, 0.95f),
                      v3_scale(out, -1.0f), glass);
            mesh_quad(&c->deck, v3_mad(b0, UP, 0.95f), v3_mad(b1, UP, 0.95f), v3_mad(b1, UP, 1.08f), v3_mad(b0, UP, 1.08f),
                      v3_scale(out, -1.0f), cyan);
        }
        // deck underside
        vec3 l0 = v3_mad(v3_mad(f0.pos, f0.right, -ROAD_HALF - 0.3f), UP, -1.6f);
        vec3 r0 = v3_mad(v3_mad(f0.pos, f0.right, ROAD_HALF + 0.3f), UP, -1.6f);
        vec3 l1 = v3_mad(v3_mad(f1.pos, f1.right, -ROAD_HALF - 0.3f), UP, -1.6f);
        vec3 r1 = v3_mad(v3_mad(f1.pos, f1.right, ROAD_HALF + 0.3f), UP, -1.6f);
        mesh_quad(&c->deck, l0, r0, r1, l1, v3(0, -1, 0), deck);
        // low median divider with a magenta light strip
        fbox(&c->deck, &f0, -0.22f, 0.22f, -ds * 0.5f, ds * 0.5f, 0.0f, 0.55f, RGB(90, 94, 110));
        fbox(&c->deck, &f0, -0.10f, 0.10f, -ds * 0.5f, ds * 0.5f, 0.55f, 0.66f, magenta);

        // pylons every ~30 m (where the deck is high enough above the street)
        float hgt = f0.pos.y - g_ground;
        if ((i % 12) == 0 && hgt > 3.0f) {
            fbox(&c->deck, &f0, -1.3f, 1.3f, -1.3f, 1.3f, -hgt, -2.6f, steel);
            fbox(&c->deck, &f0, -8.5f, 8.5f, -1.0f, 1.0f, -2.9f, -1.6f, steel);
            if (hgt > 9.0f) {
                fbox(&c->deck, &f0, -1.4f, 1.4f, -1.4f, 1.4f, -hgt + 3.0f, -hgt + 3.6f, cyan);
            }
        }
        // slim lamps on the median with glowing heads
        if ((i % 22) == 11) {
            fbox(&c->deck, &f0, -0.1f, 0.1f, -0.1f, 0.1f, 0.6f, 9.0f, RGB(120, 124, 140));
            fbox(&c->deck, &f0, -2.8f, 2.8f, -0.06f, 0.06f, 8.8f, 8.95f, RGB(120, 124, 140));
            fbox(&c->deck, &f0, 2.0f, 3.2f, -0.18f, 0.18f, 8.6f, 8.8f, RGBA(255, 236, 200, 0));
            fbox(&c->deck, &f0, -3.2f, -2.0f, -0.18f, 0.18f, 8.6f, 8.8f, RGBA(255, 236, 200, 0));
        }
    }

    // gantries spanning the whole skyway with glowing signs for the player's direction
    for (int k = 0; k < 9; k++) {
        float s = ((float)k + 0.35f) * L / 9.0f;
        track_frame_t f = track_frame(s);
        float hi = ROAD_HALF + 0.9f;
        fbox(&c->deck, &f, -hi - 0.25f, -hi + 0.25f, -0.25f, 0.25f, -1.0f, 8.6f, steel);
        fbox(&c->deck, &f, hi - 0.25f, hi + 0.25f, -0.25f, 0.25f, -1.0f, 8.6f, steel);
        fbox(&c->deck, &f, -hi, hi, -0.3f, 0.3f, 8.0f, 8.6f, steel);
        fbox(&c->deck, &f, -hi, hi, -0.32f, 0.32f, 7.85f, 8.0f, (k & 1) ? cyan : magenta);
        fbox(&c->deck, &f, 2.3f, 9.7f, -0.38f, -0.22f, 4.3f, 8.0f, RGB(40, 40, 52));
        vec3 facing = v3_scale(f.tanh, -1.0f);
        vec3 ctr = v3_mad(v3_mad(v3_mad(f.pos, f.right, 6.0f), f.tanh, -0.42f), UP, 6.15f);
        panel(&c->deck_decals, ctr, facing, 3.6f, 1.8f, (k % 3 == 1) ? DECAL_CITY : DECAL_SING_HERE, RGBA(255, 255, 255, 0));
    }
}

// ---------------------------------------------------------------------------
//  Towers
// ---------------------------------------------------------------------------
static const uint32_t GLASS_TINTS[] = {
    RGB(150, 190, 240), RGB(150, 225, 215), RGB(215, 218, 232), RGB(120, 120, 150), RGB(240, 205, 160), RGB(190, 160, 230),
};
#define NUM_TINTS ((int)(sizeof(GLASS_TINTS) / sizeof(GLASS_TINTS[0])))

static void crown(city_meshes_t* c, float x, float z, float a, float hs, float top, uint32_t* rng) {
    tbox(&c->trim, x, z, a, hs * 0.8f, hs * 0.8f, top, top + 2.5f, RGB(60, 64, 78));
    if (rng_float(rng) < 0.45f) {
        float h = rng_range(rng, 8.0f, 30.0f);
        tbox(&c->trim, x, z, a, 0.3f, 0.3f, top + 2.5f, top + 2.5f + h, RGB(150, 152, 160));
        tbox(&c->trim, x, z, a, 0.8f, 0.8f, top + 2.5f + h, top + 4.0f + h, RGBA(255, 40, 40, 0));
    }
}

static void neon_bands(city_meshes_t* c, float x, float z, float a, float hx, float hz, float y0, float y1, uint32_t* rng) {
    if (rng_float(rng) > 0.5f) return;
    uint32_t col = NEON[(int)(rng_float(rng) * NUM_NEON) % NUM_NEON];
    float step = rng_range(rng, 28.0f, 70.0f);
    for (float y = y0 + step; y < y1 - 6.0f; y += step) {
        tbox(&c->trim, x, z, a, hx + 0.3f, hz + 0.3f, y, y + 0.9f, col);
    }
}

static void build_tower(city_meshes_t* c, float x, float z, float hs, float h, vec3 to_road, uint32_t* rng) {
    const float y0 = g_ground - 2.0f;
    uint32_t tint = GLASS_TINTS[(int)(rng_float(rng) * NUM_TINTS) % NUM_TINTS];
    float a = (rng_float(rng) < 0.6f) ? 0.0f : rng_range(rng, 0.0f, PI_F * 0.5f);
    int first = c->towers.count;
    float r = rng_float(rng);
    float top = g_ground + h;
    if (r < 0.40f) {            // stepped box tower
        int tiers = 1 + (int)(rng_float(rng) * 3.0f);
        float hx = hs, hz = hs * rng_range(rng, 0.7f, 1.0f), yb = y0;
        for (int t = 0; t < tiers; t++) {
            float yt = (t == tiers - 1) ? top : yb + (top - yb) * rng_range(rng, 0.45f, 0.7f);
            tbox(&c->towers, x, z, a, hx, hz, yb, yt, tint);
            neon_bands(c, x, z, a, hx, hz, yb, yt, rng);
            yb = yt;
            hx *= 0.78f; hz *= 0.78f;
        }
        crown(c, x, z, a, fminf(hx, hz) / 0.78f, top, rng);
    } else if (r < 0.60f) {     // round tower with a glowing cone roof
        int sides = 10;
        prism(&c->towers, x, z, hs, hs * 0.88f, y0, top, sides, a, true, tint);
        uint32_t col = NEON[(int)(rng_float(rng) * NUM_NEON) % NUM_NEON];
        prism(&c->trim, x, z, hs * 0.9f, 1.0f, top, top + hs * 1.6f, sides, a, false, RGB(70, 74, 90));
        ring(&c->trim, x, z, hs * 0.9f, top + 0.6f, 0.8f, 0.6f, sides, col);
        if (rng_float(rng) < 0.5f) ring(&c->trim, x, z, hs * 0.95f, g_ground + h * 0.5f, 0.8f, 0.5f, sides, col);
    } else if (r < 0.75f) {     // twisting tower of rotated slabs
        int segs = 10;
        float da = rng_range(rng, 0.06f, 0.14f) * (rng_float(rng) < 0.5f ? -1.0f : 1.0f);
        for (int k = 0; k < segs; k++) {
            float yb = y0 + (top - y0) * (float)k / (float)segs, yt = y0 + (top - y0) * (float)(k + 1) / (float)segs;
            float sh = 1.0f - 0.25f * (float)k / (float)segs;
            tbox(&c->towers, x, z, a + da * (float)k, hs * 0.85f * sh, hs * 0.85f * sh, yb, yt, tint);
        }
        crown(c, x, z, a + da * (float)(segs - 1), hs * 0.6f, top, rng);
    } else if (r < 0.85f) {     // tapered obelisk with a glowing tip
        vec3 ax = v3(cosf(a), 0.0f, sinf(a)), az = v3(-sinf(a), 0.0f, cosf(a));
        float t = 0.42f;
        vec3 cn[8];
        const float sx[4] = { -1, 1, 1, -1 }, sz[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++) {
            vec3 o = v3_add(v3_scale(ax, sx[k] * hs), v3_scale(az, sz[k] * hs));
            cn[k] = v3(x + o.x, y0, z + o.z);
            cn[k + 4] = v3(x + o.x * t, top, z + o.z * t);
        }
        mesh_hexa(&c->towers, cn, tint);
        prism(&c->trim, x, z, hs * t * 1.41f, 0.5f, top, top + hs * 1.2f, 4, a + PI_F * 0.25f, false,
              NEON[(int)(rng_float(rng) * NUM_NEON) % NUM_NEON]);
    } else {                    // twin towers joined by a glowing sky bridge
        vec3 ax = v3(cosf(a), 0.0f, sinf(a));
        float h2 = h * rng_range(rng, 0.75f, 0.95f), hs2 = hs * 0.42f, off = hs * 0.56f;
        tbox(&c->towers, x - ax.x * off, z - ax.z * off, a, hs2, hs2, y0, top, tint);
        tbox(&c->towers, x + ax.x * off, z + ax.z * off, a, hs2, hs2, y0, g_ground + h2, tint);
        crown(c, x - ax.x * off, z - ax.z * off, a, hs2, top, rng);
        crown(c, x + ax.x * off, z + ax.z * off, a, hs2, g_ground + h2, rng);
        float yb = g_ground + h2 * rng_range(rng, 0.5f, 0.8f);
        tbox(&c->trim, x, z, a, off, hs2 * 0.45f, yb, yb + 5.0f, RGB(90, 110, 140));
        tbox(&c->trim, x, z, a, off, hs2 * 0.5f, yb + 5.0f, yb + 5.6f, NEON[(int)(rng_float(rng) * NUM_NEON) % NUM_NEON]);
    }
    offset_uv(&c->towers, first, (float)(rng_next(rng) % 8) / 8.0f, (float)(rng_next(rng) % 8) / 8.0f);

    // holographic advert on the side facing the skyway
    if (h > 50.0f && rng_float(rng) < 0.38f) {
        static const int ads[] = { DECAL_SOKOL, DECAL_RSI, DECAL_GAIA, DECAL_HORIZON, DECAL_AGENTS, DECAL_TOKENS, DECAL_CITY };
        int slot = ads[(int)(rng_float(rng) * 7.0f) % 7];
        float hw = fminf(hs * 0.85f, 16.0f);
        vec3 ctr = v3(x + to_road.x * (hs + 1.5f), g_ground + h * rng_range(rng, 0.3f, 0.6f), z + to_road.z * (hs + 1.5f));
        panel(&c->decals, ctr, to_road, hw, hw * 0.5f, slot, RGBA(255, 255, 255, 0));
    }
}

static void build_towers(city_meshes_t* c, float minx, float maxx, float minz, float maxz) {
    const float ext = 1100.0f;
    int i0 = (int)floorf((minx - ext) / CITY_BLOCK), i1 = (int)floorf((maxx + ext) / CITY_BLOCK);
    int j0 = (int)floorf((minz - ext) / CITY_BLOCK), j1 = (int)floorf((maxz + ext) / CITY_BLOCK);
    int count = 0;
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            uint32_t rng = 0x9E3779B9u ^ (uint32_t)(i * 73856093) ^ (uint32_t)(j * 19349663);
            rng_next(&rng);
            float cx = ((float)i + 0.5f) * CITY_BLOCK, cz = ((float)j + 0.5f) * CITY_BLOCK;
            float sd = sqrtf((cx - g_spire.x) * (cx - g_spire.x) + (cz - g_spire.z) * (cz - g_spire.z));
            if (sd < 170.0f) continue;                          // plaza around the spire
            float d, ry, s, side;
            track_nearest(cx, cz, &d, &ry, &s, &side);
            if (d > ext) continue;
            if (d > 700.0f && rng_float(&rng) < 0.45f) continue;
            if (rng_float(&rng) < 0.10f) continue;              // the odd open plaza
            float hs_max = fminf(27.0f, (d - ROAD_HALF - 5.0f) / 1.42f);
            if (hs_max < 7.0f) continue;
            float hs = rng_range(&rng, fminf(10.0f, hs_max), hs_max);
            float downtown = nz_fbm(cx * 0.0016f + 4.0f, cz * 0.0016f, 3, 0, 0, 93u);
            float near = 1.0f - smoothstepf(60.0f, 480.0f, d);
            float u = rng_float(&rng);
            float h = 26.0f + 300.0f * powf(u, 2.0f) * (0.3f + downtown) + 70.0f * near * rng_float(&rng);
            h += 160.0f * (1.0f - smoothstepf(170.0f, 600.0f, sd)) * rng_float(&rng);
            h = clampf(h, 18.0f, 430.0f);
            track_frame_t f = track_frame(s);
            vec3 to_road = v3_norm(v3(f.pos.x - cx, 0.0f, f.pos.z - cz));
            build_tower(c, cx, cz, hs, h, to_road, &rng);
            count++;
        }
    }
    printf("[highway] future city: %d towers\n", count);
}

// ---------------------------------------------------------------------------
//  The singularity spire in the middle of the loop
// ---------------------------------------------------------------------------
static void build_spire(city_meshes_t* c) {
    const float x = g_spire.x, z = g_spire.z, g = g_ground;
    const uint32_t body = RGB(170, 180, 215), dark = RGB(60, 62, 80);
    prism(&c->spire, x, z, 78.0f, 52.0f, g - 2.0f, g + 70.0f, 8, 0.0f, true, body);
    prism(&c->spire, x, z, 44.0f, 16.0f, g + 70.0f, g + 560.0f, 8, PI_F / 8.0f, false, body);
    prism(&c->spire_trim, x, z, 16.0f, 3.0f, g + 560.0f, g + 640.0f, 8, PI_F / 8.0f, false, dark);
    tbox(&c->spire_trim, x, z, 0.0f, 0.8f, 0.8f, g + 640.0f, g + CITY_SPIRE_TOP, RGB(170, 172, 180));
    tbox(&c->spire_trim, x, z, 0.0f, 2.0f, 2.0f, g + CITY_SPIRE_TOP, g + CITY_SPIRE_TOP + 4.0f, RGBA(255, 40, 40, 0));
    mesh_blob(&c->spire_trim, v3(x, g + 600.0f, z), 26.0f, 26.0f, 26.0f, 10, RGBA(200, 250, 255, 0));
    // glowing rings around the shaft, alternating cyan and magenta
    const float hr[6] = { 120.0f, 210.0f, 300.0f, 390.0f, 470.0f, 540.0f };
    for (int k = 0; k < 6; k++) {
        float t = (hr[k] - 70.0f) / 490.0f;
        float r = lerpf(44.0f, 16.0f, t) + 9.0f + 4.0f * (float)(k & 1);
        ring(&c->spire_trim, x, z, r, g + hr[k], 2.0f, 1.2f, 24, NEON[k & 1]);
    }
    // vertical light strips on the base
    for (int k = 0; k < 8; k++) {
        float a = ((float)k + 0.5f) / 8.0f * 2.0f * PI_F;
        tbox(&c->spire_trim, x + cosf(a) * 66.0f, z + sinf(a) * 66.0f, a, 0.6f, 1.2f, g, g + 60.0f, NEON[0]);
    }
}

// ---------------------------------------------------------------------------
//  Flying cars and the wave-front curtain
// ---------------------------------------------------------------------------
static void build_pod(mesh_t* m, uint32_t body, uint32_t glow) {
    // wedge body: wide flat tail, narrow low nose (local +z forward)
    vec3 cn[8] = {
        v3(-1.1f, 0.0f, -2.4f), v3(1.1f, 0.0f, -2.4f), v3(0.5f, 0.1f, 2.6f), v3(-0.5f, 0.1f, 2.6f),
        v3(-1.0f, 0.8f, -2.2f), v3(1.0f, 0.8f, -2.2f), v3(0.4f, 0.4f, 2.4f), v3(-0.4f, 0.4f, 2.4f),
    };
    mesh_hexa(m, cn, body);
    mesh_box(m, v3(-0.6f, 0.8f, -1.2f), v3(0.6f, 1.25f, 0.6f), RGB(30, 40, 60));       // canopy
    mesh_box(m, v3(-1.0f, -0.12f, -2.0f), v3(1.0f, 0.0f, 2.0f), glow);                // underglow
    mesh_box(m, v3(-0.9f, 0.3f, -2.46f), v3(0.9f, 0.6f, -2.38f), RGBA(255, 40, 60, 0)); // tail light
}

static void build_pods(city_meshes_t* c) {
    c->pod_first[0] = c->pods.count;
    build_pod(&c->pods, RGB(230, 232, 240), NEON[0]);
    c->pod_count[0] = c->pods.count - c->pod_first[0];
    c->pod_first[1] = c->pods.count;
    build_pod(&c->pods, RGB(52, 54, 68), NEON[1]);
    c->pod_count[1] = c->pods.count - c->pod_first[1];

    // sky lanes above the skyway; right of the centre line flies with the player
    uint32_t rng = 0x5EEDu;
    const float L = track_length();
    const float lats[4] = { 3.5f, 7.0f, 11.0f, 15.0f };
    const float hts[5] = { 14.0f, 20.0f, 28.0f, 46.0f, 75.0f };
    g_num_pods = 0;
    while (g_num_pods < 56) {
        city_pod_t* p = &g_pods[g_num_pods++];
        float side = (rng_float(&rng) < 0.5f) ? 1.0f : -1.0f;
        p->lat = side * lats[rng_next(&rng) % 4];
        p->height = hts[rng_next(&rng) % 5] + rng_range(&rng, -2.0f, 2.0f);
        p->speed = side * rng_range(&rng, 42.0f, 85.0f);
        p->s0 = rng_range(&rng, 0.0f, L);
        p->phase = rng_range(&rng, 0.0f, 6.28f);
        p->variant = (int)(rng_next(&rng) & 1);
    }
}

static void build_curtain(mesh_t* m) {
    const int seg = 128;
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / (float)seg * 2.0f * PI_F, a1 = (float)(i + 1) / (float)seg * 2.0f * PI_F;
        vec3 b0 = v3(cosf(a0), 0.0f, sinf(a0)), b1 = v3(cosf(a1), 0.0f, sinf(a1));
        vec3 m0 = v3(b0.x, 0.12f, b0.z), m1 = v3(b1.x, 0.12f, b1.z);
        vec3 t0 = v3(b0.x, 1.0f, b0.z), t1 = v3(b1.x, 1.0f, b1.z);
        const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        // two bands: a bright foot that fades upwards into a faint veil
        mesh_vertex(m, b0, b0, 0, 0, RGBA(120, 240, 255, 120));
        mesh_vertex(m, b1, b1, 1, 0, RGBA(120, 240, 255, 120));
        mesh_vertex(m, m1, b1, 1, 1, RGBA(90, 220, 255, 40));
        mesh_vertex(m, b0, b0, 0, 0, RGBA(120, 240, 255, 120));
        mesh_vertex(m, m1, b1, 1, 1, RGBA(90, 220, 255, 40));
        mesh_vertex(m, m0, b0, 0, 1, RGBA(90, 220, 255, 40));
        mesh_quad_uv(m, m0, m1, t1, t0, uv, b0, RGBA(90, 200, 255, 0));
        // fix the alpha of the lower edge of the veil (mesh_quad_uv uses one colour)
        vertex_t* v = &m->v[m->count - 6];
        v[0].color = v[1].color = v[3].color = RGBA(90, 220, 255, 40);
    }
}

// ---------------------------------------------------------------------------
void city_build(city_meshes_t* c) {
    memset(c, 0, sizeof(*c));
    float minx, maxx, minz, maxz, miny;
    track_bounds(&minx, &maxx, &minz, &maxz, &miny);
    g_ground = miny - 12.0f;
    // the spire stands in the middle of the loop
    g_spire = v3(0.0f, g_ground, 0.0f);
    {
        const float L = track_length();
        vec3 sum = v3(0, 0, 0);
        for (int i = 0; i < 256; i++) sum = v3_add(sum, track_frame((float)i * L / 256.0f).pos);
        g_spire = v3(sum.x / 256.0f, g_ground, sum.z / 256.0f);
    }
    build_ground(&c->ground, minx, maxx, minz, maxz);
    mesh_uv_scale = 0.5f;
    build_skyway(c);
    mesh_uv_scale = 1.0f / 32.0f;     // facade texture: 32 m per repeat
    build_towers(c, minx, maxx, minz, maxz);
    build_spire(c);
    mesh_uv_scale = 0.5f;
    build_pods(c);
    build_curtain(&c->curtain);
    printf("[highway] future city geometry: ground %d, road %d, deck %d, towers %d, trim %d, spire %d\n",
           c->ground.count, c->road.count, c->deck.count, c->towers.count, c->trim.count,
           c->spire.count + c->spire_trim.count);
}

void city_free(city_meshes_t* c) {
    mesh_free(&c->ground);
    mesh_free(&c->road);
    mesh_free(&c->deck);
    mesh_free(&c->deck_decals);
    mesh_free(&c->towers);
    mesh_free(&c->trim);
    mesh_free(&c->decals);
    mesh_free(&c->spire);
    mesh_free(&c->spire_trim);
    mesh_free(&c->pods);
    mesh_free(&c->curtain);
}
