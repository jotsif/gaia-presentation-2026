// Procedurally generated textures (no asset files needed).
#pragma once
#include <stdint.h>

typedef struct {
    int w, h;
    uint32_t* px;   // RGBA8, row 0 = top
} image_t;

image_t tex_gen_white(void);
image_t tex_gen_detail(void);               // grayscale grit, multiplied with vertex colours
image_t tex_gen_road(float dash_period);    // one carriageway: median -> outer shoulder
image_t tex_gen_sky(void);                  // sky cylinder panorama with sun, clouds, mountains
image_t tex_gen_atlas(void);                // billboards + overhead signs
void image_free(image_t* img);
