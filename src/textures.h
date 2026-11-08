// Procedurally generated textures (no asset files needed).
#pragma once
#include <stdint.h>
#include "mathx.h"

typedef struct {
    int w, h;
    uint32_t* px;   // RGBA8, row 0 = top
} image_t;

image_t tex_gen_white(void);
image_t tex_gen_detail(void);               // grayscale grit, multiplied with vertex colours
image_t tex_gen_road(float dash_period);    // one carriageway: median -> outer shoulder
image_t tex_gen_sky(void);                  // sky cylinder panorama with sun, clouds, mountains

typedef struct {
    float sun_elev_deg;     // sun elevation above the horizon
    float sunset;           // 0 = midday colours, 1 = golden hour
    float city;             // 0 = mountain ranges on the horizon, 1 = megacity skyline
    vec3 fog;               // colour at and below the horizon (matches the scene fog)
} sky_params_t;
image_t tex_gen_sky_ex(const sky_params_t* p);
image_t tex_gen_facade(void);               // future city tower windows (alpha 0 = lit window)
image_t tex_gen_city_ground(void);          // future city street block
image_t tex_gen_atlas(void);                // billboards + overhead signs
void image_free(image_t* img);
