#include "mesh.h"
#include <stdlib.h>
#include <string.h>

float mesh_uv_scale = 0.5f;

void mesh_free(mesh_t* m) {
    free(m->v);
    free(m->idx);
    memset(m, 0, sizeof(*m));
}

void mesh_vertex(mesh_t* m, vec3 p, vec3 n, float u, float v, uint32_t col) {
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 1024;
        m->v = (vertex_t*)realloc(m->v, (size_t)m->cap * sizeof(vertex_t));
    }
    vertex_t* vt = &m->v[m->count++];
    vt->pos[0] = p.x; vt->pos[1] = p.y; vt->pos[2] = p.z;
    vt->nrm[0] = n.x; vt->nrm[1] = n.y; vt->nrm[2] = n.z;
    vt->uv[0] = u; vt->uv[1] = v;
    vt->color = col;
}

void mesh_index(mesh_t* m, uint32_t i) {
    if (m->icount == m->icap) {
        m->icap = m->icap ? m->icap * 2 : 4096;
        m->idx = (uint32_t*)realloc(m->idx, (size_t)m->icap * sizeof(uint32_t));
    }
    m->idx[m->icount++] = i;
}

static void planar_uv(vec3 p, vec3 n, float* u, float* v) {
    float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
    if (ay >= ax && ay >= az) { *u = p.x; *v = p.z; }
    else if (ax >= az)        { *u = p.z; *v = -p.y; }
    else                      { *u = p.x; *v = -p.y; }
    *u *= mesh_uv_scale;
    *v *= mesh_uv_scale;
}

void mesh_tri(mesh_t* m, vec3 a, vec3 b, vec3 c, vec3 hint, uint32_t col) {
    vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    if (v3_dot(n, hint) < 0.0f) {
        n = v3_scale(n, -1.0f);
    }
    float u, v;
    planar_uv(a, n, &u, &v); mesh_vertex(m, a, n, u, v, col);
    planar_uv(b, n, &u, &v); mesh_vertex(m, b, n, u, v, col);
    planar_uv(c, n, &u, &v); mesh_vertex(m, c, n, u, v, col);
}

void mesh_quad(mesh_t* m, vec3 a, vec3 b, vec3 c, vec3 d, vec3 hint, uint32_t col) {
    mesh_tri(m, a, b, c, hint, col);
    mesh_tri(m, a, c, d, hint, col);
}

void mesh_quad_uv(mesh_t* m, vec3 a, vec3 b, vec3 c, vec3 d, const float uv[4][2], vec3 n, uint32_t col) {
    mesh_vertex(m, a, n, uv[0][0], uv[0][1], col);
    mesh_vertex(m, b, n, uv[1][0], uv[1][1], col);
    mesh_vertex(m, c, n, uv[2][0], uv[2][1], col);
    mesh_vertex(m, a, n, uv[0][0], uv[0][1], col);
    mesh_vertex(m, c, n, uv[2][0], uv[2][1], col);
    mesh_vertex(m, d, n, uv[3][0], uv[3][1], col);
}

static vec3 centroid4(vec3 a, vec3 b, vec3 c, vec3 d) {
    return v3_scale(v3_add(v3_add(a, b), v3_add(c, d)), 0.25f);
}

void mesh_hexa(mesh_t* m, const vec3 c[8], uint32_t col) {
    static const int faces[6][4] = {
        { 0, 1, 2, 3 },   // bottom
        { 4, 5, 6, 7 },   // top
        { 3, 2, 6, 7 },   // +z
        { 0, 1, 5, 4 },   // -z
        { 1, 2, 6, 5 },   // +x
        { 0, 3, 7, 4 },   // -x
    };
    vec3 center = v3(0, 0, 0);
    for (int i = 0; i < 8; i++) {
        center = v3_add(center, c[i]);
    }
    center = v3_scale(center, 1.0f / 8.0f);
    for (int f = 0; f < 6; f++) {
        vec3 a = c[faces[f][0]], b = c[faces[f][1]], cc = c[faces[f][2]], d = c[faces[f][3]];
        vec3 hint = v3_sub(centroid4(a, b, cc, d), center);
        mesh_quad(m, a, b, cc, d, hint, col);
    }
}

void mesh_box(mesh_t* m, vec3 mn, vec3 mx, uint32_t col) {
    vec3 c[8] = {
        v3(mn.x, mn.y, mn.z), v3(mx.x, mn.y, mn.z), v3(mx.x, mn.y, mx.z), v3(mn.x, mn.y, mx.z),
        v3(mn.x, mx.y, mn.z), v3(mx.x, mx.y, mn.z), v3(mx.x, mx.y, mx.z), v3(mn.x, mx.y, mx.z),
    };
    mesh_hexa(m, c, col);
}

void mesh_cyl_x(mesh_t* m, vec3 center, float radius, float half_width, int sides, uint32_t col) {
    vec3 l = v3(center.x - half_width, center.y, center.z);
    vec3 r = v3(center.x + half_width, center.y, center.z);
    for (int i = 0; i < sides; i++) {
        float a0 = (float)i / (float)sides * 2.0f * PI_F + PI_F / (float)sides;
        float a1 = (float)(i + 1) / (float)sides * 2.0f * PI_F + PI_F / (float)sides;
        vec3 o0 = v3(0, cosf(a0) * radius, sinf(a0) * radius);
        vec3 o1 = v3(0, cosf(a1) * radius, sinf(a1) * radius);
        vec3 out = v3_add(o0, o1);
        mesh_quad(m, v3_add(l, o0), v3_add(r, o0), v3_add(r, o1), v3_add(l, o1), out, col);
        mesh_tri(m, l, v3_add(l, o0), v3_add(l, o1), v3(-1, 0, 0), col);
        mesh_tri(m, r, v3_add(r, o0), v3_add(r, o1), v3(1, 0, 0), col);
    }
}

void mesh_cone(mesh_t* m, vec3 base, float radius, float height, int sides, uint32_t col) {
    vec3 apex = v3(base.x, base.y + height, base.z);
    for (int i = 0; i < sides; i++) {
        float a0 = (float)i / (float)sides * 2.0f * PI_F;
        float a1 = (float)(i + 1) / (float)sides * 2.0f * PI_F;
        vec3 p0 = v3(base.x + cosf(a0) * radius, base.y, base.z + sinf(a0) * radius);
        vec3 p1 = v3(base.x + cosf(a1) * radius, base.y, base.z + sinf(a1) * radius);
        vec3 out = v3(cosf((a0 + a1) * 0.5f), 0.3f, sinf((a0 + a1) * 0.5f));
        mesh_tri(m, p0, p1, apex, out, col);
        mesh_tri(m, base, p1, p0, v3(0, -1, 0), col);
    }
}

void mesh_blob(mesh_t* m, vec3 c, float rx, float ry, float rz, int sides, uint32_t col) {
    const int stacks = 4;
    for (int j = 0; j < stacks; j++) {
        float t0 = (float)j / (float)stacks * PI_F;
        float t1 = (float)(j + 1) / (float)stacks * PI_F;
        for (int i = 0; i < sides; i++) {
            float a0 = (float)i / (float)sides * 2.0f * PI_F;
            float a1 = (float)(i + 1) / (float)sides * 2.0f * PI_F;
            vec3 p00 = v3(c.x + rx * sinf(t0) * cosf(a0), c.y + ry * cosf(t0), c.z + rz * sinf(t0) * sinf(a0));
            vec3 p01 = v3(c.x + rx * sinf(t0) * cosf(a1), c.y + ry * cosf(t0), c.z + rz * sinf(t0) * sinf(a1));
            vec3 p10 = v3(c.x + rx * sinf(t1) * cosf(a0), c.y + ry * cosf(t1), c.z + rz * sinf(t1) * sinf(a0));
            vec3 p11 = v3(c.x + rx * sinf(t1) * cosf(a1), c.y + ry * cosf(t1), c.z + rz * sinf(t1) * sinf(a1));
            vec3 hint = v3_sub(centroid4(p00, p01, p11, p10), c);
            if (j == 0) {
                mesh_tri(m, p00, p11, p10, hint, col);
            } else if (j == stacks - 1) {
                mesh_tri(m, p00, p01, p10, hint, col);
            } else {
                mesh_quad(m, p00, p01, p11, p10, hint, col);
            }
        }
    }
}
