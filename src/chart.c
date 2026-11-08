#include "chart.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELIABLE_MAX    960.0f      // METR: measurements above 16 hrs are unreliable (also excluded from fits)
#define RECENT_FROM     2023.0f     // METR: start of the "recent" doubling-time fit
#define ECI_FIT_FROM    2024.0f     // ECI: start of the frontier trend fits / lag statistics
#define SWEEP_RATE      1.5f        // reveal cursor speed (years per second)
#define TREND_TIME      1.3f        // seconds for an annotation step to grow in
#define POP_TIME        0.35f
#define LABEL_SCALE     0.75f
#define AXIS_SCALE      1.0f

// ---------------------------------------------------------------------------
//  Helpers
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

static float parse_date(const char* s) {
    int y = 0, m = 0, d = 0;
    return (sscanf(s, "%d-%d-%d", &y, &m, &d) == 3) ? date_to_year(y, m, d) : 0.0f;
}

static float lerp_t(float a, float b, float t) { return a + (b - a) * t; }

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

static bool ends_with(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

// ---------------------------------------------------------------------------
//  METR benchmark_results YAML (just enough of YAML for these files)
// ---------------------------------------------------------------------------

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

static bool load_metr_yaml(chart_data_t* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    c->kind = CHART_METR;
    c->x0 = 2019.0f; c->x1 = 2026.75f;
    c->y0 = 0.03f;   c->y1 = 2400.0f;   // minutes
    c->ylog = true;
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
                    cur->t = parse_date(val);
                } else if (!strcmp(key, "is_sota")) {
                    cur->frontier = !strcmp(val, "true");
                } else if (indent == 6) {
                    in_p50 = !strcmp(key, "p50_horizon_length");
                } else if (indent == 8 && in_p50) {
                    if (!strcmp(key, "estimate")) cur->v = (float)atof(val);
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
        if (p->t < 1.0f || p->v <= 0.0f) {
            p->hidden = true;
        }
    }
    return c->n > 0;
}

// ---------------------------------------------------------------------------
//  Epoch Capabilities Index CSV (eci_scores.csv)
// ---------------------------------------------------------------------------

// splits one CSV line in place (handles quoted fields with commas / doubled quotes)
static int csv_split(char* s, char** fields, int max) {
    int n = 0;
    char* p = s;
    while (n < max) {
        char* out = p;
        fields[n++] = out;
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"') {
                    if (p[1] == '"') { *out++ = '"'; p += 2; continue; }
                    p++;
                    break;
                }
                *out++ = *p++;
            }
            while (*p && *p != ',') p++;
        } else {
            while (*p && *p != ',') p++;
            out = p;
        }
        bool more = (*p == ',');
        *out = 0;
        if (!more) break;
        p++;
    }
    return n;
}

static int find_col(char** cols, int n, const char* name) {
    for (int i = 0; i < n; i++) {
        if (!strcmp(cols[i], name)) return i;
    }
    return -1;
}

static bool load_eci_csv(chart_data_t* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    c->kind = CHART_ECI;
    c->x0 = 2023.0f; c->x1 = 2027.0f;
    c->y0 = 90.0f;   c->y1 = 175.0f;
    c->ylog = false;
    c->max_labels = 14;
    static char line[8192];
    char* cols[64];
    int i_model = -1, i_name = -1, i_eci = -1, i_lo = -1, i_hi = -1, i_date = -1, i_group = -1;
    bool header = true;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }
        int nc = csv_split(line, cols, 64);
        if (header) {
            header = false;
            i_model = find_col(cols, nc, "Model");
            i_name = find_col(cols, nc, "Display name");
            i_eci = find_col(cols, nc, "eci");
            i_lo = find_col(cols, nc, "eci_ci_low");
            i_hi = find_col(cols, nc, "eci_ci_high");
            i_date = find_col(cols, nc, "date");
            i_group = find_col(cols, nc, "Accessibility group");
            if (i_eci < 0 || i_date < 0 || i_model < 0) {
                fclose(f);
                return false;
            }
            continue;
        }
        if (i_eci >= nc || i_date >= nc || i_model >= nc || !cols[i_eci][0] || !cols[i_date][0] ||
            c->n >= CHART_MAX_POINTS) {
            continue;
        }
        chart_point_t* p = &c->pts[c->n++];
        memset(p, 0, sizeof(*p));
        snprintf(p->id, sizeof(p->id), "%s", cols[i_model]);
        snprintf(p->label, sizeof(p->label), "%s", (i_name >= 0 && i_name < nc && cols[i_name][0]) ? cols[i_name] : cols[i_model]);
        p->v = (float)atof(cols[i_eci]);
        p->t = parse_date(cols[i_date]);
        if (i_lo >= 0 && i_hi >= 0 && i_lo < nc && i_hi < nc && cols[i_lo][0] && cols[i_hi][0]) {
            p->lo = (float)atof(cols[i_lo]);
            p->hi = (float)atof(cols[i_hi]);
            p->has_ci = true;
        }
        const char* g = (i_group >= 0 && i_group < nc) ? cols[i_group] : "";
        p->group = !strcmp(g, "Closed weights") ? 0 : !strcmp(g, "Open weights") ? 1 : 2;
        if (p->t < 1.0f) p->hidden = true;
    }
    fclose(f);

    // chronological order
    for (int i = 1; i < c->n; i++) {
        chart_point_t tmp = c->pts[i];
        int j = i - 1;
        while (j >= 0 && c->pts[j].t > tmp.t) { c->pts[j + 1] = c->pts[j]; j--; }
        c->pts[j + 1] = tmp;
    }
    // frontier (new best of its group at release) + size of the improvement
    for (int g = 0; g < 2; g++) {
        float best = -1.0f;
        for (int i = 0; i < c->n; i++) {
            chart_point_t* p = &c->pts[i];
            if (p->hidden || p->group != g || p->v <= best) continue;
            p->frontier = true;
            p->jump = (best < 0.0f) ? 10.0f : p->v - best;
            best = p->v;
        }
    }
    // the newest frontier model of each group always gets a label first
    for (int g = 0; g < 2; g++) {
        for (int i = c->n - 1; i >= 0; i--) {
            if (c->pts[i].frontier && c->pts[i].group == g) { c->pts[i].jump += 100.0f; break; }
        }
    }
    // linear frontier trends from 2024 on
    for (int g = 0; g < 2; g++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (int i = 0; i < c->n; i++) {
            const chart_point_t* p = &c->pts[i];
            if (!p->frontier || p->group != g || p->t < ECI_FIT_FROM) continue;
            sx += p->t; sy += p->v; sxx += (double)p->t * p->t; sxy += (double)p->t * p->v;
            n++;
        }
        double den = n * sxx - sx * sx;
        if (n >= 3 && fabs(den) > 1e-9) {
            c->slope[g] = (float)((n * sxy - sx * sy) / den);
            c->icpt[g] = (float)((sy - c->slope[g] * sx) / n);
            c->has_trend[g] = true;
        }
    }
    // open-weights lag: when did closed weights first reach each open frontier score?
    float lag_sum = 0.0f;
    int lag_n = 0;
    for (int i = 0; i < c->n; i++) {
        chart_point_t* p = &c->pts[i];
        if (!p->frontier || p->group != 1 || p->t < ECI_FIT_FROM) continue;
        for (int k = 0; k < c->n; k++) {
            const chart_point_t* q = &c->pts[k];
            if (q->frontier && q->group == 0 && q->v >= p->v) {
                if (q->t < p->t) {
                    p->lag_t = q->t;
                    lag_sum += (p->t - q->t) * 12.0f;
                    lag_n++;
                }
                break;
            }
        }
    }
    c->lag_months = lag_n ? lag_sum / (float)lag_n : 0.0f;
    return c->n > 0;
}

bool chart_load(chart_data_t* c, const char* path) {
    memset(c, 0, sizeof(*c));
    if (ends_with(path, ".csv")) {
        return load_eci_csv(c, path);
    }
    return load_metr_yaml(c, path);
}

static chart_point_t* find(chart_data_t* c, const char* id) {
    for (int i = 0; i < c->n; i++) {
        if (!strcmp(c->pts[i].id, id)) return &c->pts[i];
    }
    for (int i = 0; i < c->n; i++) {
        if (!strcmp(c->pts[i].label, id)) return &c->pts[i];
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

static int num_annotations(const chart_data_t* c) {
    if (c->kind == CHART_ECI) {
        return ((c->has_trend[0] || c->has_trend[1]) ? 1 : 0) + (c->lag_months > 0.0f ? 1 : 0);
    }
    return (c->doubling_all > 0.0f ? 1 : 0) + (c->doubling_recent > 0.0f ? 1 : 0);
}

int chart_num_steps(const chart_data_t* c) {
    return 1 + num_sweeps(c) + num_annotations(c);
}

static void step_targets(const chart_data_t* c, int step, float* cursor, float* t1, float* t2) {
    int ns = num_sweeps(c);
    *cursor = (step == 0) ? c->x0 : (step <= ns ? ((step - 1 < c->nstops) ? c->stops[step - 1] : c->x1) : c->x1);
    int k = step - ns;
    int na = num_annotations(c);
    *t1 = (na >= 1 && k >= 1) ? 1.0f : 0.0f;
    *t2 = (na >= 2 && k >= 2) ? 1.0f : 0.0f;
}

void chart_anim_reset(chart_anim_t* a, const chart_data_t* c) {
    a->step = 0;
    a->cursor = c->x0;
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

static void clipped_line(float ax, float ay, float bx, float by, float w, uint32_t col, const float r[4]) {
    if (clip(&ax, &ay, &bx, &by, r)) line(ax, ay, bx, by, w, col);
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

// METR: least-squares intercept of log2(v) = b + slope * t with a fixed slope (METR's doubling time)
static bool fit_intercept(const chart_data_t* c, float doubling_days, float from, float* b) {
    float slope = 365.25f / doubling_days, sum = 0.0f;
    int n = 0;
    for (int i = 0; i < c->n; i++) {
        const chart_point_t* p = &c->pts[i];
        if (p->hidden || !p->frontier || p->t < from || p->v > RELIABLE_MAX) continue;
        sum += log2f(p->v) - slope * p->t;
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

static void group_color(const chart_data_t* c, int g, float* r, float* gg, float* b) {
    if (c->kind == CHART_METR) { *r = 0.30f; *gg = 0.95f; *b = 0.40f; return; }
    switch (g) {
        case 0:  *r = 1.00f; *gg = 0.70f; *b = 0.22f; break;   // closed weights
        case 1:  *r = 0.30f; *gg = 0.85f; *b = 1.00f; break;   // open weights
        default: *r = 0.62f; *gg = 0.62f; *b = 0.72f; break;
    }
}

int chart_build(const chart_data_t* c, const chart_anim_t* a, const float r[4],
                chart_vert_t* out, int max_verts, chart_text_fn text) {
    B.out = out; B.n = 0; B.max = max_verts;
    const bool metr = (c->kind == CHART_METR);
    const float cw_axis = 8.0f * AXIS_SCALE, cw_label = 8.0f * LABEL_SCALE;
    const float left = (metr ? 7.0f : 4.5f) * cw_axis;
    const float pr[4] = { r[0] + left, r[1] + 2.0f, r[2] - 4.0f, r[3] - cw_axis - 6.0f };
    const float L0 = c->ylog ? log10f(c->y0) : c->y0, L1 = c->ylog ? log10f(c->y1) : c->y1;
    #define X(t) (pr[0] + ((t) - c->x0) / (c->x1 - c->x0) * (pr[2] - pr[0]))
    #define Y(v) (pr[3] - ((c->ylog ? log10f(fmaxf((v), 1e-6f)) : (v)) - L0) / (L1 - L0) * (pr[3] - pr[1]))

    const uint32_t grid = rgba(0.20f, 0.30f, 0.60f, 0.55f);
    const uint32_t axis = rgba(0.70f, 0.78f, 0.95f, 1.0f);
    const uint32_t txt_dim = rgba(0.70f, 0.76f, 0.92f, 1.0f);
    const uint32_t yellow = rgba(1.0f, 0.82f, 0.25f, 1.0f);

    // background (+ METR's "unreliable" band)
    rect(pr[0], pr[1], pr[2], pr[3], rgba(0.05f, 0.09f, 0.27f, 1.0f));
    float ytop = pr[1];
    box_t band_box = { 0, 0, 0, 0 };
    if (metr) {
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
        band_box = (box_t){ btx - 2.0f, bty - 2.0f, btx + (float)strlen(band_txt) * cw_label + 2.0f, bty + cw_label + 2.0f };
        ytop = yb;
    }

    // grid + axis labels
    if (metr) {
        static const float yt[] = { 4.0f / 60.0f, 0.6f, 6.0f, 60.0f, 240.0f, 960.0f };
        static const char* yl[] = { "4 sec", "36 sec", "6 min", "1 hr", "4 hrs", "16 hrs" };
        for (int i = 0; i < 6; i++) {
            float y = Y(yt[i]);
            line(pr[0], y, pr[2], y, 0.6f, grid);
            line(pr[0] - 3.0f, y, pr[0], y, 1.0f, axis);
            text(pr[0] - 5.0f - (float)strlen(yl[i]) * cw_axis, y - cw_axis * 0.5f, AXIS_SCALE, txt_dim, yl[i]);
        }
    } else {
        for (int v = (int)ceilf(c->y0 / 10.0f) * 10; (float)v <= c->y1; v += 10) {
            float y = Y((float)v);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", v);
            line(pr[0], y, pr[2], y, 0.6f, grid);
            line(pr[0] - 3.0f, y, pr[0], y, 1.0f, axis);
            text(pr[0] - 5.0f - (float)strlen(buf) * cw_axis, y - cw_axis * 0.5f, AXIS_SCALE, txt_dim, buf);
        }
    }
    for (int yr = (int)ceilf(c->x0 + 0.01f); (float)yr <= c->x1 - 0.2f; yr++) {
        float x = X((float)yr);
        line(x, pr[1], x, pr[3], 0.6f, grid);
        line(x, pr[3], x, pr[3] + 3.0f, 1.0f, axis);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", yr);
        text(x - 2.0f * cw_axis, pr[3] + 5.0f, AXIS_SCALE, txt_dim, buf);
    }
    line(pr[0], pr[1], pr[0], pr[3], 1.2f, axis);
    line(pr[0], pr[3], pr[2], pr[3], 1.2f, axis);

    // annotations below the data: trend lines / lag markers
    if (metr) {
        float b;
        if (a->trend_all > 0.0f && fit_intercept(c, c->doubling_all, -1e9f, &b)) {
            float slope = 365.25f / c->doubling_all, te = c->x0 + (c->x1 - c->x0) * a->trend_all;
            float ax = X(c->x0), ay = Y(exp2f(b + slope * c->x0)), bx = X(te), by = Y(exp2f(b + slope * te));
            if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay, bx, by, 1.4f, 5.0f, 3.5f, rgba(0.30f, 0.95f, 0.40f, 0.75f));
        }
        if (a->trend_recent > 0.0f && fit_intercept(c, c->doubling_recent, RECENT_FROM, &b)) {
            float slope = 365.25f / c->doubling_recent, te = RECENT_FROM + (c->x1 - RECENT_FROM) * a->trend_recent;
            float ax = X(RECENT_FROM), ay = Y(exp2f(b + slope * RECENT_FROM)), bx = X(te), by = Y(exp2f(b + slope * te));
            if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay, bx, by, 1.6f, 5.0f, 3.5f, rgba(1.0f, 0.82f, 0.25f, 0.9f));
        }
    } else {
        if (a->trend_all > 0.0f) {
            for (int g = 0; g < 2; g++) {
                if (!c->has_trend[g]) continue;
                float cr, cg, cb;
                group_color(c, g, &cr, &cg, &cb);
                float te = ECI_FIT_FROM + (c->x1 - ECI_FIT_FROM) * a->trend_all;
                float ax = X(ECI_FIT_FROM), ay = Y(c->slope[g] * ECI_FIT_FROM + c->icpt[g]);
                float bx = X(te), by = Y(c->slope[g] * te + c->icpt[g]);
                if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay, bx, by, 1.5f, 5.0f, 3.5f, rgba(cr, cg, cb, 0.85f));
            }
        }
        if (a->trend_recent > 0.0f) {
            for (int i = 0; i < c->n; i++) {
                const chart_point_t* p = &c->pts[i];
                if (p->hidden || p->lag_t <= 0.0f || p->t > a->cursor) continue;
                float y = Y(p->v), x1 = X(p->t), x0 = X(lerp_t(p->t, p->lag_t, a->trend_recent));
                clipped_line(x0, y, x1, y, 1.1f, rgba(0.55f, 0.92f, 1.0f, 0.85f), pr);
                clipped_line(x0, y - 2.0f, x0, y + 2.0f, 1.1f, rgba(0.55f, 0.92f, 1.0f, 0.85f), pr);
            }
        }
    }

    // ECI: frontier staircases of both groups, growing with the cursor
    if (!metr) {
        for (int g = 0; g < 2; g++) {
            float cr, cg, cb;
            group_color(c, g, &cr, &cg, &cb);
            uint32_t col = rgba(cr, cg, cb, 0.75f);
            const chart_point_t* prev = NULL;
            for (int i = 0; i < c->n; i++) {
                const chart_point_t* p = &c->pts[i];
                if (p->hidden || !p->frontier || p->group != g || p->t > a->cursor) continue;
                if (prev) {
                    clipped_line(X(prev->t), Y(prev->v), X(p->t), Y(prev->v), 1.0f, col, pr);
                    clipped_line(X(p->t), Y(prev->v), X(p->t), Y(p->v), 1.0f, col, pr);
                }
                prev = p;
            }
            if (prev) {
                clipped_line(X(prev->t), Y(prev->v), X(fminf(a->cursor, c->x1)), Y(prev->v), 1.0f, col, pr);
            }
        }
    }

    // label placement for frontier points (computed for the final chart so labels never jump)
    static box_t lbox[CHART_MAX_POINTS];
    static bool lplaced[CHART_MAX_POINTS];
    static box_t placed[CHART_MAX_POINTS];
    static int order[CHART_MAX_POINTS];
    int nplaced = 0, no = 0;
    for (int i = 0; i < c->n; i++) {
        lplaced[i] = false;
        const chart_point_t* p = &c->pts[i];
        if (!p->hidden && p->frontier && p->group < 2 && p->v >= c->y0 && p->v <= c->y1) order[no++] = i;
    }
    for (int i = 1; i < no; i++) {   // METR: highest horizon first; ECI: biggest jumps first
        int k = order[i], j = i - 1;
        float pk = metr ? c->pts[k].v : c->pts[k].jump;
        while (j >= 0 && (metr ? c->pts[order[j]].v : c->pts[order[j]].jump) < pk) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    // reserved areas (legend / readout) that labels must avoid
    box_t reserved = metr ? (box_t){ pr[2] - 200.0f, pr[3] - 56.0f, pr[2], pr[3] }
                          : (box_t){ pr[0], pr[1], pr[0] + 190.0f, pr[1] + 66.0f };
    for (int oi = 0; oi < no; oi++) {
        if (c->max_labels > 0 && nplaced >= c->max_labels) break;
        const chart_point_t* p = &c->pts[order[oi]];
        float px = X(p->t), py = Y(p->v);
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
            bool ok = !overlap(bx, reserved) && (!metr || !overlap(bx, band_box));
            for (int k = 0; k < nplaced && ok; k++) ok = !overlap(bx, placed[k]);
            for (int k = 0; k < c->n && ok; k++) {
                const chart_point_t* q = &c->pts[k];
                if (q->hidden || (!metr && !q->frontier)) continue;
                box_t pb = { X(q->t) - 3.0f, Y(q->v) - 3.0f, X(q->t) + 3.0f, Y(q->v) + 3.0f };
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
    for (int pass = 0; pass < 3; pass++) {   // other, then non-frontier faded, then frontier on top
        for (int i = 0; i < c->n; i++) {
            const chart_point_t* p = &c->pts[i];
            if (p->hidden || p->t > a->cursor || p->v < c->y0) continue;
            int want = (p->group == 2) ? 0 : (p->frontier ? 2 : 1);
            if (want != pass) continue;
            float since = (a->cursor - p->t) / SWEEP_RATE;
            float pop = ease_out_back(fminf(since / POP_TIME, 1.0f));
            float alpha = p->frontier ? 1.0f : (metr ? 0.40f : 0.32f);
            float cr, cg, cb;
            group_color(c, p->group, &cr, &cg, &cb);
            float x = X(p->t), y = Y(fminf(p->v, c->y1));
            if (p->has_ci && (metr || p->frontier)) {
                float ax = x, ay = Y(fmaxf(p->lo, c->y0)), bx = x, by = Y(fminf(p->hi, c->y1));
                if (clip(&ax, &ay, &bx, &by, pr)) {
                    uint32_t cc = rgba(cr * 0.9f, cg * 0.9f, cb * 0.9f, 0.45f * alpha * fminf(since / POP_TIME, 1.0f));
                    line(ax, ay, bx, by, 0.8f, cc);
                    line(x - 1.6f, ay, x + 1.6f, ay, 0.8f, cc);
                    line(x - 1.6f, by, x + 1.6f, by, 0.8f, cc);
                }
            }
            float hs = (metr || p->frontier ? 2.3f : 1.7f) * pop;
            rect(x - hs - 0.7f, y - hs - 0.7f, x + hs + 0.7f, y + hs + 0.7f, rgba(0.0f, 0.03f, 0.10f, alpha));
            rect(x - hs, y - hs, x + hs, y + hs, rgba(cr, cg, cb, alpha));
            if (p->frontier && p->group < 2 && p->v > best) best = p->v;
            if (lplaced[i] && since > POP_TIME * 0.5f) {
                text(lbox[i].x0 + 1.0f, lbox[i].y0 + 1.0f, LABEL_SCALE, rgba(1.0f, 1.0f, 1.0f, 1.0f), p->label);
            }
        }
    }

    // sweeping scanline with the current year
    float cur_target, ta, tr;
    step_targets(c, a->step, &cur_target, &ta, &tr);
    if (a->cursor > c->x0 && a->cursor < cur_target - 1e-3f) {
        float x = X(a->cursor);
        line(x, ytop, x, pr[3], 1.2f, rgba(0.40f, 0.95f, 1.0f, 0.8f));
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)floorf(a->cursor));
        text(x - 2.0f * cw_label, ytop + 2.0f, LABEL_SCALE, rgba(0.40f, 0.95f, 1.0f, 1.0f), buf);
    }

    // readout + legend
    if (metr) {
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
            dashed(x - 18.0f, ly + cw_label * 0.5f, x - 4.0f, ly + cw_label * 0.5f, 1.4f, 4.0f, 2.0f, rgba(0.30f, 0.95f, 0.40f, 1.0f));
            text(x, ly, LABEL_SCALE, rgba(0.6f, 1.0f, 0.65f, fminf(a->trend_all * 3.0f, 1.0f)), buf);
        }
        if (a->trend_recent > 0.0f) {
            char buf[48];
            snprintf(buf, sizeof(buf), "2023+: x2 every %d days", (int)(c->doubling_recent + 0.5f));
            float x = pr[2] - 6.0f - (float)strlen(buf) * cw_label;
            dashed(x - 18.0f, ly + 11.0f + cw_label * 0.5f, x - 4.0f, ly + 11.0f + cw_label * 0.5f, 1.6f, 4.0f, 2.0f, yellow);
            text(x, ly + 11.0f, LABEL_SCALE, rgba(1.0f, 0.86f, 0.35f, fminf(a->trend_recent * 3.0f, 1.0f)), buf);
        }
    } else {
        // the top-left corner is empty in ECI charts (early models never scored high)
        float lx = pr[0] + 8.0f, ly = pr[1] + 6.0f;
        static const char* names[2] = { "closed weights", "open weights" };
        for (int g = 0; g < 2; g++) {
            float cr, cg, cb;
            group_color(c, g, &cr, &cg, &cb);
            float y = ly + (float)g * 10.0f;
            rect(lx, y + 0.5f, lx + 5.0f, y + 5.5f, rgba(cr, cg, cb, 1.0f));
            char buf[64];
            if (a->trend_all > 0.0f && c->has_trend[g]) {
                snprintf(buf, sizeof(buf), "%-14s +%.1f ECI/yr", names[g], c->slope[g]);
            } else {
                snprintf(buf, sizeof(buf), "%s", names[g]);
            }
            text(lx + 9.0f, y, LABEL_SCALE, rgba(cr, cg, cb, 1.0f), buf);
        }
        if (a->trend_recent > 0.0f) {
            char buf[64];
            snprintf(buf, sizeof(buf), "open: avg %d months behind", (int)(c->lag_months + 0.5f));
            text(lx, ly + 22.0f, LABEL_SCALE, rgba(0.6f, 0.93f, 1.0f, fminf(a->trend_recent * 3.0f, 1.0f)), buf);
        }
        if (best > 0.0f) {
            char buf[32];
            snprintf(buf, sizeof(buf), "TOP ECI %d", (int)(best + 0.5f));
            text(lx, ly + 36.0f, 1.25f, yellow, buf);
        }
    }
    #undef X
    #undef Y
    return B.n;
}
