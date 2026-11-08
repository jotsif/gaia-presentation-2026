// Low-poly car models and the highway traffic simulation.
#pragma once
#include <stdbool.h>
#include "mathx.h"
#include "mesh.h"

typedef enum { CAR_SEDAN, CAR_SPORT, CAR_VAN, CAR_TRUCK, CAR_SIGN_VAN, CAR_NUM_TYPES } car_type_t;

// presentation sign carried by CAR_SIGN_VAN (car-local units, meters)
#define SIGN_BOTTOM 2.4f
#define SIGN_W      11.6f
#define SIGN_H      (SIGN_W * 9.0f / 16.0f)

typedef struct {
    int first, count;       // vertex range in the shared car vertex buffer
    float length, width, height;
} car_model_t;

void cars_build_models(mesh_t* m, car_model_t models[CAR_NUM_TYPES]);
void cars_build_shadow(mesh_t* m);   // unit-size soft blob (scaled per car)

#define MAX_CARS 96

typedef struct {
    int type;
    vec3 tint;          // body paint colour
    float s;            // position along the track [0, L)
    int dir;            // +1: travels along the track tangent (right carriageway), -1: opposite
    int lane;           // 0 = inner/fast lane, 1 = outer/slow lane
    float lat;          // current lateral distance from the median centre line
    float lat_vel;
    float speed;        // m/s
    float v0;           // desired speed
    float amax;         // max acceleration
    float lc_timer;     // lane change decision cooldown
    bool hero;
    bool active;        // inactive cars are neither simulated nor drawn
    bool scripted;      // moved by the presentation director instead of the traffic model
    double odo;         // distance travelled
} car_t;

typedef struct {
    car_t cars[MAX_CARS];
    int num;
    float L;
    const car_model_t* models;
    bool autopilot;     // hero car drives itself
    uint32_t rng;
} traffic_t;

typedef struct {
    vec3 pos, fwd, up, right;
} car_pose_t;

// presentation == true: only oncoming traffic, the hero cruises in the slow lane
void traffic_init(traffic_t* t, const car_model_t* models, bool presentation);
// reserves an (inactive) scripted car slot, returns its index
int traffic_add_scripted(traffic_t* t, int type);
void traffic_update(traffic_t* t, float dt);
bool traffic_request_lane(traffic_t* t, int car, int lane);
car_pose_t car_pose(const car_t* c);
mat4 car_model_matrix(const car_pose_t* p, float sx, float sy, float sz, float y_off);
