// Presentation mode: slides are shown on giant signs carried by vans that
// overtake the player's car, cut in ahead of it and drive off on "next".
#pragma once
#include <stdbool.h>
#include "sokol_gfx.h"
#include "cars.h"

#define PRESENT_MAX_SIGNS 4

// camera modes (shared with main.c; slides can request one with '@camera <name>')
enum { CAM_PRESENT, CAM_CHASE, CAM_BUMPER, CAM_HELI, CAM_TV, CAM_NUM };

typedef struct {
    int car;            // index into traffic_t.cars
    int slide;
    float sign_scale;   // 0..~1.1 (pop-up animation)
    bool on_top;        // currently presenting: draw the sign over scene geometry
} present_sign_t;

// loads slides (falls back to a built-in title slide), creates the slide render targets
// and reserves the scripted sign-van slots in the traffic simulation
void present_init(const char* slides_path, traffic_t* t);
// renders the slide texts into their textures; call once inside a frame (before sg_commit)
void present_render_slides(void);
void present_update(traffic_t* t, float dt);
// automatically bring in the first slide shortly after start (default on)
void present_set_autostart(bool on);
// distance (m, centre to centre) the presenting van keeps ahead of the player car
void present_set_gap(float gap);
void present_next(traffic_t* t);
void present_prev(traffic_t* t);
int present_slide_count(void);
int present_current_slide(void);   // -1 before the first slide arrives
int present_signs(present_sign_t out[PRESENT_MAX_SIGNS]);
sg_view present_slide_texture(int slide);
// camera requested by the slide that just started (-1 = none); clears the request
int present_take_camera_request(void);
// sunset duration (s) requested by a '@futurecity' slide that just started (0 = none); clears it
float present_take_future_request(void);
// 1 while a slide that wants a close-up (image slides, '@zoom on') is presenting, else 0
float present_zoom_target(void);
// traffic index of the van whose slide is currently presented (-1 if none)
int present_focus_car(void);
void present_shutdown(void);
