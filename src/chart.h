// Native, animated benchmark charts rendered in the retro style:
//  - METR time horizons (benchmark_results YAML): log-scale 50% horizon vs. release date
//  - Epoch Capabilities Index (eci_scores.csv): ECI vs. release date, closed vs. open weights
//  - LMArena anchor ratings (date,top_model,top_model_rating,<anchor models...>): the top
//    model's Elo over time vs. the (flat) Elo of a few anchor models
//  - leaderboards as horizontal bars: model,elo[,lab,color] (e.g. GDPval-AA, compared with a
//    human reference rating of 1000) or model,score[,lab,color,cost] (% score, e.g. GDPx)
//  - LMArena anchor win rates (same layout, '<model> win %' columns): each anchor model's
//    expected win rate against the current top model
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CHART_MAX_POINTS 320
#define CHART_MAX_STOPS  8
#define CHART_MAX_ROWS   400
#define CHART_MAX_SERIES 6

typedef enum { CHART_METR, CHART_ECI, CHART_ARENA, CHART_WINRATE, CHART_THEORY, CHART_BARS } chart_kind_t;
// theory panels (step success -> task success -> 50% horizon), see chart_theory()
enum { THEORY_STEP, THEORY_TASK, THEORY_HORIZON };

typedef struct {
    char id[64];
    char label[48];
    float t;                // release date as decimal year
    float v, lo, hi;        // value + CI (METR: minutes, ECI: index points)
    int group;              // ECI: 0 closed weights, 1 open weights, 2 other; METR: 0
    float jump;             // ECI: improvement over the previous frontier point of the group
    float lag_t;            // ECI open frontier: when closed weights first reached this value (0 = n/a)
    bool frontier;          // METR: SOTA flag; ECI: new best of its group when released
    bool hidden, has_ci;
    uint32_t color;         // BARS: bar colour (RGBA8, 0 = default)
    float cost;             // BARS: $ per task (< 0 = unknown)
} chart_point_t;

typedef struct {
    int kind;
    chart_point_t pts[CHART_MAX_POINTS];
    int n;
    float x0, x1, y0, y1;   // axis ranges (y in data units)
    bool ylog;
    int max_labels;         // 0 = unlimited
    // METR
    float doubling_all, doubling_recent;     // days (0 = unknown)
    // ECI
    float slope[2], icpt[2];                 // linear frontier trends (closed, open) from 2024 on
    bool has_trend[2];
    float lag_months;                        // mean open-weights lag since 2024
    // time series (ARENA / WINRATE): one row per date
    int nrows, nseries;
    float row_t[CHART_MAX_ROWS];
    float top_v[CHART_MAX_ROWS];                          // top model rating
    float series_v[CHART_MAX_SERIES][CHART_MAX_ROWS];     // anchor models (NAN = not yet released)
    char series_name[CHART_MAX_SERIES][48];
    float series_slope[CHART_MAX_SERIES];                 // ARENA: Elo/yr after the first months
    float series_icpt[CHART_MAX_SERIES], series_from[CHART_MAX_SERIES];
    int theory;                                           // THEORY_* panel
    bool pct;                                             // BARS: percent score (else Elo)
    bool has_cost;                                        // BARS: a cost column was given
    float ystep;                                          // grid spacing (0 = 10)
    float sweep_rate;                                     // years per second (0 = default)
    // build steps
    float stops[CHART_MAX_STOPS];
    int nstops;
} chart_data_t;

typedef struct {
    int step;               // 0 = empty axes, then sweep stops, full sweep, annotation steps
    float cursor;           // reveal cursor (decimal year)
    float trend_all, trend_recent;  // 0..1 growth of the first / second annotation step
} chart_anim_t;

typedef struct {
    float x, y;             // layout units
    uint32_t color;         // RGBA8
} chart_vert_t;

typedef void (*chart_text_fn)(float x, float y, float scale, uint32_t rgba, const char* s);

// loads a METR benchmark_results .yaml or an Epoch eci_scores .csv (chosen by extension)
bool chart_load(chart_data_t* c, const char* path);
// sets up one of the "from step ability to task success" theory panels:
// "step" (sigmoid), "task" (P(D) = p^D) or "horizon" (D50 vs ability)
bool chart_theory(chart_data_t* c, const char* which);
// model lookup by id or by displayed label
bool chart_set_label(chart_data_t* c, const char* id, const char* label);
bool chart_hide(chart_data_t* c, const char* id);

int chart_num_steps(const chart_data_t* c);
void chart_anim_reset(chart_anim_t* a, const chart_data_t* c);
void chart_anim_update(chart_anim_t* a, const chart_data_t* c, float dt);
void chart_anim_snap(chart_anim_t* a, const chart_data_t* c);   // jump to the end state of a->step

// emits triangles (returns the vertex count) and text for the chart inside 'rect' (x0,y0,x1,y1)
int chart_build(const chart_data_t* c, const chart_anim_t* a, const float rect[4],
                chart_vert_t* out, int max_verts, chart_text_fn text);
