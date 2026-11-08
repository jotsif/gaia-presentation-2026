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

// ---------------------------------------------------------------------------
//  LMArena anchor CSVs: date,top_model,top_model_rating,<anchor>,<anchor>,...
//  (anchor columns are Elo ratings, or win rates vs. the top model if named '... win %')
// ---------------------------------------------------------------------------

// "claude-opus-4-6-thinking" -> "Claude Opus 4.6 Thinking", "gpt-4-turbo-2024-04-09" -> "GPT-4 Turbo";
// names that already contain spaces are kept as they are
static void arena_pretty(const char* id, char* out, size_t cap) {
    if (strchr(id, ' ')) {
        snprintf(out, cap, "%s", id);
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", id);
    out[0] = 0;
    bool prev_digit = false, glue = false;
    for (char* tok = strtok(buf, "-_"); tok; tok = strtok(NULL, "-_")) {
        bool num = all_digits(tok);
        size_t tl = strlen(tok);
        if (num && (tl == 8 || (tl == 4 && atoi(tok) > 1990))) break;   // dates end the name
        if (num && tl == 2 && tok[0] == '0') break;                      // "preview-02-24"
        char word[64];
        snprintf(word, sizeof(word), "%s", tok);
        if (!strcmp(tok, "gpt")) { append(out, cap, out[0] ? " GPT" : "GPT"); glue = true; prev_digit = false; continue; }
        if (!strcmp(tok, "chatgpt")) { append(out, cap, out[0] ? " ChatGPT" : "ChatGPT"); glue = true; prev_digit = false; continue; }
        if (!strcmp(tok, "deepseek")) snprintf(word, sizeof(word), "DeepSeek");
        else if (!strcmp(tok, "r1")) snprintf(word, sizeof(word), "R1");
        else if (!(isdigit((unsigned char)tok[0]) || (tok[0] == 'o' && isdigit((unsigned char)tok[1])))) {
            word[0] = (char)toupper((unsigned char)word[0]);
        }
        bool single_digit = num && tl == 1;
        if (glue) append(out, cap, "-");
        else if (single_digit && prev_digit) append(out, cap, ".");
        else if (out[0]) append(out, cap, " ");
        append(out, cap, word);
        prev_digit = single_digit || (isdigit((unsigned char)tok[0]) && strchr(tok, '.') != NULL);
        glue = false;
    }
}

// lower-case alphanumerics only, so "gemini-4-argon-high" == "Gemini 4 Argon (High)"
static void norm_key(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (isalnum((unsigned char)*s)) out[o++] = (char)tolower((unsigned char)*s);
    }
    out[o] = 0;
}

static void fit_line(const float* t, const float* v, int n, float from, float* slope, float* icpt) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (isnan(v[i]) || t[i] < from) continue;
        sx += t[i]; sy += v[i]; sxx += (double)t[i] * t[i]; sxy += (double)t[i] * v[i];
        k++;
    }
    double den = k * sxx - sx * sx;
    *slope = (k >= 3 && fabs(den) > 1e-9) ? (float)((k * sxy - sx * sy) / den) : 0.0f;
    *icpt = k ? (float)((sy - *slope * sx) / k) : 0.0f;
}

static bool load_arena_csv(chart_data_t* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    static char line[4096];
    char* cols[CHART_MAX_SERIES + 8];
    bool header = true;
    char last_key[64] = "";
    float top_seen[CHART_MAX_POINTS];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }
        int nc = csv_split(line, cols, CHART_MAX_SERIES + 3);
        if (header) {
            header = false;
            if (nc < 4 || strcmp(cols[0], "date") || strcmp(cols[1], "top_model")) {
                fclose(f);
                return false;
            }
            c->nseries = nc - 3;
            c->kind = CHART_ARENA;
            for (int k = 0; k < c->nseries; k++) {
                char name[64];
                snprintf(name, sizeof(name), "%s", cols[3 + k]);
                size_t nl = strlen(name);
                if (nl > 6 && !strcmp(name + nl - 6, " win %")) {
                    name[nl - 6] = 0;
                    c->kind = CHART_WINRATE;
                }
                snprintf(c->series_name[k], sizeof(c->series_name[k]), "%s", name);
            }
            continue;
        }
        if (nc < 3 || c->nrows >= CHART_MAX_ROWS) continue;
        int r = c->nrows++;
        c->row_t[r] = parse_date(cols[0]);
        c->top_v[r] = (float)atof(cols[2]);
        for (int k = 0; k < c->nseries; k++) {
            c->series_v[k][r] = (3 + k < nc && cols[3 + k][0]) ? (float)atof(cols[3 + k]) : NAN;
        }
        // a new top model: one labelled point the first time each model takes the lead
        char key[64];
        norm_key(cols[1], key, sizeof(key));
        if (strcmp(key, last_key) != 0) {
            snprintf(last_key, sizeof(last_key), "%s", key);
            bool seen = false;
            for (int i = 0; i < c->n && !seen; i++) {
                char k2[64];
                norm_key(c->pts[i].id, k2, sizeof(k2));
                seen = !strcmp(k2, key);
            }
            if (!seen && c->n < CHART_MAX_POINTS) {
                chart_point_t* p = &c->pts[c->n];
                memset(p, 0, sizeof(*p));
                snprintf(p->id, sizeof(p->id), "%s", cols[1]);
                arena_pretty(cols[1], p->label, sizeof(p->label));
                p->t = c->row_t[r];
                p->v = c->top_v[r];
                p->frontier = true;
                p->hidden = (c->kind == CHART_WINRATE);   // win-rate charts only mark the changes
                top_seen[c->n] = p->v;
                c->n++;
            }
        }
    }
    fclose(f);
    if (c->nrows < 2) return false;
    // label priority: the biggest rating jumps first, the current leader always
    for (int i = 0; i < c->n; i++) {
        c->pts[i].jump = (i == 0) ? 50.0f : top_seen[i] - top_seen[i - 1];
    }
    if (c->n > 0) c->pts[c->n - 1].jump += 1000.0f;
    c->max_labels = 9;
    c->sweep_rate = 0.8f;
    float t0 = c->row_t[0], t1 = c->row_t[c->nrows - 1];
    c->x0 = floorf(t0 * 4.0f) / 4.0f;
    c->x1 = t1 + 0.32f;   // room for the value labels at the line ends
    if (c->kind == CHART_ARENA) {
        float lo = 1e9f, hi = -1e9f;
        for (int r = 0; r < c->nrows; r++) {
            lo = fminf(lo, c->top_v[r]); hi = fmaxf(hi, c->top_v[r]);
            for (int k = 0; k < c->nseries; k++) {
                if (!isnan(c->series_v[k][r])) { lo = fminf(lo, c->series_v[k][r]); hi = fmaxf(hi, c->series_v[k][r]); }
            }
        }
        c->ystep = 50.0f;
        c->y0 = floorf((lo - 20.0f) / 50.0f) * 50.0f;
        c->y1 = ceilf((hi + 30.0f) / 50.0f) * 50.0f;
        // trends: the leader (whole period) and each anchor model once its rating has settled
        fit_line(c->row_t, c->top_v, c->nrows, -1e9f, &c->slope[0], &c->icpt[0]);
        c->has_trend[0] = true;
    } else {
        c->ystep = 10.0f;
        c->y0 = 0.0f;
        c->y1 = 60.0f;
    }
    for (int k = 0; k < c->nseries; k++) {
        float first = NAN;
        for (int r = 0; r < c->nrows && isnan(first); r++) {
            if (!isnan(c->series_v[k][r])) first = c->row_t[r];
        }
        c->series_from[k] = isnan(first) ? t1 : first + 0.25f;   // skip the first three months
        fit_line(c->row_t, c->series_v[k], c->nrows, c->series_from[k], &c->series_slope[k], &c->series_icpt[k]);
    }
    return true;
}

static uint32_t rgba(float r, float g, float b, float a);

// ---------------------------------------------------------------------------
//  Elo leaderboard CSV: model,elo[,lab][,color as #RRGGBB]  (GDPval-AA style)
// ---------------------------------------------------------------------------
#define HUMAN_ELO 1000.0f

static bool load_bars_csv(chart_data_t* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static char line[1024];
    char* cols[8];
    bool header = true;
    int i_color = -1, i_cost = -1, i_lab = -1;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        int nc = csv_split(line, cols, 8);
        if (header) {
            header = false;
            i_color = find_col(cols, nc, "color");
            i_cost = find_col(cols, nc, "cost");
            i_lab = find_col(cols, nc, "lab");
            c->pct = (nc > 1 && !strcmp(cols[1], "score"));
            c->has_cost = (i_cost >= 0);
            continue;
        }
        if (nc < 2 || !cols[1][0] || c->n >= CHART_MAX_POINTS) continue;
        chart_point_t* p = &c->pts[c->n++];
        memset(p, 0, sizeof(*p));
        snprintf(p->id, sizeof(p->id), "%s", cols[0]);
        snprintf(p->label, sizeof(p->label), "%s", cols[0]);
        p->v = (float)atof(cols[1]);
        p->group = (i_lab >= 0 && i_lab < nc && !strcmp(cols[i_lab], "human")) ? 3 : 0;   // human baseline row
        p->cost = (i_cost >= 0 && i_cost < nc && cols[i_cost][0]) ? (float)atof(cols[i_cost]) : -1.0f;
        if (i_color >= 0 && i_color < nc && cols[i_color][0] == '#') {
            unsigned int rgb = (unsigned int)strtoul(cols[i_color] + 1, NULL, 16);
            p->color = rgba((float)((rgb >> 16) & 255) / 255.0f, (float)((rgb >> 8) & 255) / 255.0f, (float)(rgb & 255) / 255.0f, 1.0f);
        }
    }
    fclose(f);
    // best first
    for (int i = 1; i < c->n; i++) {
        chart_point_t tmp = c->pts[i];
        int j = i - 1;
        while (j >= 0 && c->pts[j].v < tmp.v) { c->pts[j + 1] = c->pts[j]; j--; }
        c->pts[j + 1] = tmp;
    }
    c->kind = CHART_BARS;
    if (c->pct) {
        c->x0 = 0.0f;
        c->x1 = 5.0f;
        for (int i = 0; i < c->n; i++) c->x1 = fmaxf(c->x1, ceilf((c->pts[i].v + 1.5f) / 5.0f) * 5.0f);
        c->ystep = 5.0f;
        c->sweep_rate = c->x1 / 3.0f;
    } else {
        c->x0 = 800.0f;
        c->x1 = 2000.0f;
        for (int i = 0; i < c->n; i++) c->x1 = fmaxf(c->x1, ceilf((c->pts[i].v + 120.0f) / 200.0f) * 200.0f);
        c->ystep = 200.0f;
        c->sweep_rate = 380.0f;   // Elo points per second
    }
    return c->n > 0;
}

static bool csv_is_bars(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char head[256] = "";
    bool ok = fgets(head, sizeof(head), f) && (strncmp(head, "model,elo", 9) == 0 || strncmp(head, "model,score", 11) == 0);
    fclose(f);
    return ok;
}

static bool csv_is_arena(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char head[256] = "";
    bool ok = fgets(head, sizeof(head), f) && strncmp(head, "date,top_model,", 15) == 0;
    fclose(f);
    return ok;
}

bool chart_load(chart_data_t* c, const char* path) {
    memset(c, 0, sizeof(*c));
    if (ends_with(path, ".csv")) {
        if (csv_is_bars(path)) return load_bars_csv(c, path);
        return csv_is_arena(path) ? load_arena_csv(c, path) : load_eci_csv(c, path);
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
    for (int k = 0; k < c->nseries && !p; k++) {   // anchor model columns can be renamed too
        if (!strcmp(c->series_name[k], id)) {
            snprintf(c->series_name[k], sizeof(c->series_name[k]), "%s", label);
            return true;
        }
    }
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
    if (c->kind == CHART_ARENA) return 2;     // leader trend, then the anchors' (flat) trends
    if (c->kind == CHART_WINRATE) return 1;   // start -> now for each anchor
    if (c->kind == CHART_THEORY) return 2;
    if (c->kind == CHART_BARS) return c->pct ? (c->has_cost ? 1 : 0) : 2;   // Elo: humans, win rate; %: cost
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
    float rate = c->sweep_rate > 0.0f ? c->sweep_rate : SWEEP_RATE;
    a->cursor = (cur < a->cursor) ? cur : approach(a->cursor, cur, rate * dt);
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

// anchor model lines in the LMArena charts
static const float SERIES_COL[CHART_MAX_SERIES][3] = {
    { 0.30f, 0.95f, 0.40f }, { 1.00f, 0.55f, 0.25f }, { 0.30f, 0.85f, 1.00f },
    { 1.00f, 0.42f, 0.85f }, { 0.75f, 0.60f, 1.00f }, { 0.90f, 0.90f, 0.90f },
};

static float sweep_rate(const chart_data_t* c) {
    return c->sweep_rate > 0.0f ? c->sweep_rate : SWEEP_RATE;
}

static void group_color(const chart_data_t* c, int g, float* r, float* gg, float* b) {
    if (c->kind == CHART_METR) { *r = 0.30f; *gg = 0.95f; *b = 0.40f; return; }
    if (c->kind == CHART_ARENA) { *r = 1.00f; *gg = 0.82f; *b = 0.25f; return; }
    switch (g) {
        case 0:  *r = 1.00f; *gg = 0.70f; *b = 0.22f; break;   // closed weights
        case 1:  *r = 0.30f; *gg = 0.85f; *b = 1.00f; break;   // open weights
        default: *r = 0.62f; *gg = 0.62f; *b = 0.72f; break;
    }
}

static int build_theory(const chart_data_t* c, const chart_anim_t* a, const float r[4], chart_text_fn text);
static int build_bars(const chart_data_t* c, const chart_anim_t* a, const float r[4], chart_text_fn text);

int chart_build(const chart_data_t* c, const chart_anim_t* a, const float r[4],
                chart_vert_t* out, int max_verts, chart_text_fn text) {
    B.out = out; B.n = 0; B.max = max_verts;
    if (c->kind == CHART_THEORY) {
        return build_theory(c, a, r, text);
    }
    if (c->kind == CHART_BARS) {
        return build_bars(c, a, r, text);
    }
    const bool metr = (c->kind == CHART_METR), eci = (c->kind == CHART_ECI);
    const bool series = (c->kind == CHART_ARENA || c->kind == CHART_WINRATE);
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
        int step = c->ystep > 0.0f ? (int)c->ystep : 10;
        for (int v = (int)ceilf(c->y0 / (float)step) * step; (float)v <= c->y1; v += step) {
            float y = Y((float)v);
            char buf[8];
            snprintf(buf, sizeof(buf), c->kind == CHART_WINRATE ? "%d%%" : "%d", v);
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
    } else if (eci) {
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

    // LMArena: the leader's rating (staircase) and the anchor models' lines
    float last_top = 0.0f;
    if (series) {
        const float cur = a->cursor;
        const float tend = c->row_t[c->nrows - 1];
        if (c->kind == CHART_ARENA && a->trend_all > 0.0f) {
            float te = c->x0 + (c->x1 - c->x0) * a->trend_all;
            float ax = X(c->x0), ay = Y(c->slope[0] * c->x0 + c->icpt[0]);
            float bx = X(te), by = Y(c->slope[0] * te + c->icpt[0]);
            if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay, bx, by, 1.5f, 5.0f, 3.5f, rgba(1.0f, 0.82f, 0.25f, 0.7f));
        }
        if (c->kind == CHART_ARENA && a->trend_recent > 0.0f) {
            for (int k = 0; k < c->nseries; k++) {
                float t0 = c->series_from[k], te = t0 + (tend - t0) * a->trend_recent;
                if (t0 >= tend) continue;
                const float* sc = SERIES_COL[k];
                float ax = X(t0), ay = Y(c->series_slope[k] * t0 + c->series_icpt[k]);
                float bx = X(te), by = Y(c->series_slope[k] * te + c->series_icpt[k]);
                if (clip(&ax, &ay, &bx, &by, pr)) dashed(ax, ay + 4.0f, bx, by + 4.0f, 1.4f, 4.0f, 3.0f, rgba(sc[0], sc[1], sc[2], 0.95f));
            }
        }
        if (c->kind == CHART_WINRATE) {
            float y = Y(50.0f);
            dashed(pr[0], y, pr[2], y, 1.0f, 4.0f, 3.0f, rgba(1.0f, 1.0f, 1.0f, 0.45f));
            text(pr[2] - 4.0f - 10.0f * cw_label, y - cw_label - 2.0f, LABEL_SCALE, rgba(1.0f, 1.0f, 1.0f, 0.6f), "even match");
            for (int i = 0; i < c->n; i++) {   // a tick for every new leader
                if (c->pts[i].t > cur) break;
                float x = X(c->pts[i].t);
                line(x, pr[1], x, pr[1] + 5.0f, 1.0f, rgba(1.0f, 0.82f, 0.25f, 0.8f));
            }
        } else {
            uint32_t col = rgba(1.0f, 0.82f, 0.25f, 0.95f);
            int r = 0;
            for (; r < c->nrows && c->row_t[r] <= cur; r++) {
                if (r > 0) {
                    clipped_line(X(c->row_t[r - 1]), Y(c->top_v[r - 1]), X(c->row_t[r]), Y(c->top_v[r - 1]), 1.4f, col, pr);
                    clipped_line(X(c->row_t[r]), Y(c->top_v[r - 1]), X(c->row_t[r]), Y(c->top_v[r]), 1.4f, col, pr);
                }
                last_top = c->top_v[r];
            }
            if (r > 0) {
                clipped_line(X(c->row_t[r - 1]), Y(c->top_v[r - 1]), X(fminf(cur, tend)), Y(c->top_v[r - 1]), 1.4f, col, pr);
            }
        }
        for (int k = 0; k < c->nseries; k++) {
            const float* sc = SERIES_COL[k];
            uint32_t col = rgba(sc[0], sc[1], sc[2], 1.0f);
            float px = 0.0f, py = 0.0f, pv = 0.0f, ptime = 0.0f;
            bool have = false;
            for (int r = 0; r < c->nrows; r++) {
                float v = c->series_v[k][r], t = c->row_t[r];
                if (isnan(v)) continue;
                if (t > cur) {   // partial segment up to the sweep line
                    if (have) {
                        float u = (cur - ptime) / fmaxf(t - ptime, 1e-4f);
                        float ex = X(cur), ey = py + (Y(v) - py) * fminf(fmaxf(u, 0.0f), 1.0f);
                        clipped_line(px, py, ex, ey, 1.6f, col, pr);
                        px = ex; py = ey;
                    }
                    break;
                }
                float x = X(t), y = Y(v);
                if (have) {
                    clipped_line(px, py, x, y, 1.6f, col, pr);
                } else {   // release marker
                    rect(x - 2.6f, y - 2.6f, x + 2.6f, y + 2.6f, rgba(0.0f, 0.03f, 0.10f, 1.0f));
                    rect(x - 2.0f, y - 2.0f, x + 2.0f, y + 2.0f, col);
                }
                px = x; py = y; pv = v; ptime = t; have = true;
            }
            if (have) {   // current value riding on the end of the line (names are in the legend)
                char buf[16];
                snprintf(buf, sizeof(buf), c->kind == CHART_WINRATE ? "%d%%" : "%d", (int)(pv + 0.5f));
                float w = (float)strlen(buf) * cw_label;
                text(fminf(px + 4.0f, pr[2] - w - 2.0f), py - cw_label * 0.5f, LABEL_SCALE, col, buf);
            }
        }
    }

    // ECI: frontier staircases of both groups, growing with the cursor
    if (eci) {
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
                   : series ? (box_t){ pr[0], c->kind == CHART_WINRATE ? pr[3] - 12.0f - 10.0f * (float)(c->nseries + 1) : pr[1],
                                       pr[0] + 215.0f, c->kind == CHART_WINRATE ? pr[3] : pr[1] + 12.0f + 10.0f * (float)(c->nseries + 1) + 18.0f }
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
            float since = (a->cursor - p->t) / sweep_rate(c);
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
    } else if (series) {
        // legend in the (empty) top-left corner; trends / start -> now values appear with the annotations
        float lx = pr[0] + 8.0f, ly = pr[1] + 6.0f;
        if (c->kind == CHART_WINRATE) {   // win rates start high on the left: use the empty bottom-left
            ly = pr[3] - 6.0f - 10.0f * (float)(c->nseries + 1);
        }
        int row = 0;
        if (c->kind == CHART_ARENA) {
            char buf[80];
            if (a->trend_all > 0.0f) snprintf(buf, sizeof(buf), "%-16s %+d Elo/yr", "top model", (int)lroundf(c->slope[0]));
            else snprintf(buf, sizeof(buf), "top model");
            rect(lx, ly + 0.5f, lx + 5.0f, ly + 5.5f, yellow);
            text(lx + 9.0f, ly, LABEL_SCALE, yellow, buf);
            row++;
        } else {
            line(lx + 2.5f, ly, lx + 2.5f, ly + 5.0f, 1.0f, yellow);
            text(lx + 9.0f, ly, LABEL_SCALE, yellow, "new top model");
            row++;
        }
        for (int k = 0; k < c->nseries; k++) {
            const float* sc = SERIES_COL[k];
            uint32_t col = rgba(sc[0], sc[1], sc[2], 1.0f);
            float y = ly + (float)row * 10.0f;
            char buf[96];
            float first = NAN, last = NAN;
            for (int r = 0; r < c->nrows; r++) {
                float v = c->series_v[k][r];
                if (isnan(v)) continue;
                if (isnan(first)) first = v;
                last = v;
            }
            if (c->kind == CHART_ARENA && a->trend_recent > 0.0f) {
                snprintf(buf, sizeof(buf), "%-16s %+d Elo/yr", c->series_name[k], (int)lroundf(c->series_slope[k]));
            } else if (c->kind == CHART_WINRATE && a->trend_all > 0.0f && !isnan(first)) {
                snprintf(buf, sizeof(buf), "%-16s %d%% -> %d%%", c->series_name[k], (int)(first + 0.5f), (int)(last + 0.5f));
            } else {
                snprintf(buf, sizeof(buf), "%s", c->series_name[k]);
            }
            rect(lx, y + 0.5f, lx + 5.0f, y + 5.5f, col);
            text(lx + 9.0f, y, LABEL_SCALE, col, buf);
            row++;
        }
        if (c->kind == CHART_ARENA && last_top > 0.0f) {
            char buf[32];
            snprintf(buf, sizeof(buf), "TOP ELO %d", (int)(last_top + 0.5f));
            text(lx, ly + (float)row * 10.0f + 4.0f, 1.25f, yellow, buf);
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

// ---------------------------------------------------------------------------
//  Math text: a tiny LaTeX-like renderer for formulas on the 8x8 bitmap font.
//  Supports plain ASCII, \theta \sigma \varepsilon \alpha \beta \approx \cdot \times
//  \ge \to \ln, ^{sup}, _{sub}, \frac{num}{den} and {groups}. Greek letters and symbols
//  are drawn as pixel glyphs (matching the KC85 font) into the chart geometry.
// ---------------------------------------------------------------------------
typedef struct { const char* name; const char* rows[8]; } mglyph_t;
static const mglyph_t MGLYPHS[] = {
    { "theta",      { ".####...", "##..##..", "##..##..", "######..", "##..##..", "##..##..", ".####...", "........" } },
    { "sigma",      { "........", "........", ".######.", "##..##..", "##..##..", "##..##..", ".####...", "........" } },
    { "varepsilon", { "........", "........", ".#####..", "##......", ".####...", "##......", ".#####..", "........" } },
    { "epsilon",    { "........", "........", ".#####..", "##......", ".####...", "##......", ".#####..", "........" } },
    { "alpha",      { "........", "........", ".###.##.", "##..##..", "##..##..", "##..##..", ".###.##.", "........" } },
    { "beta",       { ".####...", "##..##..", "##.##...", "##..##..", "##..##..", "#####...", "##......", "##......" } },
    { "approx",     { "........", ".###..#.", "#..###..", "........", ".###..#.", "#..###..", "........", "........" } },
    { "cdot",       { "........", "........", "........", "..##....", "..##....", "........", "........", "........" } },
    { "times",      { "........", "##...##.", ".##.##..", "..###...", ".##.##..", "##...##.", "........", "........" } },
    { "ge",         { "##......", ".###....", "...###..", ".###....", "##......", "........", "######..", "........" } },
    { "to",         { "........", "....#...", "....##..", "#######.", "....##..", "....#...", "........", "........" } },
};

typedef struct { float w, up, down; } mbox_t;

static struct {
    chart_text_fn text;
    uint32_t col;
    bool draw;
    float space;    // width of a typed space, in font pixels
} M;

static mbox_t mbox_char(float sc) { return (mbox_t){ 8.0f * sc, 4.0f * sc, 4.0f * sc }; }

static void mbox_add(mbox_t* a, mbox_t b) {
    a->w += b.w;
    a->up = fmaxf(a->up, b.up);
    a->down = fmaxf(a->down, b.down);
}

// reads one argument: {group}, \command or a single character; returns the end of it
static const char* m_arg(const char* s, const char* e, const char** g0, const char** g1) {
    if (s >= e) { *g0 = *g1 = s; return s; }
    if (*s == '{') {
        int depth = 1;
        const char* p = s + 1;
        while (p < e && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            if (depth > 0) p++;
        }
        *g0 = s + 1; *g1 = p;
        return p < e ? p + 1 : p;
    }
    if (*s == '\\') {
        const char* p = s + 1;
        while (p < e && isalpha((unsigned char)*p)) p++;
        if (p == s + 1 && p < e) p++;
        *g0 = s; *g1 = p;
        return p;
    }
    *g0 = s; *g1 = s + 1;
    return s + 1;
}

static void m_glyph(const mglyph_t* g, float x, float top, float sc) {
    uint32_t shadow = (M.col & 0xFF000000u) | 0x140000u;
    for (int pass = 0; pass < 2; pass++) {
        float o = pass == 0 ? 1.1f * sc : 0.0f;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (g->rows[r][c] == '#') {
                    float px = x + (float)c * sc + o, py = top + (float)r * sc + o;
                    rect(px, py, px + sc, py + sc, pass == 0 ? shadow : M.col);
                }
            }
        }
    }
}

static float m_sub_scale(float sc) { return sc >= 1.5f ? sc - 0.5f : sc * 0.75f; }

// lays out (and optionally draws) [s, e) at scale sc with its math axis at y = axis
static mbox_t m_run(const char* s, const char* e, float sc, float x, float axis) {
    mbox_t box = { 0.0f, 4.0f * sc, 4.0f * sc };
    char run[128];
    int nrun = 0;
    #define FLUSH() do { if (nrun > 0) { run[nrun] = 0; \
        if (M.draw) M.text(x + box.w, axis - 4.0f * sc, sc, M.col, run); \
        mbox_add(&box, (mbox_t){ 8.0f * sc * (float)nrun, 4.0f * sc, 4.0f * sc }); nrun = 0; } } while (0)
    const char* p = s;
    while (p < e) {
        char ch = *p;
        if (ch == ' ') {
            FLUSH();
            box.w += M.space * sc;
            p++;
        } else if (ch == '{') {
            FLUSH();
            const char *g0, *g1;
            p = m_arg(p, e, &g0, &g1);
            mbox_add(&box, m_run(g0, g1, sc, x + box.w, axis));
        } else if (ch == '^' || ch == '_') {
            FLUSH();
            const char *g0, *g1;
            p = m_arg(p + 1, e, &g0, &g1);
            float ss = m_sub_scale(sc);
            float off = (ch == '^') ? -3.6f * sc : 3.2f * sc;
            mbox_t b = m_run(g0, g1, ss, x + box.w, axis + off);
            box.w += b.w + 0.5f * sc;
            if (ch == '^') box.up = fmaxf(box.up, b.up - off);
            else box.down = fmaxf(box.down, b.down + off);
        } else if (ch == '\\') {
            FLUSH();
            const char *g0, *g1;
            p = m_arg(p, e, &g0, &g1);
            char name[24];
            snprintf(name, sizeof(name), "%.*s", (int)(g1 - g0 - 1), g0 + 1);
            if (!strcmp(name, "frac")) {
                const char *n0, *n1, *d0, *d1;
                p = m_arg(p, e, &n0, &n1);
                p = m_arg(p, e, &d0, &d1);
                bool was = M.draw;
                M.draw = false;
                mbox_t nb = m_run(n0, n1, sc, 0.0f, 0.0f), db = m_run(d0, d1, sc, 0.0f, 0.0f);
                M.draw = was;
                float w = fmaxf(nb.w, db.w) + 2.0f * sc, gap = 1.6f * sc;
                if (M.draw) {
                    m_run(n0, n1, sc, x + box.w + (w - nb.w) * 0.5f, axis - gap - nb.down);
                    m_run(d0, d1, sc, x + box.w + (w - db.w) * 0.5f, axis + gap + db.up);
                    rect(x + box.w + 1.1f * sc, axis - 0.4f * sc + 1.1f * sc, x + box.w + w + 1.1f * sc, axis + 0.4f * sc + 1.1f * sc,
                         (M.col & 0xFF000000u) | 0x140000u);
                    rect(x + box.w, axis - 0.4f * sc, x + box.w + w, axis + 0.4f * sc, M.col);
                }
                mbox_add(&box, (mbox_t){ w + sc, gap + nb.down + nb.up, gap + db.up + db.down });
            } else if (!strcmp(name, "ln")) {
                if (M.draw) M.text(x + box.w, axis - 4.0f * sc, sc, M.col, "ln");
                mbox_add(&box, (mbox_t){ 16.0f * sc, 4.0f * sc, 4.0f * sc });
            } else if (!strcmp(name, ",")) {
                box.w += 2.0f * sc;
            } else {
                const mglyph_t* g = NULL;
                for (size_t k = 0; k < sizeof(MGLYPHS) / sizeof(MGLYPHS[0]); k++) {
                    if (!strcmp(MGLYPHS[k].name, name)) g = &MGLYPHS[k];
                }
                if (g) {
                    if (M.draw) m_glyph(g, x + box.w, axis - 4.0f * sc, sc);
                    mbox_add(&box, mbox_char(sc));
                } else {   // unknown command: show its name
                    if (M.draw) M.text(x + box.w, axis - 4.0f * sc, sc, M.col, name);
                    mbox_add(&box, (mbox_t){ 8.0f * sc * (float)strlen(name), 4.0f * sc, 4.0f * sc });
                }
            }
        } else if (ch == '}') {
            p++;
        } else {
            if (nrun < (int)sizeof(run) - 1) run[nrun++] = ch;
            p++;
        }
    }
    FLUSH();
    #undef FLUSH
    return box;
}

static mbox_t m_measure(const char* s, float sc) {
    bool was = M.draw;
    M.draw = false;
    mbox_t b = m_run(s, s + strlen(s), sc, 0.0f, 0.0f);
    M.draw = was;
    return b;
}

// draws math text with its top-left corner at (x, top); returns its box
static mbox_t m_draw(const char* s, float sc, float x, float top, uint32_t col) {
    mbox_t b = m_measure(s, sc);
    M.col = col;
    M.draw = true;
    m_run(s, s + strlen(s), sc, x, top + b.up);
    M.draw = false;
    return b;
}

// word-wrapped math text inside [x, x + maxw); returns the height used
static float m_para(const char* s, float sc, float x, float top, float maxw, uint32_t col) {
    char line[256] = "", word[128];
    float y = top, lh = 11.0f * sc;
    float saved_space = M.space;
    M.space = 7.0f;
    const char* p = s;
    while (true) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* w0 = p;
        int depth = 0;
        while (*p && (depth > 0 || *p != ' ')) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        snprintf(word, sizeof(word), "%.*s", (int)(p - w0), w0);
        char trial[256];
        snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && m_measure(trial, sc).w > maxw) {
            m_draw(line, sc, x, y, col);
            y += lh;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", trial);
        }
    }
    if (line[0]) {
        m_draw(line, sc, x, y, col);
        y += lh;
    }
    M.space = saved_space;
    return y - top;
}

// ---------------------------------------------------------------------------
//  Theory panels: from step ability to task success (Rasch step model + O-ring task)
// ---------------------------------------------------------------------------
static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }
static float horizon50(float m) { return logf(0.5f) / logf(sigmoid(m)); }   // exact D50

bool chart_theory(chart_data_t* c, const char* which) {
    memset(c, 0, sizeof(*c));
    c->kind = CHART_THEORY;
    if (!strcmp(which, "step")) {
        c->theory = THEORY_STEP;
        c->x0 = -6.0f; c->x1 = 6.0f; c->y0 = 0.0f; c->y1 = 1.0f;
        c->sweep_rate = 4.0f;
    } else if (!strcmp(which, "task")) {
        c->theory = THEORY_TASK;
        c->x0 = 0.0f; c->x1 = 3.0f; c->y0 = 0.0f; c->y1 = 1.0f;   // x = log10 D
        c->sweep_rate = 1.0f;
    } else if (!strcmp(which, "horizon")) {
        c->theory = THEORY_HORIZON;
        c->x0 = 0.0f; c->x1 = 6.0f; c->y0 = 0.0f; c->y1 = 300.0f;
        c->sweep_rate = 2.0f;
    } else {
        return false;
    }
    return true;
}

static int build_theory(const chart_data_t* c, const chart_anim_t* a, const float r[4], chart_text_fn text) {
    M.text = text;
    M.space = 4.0f;
    const float W = r[2] - r[0];
    const uint32_t white = rgba(1.0f, 1.0f, 1.0f, 1.0f), formula = rgba(1.0f, 0.90f, 0.45f, 1.0f);
    const uint32_t caption = rgba(0.78f, 0.84f, 1.0f, 1.0f), dim = rgba(0.70f, 0.76f, 0.92f, 1.0f);
    const uint32_t grid = rgba(0.20f, 0.30f, 0.60f, 0.55f), axis = rgba(0.70f, 0.78f, 0.95f, 1.0f);
    const uint32_t orange = rgba(1.0f, 0.55f, 0.15f, 1.0f), curve = rgba(0.35f, 0.80f, 1.0f, 1.0f);
    const uint32_t dash = rgba(0.65f, 0.70f, 0.85f, 0.65f);
    (void)white;

    // left: formulas and the explanation; right: the plot
    const float lx = r[0] + 6.0f, lw = W * 0.42f - 10.0f;
    const float pr[4] = { r[0] + W * 0.42f + 30.0f, r[1] + 16.0f, r[2] - 8.0f, r[3] - 28.0f };
    const float cur = a->cursor;
    #define X(t) (pr[0] + ((t) - c->x0) / (c->x1 - c->x0) * (pr[2] - pr[0]))
    #define Y(v) (pr[3] - ((v) - c->y0) / (c->y1 - c->y0) * (pr[3] - pr[1]))

    rect(pr[0], pr[1], pr[2], pr[3], rgba(0.05f, 0.09f, 0.27f, 1.0f));
    float ly = r[1] + 4.0f;
    const char *f1 = "", *f2 = "", *cap = "", *foot = NULL, *xlab = "", *ylab = "";
    switch (c->theory) {
        case THEORY_STEP:
            f1 = "p = \\sigma(\\theta - b)";
            f2 = "  = \\frac{1}{1 + e^{-(\\theta - b)}}";
            cap = "\\theta is ability, b the step's difficulty. Only the margin {\\theta - b} matters. "
                  "Odds grow \\times e per unit of \\theta, but p never passes 1.";
            xlab = "\\theta - b  (ability - difficulty)";
            ylab = "p, step success";
            break;
        case THEORY_TASK:
            f1 = "P(D) = p^{D} \\approx e^{-\\varepsilon D}";
            f2 = "\\varepsilon = 1 - p \\approx e^{-(\\theta - b)}";
            cap = "Every one of the D steps must succeed. Lines show {\\theta - b = 2} to 5, "
                  "brighter is more able, each \\times e further right.";
            xlab = "D, task length (log scale)";
            ylab = "P(D), task success";
            break;
        default:
            f1 = "D_{50} = \\frac{\\ln 2}{\\varepsilon}";
            f2 = "    \\approx \\ln 2 \\cdot e^{\\theta - b}";
            cap = "Each +1 in ability multiplies the reliable length by {e \\approx 2.7:} "
                  "14 steps at 3, 38 at 4, 280 at 6.";
            foot = "Tail approximation {1 - \\sigma(x) \\approx e^{-x}} is off by under 14% once {x \\ge 2.} "
                   "METR's fit {P = \\sigma(\\alpha - \\beta \\ln t)} has the same structure, with ln t as difficulty.";
            xlab = "\\theta - b  (ability - difficulty)";
            ylab = "D_{50}, steps at 50% success";
            break;
    }
    mbox_t b1 = m_draw(f1, 1.5f, lx, ly, formula);
    ly += b1.up + b1.down + 8.0f;
    mbox_t b2 = m_draw(f2, 1.5f, lx, ly, formula);
    ly += b2.up + b2.down + 14.0f;
    ly += m_para(cap, 1.0f, lx, ly, lw, caption);
    if (foot) {
        m_para(foot, 0.75f, lx, fmaxf(ly + 8.0f, r[3] - 46.0f), lw, dim);
    }

    // axes, ticks and titles
    M.space = 7.0f;
    mbox_t yb = m_measure(ylab, 0.75f);
    m_draw(ylab, 0.75f, pr[0] - 26.0f, pr[1] - 7.0f - yb.up - yb.down, dim);
    mbox_t xb = m_measure(xlab, 0.75f);
    m_draw(xlab, 0.75f, (pr[0] + pr[2]) * 0.5f - xb.w * 0.5f, pr[3] + 15.0f, dim);
    const float* yt;
    const char** yl;
    static const float yt_p[3] = { 0.0f, 0.5f, 1.0f }, yt_h[3] = { 0.0f, 150.0f, 300.0f };
    static const char* yl_p[3] = { "0", "0.5", "1" };
    static const char* yl_h[3] = { "0", "150", "300" };
    yt = (c->theory == THEORY_HORIZON) ? yt_h : yt_p;
    yl = (c->theory == THEORY_HORIZON) ? yl_h : yl_p;
    for (int i = 0; i < 3; i++) {
        float y = Y(yt[i]);
        line(pr[0], y, pr[2], y, 0.6f, grid);
        text(pr[0] - 4.0f - (float)strlen(yl[i]) * 8.0f, y - 4.0f, 1.0f, dim, yl[i]);
    }
    static const float xt_step[3] = { -6.0f, 0.0f, 6.0f }, xt_task[4] = { 0.0f, 1.0f, 2.0f, 3.0f }, xt_hor[4] = { 0.0f, 2.0f, 4.0f, 6.0f };
    static const char* xl_step[3] = { "-6", "0", "6" };
    static const char* xl_task[4] = { "1", "10", "100", "1,000" };
    static const char* xl_hor[4] = { "0", "2", "4", "6" };
    const float* xt = c->theory == THEORY_STEP ? xt_step : c->theory == THEORY_TASK ? xt_task : xt_hor;
    const char** xl = c->theory == THEORY_STEP ? xl_step : c->theory == THEORY_TASK ? xl_task : xl_hor;
    int nxt = c->theory == THEORY_STEP ? 3 : 4;
    for (int i = 0; i < nxt; i++) {
        float x = X(xt[i]);
        line(x, pr[1], x, pr[3], 0.6f, grid);
        line(x, pr[3], x, pr[3] + 3.0f, 1.0f, axis);
        float tw = (float)strlen(xl[i]) * 8.0f;
        text(fminf(fmaxf(x - tw * 0.5f, pr[0] - 6.0f), pr[2] - tw + 4.0f), pr[3] + 5.0f, 1.0f, dim, xl[i]);
    }
    line(pr[0], pr[1], pr[0], pr[3], 1.2f, axis);
    line(pr[0], pr[3], pr[2], pr[3], 1.2f, axis);

    M.space = 7.0f;
    const int SEG = 160;
    float xe = fminf(cur, c->x1);
    if (c->theory == THEORY_STEP) {
        dashed(pr[0], Y(1.0f), pr[2], Y(1.0f), 1.0f, 4.0f, 3.0f, dash);   // the ceiling
        dashed(X(0.0f), pr[1], X(0.0f), pr[3], 1.0f, 4.0f, 3.0f, dash);
        for (int i = 0; i < SEG; i++) {
            float t0 = c->x0 + (c->x1 - c->x0) * (float)i / SEG, t1 = c->x0 + (c->x1 - c->x0) * (float)(i + 1) / SEG;
            if (t0 >= xe) break;
            t1 = fminf(t1, xe);
            line(X(t0), Y(sigmoid(t0)), X(t1), Y(sigmoid(t1)), 2.0f, curve);
        }
        if (a->trend_all > 0.0f) {   // the 50% point at theta = b
            float s = ease_out_back(fminf(a->trend_all * 2.0f, 1.0f)) * 3.0f;
            float x = X(0.0f), y = Y(0.5f);
            rect(x - s, y - s, x + s, y + s, orange);
            uint32_t col = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_all * 3.0f, 1.0f) * 255.0f) << 24);
            m_draw("\\theta = b: p = 0.5", 1.0f, x + 7.0f, y + 2.0f, col);
        }
        if (a->trend_recent > 0.0f) {   // odds x e per unit, yet p stays below 1
            uint32_t col = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_recent * 3.0f, 1.0f) * 255.0f) << 24);
            static const char* odds[3] = { "odds e", "e^{2}", "e^{3}" };
            for (int k = 1; k <= 3; k++) {
                if (a->trend_recent * 3.0f < (float)k - 1.0f) break;
                float x = X((float)k), y = Y(sigmoid((float)k));
                rect(x - 2.0f, y - 2.0f, x + 2.0f, y + 2.0f, col);
                mbox_t ob = m_measure(odds[k - 1], 0.75f);
                m_draw(odds[k - 1], 0.75f, x - ob.w - 3.0f, y - ob.up - ob.down - 2.0f, col);
            }
            m_draw("p < 1, always", 0.75f, X(-5.6f), Y(1.0f) + 5.0f, col);
        }
    } else if (c->theory == THEORY_TASK) {
        static const float shade[4][3] = {
            { 0.38f, 0.52f, 0.95f }, { 0.36f, 0.64f, 1.0f }, { 0.40f, 0.82f, 1.0f }, { 0.72f, 0.97f, 1.0f },
        };
        dashed(pr[0], Y(1.0f), pr[2], Y(1.0f), 1.0f, 4.0f, 3.0f, dash);
        if (a->trend_all > 0.0f) {   // the 50% level grows in from the left
            dashed(pr[0], Y(0.5f), pr[0] + (pr[2] - pr[0]) * a->trend_all, Y(0.5f), 1.2f, 5.0f, 3.5f, orange);
        }
        for (int k = 0; k < 4; k++) {
            float m = (float)(k + 2), p = sigmoid(m);
            uint32_t col = rgba(shade[k][0], shade[k][1], shade[k][2], 1.0f);
            for (int i = 0; i < SEG; i++) {
                float t0 = c->x0 + (c->x1 - c->x0) * (float)i / SEG, t1 = c->x0 + (c->x1 - c->x0) * (float)(i + 1) / SEG;
                if (t0 >= xe) break;
                t1 = fminf(t1, xe);
                line(X(t0), Y(powf(p, powf(10.0f, t0))), X(t1), Y(powf(p, powf(10.0f, t1))), 2.0f, col);
            }
            // legend in the empty bottom-left corner
            char buf[32];
            snprintf(buf, sizeof(buf), "\\theta - b = %d", k + 2);
            m_draw(buf, 0.75f, pr[0] + 8.0f, pr[3] - 44.0f + (float)k * 9.0f, col);
            float d50 = horizon50(m), xd = log10f(d50);
            float shown = (xd - c->x0) / (c->x1 - c->x0);
            if (a->trend_all >= shown && xd <= cur) {
                float x = X(xd), y = Y(0.5f);
                rect(x - 3.0f, y - 3.0f, x + 3.0f, y + 3.0f, orange);
                snprintf(buf, sizeof(buf), d50 < 10.0f ? "%.1f" : "%.0f", d50);
                text(x - (float)strlen(buf) * 3.0f, y + 6.0f, 0.75f, orange, buf);
                if (a->trend_recent > 0.0f && k > 0) {   // each step of ability: x e further right
                    float xp = X(log10f(horizon50(m - 1.0f)));
                    uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_recent * 2.0f, 1.0f) * 255.0f) << 24);
                    mbox_t tb = m_measure("\\times e", 0.75f);
                    m_draw("\\times e", 0.75f, (x + xp) * 0.5f - tb.w * 0.5f, y - 13.0f, tc);
                }
            }
        }
    } else {
        for (int i = 0; i < SEG; i++) {
            float t0 = c->x0 + (c->x1 - c->x0) * (float)i / SEG, t1 = c->x0 + (c->x1 - c->x0) * (float)(i + 1) / SEG;
            if (t0 >= xe) break;
            t1 = fminf(t1, xe);
            float v0 = t0 < 0.01f ? 1.0f : horizon50(t0), v1 = horizon50(fmaxf(t1, 0.01f));
            line(X(t0), Y(v0), X(t1), Y(fminf(v1, c->y1)), 2.0f, curve);
        }
        for (int k = 2; k <= 6; k++) {
            float m = (float)k, d50 = horizon50(m);
            float shown = (m - c->x0) / (c->x1 - c->x0);
            if (a->trend_all < shown * 0.999f) continue;
            float x = X(m), y = Y(d50);
            rect(x - 3.0f, y - 3.0f, x + 3.0f, y + 3.0f, orange);
            char buf[16];
            snprintf(buf, sizeof(buf), d50 < 10.0f ? "%.1f" : "%.0f", d50);
            float tw = (float)strlen(buf) * 8.0f;
            text(k == 6 ? x - tw - 6.0f : x - tw * 0.5f, k == 6 ? y - 4.0f : y - 13.0f, 1.0f, orange, buf);
            if (a->trend_recent > 0.0f && k >= 4) {
                float xp = X(m - 1.0f), yp = Y(horizon50(m - 1.0f));
                uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_recent * 2.0f, 1.0f) * 255.0f) << 24);
                m_draw("\\times e", 0.75f, (x + xp) * 0.5f + 4.0f, (y + yp) * 0.5f + 2.0f, tc);
            }
        }
    }
    // sweep line
    float cur_target, ta, tr;
    step_targets(c, a->step, &cur_target, &ta, &tr);
    if (cur > c->x0 && cur < cur_target - 1e-3f) {
        float x = X(cur);
        line(x, pr[1], x, pr[3], 1.2f, rgba(0.40f, 0.95f, 1.0f, 0.8f));
    }
    #undef X
    #undef Y
    return B.n;
}

// ---------------------------------------------------------------------------
//  Elo leaderboard: horizontal bars that grow with the sweep, a human reference line,
//  then each model's implied win rate against a human expert
// ---------------------------------------------------------------------------
static int build_bars(const chart_data_t* c, const chart_anim_t* a, const float r[4], chart_text_fn text) {
    const float ls = LABEL_SCALE, cw = 8.0f * ls;
    float name_w = 0.0f;
    for (int i = 0; i < c->n; i++) {
        if (!c->pts[i].hidden) name_w = fmaxf(name_w, (float)strlen(c->pts[i].label) * cw);
    }
    const float pr[4] = { r[0] + name_w + 8.0f, r[1] + 12.0f, r[2] - 26.0f, r[3] - 14.0f };
    #define X(v) (pr[0] + ((v) - c->x0) / (c->x1 - c->x0) * (pr[2] - pr[0]))
    const uint32_t grid = rgba(0.20f, 0.30f, 0.60f, 0.55f), axis = rgba(0.70f, 0.78f, 0.95f, 1.0f);
    const uint32_t dim = rgba(0.70f, 0.76f, 0.92f, 1.0f), orange = rgba(1.0f, 0.62f, 0.20f, 1.0f);
    rect(pr[0], pr[1], pr[2], pr[3], rgba(0.05f, 0.09f, 0.27f, 1.0f));
    for (float v = ceilf(c->x0 / c->ystep) * c->ystep; v <= c->x1 + 0.01f; v += c->ystep) {
        float x = X(v);
        char buf[8];
        snprintf(buf, sizeof(buf), c->pct ? "%d%%" : "%d", (int)v);
        line(x, pr[1], x, pr[3], 0.6f, grid);
        line(x, pr[3], x, pr[3] + 3.0f, 1.0f, axis);
        text(x - (float)strlen(buf) * 4.0f, pr[3] + 5.0f, 1.0f, dim, buf);
    }
    line(pr[0], pr[1], pr[0], pr[3], 1.2f, axis);
    line(pr[0], pr[3], pr[2], pr[3], 1.2f, axis);

    int shown = 0;
    for (int i = 0; i < c->n; i++) shown += c->pts[i].hidden ? 0 : 1;
    const float row = (pr[3] - pr[1] - 2.0f) / (float)(shown > 0 ? shown : 1);
    const float bh = fminf(row * 0.72f, 7.0f);
    // annotation 1: humans as the reference
    if (!c->pct && a->trend_all > 0.0f) {
        float x = X(HUMAN_ELO), yb = pr[1] + (pr[3] - pr[1]) * a->trend_all;
        rect(pr[0], pr[1], x, yb, rgba(1.0f, 0.62f, 0.20f, 0.10f));
    }
    int k = 0;
    for (int i = 0; i < c->n; i++) {
        const chart_point_t* p = &c->pts[i];
        if (p->hidden) continue;
        float yc = pr[1] + 1.0f + ((float)k + 0.5f) * row;
        k++;
        uint32_t col = p->color ? p->color : rgba(0.35f, 0.80f, 1.0f, 1.0f);
        if (p->group == 3) {   // the human baseline: revealed with the human line, hatched bar
            if (a->trend_all <= 0.0f) continue;
            float ha = fminf(a->trend_all * 2.0f, 1.0f);
            uint32_t hc = (orange & 0x00FFFFFFu) | ((uint32_t)(ha * 255.0f) << 24);
            text(pr[0] - 6.0f - (float)strlen(p->label) * cw, yc - cw * 0.5f, ls, hc, p->label);
            bool off = c->pct;   // no published score: the bar runs off the scale
            float xe = pr[0] + ((off ? pr[2] + 4.0f : X(p->v)) - pr[0]) * fminf(a->trend_all * 1.5f, 1.0f);
            const float hb[4] = { pr[0], yc - bh * 0.5f, xe, yc + bh * 0.5f };
            for (float k2 = -bh; k2 < xe - pr[0]; k2 += 3.0f) {
                float ax = pr[0] + k2, ay = hb[3], bx = ax + bh, by = hb[1];
                if (clip(&ax, &ay, &bx, &by, hb)) line(ax, ay, bx, by, 0.8f, hc);
            }
            line(pr[0], hb[1], xe, hb[1], 0.8f, hc);
            line(pr[0], hb[3], xe, hb[3], 0.8f, hc);
            if (off) {
                if (a->trend_all >= 0.67f) {   // arrow head past the axis end + note on the bar
                    float ax = pr[2] + 4.0f, ym = yc;
                    for (int k3 = 0; k3 < 5; k3++) {
                        float h3 = (5.0f - (float)k3) * 1.0f;
                        rect(ax + (float)k3, ym - h3, ax + (float)k3 + 1.0f, ym + h3, hc);
                    }
                    const char* note = " real expert work, no published score ";
                    float tw = (float)strlen(note) * cw, tx = (pr[0] + pr[2]) * 0.5f - tw * 0.5f;
                    rect(tx, yc - cw * 0.5f - 1.5f, tx + tw, yc + cw * 0.5f + 1.5f, rgba(0.05f, 0.09f, 0.27f, ha));
                    text(tx, yc - cw * 0.5f, ls, hc, note);
                }
                continue;
            }
            line(xe, hb[1], xe, hb[3], 0.8f, hc);
            if (a->trend_all >= 0.67f) {
                char buf[24];
                snprintf(buf, sizeof(buf), "%d", (int)(p->v + 0.5f));
                text(X(p->v) + 3.0f, yc - cw * 0.5f, ls, hc, buf);
                if (a->trend_recent > 0.0f) text(X(p->v) + 3.0f + 5.0f * cw, yc - cw * 0.5f, ls, hc, "50%");
            }
            continue;
        }
        text(pr[0] - 6.0f - (float)strlen(p->label) * cw, yc - cw * 0.5f, ls, rgba(1.0f, 1.0f, 1.0f, 1.0f), p->label);
        float ve = fminf(p->v, a->cursor);
        if (ve > c->x0) {
            float xe = X(ve);
            rect(pr[0], yc - bh * 0.5f + 0.8f, xe + 0.8f, yc + bh * 0.5f + 0.8f, rgba(0.0f, 0.02f, 0.08f, 0.8f));
            rect(pr[0], yc - bh * 0.5f, xe, yc + bh * 0.5f, col);
        }
        if (a->cursor >= p->v) {   // the bar has stopped: show its rating
            char buf[24];
            if (c->pct) snprintf(buf, sizeof(buf), "%.1f%%", p->v);
            else snprintf(buf, sizeof(buf), "%d", (int)(p->v + 0.5f));
            text(X(p->v) + 3.0f, yc - cw * 0.5f, ls, rgba(1.0f, 1.0f, 1.0f, 1.0f), buf);
            if (c->pct && a->trend_all > 0.0f) {   // cost per task
                uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_all * 2.0f, 1.0f) * 255.0f) << 24);
                if (p->cost >= 0.0f) snprintf(buf, sizeof(buf), p->cost < 10.0f ? "$%.2f" : "$%.0f", p->cost);
                else snprintf(buf, sizeof(buf), "no price");
                text(X(p->v) + 3.0f + 6.0f * cw, yc - cw * 0.5f, ls, tc, buf);
            }
            if (!c->pct && a->trend_recent > 0.0f) {   // implied win rate against a human expert (Elo, 400 scale)
                float w = 100.0f / (1.0f + powf(10.0f, -(p->v - HUMAN_ELO) / 400.0f));
                uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_recent * 2.0f, 1.0f) * 255.0f) << 24);
                snprintf(buf, sizeof(buf), "%d%%", (int)(w + 0.5f));
                text(X(p->v) + 3.0f + 5.0f * cw, yc - cw * 0.5f, ls, tc, buf);
            }
        }
    }
    if (c->pct && a->trend_all > 0.0f) {
        uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_all * 2.0f, 1.0f) * 255.0f) << 24);
        const char* h = "$ = average cost per task";
        text(pr[2] - (float)strlen(h) * cw, pr[1] - 10.0f, ls, tc, h);
    }
    if (!c->pct && a->trend_all > 0.0f) {   // the human line on top of the bars
        float x = X(HUMAN_ELO), yb = pr[1] + (pr[3] - pr[1]) * a->trend_all;
        dashed(x + 0.8f, pr[1], x + 0.8f, yb, 1.8f, 4.0f, 3.0f, rgba(0.0f, 0.02f, 0.08f, 0.8f));
        dashed(x, pr[1], x, yb, 1.8f, 4.0f, 3.0f, orange);
        uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_all * 2.0f, 1.0f) * 255.0f) << 24);
        const char* t = "human expert = 1000";
        text(fmaxf(x - (float)strlen(t) * 4.0f, r[0] + 2.0f), pr[1] - 10.0f, 1.0f, tc, t);
    }
    if (!c->pct && a->trend_recent > 0.0f) {
        uint32_t tc = (orange & 0x00FFFFFFu) | ((uint32_t)(fminf(a->trend_recent * 2.0f, 1.0f) * 255.0f) << 24);
        const char* h = "% = win rate vs. human";
        text(pr[2] - (float)strlen(h) * cw, pr[1] - 10.0f, ls, tc, h);
    }
    // sweep line
    float cur_target, ta, tr;
    step_targets(c, a->step, &cur_target, &ta, &tr);
    if (a->cursor > c->x0 && a->cursor < cur_target - 1e-3f) {
        float x = X(a->cursor);
        line(x, pr[1], x, pr[3], 1.2f, rgba(0.40f, 0.95f, 1.0f, 0.8f));
    }
    #undef X
    return B.n;
}
