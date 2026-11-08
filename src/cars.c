#include "cars.h"
#include "world.h"
#include <string.h>

// vertex colours: alpha == 0 marks body-paint vertices that get tinted per car
#define PAINT   RGBA(255, 255, 255, 0)
#define GLASS   RGB(28, 38, 58)
#define TYRE    RGB(22, 22, 24)
#define TRIM    RGB(64, 64, 70)
#define HEAD    RGB(255, 250, 215)
#define TAIL    RGB(215, 18, 18)
#define CHASSIS RGB(40, 40, 44)

// hexahedron from a bottom rectangle and a (possibly smaller / shifted) top rectangle
static void hexa_tb(mesh_t* m,
                    float bx0, float bx1, float bz0, float bz1, float by,
                    float tx0, float tx1, float tz0, float tz1, float ty, uint32_t col) {
    vec3 c[8] = {
        v3(bx0, by, bz0), v3(bx1, by, bz0), v3(bx1, by, bz1), v3(bx0, by, bz1),
        v3(tx0, ty, tz0), v3(tx1, ty, tz0), v3(tx1, ty, tz1), v3(tx0, ty, tz1),
    };
    mesh_hexa(m, c, col);
}

static void box(mesh_t* m, float x0, float y0, float z0, float x1, float y1, float z1, uint32_t col) {
    mesh_box(m, v3(x0, y0, z0), v3(x1, y1, z1), col);
}

static void wheels(mesh_t* m, float x, float r, float hw, const float* zs, int nz) {
    for (int i = 0; i < nz; i++) {
        mesh_cyl_x(m, v3(x, r, zs[i]), r, hw, 8, TYRE);
        mesh_cyl_x(m, v3(-x, r, zs[i]), r, hw, 8, TYRE);
    }
}

static void build_sedan(mesh_t* m) {
    hexa_tb(m, -0.90f, 0.90f, -2.30f, 2.30f, 0.28f, -0.88f, 0.88f, -2.22f, 2.18f, 0.88f, PAINT);
    hexa_tb(m, -0.84f, 0.84f, -1.45f, 0.85f, 0.88f, -0.72f, 0.72f, -1.05f, 0.20f, 1.36f, GLASS);
    hexa_tb(m, -0.72f, 0.72f, -1.05f, 0.20f, 1.36f, -0.70f, 0.70f, -1.00f, 0.15f, 1.41f, PAINT);
    box(m, -0.92f, 0.22f, 2.18f, 0.92f, 0.44f, 2.42f, TRIM);
    box(m, -0.92f, 0.22f, -2.42f, 0.92f, 0.44f, -2.18f, TRIM);
    box(m, 0.48f, 0.58f, 2.20f, 0.82f, 0.72f, 2.33f, HEAD);
    box(m, -0.82f, 0.58f, 2.20f, -0.48f, 0.72f, 2.33f, HEAD);
    box(m, 0.52f, 0.60f, -2.33f, 0.86f, 0.75f, -2.24f, TAIL);
    box(m, -0.86f, 0.60f, -2.33f, -0.52f, 0.75f, -2.24f, TAIL);
    const float zs[2] = { -1.45f, 1.45f };
    wheels(m, 0.80f, 0.33f, 0.13f, zs, 2);
}

static void build_sport(mesh_t* m) {
    hexa_tb(m, -0.98f, 0.98f, -2.25f, 2.25f, 0.22f, -0.92f, 0.92f, -2.10f, 2.00f, 0.72f, PAINT);
    hexa_tb(m, -0.85f, 0.85f, -1.10f, 0.80f, 0.72f, -0.68f, 0.68f, -0.75f, -0.05f, 1.12f, GLASS);
    hexa_tb(m, -0.68f, 0.68f, -0.75f, -0.05f, 1.12f, -0.66f, 0.66f, -0.72f, -0.10f, 1.16f, PAINT);
    // side intakes
    box(m, 0.93f, 0.38f, -1.25f, 0.99f, 0.58f, -0.45f, TRIM);
    box(m, -0.99f, 0.38f, -1.25f, -0.93f, 0.58f, -0.45f, TRIM);
    // rear wing
    box(m, 0.55f, 0.70f, -2.05f, 0.65f, 0.98f, -1.95f, TRIM);
    box(m, -0.65f, 0.70f, -2.05f, -0.55f, 0.98f, -1.95f, TRIM);
    box(m, -0.98f, 0.96f, -2.20f, 0.98f, 1.02f, -1.80f, PAINT);
    // lights / lip
    box(m, 0.42f, 0.52f, 2.06f, 0.84f, 0.60f, 2.20f, HEAD);
    box(m, -0.84f, 0.52f, 2.06f, -0.42f, 0.60f, 2.20f, HEAD);
    box(m, -0.90f, 0.48f, -2.27f, 0.90f, 0.64f, -2.18f, CHASSIS);
    box(m, 0.42f, 0.51f, -2.29f, 0.86f, 0.61f, -2.20f, TAIL);
    box(m, -0.86f, 0.51f, -2.29f, -0.42f, 0.61f, -2.20f, TAIL);
    box(m, -0.98f, 0.16f, 2.15f, 0.98f, 0.28f, 2.34f, TRIM);
    box(m, -0.98f, 0.16f, -2.32f, 0.98f, 0.30f, -2.15f, TRIM);
    const float zs[2] = { -1.40f, 1.40f };
    wheels(m, 0.84f, 0.34f, 0.16f, zs, 2);
}

static void build_van(mesh_t* m) {
    hexa_tb(m, -0.97f, 0.97f, -2.40f, 2.40f, 0.32f, -0.97f, 0.97f, -2.40f, 2.30f, 1.05f, PAINT);
    hexa_tb(m, -0.97f, 0.97f, -2.40f, 1.20f, 1.05f, -0.93f, 0.93f, -2.35f, 0.70f, 1.95f, PAINT);
    hexa_tb(m, -0.98f, 0.98f, -2.41f, 1.13f, 1.22f, -0.952f, 0.952f, -2.375f, 0.85f, 1.72f, GLASS);
    box(m, -0.99f, 0.25f, 2.28f, 0.99f, 0.50f, 2.50f, TRIM);
    box(m, -0.99f, 0.25f, -2.50f, 0.99f, 0.50f, -2.28f, TRIM);
    box(m, 0.50f, 0.70f, 2.28f, 0.88f, 0.86f, 2.33f, HEAD);
    box(m, -0.88f, 0.70f, 2.28f, -0.50f, 0.86f, 2.33f, HEAD);
    box(m, 0.62f, 0.75f, -2.45f, 0.94f, 1.00f, -2.38f, TAIL);
    box(m, -0.94f, 0.75f, -2.45f, -0.62f, 1.00f, -2.38f, TAIL);
    const float zs[2] = { -1.55f, 1.55f };
    wheels(m, 0.84f, 0.36f, 0.14f, zs, 2);
}

static void build_truck(mesh_t* m) {
    // tractor
    hexa_tb(m, -1.20f, 1.20f, 3.60f, 6.00f, 0.50f, -1.20f, 1.20f, 3.60f, 6.00f, 1.60f, PAINT);
    hexa_tb(m, -1.20f, 1.20f, 3.60f, 6.00f, 1.60f, -1.15f, 1.15f, 3.60f, 5.70f, 3.00f, PAINT);
    hexa_tb(m, -1.21f, 1.21f, 4.60f, 5.955f, 1.90f, -1.175f, 1.175f, 4.60f, 5.785f, 2.70f, GLASS);
    hexa_tb(m, -1.15f, 1.15f, 3.60f, 5.00f, 3.00f, -1.10f, 1.10f, 3.60f, 4.40f, 3.70f, PAINT);
    box(m, -1.20f, 0.35f, 5.95f, 1.20f, 0.65f, 6.15f, TRIM);
    box(m, 0.72f, 0.75f, 5.98f, 1.10f, 0.95f, 6.06f, HEAD);
    box(m, -1.10f, 0.75f, 5.98f, -0.72f, 0.95f, 6.06f, HEAD);
    // chassis + trailer
    box(m, -0.90f, 0.45f, -6.00f, 0.90f, 1.00f, 3.60f, CHASSIS);
    box(m, -1.25f, 1.10f, -6.10f, 1.25f, 4.00f, 3.40f, RGB(232, 232, 226));
    box(m, -1.26f, 2.20f, -6.00f, 1.26f, 2.60f, 3.30f, PAINT);
    box(m, 0.80f, 1.15f, -6.16f, 1.15f, 1.35f, -6.08f, TAIL);
    box(m, -1.15f, 1.15f, -6.16f, -0.80f, 1.35f, -6.08f, TAIL);
    const float zs[4] = { -4.60f, -3.50f, 2.60f, 5.00f };
    wheels(m, 1.00f, 0.50f, 0.20f, zs, 4);
}

// van with a roof rack and two masts carrying the (separately drawn) presentation sign
static void build_sign_van(mesh_t* m) {
    build_van(m);
    const uint32_t mast = RGB(60, 62, 70);
    box(m, -0.95f, 1.95f, -1.9f, 0.95f, 2.02f, 0.5f, mast);
    box(m, -0.14f, 1.95f, -0.14f, 0.14f, SIGN_BOTTOM - 0.05f, 0.14f, mast);              // turntable mast
    box(m, -0.40f, SIGN_BOTTOM - 0.18f, -0.40f, 0.40f, SIGN_BOTTOM - 0.02f, 0.40f, RGB(90, 92, 100));
    box(m, -0.30f, 2.02f, 1.0f, 0.30f, 2.12f, 1.25f, RGB(255, 150, 20));   // beacon
}

void cars_build_models(mesh_t* m, car_model_t models[CAR_NUM_TYPES]) {
    void (*builders[CAR_NUM_TYPES])(mesh_t*) = { build_sedan, build_sport, build_van, build_truck, build_sign_van };
    const float dims[CAR_NUM_TYPES][3] = {
        { 4.84f, 1.86f, 1.41f },
        { 4.66f, 1.98f, 1.16f },
        { 5.00f, 1.98f, 1.95f },
        { 12.3f, 2.52f, 4.00f },
        { 5.00f, 1.98f, 1.95f },
    };
    for (int i = 0; i < CAR_NUM_TYPES; i++) {
        models[i].first = m->count;
        builders[i](m);
        models[i].count = m->count - models[i].first;
        models[i].length = dims[i][0];
        models[i].width = dims[i][1];
        models[i].height = dims[i][2];
    }
}

void cars_build_shadow(mesh_t* m) {
    const int seg = 16;
    const vec3 n = v3(0, 1, 0);
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / (float)seg * 2.0f * PI_F;
        float a1 = (float)(i + 1) / (float)seg * 2.0f * PI_F;
        mesh_vertex(m, v3(0, 0, 0), n, 0.5f, 0.5f, RGBA(0, 0, 0, 150));
        mesh_vertex(m, v3(cosf(a0) * 0.5f, 0, sinf(a0) * 0.5f), n, 0.5f, 0.5f, RGBA(0, 0, 0, 0));
        mesh_vertex(m, v3(cosf(a1) * 0.5f, 0, sinf(a1) * 0.5f), n, 0.5f, 0.5f, RGBA(0, 0, 0, 0));
    }
}

// ---------------------------------------------------------------------------
//  Traffic
// ---------------------------------------------------------------------------
static const float LANE_LAT[2] = { LANE_INNER_LAT, LANE_OUTER_LAT };

static const vec3 PALETTE[] = {
    { 0.85f, 0.10f, 0.10f }, { 0.95f, 0.80f, 0.10f }, { 0.12f, 0.30f, 0.85f },
    { 0.92f, 0.92f, 0.92f }, { 0.12f, 0.12f, 0.14f }, { 0.10f, 0.55f, 0.25f },
    { 0.65f, 0.67f, 0.72f }, { 0.95f, 0.45f, 0.08f }, { 0.50f, 0.15f, 0.60f },
    { 0.10f, 0.65f, 0.75f },
};

static float car_len(const traffic_t* t, const car_t* c) {
    return t->models[c->type].length;
}

typedef struct { float gap; float v; int idx; } neighbour_t;

static neighbour_t find_ahead(const traffic_t* t, int me, float lane_lat, float tol) {
    const car_t* c = &t->cars[me];
    neighbour_t r = { 1e9f, 0.0f, -1 };
    for (int i = 0; i < t->num; i++) {
        const car_t* o = &t->cars[i];
        if (i == me || !o->active || o->scripted || o->dir != c->dir || fabsf(o->lat - lane_lat) > tol) {
            continue;
        }
        float d = wrapf((o->s - c->s) * (float)c->dir, t->L);
        if (d > t->L * 0.5f) {
            continue;
        }
        float gap = d - 0.5f * (car_len(t, c) + car_len(t, o));
        if (gap < r.gap) {
            r.gap = gap; r.v = o->speed; r.idx = i;
        }
    }
    return r;
}

static neighbour_t find_behind(const traffic_t* t, int me, float lane_lat, float tol) {
    const car_t* c = &t->cars[me];
    neighbour_t r = { 1e9f, 0.0f, -1 };
    for (int i = 0; i < t->num; i++) {
        const car_t* o = &t->cars[i];
        if (i == me || !o->active || o->scripted || o->dir != c->dir || fabsf(o->lat - lane_lat) > tol) {
            continue;
        }
        float d = wrapf((c->s - o->s) * (float)c->dir, t->L);
        if (d > t->L * 0.5f) {
            continue;
        }
        float gap = d - 0.5f * (car_len(t, c) + car_len(t, o));
        if (gap < r.gap) {
            r.gap = gap; r.v = o->speed; r.idx = i;
        }
    }
    return r;
}

// Intelligent Driver Model acceleration
static float idm(float v, float v0, float gap, float dv, float a, float b) {
    const float s0 = 5.0f, T = 1.1f;
    float free_term = 1.0f - powf(v / fmaxf(v0, 0.1f), 4.0f);
    if (gap >= 1e8f) {
        return a * free_term;
    }
    float sstar = s0 + fmaxf(0.0f, v * T + v * dv / (2.0f * sqrtf(a * b)));
    float g = fmaxf(gap, 0.1f);
    return a * (free_term - (sstar / g) * (sstar / g));
}

static bool lane_safe(const traffic_t* t, int me, int lane, float cur_gap, float cur_lead_v) {
    const car_t* c = &t->cars[me];
    neighbour_t a = find_ahead(t, me, LANE_LAT[lane], 1.9f);
    neighbour_t b = find_behind(t, me, LANE_LAT[lane], 1.9f);
    bool ahead_ok = a.gap > fmaxf(20.0f, c->speed * 0.8f) &&
                    (a.idx < 0 || a.gap > cur_gap + 5.0f || a.v > cur_lead_v + 2.0f);
    bool behind_ok = b.gap > 8.0f + fmaxf(0.0f, b.v - c->speed) * 2.5f;
    return ahead_ok && behind_ok;
}

bool traffic_request_lane(traffic_t* t, int car, int lane) {
    car_t* c = &t->cars[car];
    if (lane < 0 || lane > 1 || lane == c->lane) {
        return false;
    }
    if (lane_safe(t, car, lane, -1e9f, -1e9f)) {
        c->lane = lane;
        return true;
    }
    return false;
}

int traffic_add_scripted(traffic_t* t, int type) {
    if (t->num >= MAX_CARS) {
        return -1;
    }
    int idx = t->num++;
    car_t* c = &t->cars[idx];
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->dir = 1;
    c->scripted = true;
    c->active = false;
    c->tint = v3(1, 1, 1);
    return idx;
}

void traffic_init(traffic_t* t, const car_model_t* models, bool presentation) {
    memset(t, 0, sizeof(*t));
    t->L = track_length();
    t->models = models;
    t->autopilot = true;
    t->rng = 1337u;

    car_t* h = &t->cars[t->num++];
    h->type = CAR_SPORT;
    h->tint = v3(0.90f, 0.06f, 0.06f);
    h->s = 10.0f; h->dir = 1; h->lane = 0; h->lat = LANE_LAT[0];
    h->speed = 30.0f; h->v0 = 62.0f; h->amax = 5.0f; h->hero = true; h->active = true;
    if (presentation) {
        // calm cruise in the slow lane, the fast lane is kept free for the slide cars
        h->lane = 1; h->lat = LANE_LAT[1];
        h->speed = 33.0f; h->v0 = 33.0f;
        t->autopilot = false;
    }

    const int per_dir = 36;
    const int npal = (int)(sizeof(PALETTE) / sizeof(PALETTE[0]));
    for (int dir = 1; dir >= -1; dir -= 2) {
        for (int k = 0; k < per_dir; k++) {
            if ((dir == 1 && k == 0) || (dir == 1 && presentation)) {
                continue;   // keep the hero's start area (or its whole carriageway) clear
            }
            car_t* c = &t->cars[t->num++];
            c->active = true;
            float r = rng_float(&t->rng);
            c->type = (r < 0.52f) ? CAR_SEDAN : (r < 0.66f) ? CAR_SPORT : (r < 0.83f) ? CAR_VAN : CAR_TRUCK;
            c->tint = PALETTE[rng_next(&t->rng) % (uint32_t)npal];
            c->dir = dir;
            c->s = wrapf(((float)k + rng_range(&t->rng, 0.1f, 0.6f)) * t->L / (float)per_dir, t->L);
            c->lane = (c->type == CAR_TRUCK) ? 1 : (rng_float(&t->rng) < 0.6f ? 1 : 0);
            c->lat = LANE_LAT[c->lane];
            switch (c->type) {
                case CAR_TRUCK: c->v0 = rng_range(&t->rng, 22.0f, 25.0f); c->amax = 0.9f; break;
                case CAR_VAN:   c->v0 = rng_range(&t->rng, 27.0f, 32.0f); c->amax = 1.6f; break;
                case CAR_SPORT: c->v0 = rng_range(&t->rng, 34.0f, 41.0f); c->amax = 3.0f; break;
                default:        c->v0 = rng_range(&t->rng, 29.0f, 35.0f); c->amax = 2.0f; break;
            }
            c->speed = c->v0 * 0.95f;
            c->lc_timer = rng_range(&t->rng, 0.0f, 3.0f);
        }
    }
}

static void decide_lane(traffic_t* t, int me) {
    car_t* c = &t->cars[me];
    if (c->type == CAR_TRUCK) {
        return;
    }
    neighbour_t cur = find_ahead(t, me, LANE_LAT[c->lane], 1.9f);
    float look = c->hero ? 100.0f : 60.0f;
    bool blocked = cur.idx >= 0 && cur.gap < look && cur.v < c->v0 - 1.5f;
    int other = 1 - c->lane;
    if (blocked) {
        if (lane_safe(t, me, other, cur.gap, cur.v)) {
            c->lane = other;
            c->lc_timer = c->hero ? 1.5f : rng_range(&t->rng, 4.0f, 8.0f);
        }
        return;
    }
    // drift back: normal traffic keeps right, the hero prefers the fast lane
    int preferred = c->hero ? 0 : 1;
    if (c->lane != preferred && rng_float(&t->rng) < (c->hero ? 0.5f : 0.3f)) {
        neighbour_t p = find_ahead(t, me, LANE_LAT[preferred], 1.9f);
        if ((p.idx < 0 || p.gap > 140.0f || p.v >= c->v0 - 1.0f) && lane_safe(t, me, preferred, 0.0f, 0.0f)) {
            c->lane = preferred;
            c->lc_timer = rng_range(&t->rng, 3.0f, 6.0f);
        }
    }
}

void traffic_update(traffic_t* t, float dt) {
    for (int i = 0; i < t->num; i++) {
        car_t* c = &t->cars[i];
        if (!c->active || c->scripted) {
            continue;
        }
        float target_lat = LANE_LAT[c->lane];

        // longitudinal: follow the closest car in the physical or the target lane
        neighbour_t a1 = find_ahead(t, i, c->lat, 1.9f);
        neighbour_t a2 = find_ahead(t, i, target_lat, 1.9f);
        neighbour_t lead = (a1.gap < a2.gap) ? a1 : a2;
        float acc = idm(c->speed, c->v0, lead.gap, c->speed - lead.v, c->amax, 4.0f);
        acc = clampf(acc, -14.0f, c->amax);
        c->speed = fmaxf(0.0f, c->speed + acc * dt);
        c->s = wrapf(c->s + (float)c->dir * c->speed * dt, t->L);
        c->odo += (double)(c->speed * dt);

        // lateral: critically damped spring towards the lane centre
        float k = c->hero ? 6.0f : 2.5f;
        float damp = 2.0f * sqrtf(k);
        c->lat_vel += (k * (target_lat - c->lat) - damp * c->lat_vel) * dt;
        c->lat += c->lat_vel * dt;

        c->lc_timer -= dt;
        if (c->lc_timer <= 0.0f && fabsf(c->lat - target_lat) < 0.3f && (!c->hero || t->autopilot)) {
            c->lc_timer = c->hero ? 0.3f : rng_range(&t->rng, 0.5f, 1.5f);
            decide_lane(t, i);
        }
    }
}

car_pose_t car_pose(const car_t* c) {
    track_frame_t f = track_frame(c->s);
    const vec3 up = v3(0, 1, 0);
    car_pose_t p;
    p.pos = v3_mad(f.pos, f.right, (float)c->dir * c->lat);
    vec3 fwd = v3_scale(f.tan, (float)c->dir * fmaxf(c->speed, 4.0f));
    fwd = v3_mad(fwd, f.right, (float)c->dir * c->lat_vel);
    p.fwd = v3_norm(fwd);
    p.right = v3_norm(v3_cross(p.fwd, up));
    p.up = v3_cross(p.right, p.fwd);
    return p;
}

mat4 car_model_matrix(const car_pose_t* p, float sx, float sy, float sz, float y_off) {
    // local +x = left (up x fwd), +y = up, +z = forward
    vec3 x = v3_scale(v3_cross(p->up, p->fwd), sx);
    vec3 y = v3_scale(p->up, sy);
    vec3 z = v3_scale(p->fwd, sz);
    return m4_basis(x, y, z, v3_mad(p->pos, p->up, y_off));
}
