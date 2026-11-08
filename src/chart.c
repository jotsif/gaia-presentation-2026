#include "chart.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// plot ranges
#define T0              2019.0f     // x axis start (decimal year)
#define T1              2026.75f    // x axis end
#define M_LO            0.03f       // y axis bottom (minutes)
#define M_HI            2400.0f     // y axis top (minutes)
#define RELIABLE_MAX    960.0f      // METR: measurements above 16 hrs are unreliable (also excluded from fits)
#define RECENT_FROM     2023.0f
#define SWEEP_RATE      1.5f        // reveal cursor speed (years per second)
#define TREND_TIME      1.3f        // seconds for a trend line to grow
#define POP_TIME        0.35f
#define LABEL_SCALE     0.75f
#define AXIS_SCALE      1.0f

// ---------------------------------------------------------------------------
//  YAML loading (just enough for METR's benchmark_results files)
// ---------------------------------------------------------------------------
static float date_to_year(int y, int m, int d) {
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (m < 1 || m > 12) {
        return (float)y;
    }
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    int doy = cum[m - 1] + d + ((leap && m > 2) ? 1 : 0);
    return (float)y + ((float)doy - 0.5f) / (leap ? 366.0f : 365.0f);
}

static void append(char* out, size_t cap, const char* s) {
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "%s", s);
}

static bool all_digits(const char* s) {
    if (!*s) return false;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }
    return true;
}

// "claude_opus_4_6_inspect" -> "Claude Opus 4.6", "gpt_5_2" -> "GPT-5.2" (override with @label)
static void prettify(const char* id, char* out, size_t cap) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", id);
    size_t bl = strlen(buf);
    if (bl > 8 && strcmp(buf + bl - 8, "_inspect") == 0) {
        buf[bl - 8] = 0;
    }
    out[0] = 0;
    bool prev_num = false, prev_gpt = false;
    for (char* tok = strtok(buf, "_"); tok; tok = strtok(NULL, "_")) {
        bool num = all_digits(tok);
        if (num && strlen(tok) == 8) continue;                    // 20240620
        if (num && strlen(tok) == 4 && atoi(tok) > 1990) break;   // 2025_08_07
        char word[64];
        if (strncmp(tok, "gpt", 3) == 0) {
            snprintf(word, sizeof(word), "GPT%s%s", tok[3] ? "-" : "", tok + 3);
            append(out, cap, out[0] ? " " : "");
            append(out, cap, word);
            prev_gpt = (tok[3] == 0);
            prev_num = (tok[3] != 0);
            continue;
        }
        if (num && prev_num) {
            append(out, cap, ".");
            append(out, cap, tok);
        } else if (prev_gpt) {
            append(out, cap, "-");
            append(out, cap, tok);
        } else {
            snprintf(word, sizeof(word), "%s", tok);
            if (!(word[0] == 'o' && isdigit((unsigned char)word[1]))) {
                word[0] = (char)toupper((unsigned char)word[0]);
            }
            append(out, cap, out[0] ? " " : "");
            append(out, cap, word);
        }
        prev_num = num || (prev_gpt && isdigit((unsigned char)tok[0]) && all_digits(tok));
        prev_gpt = false;
    }
}

bool chart_load_yaml(chart_data_t* c, const char* path) {
    memset(c, 0, sizeof(*c));
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    enum { SEC_NONE, SEC_DOUBLING, SEC_RESULTS } sec = SEC_NONE;
    char sub[64] = "";
    chart_point_t* cur = NULL;
    bool in_p50 = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
            line[--n] = 0;
        }
        int indent = 0;
        while (line[indent] == ' ') indent++;
        const char* s = line + indent;
        if (!*s || *s == '#' || *s == '-') {
            continue;
        }
        const char* colon = strchr(s, ':');
        if (!colon) {
            continue;
        }
        char key[96], val[128];
        snprintf(key, sizeof(key), "%.*s", (int)(colon - s), s);
        const char* v = colon + 1;
        while (*v == ' ') v++;
        snprintf(val, sizeof(val), "%s", v);
        char* hash = strstr(val, " #");
        if (hash) *hash = 0;
        if (val[0] == '#') val[0] = 0;

        if (indent == 0) {
            sec = !strcmp(key, "results") ? SEC_RESULTS : !strcmp(key, "doubling_time_in_days") ? SEC_DOUBLING : SEC_NONE;
            continue;
        }
        if (sec == SEC_DOUBLING) {
            if (indent == 2) {
                snprintf(sub, sizeof(sub), "%s", key);
            } else if (!strcmp(key, "point_estimate")) {
                if (!strcmp(sub, "all_time_stitched")) c->doubling_all = (float)atof(val);
                if (!strcmp(sub, "from_2023_on")) c->doubling_recent = (float)atof(val);
            }
        } else if (sec == SEC_RESULTS) {
            if (indent == 2 && !val[0]) {
                cur = (c->n < CHART_MAX_POINTS) ? &c->pts[c->n++] : NULL;
                if (cur) {
                    memset(cur, 0, sizeof(*cur));
                    snprintf(cur->id, sizeof(cur->id), "%s", key);
                }
                in_p50 = false;
            } else if (cur) {
                if (!strcmp(key, "release_date")) {
                    int y = 0, m = 0, d = 0;
                    if (sscanf(val, "%d-%d-%d", &y, &m, &d) == 3) cur->t = date_to_year(y, m, d);
                } else if (!strcmp(key, "is_sota")) {
                    cur->sota = !strcmp(val, "true");
                } else if (indent == 6) {
                    in_p50 = !strcmp(key, "p50_horizon_length");
                } else if (indent == 8 && in_p50) {
                    if (!strcmp(key, "estimate")) cur->p50 = (float)atof(val);
                    if (!strcmp(key, "ci_low")) { cur->lo = (float)atof(val); cur->has_ci = true; }
                    if (!strcmp(key, "ci_high")) { cur->hi = (float)atof(val); cur->has_ci = true; }
                }
            }
        }
    }
    fclose(f);
    for (int i = 0; i < c->n; i++) {
        chart_point_t* p = &c->pts[i];
        prettify(p->id, p->label, sizeof(p->label));
        if (p->t < 1.0f || p->p50 <= 0.0f) {
            p->hidden = true;
        }
    }
    return c->n > 0;
}

static chart_point_t* find(chart_data_t* c, const char* id) {
    for (int i = 0; i < c->n; i++) {
        if (!strcmp(c->pts[i].id, id)) return &c->pts[i];
    }
    return NULL;
}

bool chart_set_label(chart_data_t* c, const char* id, const char* label) {
    chart_point_t* p = find(c, id);
    if (p) snprintf(p->label, sizeof(p->label), "%s", label);
    return p != NULL;
}

bool chart_hide(chart_data_t* c, const char* id) {
    chart_point_t* p = find(c, id);
    if (p) p->hidden = true;
    return p != NULL;
}

// ---------------------------------------------------------------------------
//  Build steps / animation
// ---------------------------------------------------------------------------
static int num_sweeps(const chart_data_t* c) { return c->nstops + 1; }

int chart_num_steps(const chart_data_t* c) {
    return 1 + num_sweeps(c) + (c->doubling_all > 0.0f ? 1 : 0) + (c->doubling_recent > 0.0f ? 1 : 0);
}

static void step_targets(const chart_data_t* c, int step, float* cursor, float* t_all, float* t_rec) {
    int ns = num_sweeps(c);
    *cursor = (step == 0) ? T0 : (step <= ns ? ((step - 1 < c->nstops) ? c->stops[step - 1] : T1) : T1);
    int k = step - ns;   // trend steps after the sweeps
    bool has_all = c->doubling_all > 0.0f;
    *t_all = (has_all && k >= 1) ? 1.0f : 0.0f;
    *t_rec = (c->doubling_recent > 0.0f && k >= (has_all ? 2 : 1)) ? 1.0f : 0.0f;
}

void chart_anim_reset(chart_anim_t* a) {
    a->step = 0;
    a->cursor = T0;
    a->trend_all = a->trend_recent = 0.0f;
}

static float approach(float v, float target, float rate) {
    if (v < target) return fminf(v + rate, target);
    return fmaxf(v - rate, target);
}

void chart_anim_update(chart_anim_t* a, const chart_data_t* c, float dt) {
    float cur, ta, tr;
    step_targets(c, a->step, &cur, &ta, &tr);
    a->cursor = (cur < a->cursor) ? cur : approach(a->cursor, cur, SWEEP_RATE * dt);
    a->trend_all = approach(a->trend_all, ta, dt / TREND_TIME);
    a->trend_recent = approach(a->trend_recent, tr, dt / TREND_TIME);
}

void chart_anim_snap(chart_anim_t* a, const chart_data_t* c) {
    step_targets(c, a->step, &a->cursor, &a->trend_all, &a->trend_recent);
}

// ---------------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------------
static struct {
    chart_vert_t* out;
    int n, max;
} B;

static uint32_t rgba(float r, float g, float b, float a) {
    uint32_t ir = (uint32_t)(fminf(fmaxf(r, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t ig = (uint32_t)(fminf(fmaxf(g, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t ib = (uint32_t)(fminf(fmaxf(b, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t ia = (uint32_t)(fminf(fmaxf(a, 0.0f), 1.0f) * 255.0f + 0.5f);
    return ir | (ig << 8) | (ib << 16) | (ia << 24);
}

static void vtx(float x, float y, uint32_t col) {
    if (B.n < B.max) {
        B.out[B.n++] = (chart_vert_t){ x, y, col };
    }
}

static void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t col) {
    vtx(x0, y0, col); vtx(x1, y1, col); vtx(x2, y2, col);
    vtx(x0, y0, col); vtx(x2, y2, col); vtx(x3, y3, col);
}

static void rect(float x0, float y0, float x1, float y1, uint32_t col) {
    quad(x0, y0, x1, y0, x1, y1, x0, y1, col);
}

static void line(float x0, float y0, float x1, float y1, float w, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, col);
}

static void dashed(float x0, float y0, float x1, float y1, float w, float dash, float gap, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float ux = dx / len, uy = dy / len;
    for (float d = 0.0f; d < len; d += dash + gap) {
        float e = fminf(d + dash, len);
        line(x0 + ux * d, y0 + uy * d, x0 + ux * e, y0 + uy * e, w, col);
    }
}

// Liang-Barsky line clipping against r = (x0, y0, x1, y1)
static bool clip(float* ax, float* ay, float* bx, float* by, const float r[4]) {
    float t0 = 0.0f, t1 = 1.0f, dx = *bx - *ax, dy = *by - *ay;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { *ax - r[0], r[2] - *ax, *ay - r[1], r[3] - *ay };
    for (int i = 0; i < 4; i++) {
        if (fabsf(p[i]) < 1e-9f) {
            if (q[i] < 0.0f) return false;
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) { if (t > t1) return false; if (t > t0) t0 = t; }
            else             { if (t < t0) return false; if (t < t1) t1 = t; }
        }
    }
    float sx = *ax, sy = *ay;
    *ax = sx + dx * t0; *ay = sy + dy * t0;
    *bx = sx + dx * t1; *by = sy + dy * t1;
    return true;
}

static float ease_out_back(float x) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float t = x - 1.0f;
    return 1.0f + c3 * t * t * t + c1 * t * t;
}

static void fmt_minutes(float m, char* out, size_t cap) {
    if (m < 1.0f) snprintf(out, cap, "%d sec", (int)(m * 60.0f + 0.5f));
    else if (m < 60.0f) snprintf(out, cap, "%d min", (int)(m + 0.5f));
    else if (m < 600.0f) snprintf(out, cap, "%.1f hrs", m / 60.0f);
    else snprintf(out, cap, "%d hrs", (int)(m / 60.0f + 0.5f));
}

// least-squares intercept of log2(p50) = b + slope * t with a fixed slope (from METR's doubling time)
static bool fit_intercept(const chart_data_t* c, float doubling_days, float from, float* b) {
    float slope = 365.25f / doubling_days, sum = 0.0f;
    int n = 0;
    for (int i = 0; i < c->n; i++) {
        const chart_point_t* p = &c->pts[i];
        if (p->hidden || !p->sota || p->t < from || p->p50 > RELIABLE_MAX) continue;
        sum += log2f(p->p50) - slope * p->t;
        n++;
    }
    if (n == 0) return false;
    *b = sum / (float)n;
    return true;
}

typedef struct { float x0, y0, x1, y1; } box_t;

static bool overlap(box_t a, box_t b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

int chart_build(const chart_data_t* c, const chart_anim_t* a, const float r[4],
                chart_vert_t* out, int max_verts, chart_text_fn text) {
    B.out = out; B.n = 0; B.max = max_verts;
    const float cw_axis = 8.0f * AXIS_SCALE, cw_label = 8.0f * LABEL_SCALE;
    const float pr[4] = { r[0] + 7.0f * cw_axis, r[1] + 2.0f, r[2] - 4.0f, r[3] - cw_axis - 6.0f };
    const float L0 = log10f(M_LO), L1 = log10f(M_HI);
    #define X(t) (pr[0] + ((t) - T0) / (T1 - T0) * (pr[2] - pr[0]))
    #define Y(m) (pr[3] - (log10f(fmaxf((m), 1e-6f)) - L0) / (L1 - L0) * (pr[3] - pr[1]))

    const uint32_t grid = rgba(0.20f, 0.30f, 0.60f, 0.55f);
    const uint32_t axis = rgba(0.70f, 0.78f, 0.95f, 1.0f);
    const uint32_t txt_dim = rgba(0.70f, 0.76f, 0.92f, 1.0f);
    const uint32_t green = rgba(0.30f, 0.95f, 0.40f, 1.0f);
    const uint32_t yellow = rgba(1.0f, 0.82f, 0.25f, 1.0f);

    // plot background + "unreliable" band
    rect(pr[0], pr[1], pr[2], pr[3], rgba(0.05f, 0.09f, 0.27f, 1.0f));
    float yb = Y(RELIABLE_MAX);
    rect(pr[0], pr[1], pr[2], yb, rgba(1.0f, 1.0f, 1.0f, 0.05f));
    const float band[4] = { pr[0], pr[1], pr[2], yb };
    for (float k = -(yb - pr[1]); k < pr[2] - pr[0]; k += 7.0f) {
        float ax = pr[0] + k, ay = yb, bx = pr[0] + k + (yb - pr[1]), by = pr[1];
        if (clip(&ax, &ay, &bx, &by, band)) line(ax, ay, bx, by, 0.7f, rgba(1.0f, 1.0f, 1.0f, 0.10f));
    }
    const char* band_txt = "above 16 hrs: unreliable";
    float btx = (pr[0] + pr[2]) * 0.5f - (float)strlen(band_txt) * cw_label * 0.5f;
    float bty = (pr[1] + yb) * 0.5f - cw_label * 0.5f;
    text(btx, bty, LABEL_SCALE, rgba(0.85f, 0.88f, 1.0f, 0.75f), band_txt);
    const box_t band_box = { btx - 2.0f, bty - 2.0f, btx + (float)strlen(band_txt) * cw_label + 2.0f, bty + cw_label + 2.0f };

    // grid + axis labels
    static const float yt[] = { 4.0f / 60.0f, 0.6f, 6.0f, 60.0f, 240.0f, 960.0f };
    static const char* yl[] = { "4 sec", "36 sec", "6 min", "1 hr", "4 hrs", "16 hrs" };
    for (int i = 0; i < 6; i++) {
        float y = Y(yt[i]);
        line(pr[0], y, pr[2], y, 0.6f, grid);
        line(pr[0] - 3.0f, y, pr[0], y, 1.0f, axis);
        text(pr[0] - 5.0f - (float)strlen(yl[i]) * cw_axis, y - cw_axis * 0.5f, AXIS_SCALE, txt_dim, yl[i]);
    }
    for (int yr = 2020; yr <= 2026; yr++) {
        float x = X((float)yr);
        line(x, pr[1], x, pr[3], 0.6f, grid);
        line(x, pr[3], x, pr[3] + 3.0f, 1.0f, axis);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", yr);
        text(x - 2.0f * cw_axis, pr[3] + 5.0f, AXIS_SCALE, txt_dim, buf);
    }
    line(pr[0], pr[1], pr[0], pr[3], 1.2f, axis);
    line(pr[0], pr[3], pr[2], pr[3], 1.2f, axis);

    // trend lines
    float b;
    if (a->trend_all > 0.0f && fit_intercept(c, c->doubling_all, -1e9f, &b)) {
        float slope = 365.25f / c->doubling_all, te = T0 + (T1 - T0) * a->trend_all;
        float ax = X(T0), ay = Y(exp2f(b + slope * T0)), bx = X(te), by = Y(exp2f(b + slope * te));
        if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay, bx, by, 1.4f, 5.0f, 3.5f, rgba(0.30f, 0.95f, 0.40f, 0.75f));
    }
    if (a->trend_recent > 0.0f && fit_intercept(c, c->doubling_recent, RECENT_FROM, &b)) {
        float slope = 365.25f / c->doubling_recent, te = RECENT_FROM + (T1 - RECENT_FROM) * a->trend_recent;
        float ax = X(RECENT_FROM), ay = Y(exp2f(b + slope * RECENT_FROM)), bx = X(te), by = Y(exp2f(b + slope * te));
        if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay, bx, by, 1.6f, 5.0f, 3.5f, rgba(1.0f, 0.82f, 0.25f, 0.9f));
    }

    // label placement for all SOTA points (computed for the final chart so labels never jump)
    static box_t lbox[CHART_MAX_POINTS];
    static bool lplaced[CHART_MAX_POINTS];
    static box_t placed[CHART_MAX_POINTS];
    int nplaced = 0;
    int order[CHART_MAX_POINTS], no = 0;
    for (int i = 0; i < c->n; i++) {
        lplaced[i] = false;
        if (!c->pts[i].hidden && c->pts[i].sota) order[no++] = i;
    }
    for (int i = 1; i < no; i++) {   // highest horizon first
        int k = order[i], j = i - 1;
        while (j >= 0 && c->pts[order[j]].p50 < c->pts[k].p50) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int oi = 0; oi < no; oi++) {
        const chart_point_t* p = &c->pts[order[oi]];
        float px = X(p->t), py = Y(p->p50);
        float w = (float)strlen(p->label) * cw_label, h = cw_label;
        const float cand[10][2] = {
            { px - 4.0f - w, py - h * 0.5f }, { px + 4.0f, py - h * 0.5f },
            { px - w * 0.5f, py - h - 5.0f }, { px - w * 0.5f, py + 5.0f },
            { px - w - 2.0f, py - h - 4.0f }, { px + 2.0f, py + 4.0f },
            { px - w - 2.0f, py + 4.0f },      { px + 2.0f, py - h - 4.0f },
            { px - w - 10.0f, py + h * 0.6f }, { px - w - 10.0f, py - h * 1.6f },
        };
        for (int ci = 0; ci < 10; ci++) {
            box_t bx = { cand[ci][0] - 1.0f, cand[ci][1] - 1.0f, cand[ci][0] + w + 1.0f, cand[ci][1] + h + 1.0f };
            if (bx.x0 < pr[0] + 2.0f || bx.x1 > pr[2] - 2.0f || bx.y0 < pr[1] + 1.0f || bx.y1 > pr[3] - 2.0f) continue;
            bool ok = !overlap(bx, band_box);
            for (int k = 0; k < nplaced && ok; k++) ok = !overlap(bx, placed[k]);
            for (int k = 0; k < c->n && ok; k++) {
                const chart_point_t* q = &c->pts[k];
                if (q->hidden) continue;
                box_t pb = { X(q->t) - 3.0f, Y(q->p50) - 3.0f, X(q->t) + 3.0f, Y(q->p50) + 3.0f };
                ok = !overlap(bx, pb);
            }
            if (ok) {
                lbox[order[oi]] = bx;
                lplaced[order[oi]] = true;
                placed[nplaced++] = bx;
                break;
            }
        }
    }

    // points (revealed by the cursor), confidence intervals, labels
    float best = 0.0f;
    for (int pass = 0; pass < 2; pass++) {   // faded non-SOTA first, SOTA on top
        for (int i = 0; i < c->n; i++) {
            const chart_point_t* p = &c->pts[i];
            if (p->hidden || p->sota != (pass == 1) || p->t > a->cursor) continue;
            float since = (a->cursor - p->t) / SWEEP_RATE;
            float pop = ease_out_back(fminf(since / POP_TIME, 1.0f));
            float alpha = p->sota ? 1.0f : 0.40f;
            float x = X(p->t), y = Y(fminf(p->p50, M_HI));
            if (p->has_ci) {
                float ax = x, ay = Y(fmaxf(p->lo, M_LO)), bx = x, by = Y(fminf(p->hi, M_HI));
                if (clip(&ax, &ay, &bx, &by, pr)) {
                    uint32_t cc = rgba(0.40f, 0.85f, 0.50f, 0.45f * alpha * fminf(since / POP_TIME, 1.0f));
                    line(ax, ay, bx, by, 0.8f, cc);
                    line(x - 1.6f, ay, x + 1.6f, ay, 0.8f, cc);
                    line(x - 1.6f, by, x + 1.6f, by, 0.8f, cc);
                }
            }
            float hs = 2.3f * pop;
            rect(x - hs - 0.7f, y - hs - 0.7f, x + hs + 0.7f, y + hs + 0.7f, rgba(0.0f, 0.03f, 0.10f, alpha));
            rect(x - hs, y - hs, x + hs, y + hs, rgba(0.30f, 0.95f, 0.40f, alpha));
            if (p->sota && p->p50 > best) best = p->p50;
            if (lplaced[i] && since > POP_TIME * 0.5f) {
                text(lbox[i].x0 + 1.0f, lbox[i].y0 + 1.0f, LABEL_SCALE, rgba(1.0f, 1.0f, 1.0f, 1.0f), p->label);
            }
        }
    }

    // sweeping scanline with the current date
    float cur_target, ta, tr;
    step_targets(c, a->step, &cur_target, &ta, &tr);
    if (a->cursor > T0 && a->cursor < cur_target - 1e-3f) {
        float x = X(a->cursor);
        line(x, yb, x, pr[3], 1.2f, rgba(0.40f, 0.95f, 1.0f, 0.8f));
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)floorf(a->cursor));
        text(x - 2.0f * cw_label, yb + 2.0f, LABEL_SCALE, rgba(0.40f, 0.95f, 1.0f, 1.0f), buf);
    }

    // readout + legend in the empty lower-right corner
    if (best > 0.0f) {
        char buf[48], dur[24];
        fmt_minutes(best, dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "BEST %s", dur);
        float s = 1.25f;
        text(pr[2] - 6.0f - (float)strlen(buf) * 8.0f * s, pr[3] - 50.0f, s, yellow, buf);
    }
    float ly = pr[3] - 28.0f;
    if (a->trend_all > 0.0f) {
        char buf[48];
        snprintf(buf, sizeof(buf), "2019+: x2 every %d days", (int)(c->doubling_all + 0.5f));
        float x = pr[2] - 6.0f - (float)strlen(buf) * cw_label;
        dashed(x - 18.0f, ly + cw_label * 0.5f, x - 4.0f, ly + cw_label * 0.5f, 1.4f, 4.0f, 2.0f, green);
        text(x, ly, LABEL_SCALE, rgba(0.6f, 1.0f, 0.65f, fminf(a->trend_all * 3.0f, 1.0f)), buf);
    }
    if (a->trend_recent > 0.0f) {
        char buf[48];
        snprintf(buf, sizeof(buf), "2023+: x2 every %d days", (int)(c->doubling_recent + 0.5f));
        float x = pr[2] - 6.0f - (float)strlen(buf) * cw_label;
        dashed(x - 18.0f, ly + 11.0f + cw_label * 0.5f, x - 4.0f, ly + 11.0f + cw_label * 0.5f, 1.6f, 4.0f, 2.0f, yellow);
        text(x, ly + 11.0f, LABEL_SCALE, rgba(1.0f, 0.86f, 0.35f, fminf(a->trend_recent * 3.0f, 1.0f)), buf);
    }
    #undef X
    #undef Y
    return B.n;
}
