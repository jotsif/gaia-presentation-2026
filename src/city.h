// The future city ("phase two" of the presentation): a procedurally generated megacity
// around the same loop. The highway becomes a neon skyway on pylons, towers stand on a
// flat street grid, flying cars use sky lanes above the road and a giant spire marks the
// singularity in the middle of the loop.
#pragma once
#include "mathx.h"
#include "mesh.h"

#define CITY_BLOCK      64.0f    // street grid; each tower stays inside its block (rise anchor)
#define CITY_MAX_PODS   64
#define CITY_SPIRE_TOP  720.0f   // spire height above the city ground

typedef struct {
    mesh_t ground;      // indexed street grid (city ground texture)
    mesh_t road;        // skyway surface (road texture)
    mesh_t deck;        // detail texture: deck, pylons, barriers, neon rails, lamps, gantries
    mesh_t deck_decals; // atlas: gantry signs
    mesh_t towers;      // facade texture (towers rise out of the ground per block)
    mesh_t trim;        // detail texture: crowns, neon bands, antennas, sky bridges
    mesh_t decals;      // atlas: holographic ads on towers
    mesh_t spire;       // facade texture: the singularity spire (rises as one piece)
    mesh_t spire_trim;  // detail texture: its glowing rings, core and needle
    mesh_t pods;        // flying cars, two variants (local +z forward, +y up)
    mesh_t curtain;     // unit cylinder (radius 1, height 1) for the wave front
    int pod_first[2], pod_count[2];
} city_meshes_t;

typedef struct {
    float s0;           // track position at time 0
    float lat;          // lateral offset from the centre line (m)
    float height;       // height above the road
    float speed;        // m/s along the track (sign = direction)
    float phase;
    int variant;
} city_pod_t;

void city_build(city_meshes_t* c);
void city_free(city_meshes_t* c);
float city_ground_y(void);
vec3 city_spire_pos(void);
int city_pods(const city_pod_t** out);
