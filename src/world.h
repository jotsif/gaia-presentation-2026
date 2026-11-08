// The highway track, terrain and static scenery.
#pragma once
#include "mathx.h"
#include "mesh.h"

// --- road cross-section (meters, measured from the median centre line) ---
#define LANE_WIDTH      3.6f
#define MEDIAN_HALF     1.0f
#define ROAD_HALF       10.2f    // centre line -> outer shoulder edge
#define LANE_INNER_LAT  (MEDIAN_HALF + LANE_WIDTH * 0.5f)          // 2.8  (fast lane)
#define LANE_OUTER_LAT  (MEDIAN_HALF + LANE_WIDTH * 1.5f)          // 6.4  (slow lane)

// --- sky / lighting (shared by the sky texture generator and the renderer) ---
#define SKY_RADIUS      900.0f
#define SKY_TOP         700.0f
#define SKY_BOTTOM      (-150.0f)
#define SUN_U           0.62f     // horizontal position of the sun on the sky cylinder [0..1]
#define SUN_ELEV_DEG    24.0f
#define FOG_R           0.71f
#define FOG_G           0.79f
#define FOG_B           0.90f
#define FOG_START       160.0f
#define FOG_END         1250.0f

// decal texture atlas: 256x128 slots stacked vertically
#define ATLAS_SLOTS     16
enum {
    DECAL_SOKOL,            // billboards
    DECAL_RSI,
    DECAL_GAIA,
    DECAL_HORIZON,
    DECAL_EXIT_GAIABY,      // overhead gantry signs
    DECAL_CAMPUS_EXIT,
    DECAL_SERVICE,
    DECAL_TOWN,             // village name sign (entry)
    DECAL_TOWN_END,         // village name sign, crossed out (exit)
    DECAL_AGENTS,           // billboard
    DECAL_CAMPUS_WALL,      // data centre facade
    DECAL_SINGULARITY,      // gantry
    DECAL_MOUNTAIN,         // gantry
    DECAL_TOKENS,           // billboard
    DECAL_COUNT
};

// scenery zones along the loop
enum { ZONE_COUNTRY, ZONE_FOREST, ZONE_VILLAGE, ZONE_MOUNTAIN, ZONE_CAMPUS, ZONE_FARM, ZONE_COUNT };

#define MAX_TURBINES    24
typedef struct {
    vec3 hub;       // rotor centre
    vec3 axis;      // horizontal rotor axis (direction the rotor faces)
    float phase;
} turbine_t;

typedef struct {
    vec3 pos;      // centre line position (incl. road height)
    vec3 tan;      // 3D tangent (follows road slope)
    vec3 tanh;     // horizontal tangent
    vec3 right;    // horizontal right vector
} track_frame_t;

typedef struct {
    mesh_t terrain;    // indexed, detail texture
    mesh_t road;       // road texture
    mesh_t props;      // detail texture (barriers, rails, lamps, trees, bridges ...)
    mesh_t decals;     // atlas texture (billboards, overhead signs)
    mesh_t sky;        // sky texture, drawn around the camera
    mesh_t blades;     // wind turbine rotor (unit placement, drawn per turbine with rotation)
} world_meshes_t;

void track_init(void);
float track_length(void);
float track_dash_period(void);
track_frame_t track_frame(float s);
// distance (xz) from a point to the centre line, the road height there, the track position
// and the side of the road (+1 right of the track direction, -1 left); outputs may be NULL
void track_nearest(float x, float z, float* out_dist, float* out_road_y, float* out_s, float* out_side);
// weight (0..1) of a scenery zone at track position s
float zone_weight(float s, int zone);
// final terrain height at a point (natural terrain blended into the road embankment)
float terrain_height(float x, float z, float* out_dist);
vec3 sun_direction(void);

void world_build(world_meshes_t* w);
// wind turbines placed by world_build (their rotors are animated by the renderer)
int world_turbines(const turbine_t** out);
void world_free(world_meshes_t* w);
