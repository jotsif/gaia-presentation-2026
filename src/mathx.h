// Tiny vector / matrix helpers (column-major matrices, right-handed, y-up).
#pragma once
#include <math.h>
#include <stdint.h>

#define PI_F 3.14159265358979f

typedef struct { float x, y, z; } vec3;
typedef struct { float m[16]; } mat4;   // m[col*4 + row]

static inline vec3 v3(float x, float y, float z) { vec3 r = { x, y, z }; return r; }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline vec3 v3_mad(vec3 a, vec3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_norm(vec3 a) {
    float l = v3_len(a);
    return (l > 1e-8f) ? v3_scale(a, 1.0f / l) : v3(0.0f, 0.0f, 0.0f);
}
static inline vec3 v3_lerp(vec3 a, vec3 b, float t) { return v3_mad(a, v3_sub(b, a), t); }

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// wrap x into [0, len)
static inline float wrapf(float x, float len) {
    x = fmodf(x, len);
    return (x < 0.0f) ? x + len : x;
}

static inline mat4 m4_identity(void) {
    mat4 r = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
    return r;
}

static inline mat4 m4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

static inline mat4 m4_translate(vec3 p) {
    mat4 r = m4_identity();
    r.m[12] = p.x; r.m[13] = p.y; r.m[14] = p.z;
    return r;
}

// matrix with the given basis vectors as columns and p as translation
static inline mat4 m4_basis(vec3 x, vec3 y, vec3 z, vec3 p) {
    mat4 r = { { x.x, x.y, x.z, 0,  y.x, y.y, y.z, 0,  z.x, z.y, z.z, 0,  p.x, p.y, p.z, 1 } };
    return r;
}

// Reversed-Z, infinite far plane perspective projection (clip z in [0,1], near -> 1, infinity -> 0).
static inline mat4 m4_perspective_revz(float fovy, float aspect, float znear) {
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4 r = { { 0 } };
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[11] = -1.0f;
    r.m[14] = znear;
    return r;
}

static inline mat4 m4_lookat(vec3 eye, vec3 center, vec3 up) {
    vec3 f = v3_norm(v3_sub(center, eye));
    vec3 s = v3_norm(v3_cross(f, up));
    vec3 u = v3_cross(s, f);
    mat4 r = m4_identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3_dot(s, eye);
    r.m[13] = -v3_dot(u, eye);
    r.m[14] = v3_dot(f, eye);
    return r;
}

// small deterministic RNG
static inline uint32_t rng_next(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}
static inline float rng_float(uint32_t* state) {   // [0,1)
    return (float)(rng_next(state) >> 8) * (1.0f / 16777216.0f);
}
static inline float rng_range(uint32_t* state, float lo, float hi) {
    return lo + (hi - lo) * rng_float(state);
}
