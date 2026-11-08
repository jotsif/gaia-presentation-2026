// SOKOL HIGHWAY - a 90s style 3D racing demo (sokol_app + sokol_gfx, Metal backend).
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_debugtext.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "mathx.h"
#include "mesh.h"
#include "world.h"
#include "cars.h"
#include "textures.h"
#include "shaders.h"
#include "present.h"
#include "textfx.h"

// implemented in sokol_impl.m (Metal readback + PNG encoding)
bool highway_save_png(sg_image img, int w, int h, bool bgra, const char* path);

typedef struct {
    mat4 mvp;
    mat4 model;
    float cam_pos[4];
    float light_dir[4];
    float fog_color[4];
    float snap[4];
    float tint[4];
} vs_params_t;

typedef struct {
    float res[4];
    float flags[4];
} disp_params_t;

static const char* CAM_NAMES[CAM_NUM] = { "PRESENT CAM", "CHASE CAM", "BUMPER CAM", "HELI CAM", "TV CAM" };
static const int RES_MODES[] = { 240, 360, 540, 720, 0 };   // render target height, 0 = native
#define NUM_RES_MODES 5
#define DEFAULT_RES_MODE 3
#define HUD_ROWS 240.0f
#define ATTRACT_INTERVAL 11.0f

typedef struct {
    sg_buffer vbuf;
    sg_buffer ibuf;
    int count;
} gpu_mesh_t;

static struct {
    // pipelines & resources
    sg_pipeline pip_scene, pip_scene_idx, pip_sky, pip_shadow, pip_display, pip_sign_top;
    sg_sampler smp_nearest, smp_linear, smp_sky_nearest, smp_sky_linear, smp_display, smp_slide;
    gpu_mesh_t terrain, road, props, decals, sky, cars, shadow, sign_quad, sign_frame;
    sg_view tex_white, tex_detail, tex_road, tex_sky, tex_atlas;
    car_model_t car_models[CAR_NUM_TYPES];
    // low-res render target
    sg_image rt_color, rt_depth;
    sg_view rt_color_att, rt_depth_att, rt_color_tex;
    int rt_w, rt_h;
    // simulation
    traffic_t traffic;
    bool present_mode;      // slides on sign vans (default) vs. the free-roaming demo
    float present_zoom;     // 0 = normal framing, 1 = the sign fills the screen
    // title screen ("press a button to start")
    bool intro, intro_leaving;
    float intro_alpha;      // title text opacity
    float intro_dim;        // darkening of the scene behind the title
    float fade_black;       // fade in from black at startup
    float intro_cam_timer;
    int intro_cam;
    vec3 zoom_c, zoom_dir;  // smoothed close-up target
    float focus;            // 0..1: blends the non-presentation cameras towards framing the sign
    vec3 focus_c;           // smoothed centre of the presenting sign
    float sign_yaw[MAX_CARS], sign_pitch[MAX_CARS];   // turntable orientation of each sign
    const char* slides_path;
    float time;
    // camera
    int cam_mode;
    int follow;
    bool cam_snap;
    vec3 chase_dir;
    vec3 cam_eye, cam_target;
    float cam_fov;
    float heli_angle;
    float attract_timer;
    // input
    bool key_up, key_down;
    // settings
    int res_mode;
    bool jitter, affine, dither, bilinear, scanlines, show_hud, show_help, attract;
    float help_timer;
    // lap timing (hero car)
    int lap;
    float lap_start, last_lap, best_lap;
    vec3 light_dir;
    // screenshots: full-window capture target with the swapchain's formats
    sg_image cap_color, cap_depth;
    sg_view cap_color_att, cap_depth_att;
    int cap_w, cap_h;
    bool shot_requested;
    char shot_path[512];
    // command line
    const char* cli_shot;    // --shot PATH: save a screenshot after --shot-frame frames, then quit
    int cli_shot_frame;
    int frame_count;
} S;

static bool g_no_intro = false;              // --no-intro: skip the title screen
static int g_next_at[16], g_num_next_at = 0;   // --next-at N: scripted "next slide" at frame N (testing)

// ---------------------------------------------------------------------------
//  Resource helpers
// ---------------------------------------------------------------------------
static gpu_mesh_t upload_mesh(const mesh_t* m, const char* label) {
    gpu_mesh_t g = { 0 };
    g.vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data = { m->v, (size_t)m->count * sizeof(vertex_t) },
        .label = label,
    });
    g.count = m->count;
    if (m->icount > 0) {
        g.ibuf = sg_make_buffer(&(sg_buffer_desc){
            .usage.index_buffer = true,
            .data = { m->idx, (size_t)m->icount * sizeof(uint32_t) },
            .label = label,
        });
        g.count = m->icount;
    }
    return g;
}

// creates a texture with a full box-filtered mip chain
static sg_view make_texture(image_t* img, bool mips, const char* label) {
    int levels = 1;
    if (mips) {
        int m = img->w > img->h ? img->w : img->h;
        while ((m >>= 1) > 0) {
            levels++;
        }
    }
    size_t total = 0;
    for (int l = 0; l < levels; l++) {
        int w = img->w >> l, h = img->h >> l;
        w = w < 1 ? 1 : w; h = h < 1 ? 1 : h;
        total += (size_t)w * (size_t)h;
    }
    uint32_t* buf = (uint32_t*)malloc(total * sizeof(uint32_t));
    memcpy(buf, img->px, (size_t)img->w * (size_t)img->h * sizeof(uint32_t));
    uint32_t* prev = buf;
    int pw = img->w, ph = img->h;
    uint32_t* cur = buf + (size_t)pw * (size_t)ph;
    for (int l = 1; l < levels; l++) {
        int w = pw > 1 ? pw / 2 : 1, h = ph > 1 ? ph / 2 : 1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int x0 = (x * 2) % pw, x1 = (x * 2 + 1) % pw;
                int y0 = (y * 2) % ph, y1 = (y * 2 + 1) % ph;
                uint32_t p[4] = { prev[y0 * pw + x0], prev[y0 * pw + x1], prev[y1 * pw + x0], prev[y1 * pw + x1] };
                uint32_t out = 0;
                for (int ch = 0; ch < 4; ch++) {
                    uint32_t s = 0;
                    for (int k = 0; k < 4; k++) {
                        s += (p[k] >> (ch * 8)) & 0xFF;
                    }
                    out |= ((s + 2) / 4) << (ch * 8);
                }
                cur[y * w + x] = out;
            }
        }
        prev = cur;
        cur += (size_t)w * (size_t)h;
        pw = w; ph = h;
    }
    sg_image simg = sg_make_image(&(sg_image_desc){
        .width = img->w,
        .height = img->h,
        .num_mipmaps = levels,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data = { buf, total * sizeof(uint32_t) },
        .label = label,
    });
    free(buf);
    return sg_make_view(&(sg_view_desc){ .texture.image = simg, .label = label });
}

static sg_view make_texture_from(image_t img, bool mips, const char* label) {
    sg_view v = make_texture(&img, mips, label);
    image_free(&img);
    return v;
}

static void create_render_target(int w, int h) {
    if (S.rt_w > 0) {
        sg_destroy_view(S.rt_color_att);
        sg_destroy_view(S.rt_depth_att);
        sg_destroy_view(S.rt_color_tex);
        sg_destroy_image(S.rt_color);
        sg_destroy_image(S.rt_depth);
    }
    S.rt_w = w;
    S.rt_h = h;
    S.rt_color = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = w, .height = h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .sample_count = 1,
        .label = "rt-color",
    });
    S.rt_depth = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true,
        .width = w, .height = h,
        .pixel_format = SG_PIXELFORMAT_DEPTH,
        .sample_count = 1,
        .label = "rt-depth",
    });
    S.rt_color_att = sg_make_view(&(sg_view_desc){ .color_attachment.image = S.rt_color });
    S.rt_depth_att = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = S.rt_depth });
    S.rt_color_tex = sg_make_view(&(sg_view_desc){ .texture.image = S.rt_color });
}

static void create_pipelines(void) {
    sg_shader scene_shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func = { .source = SCENE_MSL, .entry = "vs_scene" },
        .fragment_func = { .source = SCENE_MSL, .entry = "fs_scene" },
        .uniform_blocks[0] = { .stage = SG_SHADERSTAGE_VERTEX, .size = sizeof(vs_params_t), .msl_buffer_n = 0 },
        .views[0].texture = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .image_type = SG_IMAGETYPE_2D,
            .sample_type = SG_IMAGESAMPLETYPE_FLOAT,
            .msl_texture_n = 0,
        },
        .samplers[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .sampler_type = SG_SAMPLERTYPE_FILTERING, .msl_sampler_n = 0 },
        .texture_sampler_pairs[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 0, .sampler_slot = 0 },
        .label = "scene-shader",
    });

    sg_pipeline_desc pd = {
        .shader = scene_shd,
        .layout = {
            .buffers[0].stride = sizeof(vertex_t),
            .attrs = {
                [0] = { .offset = offsetof(vertex_t, pos), .format = SG_VERTEXFORMAT_FLOAT3 },
                [1] = { .offset = offsetof(vertex_t, nrm), .format = SG_VERTEXFORMAT_FLOAT3 },
                [2] = { .offset = offsetof(vertex_t, uv), .format = SG_VERTEXFORMAT_FLOAT2 },
                [3] = { .offset = offsetof(vertex_t, color), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .depth = {
            .pixel_format = SG_PIXELFORMAT_DEPTH,
            .compare = SG_COMPAREFUNC_GREATER_EQUAL,   // reversed-Z
            .write_enabled = true,
        },
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .sample_count = 1,
        .cull_mode = SG_CULLMODE_NONE,
        .label = "scene",
    };
    S.pip_scene = sg_make_pipeline(&pd);

    pd.index_type = SG_INDEXTYPE_UINT32;
    pd.label = "scene-indexed";
    S.pip_scene_idx = sg_make_pipeline(&pd);
    pd.index_type = SG_INDEXTYPE_NONE;

    pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pd.depth.write_enabled = false;
    pd.label = "sky";
    S.pip_sky = sg_make_pipeline(&pd);

    pd.depth.compare = SG_COMPAREFUNC_GREATER_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].blend = (sg_blend_state){
        .enabled = true,
        .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
        .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .src_factor_alpha = SG_BLENDFACTOR_ZERO,
        .dst_factor_alpha = SG_BLENDFACTOR_ONE,
    };
    pd.label = "shadow";
    S.pip_shadow = sg_make_pipeline(&pd);

    // presentation signs in the presentation camera: always visible on top of the scene
    pd.colors[0].blend = (sg_blend_state){ 0 };
    pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pd.depth.write_enabled = false;
    pd.label = "sign-on-top";
    S.pip_sign_top = sg_make_pipeline(&pd);

    sg_shader disp_shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func = { .source = DISPLAY_MSL, .entry = "vs_display" },
        .fragment_func = { .source = DISPLAY_MSL, .entry = "fs_display" },
        .uniform_blocks[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .size = sizeof(disp_params_t), .msl_buffer_n = 0 },
        .views[0].texture = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .image_type = SG_IMAGETYPE_2D,
            .sample_type = SG_IMAGESAMPLETYPE_FLOAT,
            .msl_texture_n = 0,
        },
        .samplers[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .sampler_type = SG_SAMPLERTYPE_FILTERING, .msl_sampler_n = 0 },
        .texture_sampler_pairs[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 0, .sampler_slot = 0 },
        .label = "display-shader",
    });
    S.pip_display = sg_make_pipeline(&(sg_pipeline_desc){ .shader = disp_shd, .label = "display" });

    S.smp_nearest = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST, .mipmap_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_REPEAT, .wrap_v = SG_WRAP_REPEAT,
    });
    S.smp_linear = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR, .mipmap_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_REPEAT, .wrap_v = SG_WRAP_REPEAT,
    });
    S.smp_sky_nearest = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_REPEAT, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    S.smp_sky_linear = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_REPEAT, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    S.smp_slide = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    S.smp_display = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------
static void init(void) {
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    sdtx_setup(&(sdtx_desc_t){
        .fonts = { sdtx_font_kc854(), sdtx_font_cpc(), sdtx_font_c64(), sdtx_font_oric() },
        .logger.func = slog_func,
    });

    track_init();
    world_meshes_t w;
    world_build(&w);
    S.terrain = upload_mesh(&w.terrain, "terrain");
    S.road = upload_mesh(&w.road, "road");
    S.props = upload_mesh(&w.props, "props");
    S.decals = upload_mesh(&w.decals, "decals");
    S.sky = upload_mesh(&w.sky, "sky");
    world_free(&w);

    mesh_t cars = { 0 }, shadow = { 0 };
    cars_build_models(&cars, S.car_models);
    cars_build_shadow(&shadow);
    S.cars = upload_mesh(&cars, "cars");
    S.shadow = upload_mesh(&shadow, "shadow");
    mesh_free(&cars);
    mesh_free(&shadow);

    // presentation sign: unit quads (x in [-0.5,0.5], y in [0,1]) scaled per sign,
    // the slide is shown on both faces so it reads correctly from any camera
    {
        mesh_t quad = { 0 }, frame = { 0 };
        const uint32_t white = RGB(255, 255, 255);
        // rear face (seen from behind the van): viewer's left is local +x
        const float uv_back[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
        mesh_quad_uv(&quad, v3(0.5f, 0, -0.07f), v3(-0.5f, 0, -0.07f), v3(-0.5f, 1, -0.07f), v3(0.5f, 1, -0.07f),
                     uv_back, v3(0, 0, -1), white);
        // front face (seen from ahead of the van): viewer's left is local -x
        mesh_quad_uv(&quad, v3(-0.5f, 0, 0.12f), v3(0.5f, 0, 0.12f), v3(0.5f, 1, 0.12f), v3(-0.5f, 1, 0.12f),
                     uv_back, v3(0, 0, 1), white);
        mesh_box(&frame, v3(-0.515f, -0.03f, -0.05f), v3(0.515f, 1.03f, 0.10f), RGB(240, 196, 36));
        S.sign_quad = upload_mesh(&quad, "sign-quad");
        S.sign_frame = upload_mesh(&frame, "sign-frame");
        mesh_free(&quad);
        mesh_free(&frame);
    }

    S.tex_white = make_texture_from(tex_gen_white(), false, "tex-white");
    S.tex_detail = make_texture_from(tex_gen_detail(), true, "tex-detail");
    S.tex_road = make_texture_from(tex_gen_road(track_dash_period()), true, "tex-road");
    S.tex_sky = make_texture_from(tex_gen_sky(), false, "tex-sky");
    S.tex_atlas = make_texture_from(tex_gen_atlas(), true, "tex-atlas");

    create_pipelines();
    traffic_init(&S.traffic, S.car_models, S.present_mode);
    if (S.present_mode) {
        present_init(S.slides_path, &S.traffic);
    }

    S.light_dir = sun_direction();
    S.cam_mode = S.present_mode ? CAM_PRESENT : CAM_CHASE;
    S.intro = S.present_mode && !g_no_intro;
    if (S.intro) {
        present_set_autostart(false);
        S.cam_mode = CAM_HELI;
        S.intro_alpha = 1.0f;
        S.intro_dim = 1.0f;
        S.fade_black = 1.0f;
    }
    S.follow = 0;
    S.cam_snap = true;
    S.res_mode = DEFAULT_RES_MODE;
    S.jitter = true;
    S.affine = false;
    S.dither = true;
    S.bilinear = false;
    S.scanlines = false;
    S.show_hud = true;
    S.show_help = !S.present_mode;
    S.help_timer = S.present_mode ? 0.0f : 12.0f;
    S.attract = !S.present_mode;
    S.best_lap = 0.0f;
}

// ---------------------------------------------------------------------------
//  Camera
// ---------------------------------------------------------------------------
static void update_camera(float dt) {
    const car_t* c = &S.traffic.cars[S.follow];
    const car_model_t* cm = &S.car_models[c->type];
    car_pose_t p = car_pose(c);
    const vec3 up = v3(0, 1, 0);
    vec3 fwd_h = v3_norm(v3(p.fwd.x, 0, p.fwd.z));
    vec3 eye, target;
    float fov = 60.0f;

    if (S.cam_snap) {
        S.chase_dir = fwd_h;
    }
    // slide-aware framing: while a sign is presenting, every camera mode leans towards it
    int fcar = S.present_mode ? present_focus_car() : -1;
    S.focus += ((fcar >= 0 ? 1.0f : 0.0f) - S.focus) * (1.0f - expf(-dt * 2.0f));
    if (fcar >= 0) {
        car_pose_t vp = car_pose(&S.traffic.cars[fcar]);
        vec3 c_now = v3_mad(vp.pos, vp.up, SIGN_BOTTOM + SIGN_H * 0.5f);
        S.focus_c = (S.focus < 0.02f || S.cam_snap) ? c_now : v3_lerp(S.focus_c, c_now, 1.0f - expf(-dt * 8.0f));
    }
    const float foc = smoothstepf(0.0f, 1.0f, S.focus);
    const float aspect = (float)S.rt_w / (float)S.rt_h;
    // vertical fov (degrees) that fits the sign seen from 'from' with the given margin
    #define FIT_FOV(from, margin) (2.0f * atanf(fmaxf((SIGN_W * 0.5f / (margin)) / aspect, SIGN_H * 0.5f / (margin)) / \
                                    fmaxf(v3_len(v3_sub(S.focus_c, (from))), 1.0f)) * 180.0f / PI_F)
    if (S.present_mode) {
        // the sign van keeps more distance in bumper cam so the sign fits the view
        present_set_gap(S.cam_mode == CAM_BUMPER ? 22.0f : (S.cam_mode == CAM_CHASE ? 14.0f : 12.0f));
    }
    switch (S.cam_mode) {
        case CAM_PRESENT: {
            // low chase camera with a narrow lens, framing the sign van ahead of the car
            S.chase_dir = v3_norm(v3_lerp(S.chase_dir, fwd_h, 1.0f - expf(-dt * 4.0f)));
            // smoothly blend into a close-up that fills the screen with the sign (charts)
            float zt = S.present_mode ? present_zoom_target() : 0.0f;
            S.present_zoom += (zt - S.present_zoom) * (1.0f - expf(-dt * 2.5f));
            float z = smoothstepf(0.0f, 1.0f, S.present_zoom);
            vec3 sign_c = v3_mad(v3_mad(p.pos, S.chase_dir, 12.0f), up, SIGN_BOTTOM + SIGN_H * 0.5f);
            vec3 sign_dir = S.chase_dir;
            int focus = S.present_mode ? present_focus_car() : -1;
            if (focus >= 0) {
                // aim at the real van and look straight along its axis (no skew on curves)
                car_pose_t vp = car_pose(&S.traffic.cars[focus]);
                sign_c = v3_mad(vp.pos, vp.up, SIGN_BOTTOM + SIGN_H * 0.5f);
                sign_dir = v3_norm(v3(vp.fwd.x, 0.0f, vp.fwd.z));
            }
            S.zoom_c = (S.cam_snap || S.present_zoom < 0.01f) ? sign_c : v3_lerp(S.zoom_c, sign_c, 1.0f - expf(-dt * 8.0f));
            S.zoom_dir = (S.cam_snap || S.present_zoom < 0.01f) ? sign_dir : v3_norm(v3_lerp(S.zoom_dir, sign_dir, 1.0f - expf(-dt * 8.0f)));
            sign_c = S.zoom_c;
            vec3 eye_n = v3_mad(v3_mad(p.pos, S.chase_dir, -11.0f), up, 2.6f);
            vec3 eye_z = v3_mad(v3_mad(sign_c, S.zoom_dir, -22.0f), up, -0.6f);
            eye = v3_lerp(eye_n, eye_z, z);
            target = v3_lerp(v3_mad(v3_mad(p.pos, S.chase_dir, 12.0f), up, 5.4f), sign_c, z);
            float aspect = (float)S.rt_w / (float)S.rt_h;
            float d = v3_len(v3_sub(sign_c, eye_z));
            float half_tan = fmaxf((SIGN_W * 0.5f / 0.92f) / aspect, SIGN_H * 0.5f / 0.92f) / d;
            float fov_fill = 2.0f * atanf(half_tan) * 180.0f / PI_F;
            fov = lerpf(34.0f, fov_fill, z);
        } break;
        case CAM_CHASE: {
            S.chase_dir = v3_norm(v3_lerp(S.chase_dir, fwd_h, 1.0f - expf(-dt * 5.0f)));
            float back = 6.0f + cm->length * 0.6f;
            float height = 1.6f + cm->height;
            eye = v3_mad(v3_mad(p.pos, S.chase_dir, -back), up, height);
            target = v3_mad(v3_mad(p.pos, p.fwd, 6.0f), up, cm->height * 0.7f);
            fov = 62.0f;
            if (foc > 0.0f) {   // aim between the car and the sign so both are in view
                target = v3_lerp(target, v3_lerp(v3_mad(p.pos, up, 1.0f), S.focus_c, 0.5f), foc);
                fov = lerpf(fov, fminf(66.0f, FIT_FOV(eye, 0.5f)), foc);
            }
        } break;
        case CAM_BUMPER: {
            eye = v3_mad(v3_mad(p.pos, p.fwd, cm->length * 0.5f + 0.05f), p.up, 0.65f);
            target = v3_mad(eye, p.fwd, 20.0f);
            fov = 72.0f;
            if (foc > 0.0f) {   // tilt up and zoom in on the sign
                target = v3_lerp(target, S.focus_c, foc);
                fov = lerpf(fov, FIT_FOV(eye, 0.72f), foc);
            }
        } break;
        case CAM_HELI: {
            S.heli_angle += dt * 0.18f;
            if (S.present_mode && !S.intro) {
                // news-chopper shot: hover ahead of the sign van, looking back at the sign's
                // front face, swinging gently from side to side
                vec3 centre = v3_mad(v3_mad(p.pos, S.chase_dir, 12.0f), up, SIGN_BOTTOM + SIGN_H * 0.5f);
                S.chase_dir = v3_norm(v3_lerp(S.chase_dir, fwd_h, 1.0f - expf(-dt * 3.0f)));
                vec3 side = v3_cross(S.chase_dir, up);
                float a = sinf(S.time * 0.25f) * 0.55f;
                vec3 dir = v3_add(v3_scale(S.chase_dir, cosf(a)), v3_scale(side, sinf(a)));
                eye = v3_mad(v3_mad(centre, dir, 24.0f), up, 9.0f);
                target = v3_mad(centre, up, -1.0f);
                fov = 38.0f;
            } else {
                vec3 centre = v3_mad(p.pos, up, 1.0f);
                float rad = S.intro ? 24.0f : 38.0f, hgt = S.intro ? 8.0f : 24.0f;
                eye = v3_add(centre, v3(cosf(S.heli_angle) * rad, hgt, sinf(S.heli_angle) * rad));
                target = S.intro ? v3_mad(centre, up, 3.0f) : centre;
                fov = 50.0f;
            }
            float th = terrain_height(eye.x, eye.z, NULL);
            eye.y = fmaxf(eye.y, th + 6.0f);
        } break;
        default: {   // CAM_TV: trackside cameras that the car drives past
            const float spacing = 260.0f;
            float sl = wrapf(c->s + (float)c->dir * 120.0f, S.traffic.L);
            int k = (int)(sl / spacing);
            track_frame_t f = track_frame((float)k * spacing);
            float side = (k & 1) ? 1.0f : -1.0f;
            eye = v3_mad(v3_mad(f.pos, f.right, side * 17.0f), up, 4.0f + (float)(k % 3) * 3.0f);
            float th = terrain_height(eye.x, eye.z, NULL);
            eye.y = fmaxf(eye.y, th + 2.5f);
            target = v3_mad(p.pos, up, 0.8f);
            float dist = v3_len(v3_sub(target, eye));
            fov = clampf(2.0f * atanf(9.0f / dist) * 180.0f / PI_F, 6.0f, 55.0f);
            if (foc > 0.0f) {   // keep the sign centred with a moderate lens (no edge stretching)
                target = v3_lerp(target, S.focus_c, foc);
                fov = lerpf(fov, clampf(FIT_FOV(eye, 0.80f), 6.0f, 40.0f), foc);
            }
        } break;
    }
    #undef FIT_FOV
    S.cam_eye = eye;
    S.cam_target = target;
    S.cam_fov = fov * PI_F / 180.0f;
    S.cam_snap = false;
}

// Signs sit on a turntable: they turn (and tilt) towards the current camera so they read
// well from any angle. Angles are smoothed per sign van.
static void update_sign_orientation(float dt) {
    if (!S.present_mode) {
        return;
    }
    present_sign_t signs[PRESENT_MAX_SIGNS];
    int n = present_signs(signs);
    const float max_yaw = 70.0f * PI_F / 180.0f;
    for (int i = 0; i < n; i++) {
        int ci = signs[i].car;
        car_pose_t pose = car_pose(&S.traffic.cars[ci]);
        vec3 left = v3_cross(pose.up, pose.fwd);
        vec3 pivot = v3_mad(pose.pos, pose.up, SIGN_BOTTOM + SIGN_H * signs[i].sign_scale * 0.5f);
        vec3 d = v3_sub(S.cam_eye, pivot);
        float dx = v3_dot(d, left), dy = v3_dot(d, pose.up), dz = v3_dot(d, pose.fwd);
        float yaw = (dz < 0.0f) ? atan2f(-dx, -dz) : atan2f(dx, dz);
        yaw = clampf(yaw, -max_yaw, max_yaw);
        float elev = atan2f(dy, sqrtf(dx * dx + dz * dz));
        float pitch = clampf(elev * 0.85f, -0.15f, 0.75f);
        float k = 1.0f - expf(-dt * 4.0f);
        if (S.cam_snap) k = 1.0f;
        S.sign_yaw[ci] += (yaw - S.sign_yaw[ci]) * k;
        S.sign_pitch[ci] += (pitch - S.sign_pitch[ci]) * k;
    }
}

// ---------------------------------------------------------------------------
//  Rendering
// ---------------------------------------------------------------------------
static vs_params_t base_params(void) {
    vs_params_t p;
    memset(&p, 0, sizeof(p));
    p.model = m4_identity();
    p.cam_pos[0] = S.cam_eye.x; p.cam_pos[1] = S.cam_eye.y; p.cam_pos[2] = S.cam_eye.z; p.cam_pos[3] = FOG_START;
    p.light_dir[0] = S.light_dir.x; p.light_dir[1] = S.light_dir.y; p.light_dir[2] = S.light_dir.z; p.light_dir[3] = FOG_END;
    p.fog_color[0] = FOG_R; p.fog_color[1] = FOG_G; p.fog_color[2] = FOG_B; p.fog_color[3] = 0.0f;
    p.snap[0] = (float)S.rt_w * 0.5f;
    p.snap[1] = (float)S.rt_h * 0.5f;
    p.snap[2] = S.jitter ? 1.0f : 0.0f;
    p.snap[3] = S.affine ? 1.0f : 0.0f;
    return p;
}

static void draw_mesh(const gpu_mesh_t* m, sg_view tex, sg_sampler smp, int first, int count) {
    sg_bindings b = {
        .vertex_buffers[0] = m->vbuf,
        .index_buffer = m->ibuf,
        .views[0] = tex,
        .samplers[0] = smp,
    };
    sg_apply_bindings(&b);
    sg_draw(first, count, 1);
}

// slide signs carried by the presentation vans (frame lit, slide face unlit like a light box)
static void render_signs(mat4 vp, sg_sampler smp) {
    present_sign_t signs[PRESENT_MAX_SIGNS];
    int n = present_signs(signs);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const car_t* c = &S.traffic.cars[signs[i].car];
            car_pose_t pose = car_pose(c);
            mat4 car_m = car_model_matrix(&pose, 1.0f, 1.0f, 1.0f, 0.0f);
            float sc = signs[i].sign_scale;
            // local = T(pivot) * Ry(yaw) * Rx(tilt) * S(w, h, 1); the rear face tilts with +pitch,
            // the front face with -pitch, depending on which side the camera is on
            vec3 left = v3_cross(pose.up, pose.fwd);
            float dz = v3_dot(v3_sub(S.cam_eye, pose.pos), pose.fwd);
            float a = S.sign_yaw[signs[i].car];
            float b = (dz < 0.0f) ? S.sign_pitch[signs[i].car] : -S.sign_pitch[signs[i].car];
            (void)left;
            vec3 cx = v3(cosf(a), 0.0f, -sinf(a));
            vec3 cy = v3(sinf(b) * sinf(a), cosf(b), sinf(b) * cosf(a));
            vec3 cz = v3(cosf(b) * sinf(a), -sinf(b), cosf(b) * cosf(a));
            mat4 local = m4_basis(v3_scale(cx, SIGN_W * sc), v3_scale(cy, SIGN_H * sc), cz, v3(0, SIGN_BOTTOM, 0));
            vs_params_t p = base_params();
            p.model = m4_mul(car_m, local);
            p.mvp = m4_mul(vp, p.model);
            if (pass == 0) {
                sg_apply_pipeline(S.pip_scene);
                sg_apply_uniforms(0, &SG_RANGE(p));
                draw_mesh(&S.sign_frame, S.tex_white, smp, 0, S.sign_frame.count);
            } else {
                bool top = signs[i].on_top && S.cam_mode == CAM_PRESENT;
                p.fog_color[3] = 1.0f;   // unlit, no fog: keeps the text crisp and bright
                p.snap[2] = 0.0f;        // no vertex jitter on the slide
                // only draw the face that looks towards the camera (vertices 0..5 rear, 6..11 front)
                bool front = dz > 0.0f;
                sg_apply_pipeline(top ? S.pip_sign_top : S.pip_scene);
                sg_apply_uniforms(0, &SG_RANGE(p));
                draw_mesh(&S.sign_quad, present_slide_texture(signs[i].slide), S.smp_slide, front ? 6 : 0, 6);
            }
        }
    }
}

static void render_scene(void) {
    float aspect = (float)S.rt_w / (float)S.rt_h;
    mat4 proj = m4_perspective_revz(S.cam_fov, aspect, 0.3f);
    mat4 view = m4_lookat(S.cam_eye, S.cam_target, v3(0, 1, 0));
    mat4 vp = m4_mul(proj, view);
    sg_sampler smp = S.bilinear ? S.smp_linear : S.smp_nearest;

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { FOG_R, FOG_G, FOG_B, 1.0f } },
            .depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 0.0f },
        },
        .attachments = { .colors[0] = S.rt_color_att, .depth_stencil = S.rt_depth_att },
        .label = "scene-pass",
    });

    // sky cylinder, centred on the camera
    {
        vs_params_t p = base_params();
        p.model = m4_translate(S.cam_eye);
        p.mvp = m4_mul(vp, p.model);
        p.fog_color[3] = 1.0f;   // unlit, no fog
        p.snap[2] = 0.0f;
        sg_apply_pipeline(S.pip_sky);
        sg_apply_uniforms(0, &SG_RANGE(p));
        draw_mesh(&S.sky, S.tex_sky, S.bilinear ? S.smp_sky_linear : S.smp_sky_nearest, 0, S.sky.count);
    }

    vs_params_t p = base_params();
    p.mvp = vp;

    // terrain (indexed)
    sg_apply_pipeline(S.pip_scene_idx);
    sg_apply_uniforms(0, &SG_RANGE(p));
    draw_mesh(&S.terrain, S.tex_detail, smp, 0, S.terrain.count);

    // static world
    sg_apply_pipeline(S.pip_scene);
    sg_apply_uniforms(0, &SG_RANGE(p));
    draw_mesh(&S.road, S.tex_road, smp, 0, S.road.count);
    draw_mesh(&S.props, S.tex_detail, smp, 0, S.props.count);
    draw_mesh(&S.decals, S.tex_atlas, smp, 0, S.decals.count);

    // blob shadows
    sg_apply_pipeline(S.pip_shadow);
    for (int i = 0; i < S.traffic.num; i++) {
        const car_t* c = &S.traffic.cars[i];
        if (!c->active) {
            continue;
        }
        const car_model_t* cm = &S.car_models[c->type];
        car_pose_t pose = car_pose(c);
        vs_params_t sp = base_params();
        sp.model = car_model_matrix(&pose, cm->width * 1.25f, 1.0f, cm->length * 1.12f, 0.06f);
        sp.mvp = m4_mul(vp, sp.model);
        sg_apply_uniforms(0, &SG_RANGE(sp));
        draw_mesh(&S.shadow, S.tex_white, smp, 0, S.shadow.count);
    }

    // cars
    sg_apply_pipeline(S.pip_scene);
    for (int i = 0; i < S.traffic.num; i++) {
        const car_t* c = &S.traffic.cars[i];
        if (!c->active) {
            continue;
        }
        const car_model_t* cm = &S.car_models[c->type];
        car_pose_t pose = car_pose(c);
        vs_params_t cp = base_params();
        cp.model = car_model_matrix(&pose, 1.0f, 1.0f, 1.0f, 0.0f);
        cp.mvp = m4_mul(vp, cp.model);
        cp.tint[0] = c->tint.x; cp.tint[1] = c->tint.y; cp.tint[2] = c->tint.z; cp.tint[3] = 1.0f;
        sg_apply_uniforms(0, &SG_RANGE(cp));
        draw_mesh(&S.cars, S.tex_white, smp, cm->first, cm->count);
    }

    if (S.present_mode) {
        render_signs(vp, smp);
    }
    sg_end_pass();
}

// text with a 1-pixel drop shadow (positions in character cells)
static void hud_text(float x, float y, uint8_t r, uint8_t g, uint8_t b, const char* fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    sdtx_origin(0.125f, 0.125f);
    sdtx_pos(x, y);
    sdtx_color3b(0, 0, 0);
    sdtx_puts(buf);
    sdtx_origin(0.0f, 0.0f);
    sdtx_pos(x, y);
    sdtx_color3b(r, g, b);
    sdtx_puts(buf);
}

static void format_time(char* buf, size_t n, float t) {
    int cs = (int)(t * 100.0f);
    snprintf(buf, n, "%02d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

// centred text line at a given scale (y in HUD canvas pixels), with drop shadow
static void title_line(float y, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* s) {
    char txt[256];
    textfx_from_utf8(s, txt, sizeof(txt));
    float cw = HUD_ROWS * (float)sapp_width() / (float)sapp_height();
    sdtx_canvas(cw / scale, HUD_ROWS / scale);
    float x = ((cw / scale) / 8.0f - (float)strlen(txt)) * 0.5f;
    float yc = y / (8.0f * scale);
    sdtx_origin(0.15f, 0.15f);
    sdtx_pos(x, yc);
    sdtx_color4b(0, 0, 0, a);
    textfx_puts(txt);
    sdtx_origin(0.0f, 0.0f);
    sdtx_pos(x, yc);
    sdtx_color4b(r, g, b, a);
    textfx_puts(txt);
}

static void draw_title_screen(void) {
    float cw = HUD_ROWS * (float)sapp_width() / (float)sapp_height();
    uint8_t a = (uint8_t)(clampf(S.intro_alpha, 0.0f, 1.0f) * 255.0f);
    float t = S.time;

    title_line(34.0f, 1.5f, 255, 255, 255, a, "Welcome to the");

    // big waving, colour-cycling logo
    const char* logo = "GAIA";
    const float ls = 6.0f;
    int n = (int)strlen(logo);
    sdtx_canvas(cw / ls, HUD_ROWS / ls);
    float x0 = ((cw / ls) / 8.0f - (float)n) * 0.5f;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            float wave = sinf(t * 3.0f + (float)i * 0.8f) * 0.12f;
            sdtx_origin(pass == 0 ? 0.08f : 0.0f, pass == 0 ? 0.08f : 0.0f);
            sdtx_pos(x0 + (float)i, 54.0f / (8.0f * ls) + wave);
            if (pass == 0) {
                sdtx_color4b(60, 0, 40, a);
            } else {
                float h = t * 1.5f + (float)i * 0.6f;
                uint8_t r = 255, g = (uint8_t)(150.0f + 90.0f * sinf(h)), b = (uint8_t)(40.0f + 40.0f * sinf(h + 2.0f));
                sdtx_color4b(r, g, b, a);
            }
            sdtx_putc(logo[i]);
        }
    }
    sdtx_origin(0.0f, 0.0f);

    title_line(112.0f, 2.0f, 90, 230, 255, a, "ANNUAL MEETING 2026");
    title_line(146.0f, 1.25f, 255, 255, 255, a, "Talk on RSI and model performance");
    title_line(162.0f, 1.25f, 255, 214, 90, a, "by Josef Lindman H\xC3\xB6rnlund");
    if (!S.intro_leaving && fmodf(t, 1.1f) < 0.75f) {
        title_line(200.0f, 1.5f, 255, 255, 255, a, "PRESS A BUTTON TO START");
    }
}

static void intro_start(void) {
    if (!S.intro || S.intro_leaving) {
        return;
    }
    S.intro_leaving = true;
    S.cam_mode = CAM_PRESENT;
    S.cam_snap = true;
    present_next(&S.traffic);   // the first slide van starts driving up
}

static void draw_hud(void) {
    float cw = HUD_ROWS * (float)sapp_width() / (float)sapp_height();
    float ch = HUD_ROWS;
    int cols = (int)(cw / 8.0f);
    int rows = (int)(ch / 8.0f);
    sdtx_canvas(cw, ch);
    sdtx_font(0);

    const car_t* hero = &S.traffic.cars[0];
    const car_t* fc = &S.traffic.cars[S.follow];
    char t1[32], t2[32];

    if (S.intro) {
        draw_title_screen();
        return;
    }
    if (S.present_mode) {
        // keep the screen clean: only a small slide counter (H shows the full HUD)
        int cur = present_current_slide();
        if (cur >= 0) {
            hud_text((float)(cols - 8), (float)(rows - 2), 200, 200, 210, "%2d/%-2d", cur + 1, present_slide_count());
        }
        if (S.cam_mode != CAM_PRESENT && S.show_hud) {
            hud_text(1, (float)(rows - 2), 160, 255, 160, "%s", CAM_NAMES[S.cam_mode]);
        }
    }
    if (S.show_hud && !S.present_mode) {
        format_time(t1, sizeof(t1), S.time - S.lap_start);
        hud_text(1, 1, 255, 210, 40, "TIME");
        hud_text(6, 1, 255, 255, 255, "%s", t1);
        hud_text(1, 2, 255, 210, 40, "LAP");
        hud_text(6, 2, 255, 255, 255, "%d", S.lap + 1);
        if (S.best_lap > 0.0f) {
            format_time(t2, sizeof(t2), S.best_lap);
            hud_text((float)(cols - 15), 1, 255, 210, 40, "BEST");
            hud_text((float)(cols - 10), 1, 255, 255, 255, "%s", t2);
        }
        const char* title = "SOKOL HIGHWAY";
        hud_text((float)(cols - (int)strlen(title)) * 0.5f, 1, 80, 230, 255, "%s", title);
        if (S.attract && fmodf(S.time, 1.2f) < 0.8f) {
            hud_text((float)(cols - 4) * 0.5f, 2, 255, 90, 160, "DEMO");
        }

        // camera / car info bottom-left
        hud_text(1, (float)(rows - 3), 160, 255, 160, "%s", CAM_NAMES[S.cam_mode]);
        hud_text(1, (float)(rows - 2), 200, 200, 200, "%s%s",
                 S.follow == 0 ? "PLAYER" : "TRAFFIC",
                 (S.follow == 0 && !S.traffic.autopilot) ? " - MANUAL" : (S.follow == 0 ? " - AUTO" : ""));

        // big speedometer bottom-right (double-size canvas)
        sdtx_canvas(cw * 0.5f, ch * 0.5f);
        int kmh = (int)(fc->speed * 3.6f + 0.5f);
        // digits occupy small-canvas columns [cols-13, cols-7) and rows [rows-4, rows-2)
        float bx = (float)(cols - 13) * 0.5f, by = (float)(rows - 4) * 0.5f;
        uint8_t sg = kmh > 200 ? 70 : 230, sb = kmh > 200 ? 50 : 60;
        hud_text(bx, by, 255, sg, sb, "%3d", kmh);
        sdtx_canvas(cw, ch);
        hud_text((float)(cols - 6), (float)(rows - 3), 255, 255, 255, "KM/H");
        (void)hero;
    }

    if (S.show_help) {
        static const char* demo_lines[] = {
            "ARROWS DRIVE   P AUTOPILOT",
            "C CAMERA  TAB CAR  A DEMO",
            "R RES  J JITTER  T AFFINE",
            "D DITHER F FILTER S SCAN",
            "H HUD  F1 HELP  ESC QUIT",
        };
        static const char* present_lines[] = {
            "NEXT  SPACE > PGDN CLICK",
            "PREV  < PGUP RIGHT-CLICK",
            "C CAMERA  R RES  F FILTER",
            "J JITTER  D DITHER  S SCAN",
            "H HUD  F1 HELP  ESC QUIT",
        };
        const char** lines = S.present_mode ? present_lines : demo_lines;
        int n = 5;
        float y0 = (float)rows * 0.5f - (float)n * 0.5f;
        for (int i = 0; i < n; i++) {
            hud_text((float)(cols - (int)strlen(lines[i])) * 0.5f, y0 + (float)i, 255, 255, 255, "%s", lines[i]);
        }
    }
}

// upscale the low-res image to the current pass + debug text overlay
static void render_display(void) {
    disp_params_t dp = {
        .res = { (float)S.rt_w, (float)S.rt_h, 0, 0 },
        .flags = { S.dither ? 1.0f : 0.0f, S.scanlines ? 0.35f : 0.0f, S.intro_dim, S.fade_black },
    };
    sg_apply_pipeline(S.pip_display);
    sg_apply_bindings(&(sg_bindings){ .views[0] = S.rt_color_tex, .samplers[0] = S.smp_display });
    sg_apply_uniforms(0, &SG_RANGE(dp));
    sg_draw(0, 3, 1);
    sdtx_draw();
}

static void create_capture_target(int w, int h) {
    if (S.cap_w > 0) {
        sg_destroy_view(S.cap_color_att);
        sg_destroy_view(S.cap_depth_att);
        sg_destroy_image(S.cap_color);
        sg_destroy_image(S.cap_depth);
    }
    sg_environment env = sg_query_desc().environment;
    S.cap_w = w;
    S.cap_h = h;
    S.cap_color = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = w, .height = h,
        .pixel_format = env.defaults.color_format,
        .sample_count = env.defaults.sample_count,
        .label = "capture-color",
    });
    S.cap_depth = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true,
        .width = w, .height = h,
        .pixel_format = env.defaults.depth_format,
        .sample_count = env.defaults.sample_count,
        .label = "capture-depth",
    });
    S.cap_color_att = sg_make_view(&(sg_view_desc){ .color_attachment.image = S.cap_color });
    S.cap_depth_att = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = S.cap_depth });
}

// ---------------------------------------------------------------------------
//  Frame
// ---------------------------------------------------------------------------
static void frame(void) {
    float dt = (float)sapp_frame_duration();
    dt = clampf(dt, 0.0f, 1.0f / 20.0f);
    S.time += dt;

    // manual hero control (cruise speed)
    car_t* hero = &S.traffic.cars[0];
    if (S.present_mode) {
        present_update(&S.traffic, dt);
        int cam = present_take_camera_request();
        if (cam >= 0 && cam != S.cam_mode) {
            S.cam_mode = cam;
            S.cam_snap = true;
        }
    } else if (S.key_up) {
        hero->v0 = fminf(hero->v0 + 18.0f * dt, 80.0f);
    }
    if (S.key_down) {
        hero->v0 = fmaxf(hero->v0 - 30.0f * dt, 0.0f);
    }

    traffic_update(&S.traffic, dt);

    int lap = (int)(hero->odo / (double)S.traffic.L);
    if (lap > S.lap) {
        S.last_lap = S.time - S.lap_start;
        if (S.best_lap <= 0.0f || S.last_lap < S.best_lap) {
            S.best_lap = S.last_lap;
        }
        S.lap = lap;
        S.lap_start = S.time;
    }

    if (S.intro) {
        S.fade_black = fmaxf(0.0f, S.fade_black - dt / 1.5f);
        if (S.intro_leaving) {
            S.intro_alpha = fmaxf(0.0f, S.intro_alpha - dt / 0.5f);
            S.intro_dim = fmaxf(0.0f, S.intro_dim - dt / 0.8f);
            if (S.intro_alpha <= 0.0f && S.intro_dim <= 0.0f) {
                S.intro = false;
            }
        } else {
            // title-screen attract cameras
            static const int cams[3] = { CAM_HELI, CAM_TV, CAM_CHASE };
            S.intro_cam_timer += dt;
            if (S.intro_cam_timer > 8.0f) {
                S.intro_cam_timer = 0.0f;
                S.intro_cam = (S.intro_cam + 1) % 3;
                S.cam_mode = cams[S.intro_cam];
                S.cam_snap = true;
            }
        }
    }
    if (S.attract) {
        S.attract_timer += dt;
        if (S.attract_timer > ATTRACT_INTERVAL) {
            S.attract_timer = 0.0f;
            S.cam_mode = (S.cam_mode + 1) % CAM_NUM;
            S.cam_snap = true;
        }
    }
    if (S.help_timer > 0.0f) {
        S.help_timer -= dt;
        if (S.help_timer <= 0.0f) {
            S.show_help = false;
        }
    }

    // (re)create the low-res render target when the window or mode changes
    int win_w = sapp_width(), win_h = sapp_height();
    int th = RES_MODES[S.res_mode] ? RES_MODES[S.res_mode] : win_h;
    int tw = RES_MODES[S.res_mode] ? (int)((float)th * (float)win_w / (float)win_h + 0.5f) : win_w;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw != S.rt_w || th != S.rt_h) {
        create_render_target(tw, th);
    }

    if (S.present_mode) {
        present_render_slides();   // no-op after the first frame
    }
    bool snapped = S.cam_snap;
    update_camera(dt);
    S.cam_snap = snapped;
    update_sign_orientation(dt);
    S.cam_snap = false;
    render_scene();

    draw_hud();
    sg_begin_pass(&(sg_pass){
        .action.colors[0] = { .load_action = SG_LOADACTION_DONTCARE },
        .swapchain = sglue_swapchain(),
        .label = "display-pass",
    });
    render_display();
    sg_end_pass();

    S.frame_count++;
    for (int i = 0; i < g_num_next_at; i++) {
        if (S.present_mode && g_next_at[i] == S.frame_count) {
            if (S.intro && !S.intro_leaving) {
                intro_start();
            } else {
                present_next(&S.traffic);
            }
        }
    }
    if (S.cli_shot && S.frame_count == S.cli_shot_frame) {
        snprintf(S.shot_path, sizeof(S.shot_path), "%s", S.cli_shot);
        S.shot_requested = true;
    }
    bool shot = S.shot_requested;
    if (shot) {
        // re-run the display pass into an offscreen image that can be read back
        if (S.cap_w != win_w || S.cap_h != win_h) {
            create_capture_target(win_w, win_h);
        }
        sg_begin_pass(&(sg_pass){
            .action = {
                .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0, 0, 0, 1 } },
                .depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 1.0f },
            },
            .attachments = { .colors[0] = S.cap_color_att, .depth_stencil = S.cap_depth_att },
            .label = "capture-pass",
        });
        render_display();
        sg_end_pass();
    }
    sg_commit();

    if (shot) {
        S.shot_requested = false;
        bool bgra = sg_query_desc().environment.defaults.color_format == SG_PIXELFORMAT_BGRA8;
        if (highway_save_png(S.cap_color, S.cap_w, S.cap_h, bgra, S.shot_path)) {
            printf("[highway] saved screenshot %s\n", S.shot_path);
        } else {
            printf("[highway] failed to save screenshot %s\n", S.shot_path);
        }
        if (S.cli_shot) {
            sapp_quit();
        }
    }
}

static void event(const sapp_event* e) {
    if (S.cli_shot) {
        return;   // scripted screenshot runs ignore input so they stay reproducible
    }
    if (S.intro) {
        bool key = e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat && e->key_code != SAPP_KEYCODE_ESCAPE;
        if (key || e->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
            intro_start();
            return;
        }
        if (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code != SAPP_KEYCODE_ESCAPE) {
            return;
        }
    }
    if (S.present_mode) {
        if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
            if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) present_next(&S.traffic);
            if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) present_prev(&S.traffic);
            return;
        }
        if (e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat) {
            switch (e->key_code) {
                case SAPP_KEYCODE_SPACE: case SAPP_KEYCODE_RIGHT: case SAPP_KEYCODE_DOWN:
                case SAPP_KEYCODE_PAGE_DOWN: case SAPP_KEYCODE_ENTER: case SAPP_KEYCODE_N:
                    present_next(&S.traffic);
                    return;
                case SAPP_KEYCODE_LEFT: case SAPP_KEYCODE_UP: case SAPP_KEYCODE_PAGE_UP:
                case SAPP_KEYCODE_BACKSPACE:
                    present_prev(&S.traffic);
                    return;
                default: break;
            }
        }
        if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
            (e->key_code == SAPP_KEYCODE_P || e->key_code == SAPP_KEYCODE_A || e->key_code == SAPP_KEYCODE_TAB)) {
            return;   // driving / demo keys are not used while presenting
        }
    }
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN) {
        switch (e->key_code) {
            case SAPP_KEYCODE_ESCAPE: sapp_request_quit(); break;
            case SAPP_KEYCODE_UP:     S.key_up = true; S.traffic.autopilot = false; break;
            case SAPP_KEYCODE_DOWN:   S.key_down = true; S.traffic.autopilot = false; break;
            case SAPP_KEYCODE_LEFT:
                if (!e->key_repeat) {
                    S.traffic.autopilot = false;
                    traffic_request_lane(&S.traffic, 0, 0);
                }
                break;
            case SAPP_KEYCODE_RIGHT:
                if (!e->key_repeat) {
                    S.traffic.autopilot = false;
                    traffic_request_lane(&S.traffic, 0, 1);
                }
                break;
            default: break;
        }
        if (e->key_repeat) {
            return;
        }
        switch (e->key_code) {
            case SAPP_KEYCODE_P:
                S.traffic.autopilot = !S.traffic.autopilot;
                if (S.traffic.autopilot) {
                    S.traffic.cars[0].v0 = 62.0f;
                }
                break;
            case SAPP_KEYCODE_C: S.cam_mode = (S.cam_mode + 1) % CAM_NUM; S.cam_snap = true; S.attract = false; break;
            case SAPP_KEYCODE_TAB:
                S.follow = (S.follow + 1) % S.traffic.num;
                S.cam_snap = true;
                break;
            case SAPP_KEYCODE_A: S.attract = !S.attract; S.attract_timer = 0.0f; break;
            case SAPP_KEYCODE_R: S.res_mode = (S.res_mode + 1) % NUM_RES_MODES; break;
            case SAPP_KEYCODE_J: S.jitter = !S.jitter; break;
            case SAPP_KEYCODE_T: S.affine = !S.affine; break;
            case SAPP_KEYCODE_D: S.dither = !S.dither; break;
            case SAPP_KEYCODE_F: S.bilinear = !S.bilinear; break;
            case SAPP_KEYCODE_S: S.scanlines = !S.scanlines; break;
            case SAPP_KEYCODE_H: S.show_hud = !S.show_hud; break;
            case SAPP_KEYCODE_F1: S.show_help = !S.show_help; S.help_timer = 0.0f; break;
            case SAPP_KEYCODE_0: S.follow = 0; S.cam_snap = true; break;
            case SAPP_KEYCODE_F12: {
                static int shot_index = 0;
                snprintf(S.shot_path, sizeof(S.shot_path), "highway-shot-%03d.png", ++shot_index);
                S.shot_requested = true;
            } break;
            default: break;
        }
    } else if (e->type == SAPP_EVENTTYPE_KEY_UP) {
        if (e->key_code == SAPP_KEYCODE_UP) S.key_up = false;
        if (e->key_code == SAPP_KEYCODE_DOWN) S.key_down = false;
    }
}

static void cleanup(void) {
    sdtx_shutdown();
    sg_shutdown();
}

static void print_usage(void) {
    printf("usage: highway [options]\n"
           "  --slides PATH    slides file (default: slides.txt)\n"
           "  --demo           free-roaming demo with traffic instead of the presentation\n"
           "  --no-intro       skip the title screen\n"
           "  --cam N          start camera (0 present, 1 chase, 2 bumper, 3 heli, 4 tv)\n"
           "  --res N          render resolution (0 = 240p, 1 = 360p, 2 = 540p, 3 = 720p (default), 4 = native)\n"
           "  --car N          follow car N (0 = player)\n"
           "  --fullscreen     start in fullscreen\n"
           "  --shot PATH      save a PNG screenshot after --shot-frame frames and quit\n"
           "  --shot-frame N   frame for --shot (default 240)\n");
}

static int g_start_cam = -1, g_start_res = DEFAULT_RES_MODE, g_start_car = 0;
static bool g_fullscreen = false;

static void apply_cli_settings(void) {
    if (g_start_cam >= 0 && g_start_cam < CAM_NUM) {
        S.cam_mode = g_start_cam;
        S.attract = false;
    }
    if (g_start_res >= 0 && g_start_res < NUM_RES_MODES) {
        S.res_mode = g_start_res;
    }
    if (g_start_car >= 0 && g_start_car < S.traffic.num) {
        S.follow = g_start_car;
    }
}

static void init_cb(void) {
    init();
    apply_cli_settings();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    S.cli_shot_frame = 240;
    S.present_mode = true;
    S.slides_path = "slides.txt";
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool has_val = (i + 1 < argc);
        if (!strcmp(a, "--cam") && has_val) { g_start_cam = atoi(argv[++i]); }
        else if (!strcmp(a, "--res") && has_val) { g_start_res = atoi(argv[++i]); }
        else if (!strcmp(a, "--car") && has_val) { g_start_car = atoi(argv[++i]); }
        else if (!strcmp(a, "--shot") && has_val) { S.cli_shot = argv[++i]; }
        else if (!strcmp(a, "--shot-frame") && has_val) { S.cli_shot_frame = atoi(argv[++i]); }
        else if (!strcmp(a, "--fullscreen")) { g_fullscreen = true; }
        else if (!strcmp(a, "--demo")) { S.present_mode = false; }
        else if (!strcmp(a, "--no-intro")) { g_no_intro = true; }
        else if (!strcmp(a, "--slides") && has_val) { S.slides_path = argv[++i]; }
        else if (!strcmp(a, "--next-at") && has_val) { g_next_at[g_num_next_at < 16 ? g_num_next_at++ : 15] = atoi(argv[++i]); }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { print_usage(); exit(0); }
        else { printf("unknown option: %s\n", a); print_usage(); exit(1); }
    }
    return (sapp_desc){
        .init_cb = init_cb,
        .fullscreen = g_fullscreen,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 1280,
        .height = 720,
        .high_dpi = true,
        .sample_count = 1,
        .swap_interval = 1,
        .window_title = "Sokol Highway",
        .logger.func = slog_func,
    };
}
