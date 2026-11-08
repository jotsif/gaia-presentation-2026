// Native, animated benchmark charts rendered in the retro style:
//  - METR time horizons (benchmark_results YAML): log-scale 50% horizon vs. release date
//  - Epoch Capabilities Index (eci_scores.csv): ECI vs. release date, closed vs. open weights
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CHART_MAX_POINTS 320
#define CHART_MAX_STOPS  8

typedef enum { CHART_METR, CHART_ECI } chart_kind_t;

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
