// Hash-based value noise with optional periodic wrapping (for tileable textures).
#pragma once
#include <math.h>
#include <stdint.h>

static inline uint32_t nz_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline float nz_hash2(int x, int y, uint32_t seed) {
    uint32_t h = nz_hash((uint32_t)x * 0x8da6b343u ^ nz_hash((uint32_t)y * 0xd8163841u ^ seed));
    return (float)(h & 0xffffff) * (1.0f / 16777215.0f);
}

static inline int nz_wrap(int i, int period) {
    if (period <= 0) {
        return i;
    }
    i %= period;
    return i < 0 ? i + period : i;
}

// value noise in [0,1]; px/py > 0 makes it periodic with that integer period
static inline float nz_value(float x, float y, int px, int py, uint32_t seed) {
    float fx0 = floorf(x), fy0 = floorf(y);
    int ix = (int)fx0, iy = (int)fy0;
    float fx = x - fx0, fy = y - fy0;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    int x0 = nz_wrap(ix, px), x1 = nz_wrap(ix + 1, px);
    int y0 = nz_wrap(iy, py), y1 = nz_wrap(iy + 1, py);
    float a = nz_hash2(x0, y0, seed), b = nz_hash2(x1, y0, seed);
    float c = nz_hash2(x0, y1, seed), d = nz_hash2(x1, y1, seed);
    float ab = a + (b - a) * fx;
    float cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

// fractal sum of value noise, normalized to [0,1]
static inline float nz_fbm(float x, float y, int octaves, int px, int py, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; o++) {
        sum += amp * nz_value(x, y, px, py, seed + (uint32_t)o * 101u);
        norm += amp;
        x *= 2.0f; y *= 2.0f;
        px *= 2; py *= 2;
        amp *= 0.5f;
    }
    return sum / norm;
}
