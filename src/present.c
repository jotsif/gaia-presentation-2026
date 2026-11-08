#include "present.h"
#include "sokol_debugtext.h"
#include "world.h"
#include "chart.h"
#include "textfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define MAX_SLIDES      64
#define MAX_LINES       32
#define LINE_LEN        160
#define MAX_ROWS        96

// slide texture / text layout space (image slides get a 2x larger texture)
#define SLIDE_TEX_W     1024
#define SLIDE_TEX_H     576
#define IMAGE_TEX_SCALE 2
#define MAX_IMAGE_DIM   4096
#define CANVAS_W        512.0f      // layout units (each font pixel at scale 1 = 2x2 texels)
#define CANVAS_H        288.0f
#define MARGIN          20.0f
#define SLIDE_FONT      0

// choreography
#define PRESENT_GAP     12.0f       // centre distance ahead of the player car while presenting
#define START_REL       (-50.0f)    // spawn distance behind the player car
#define START_REL_VEL   25.0f       // closing speed when overtaking
#define SMALL_SCALE     0.18f       // folded sign size while overtaking
#define GROW_TIME       0.9f

enum { K_TITLE, K_SUB, K_H3, K_TEXT, K_BULLET, K_BLANK };
// font scales; with 2 texels per layout unit, x.5 scales still map font pixels to whole texels
static const float KIND_SCALE[] = { 4.0f, 3.0f, 2.5f, 2.5f, 2.5f, 2.5f };

typedef struct {
    int kind;
    char text[LINE_LEN];
} sline_t;

typedef struct {
    sline_t lines[MAX_LINES];
    int n;
    int camera;     // camera requested via '@camera', -1 = keep the current one
    int zoom;       // '@zoom on/off': 1/0, -1 = automatic (on for image slides)
    char image_path[512];
    // runtime
    bool has_image;
    int img_w, img_h;
    sg_image img;
    sg_view img_view;
    float img_rect[4];  // x0, y0, x1, y1 in layout units (image or chart area)
    chart_data_t* chart;    // '@chart file.yaml'
    chart_anim_t anim;
} slide_t;

#define CHART_MAX_VERTS (1 << 16)

// vector graphics for charts: positions in layout units, per-vertex colour, alpha blended
static const char* CANVAS_MSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct params { float4 canvas; };\n"
"struct vs_in { float2 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };\n"
"struct vs_out { float4 pos [[position]]; float4 color; };\n"
"vertex vs_out vs_canvas(vs_in in [[stage_in]], constant params& p [[buffer(0)]]) {\n"
"    vs_out o;\n"
"    o.pos = float4(in.pos.x / p.canvas.x * 2.0 - 1.0, 1.0 - in.pos.y / p.canvas.y * 2.0, 0.0, 1.0);\n"
"    o.color = in.color;\n"
"    return o;\n"
"}\n"
"fragment float4 fs_canvas(vs_out in [[stage_in]]) { return in.color; }\n";

// implemented in sokol_impl.m
bool highway_load_image(const char* path, int max_dim, int* out_w, int* out_h, uint8_t** out_rgba);

// draws an image into a rectangle of the slide render target
static const char* BLIT_MSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct params { float4 rect; };\n"
"struct vs_out { float4 pos [[position]]; float2 uv; };\n"
"vertex vs_out vs_blit(uint vid [[vertex_id]], constant params& p [[buffer(0)]]) {\n"
"    float2 t = float2(float(vid & 1), float(vid >> 1));\n"
"    vs_out o;\n"
"    o.pos = float4(mix(p.rect.x, p.rect.z, t.x), mix(p.rect.y, p.rect.w, t.y), 0.0, 1.0);\n"
"    o.uv = t;\n"
"    return o;\n"
"}\n"
"fragment float4 fs_blit(vs_out in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
"    return tex.sample(smp, in.uv);\n"
"}\n";

typedef enum { SC_FREE, SC_APPROACH, SC_PRESENT, SC_LEAVE } sc_state_t;

typedef struct {
    int car;
    int slide;
    sc_state_t state;
    float rel, rel_vel;     // position/velocity relative to the player car (along the road)
    float grow;             // 0..1 sign pop-up progress
} sign_car_t;

static struct {
    slide_t* slides;
    int nslides;
    sg_image img[MAX_SLIDES];
    sg_view att[MAX_SLIDES];
    sg_view tex[MAX_SLIDES];
    sdtx_context ctx;
    bool rendered;
    sign_car_t cars[PRESENT_MAX_SIGNS];
    int current;
    float start_timer;
    int cam_request;
    float gap;              // current presenting distance (PRESENT_GAP by default)
    char base_dir[512];     // directory of the slides file (image paths are relative to it)
    sg_pipeline blit_pip;
    sg_sampler blit_smp;
    sg_pipeline canvas_pip;
    sg_buffer canvas_buf;
    chart_vert_t* canvas_verts;
} P;

static const vec3 SIGN_VAN_COLORS[] = {
    { 0.95f, 0.80f, 0.10f }, { 0.12f, 0.35f, 0.90f }, { 0.95f, 0.45f, 0.08f },
    { 0.10f, 0.60f, 0.30f }, { 0.60f, 0.20f, 0.70f }, { 0.92f, 0.92f, 0.92f },
};

// ---------------------------------------------------------------------------
//  Slide file parsing
// ---------------------------------------------------------------------------

// converts UTF-8 to the 7-bit ASCII the bitmap font can show
static void sanitize(const char* in, char* out, size_t cap) {
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)in;
    while (*p && o + 4 < cap) {
        if (*p < 0x80) {
            out[o++] = (char)*p++;
            continue;
        }
        const char* rep = "?";
        char code[2] = { 0, 0 };
        int len = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : (*p >= 0xC0) ? 2 : 1;
        if (len == 3 && p[0] == 0xE2 && p[1] == 0x80) {
            if (p[2] == 0x93 || p[2] == 0x94) rep = "-";
            else if (p[2] == 0x98 || p[2] == 0x99) rep = "'";
            else if (p[2] == 0x9C || p[2] == 0x9D) rep = "\"";
            else if (p[2] == 0xA6) rep = "...";
            else if (p[2] == 0xA2) rep = "*";
        } else if (len == 3 && p[0] == 0xE2 && p[1] == 0x86 && p[2] == 0x92) {
            rep = "->";
        } else if (len == 2 && p[0] == 0xC2 && p[1] == 0xA0) {
            rep = " ";
        } else if (len == 2 && p[0] == 0xC3 && textfx_code(p[1])) {
            code[0] = (char)textfx_code(p[1]);   // composed letter (drawn by textfx_puts)
            rep = code;
        }
        for (const char* r = rep; *r && o + 1 < cap; r++) {
            out[o++] = *r;
        }
        for (int i = 0; i < len && *p; i++) {
            p++;
        }
    }
    out[o] = 0;
}

static void add_line(slide_t* sl, int kind, const char* text) {
    if (sl->n >= MAX_LINES) {
        return;
    }
    sl->lines[sl->n].kind = kind;
    sanitize(text, sl->lines[sl->n].text, LINE_LEN);
    sl->n++;
}

static slide_t* new_slide(void) {
    if (P.nslides >= MAX_SLIDES) {
        return NULL;
    }
    slide_t* sl = &P.slides[P.nslides++];
    memset(sl, 0, sizeof(*sl));
    sl->camera = -1;
    sl->zoom = -1;
    return sl;
}

static bool load_slides(const char* path) {
    FILE* f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        return false;
    }
    char buf[1024];
    slide_t* sl = new_slide();
    while (sl && fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
            buf[--n] = 0;
        }
        const char* s = buf;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        // an indented line directly after a bullet continues that bullet
        if (s != buf && *s && sl->n > 0 && sl->lines[sl->n - 1].kind == K_BULLET && s[0] != '-' && s[0] != '*') {
            char* prev = sl->lines[sl->n - 1].text;
            char tmp[LINE_LEN];
            sanitize(s, tmp, sizeof(tmp));
            size_t pl = strlen(prev);
            snprintf(prev + pl, LINE_LEN - pl, " %s", tmp);
            continue;
        }
        if (strcmp(s, "---") == 0) {
            if (sl->n > 0 || sl->camera >= 0 || sl->image_path[0] || sl->chart) {
                sl = new_slide();
            }
        } else if (strncmp(s, "@image ", 7) == 0) {
            const char* ip = s + 7;
            if (ip[0] == '/') {
                snprintf(sl->image_path, sizeof(sl->image_path), "%s", ip);
            } else if (ip[0] == '~' && getenv("HOME")) {
                snprintf(sl->image_path, sizeof(sl->image_path), "%s%s", getenv("HOME"), ip + 1);
            } else {
                snprintf(sl->image_path, sizeof(sl->image_path), "%s%s", P.base_dir, ip);
            }
        } else if (strncmp(s, "@chart ", 7) == 0) {
            char path[512];
            const char* cp = s + 7;
            if (cp[0] == '/') snprintf(path, sizeof(path), "%s", cp);
            else if (cp[0] == '~' && getenv("HOME")) snprintf(path, sizeof(path), "%s%s", getenv("HOME"), cp + 1);
            else snprintf(path, sizeof(path), "%s%s", P.base_dir, cp);
            sl->chart = (chart_data_t*)calloc(1, sizeof(chart_data_t));
            if (chart_load_yaml(sl->chart, path)) {
                printf("[highway] chart: %d models from %s\n", sl->chart->n, path);
            } else {
                printf("[highway] could not load chart data %s\n", path);
                free(sl->chart);
                sl->chart = NULL;
                add_line(sl, K_H3, "missing chart data:");
                add_line(sl, K_TEXT, path);
            }
        } else if (strncmp(s, "@label ", 7) == 0 || strncmp(s, "@hide ", 6) == 0) {
            bool hide = (s[1] == 'h');
            char id[96];
            const char* rest = s + (hide ? 6 : 7);
            int n_id = 0;
            while (rest[n_id] && rest[n_id] != ' ' && n_id < 95) n_id++;
            snprintf(id, sizeof(id), "%.*s", n_id, rest);
            const char* label = rest + n_id;
            while (*label == ' ') label++;
            bool ok = sl->chart && (hide ? chart_hide(sl->chart, id) : chart_set_label(sl->chart, id, label));
            if (!ok) printf("[highway] %s: unknown model '%s' (put it after @chart)\n", hide ? "@hide" : "@label", id);
        } else if (strncmp(s, "@steps ", 7) == 0) {
            if (sl->chart) {
                const char* sp = s + 7;
                char* end;
                float v;
                while (sl->chart->nstops < CHART_MAX_STOPS && (v = strtof(sp, &end), end != sp)) {
                    sl->chart->stops[sl->chart->nstops++] = v;
                    sp = end;
                }
            }
        } else if (strncmp(s, "@zoom ", 6) == 0) {
            sl->zoom = (strcmp(s + 6, "on") == 0) ? 1 : 0;
        } else if (strncmp(s, "@camera ", 8) == 0) {
            static const char* names[CAM_NUM] = { "present", "chase", "bumper", "heli", "tv" };
            for (int c = 0; c < CAM_NUM; c++) {
                if (strcmp(s + 8, names[c]) == 0) {
                    sl->camera = c;
                }
            }
        } else if (strncmp(s, "### ", 4) == 0) { add_line(sl, K_H3, s + 4); }
        else if (strncmp(s, "## ", 3) == 0)   { add_line(sl, K_SUB, s + 3); }
        else if (strncmp(s, "# ", 2) == 0)    { add_line(sl, K_TITLE, s + 2); }
        else if (strncmp(s, "- ", 2) == 0 || strncmp(s, "* ", 2) == 0) { add_line(sl, K_BULLET, s + 2); }
        else if (*s == 0)                     { add_line(sl, K_BLANK, ""); }
        else                                  { add_line(sl, K_TEXT, s); }
    }
    fclose(f);
    // drop a trailing empty slide
    if (P.nslides > 0 && P.slides[P.nslides - 1].n == 0 && !P.slides[P.nslides - 1].image_path[0] &&
        !P.slides[P.nslides - 1].chart) {
        P.nslides--;
    }
    return P.nslides > 0;
}

// ---------------------------------------------------------------------------
//  Slide rendering (sokol-debugtext into an offscreen texture)
// ---------------------------------------------------------------------------
typedef struct {
    int kind;
    float scale;
    bool center, bullet_first;
    char text[LINE_LEN];
} row_t;

// word-wraps 'text' into rows of at most 'cols' characters
static int wrap(const char* text, int cols, char out[][LINE_LEN], int max_out) {
    int n = 0;
    const char* p = text;
    if (cols < 4) {
        cols = 4;
    }
    while (*p && n < max_out) {
        while (*p == ' ') {
            p++;
        }
        int len = (int)strlen(p);
        if (len <= cols) {
            snprintf(out[n++], LINE_LEN, "%s", p);
            break;
        }
        int cut = cols;
        while (cut > 0 && p[cut] != ' ') {
            cut--;
        }
        if (cut == 0) {
            cut = cols;   // single long word: hard break
        }
        snprintf(out[n++], LINE_LEN, "%.*s", cut, p);
        p += cut;
    }
    if (n == 0 && max_out > 0) {
        out[n++][0] = 0;
    }
    return n;
}

static void put_text(float x_px, float y_px, float scale, uint8_t r, uint8_t g, uint8_t b, const char* txt) {
    sdtx_canvas(CANVAS_W / scale, CANVAS_H / scale);
    float cx = x_px / (8.0f * scale), cy = y_px / (8.0f * scale);
    sdtx_origin(0.14f, 0.14f);
    sdtx_pos(cx, cy);
    sdtx_color3b(0, 0, 20);
    textfx_puts(txt);
    sdtx_origin(0.0f, 0.0f);
    sdtx_pos(cx, cy);
    sdtx_color3b(r, g, b);
    textfx_puts(txt);
}

static void layout_slide(slide_t* sl) {
    static row_t rows[MAX_ROWS];
    int nr = 0;
    bool has_bullets = false;
    for (int i = 0; i < sl->n; i++) {
        has_bullets |= (sl->lines[i].kind == K_BULLET);
    }
    // skip leading / trailing blank lines
    int first = 0, last = sl->n - 1;
    while (first <= last && sl->lines[first].kind == K_BLANK) first++;
    while (last >= first && sl->lines[last].kind == K_BLANK) last--;

    for (int i = first; i <= last && nr < MAX_ROWS; i++) {
        const sline_t* ln = &sl->lines[i];
        float scale = KIND_SCALE[ln->kind];
        if (ln->kind == K_BLANK) {
            rows[nr++] = (row_t){ .kind = K_BLANK, .scale = scale };
            continue;
        }
        int cols = (int)((CANVAS_W - 2.0f * MARGIN) / (8.0f * scale));
        int indent = (ln->kind == K_BULLET) ? 2 : 0;
        char wrapped[16][LINE_LEN];
        int nw = wrap(ln->text, cols - indent, wrapped, 16);
        for (int w = 0; w < nw && nr < MAX_ROWS; w++) {
            row_t* r = &rows[nr++];
            r->kind = ln->kind;
            r->scale = scale;
            r->center = (ln->kind == K_TITLE || ln->kind == K_SUB || ln->kind == K_H3) ||
                        (ln->kind == K_TEXT && !has_bullets);
            r->bullet_first = (ln->kind == K_BULLET && w == 0);
            snprintf(r->text, LINE_LEN, "%s", wrapped[w]);
        }
    }

    float total = 0.0f;
    for (int i = 0; i < nr; i++) {
        total += (rows[i].kind == K_BLANK) ? 9.0f : 8.0f * rows[i].scale * 1.35f;
    }
    float y = (CANVAS_H - total) * 0.5f;
    if (y < MARGIN) {
        y = MARGIN;
    }
    if (sl->has_image || sl->chart) {
        // text (usually a title) on top, the image / chart in the remaining space
        const float pad = 8.0f;
        y = (nr > 0) ? pad + 2.0f : pad;
        float top = (nr > 0) ? y + total + 2.0f : pad;
        float aw = CANVAS_W - 2.0f * pad, ah = CANVAS_H - pad - top;
        float ia = sl->chart ? aw / ah : (float)sl->img_w / (float)sl->img_h;
        float w = aw, h = aw / ia;
        if (h > ah) {
            h = ah;
            w = ah * ia;
        }
        float x0 = (CANVAS_W - w) * 0.5f, y0 = top + (ah - h) * 0.5f;
        sl->img_rect[0] = x0; sl->img_rect[1] = y0;
        sl->img_rect[2] = x0 + w; sl->img_rect[3] = y0 + h;
    }
    for (int i = 0; i < nr; i++) {
        const row_t* r = &rows[i];
        if (r->kind == K_BLANK) {
            y += 9.0f;
            continue;
        }
        float cw = 8.0f * r->scale;
        float x = MARGIN;
        if (r->center) {
            x = (CANVAS_W - (float)strlen(r->text) * cw) * 0.5f;
        }
        switch (r->kind) {
            case K_TITLE: put_text(x, y, r->scale, 255, 214, 64, r->text); break;
            case K_SUB:   put_text(x, y, r->scale, 255, 255, 255, r->text); break;
            case K_H3:    put_text(x, y, r->scale, 110, 225, 255, r->text); break;
            case K_BULLET:
                if (r->bullet_first) {
                    put_text(x, y, r->scale, 255, 214, 64, ">");
                }
                put_text(x + 2.0f * cw, y, r->scale, 235, 238, 245, r->text);
                break;
            default:      put_text(x, y, r->scale, 225, 230, 240, r->text); break;
        }
        y += 8.0f * r->scale * 1.35f;
    }
}

static void chart_text(float x, float y, float scale, uint32_t rgba, const char* s) {
    sdtx_canvas(CANVAS_W / scale, CANVAS_H / scale);
    float cx = x / (8.0f * scale), cy = y / (8.0f * scale);
    uint8_t a = (uint8_t)(rgba >> 24);
    sdtx_origin(0.14f, 0.14f);
    sdtx_pos(cx, cy);
    sdtx_color4b(0, 0, 20, a);
    textfx_puts(s);
    sdtx_origin(0.0f, 0.0f);
    sdtx_pos(cx, cy);
    sdtx_color4b((uint8_t)rgba, (uint8_t)(rgba >> 8), (uint8_t)(rgba >> 16), a);
    textfx_puts(s);
}

static bool slide_on_screen(int slide) {
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        if (P.cars[i].state != SC_FREE && P.cars[i].slide == slide) return true;
    }
    return false;
}

void present_render_slides(void) {
    bool todo[MAX_SLIDES] = { false };
    int ntodo = 0;
    for (int i = 0; i < P.nslides; i++) {
        todo[i] = !P.rendered || (P.slides[i].chart && slide_on_screen(i));
        ntodo += todo[i] ? 1 : 0;
    }
    P.rendered = true;
    if (ntodo == 0) {
        return;
    }
    int first[MAX_SLIDES] = { 0 }, count[MAX_SLIDES] = { 0 }, nverts = 0;
    sdtx_set_context(P.ctx);
    for (int i = 0; i < P.nslides; i++) {
        if (!todo[i]) continue;
        sdtx_layer(i);
        sdtx_font(SLIDE_FONT);
        layout_slide(&P.slides[i]);
        if (P.slides[i].chart) {
            first[i] = nverts;
            count[i] = chart_build(P.slides[i].chart, &P.slides[i].anim, P.slides[i].img_rect,
                                   P.canvas_verts + nverts, CHART_MAX_VERTS - nverts, chart_text);
            nverts += count[i];
        }
    }
    if (nverts > 0) {
        sg_write_buffer_transient(&(sg_write_buffer_desc){
            .src.data = { P.canvas_verts, (size_t)nverts * sizeof(chart_vert_t) },
            .dst.buffer = P.canvas_buf,
        });
    }
    for (int i = 0; i < P.nslides; i++) {
        if (!todo[i]) continue;
        const slide_t* sl = &P.slides[i];
        sg_begin_pass(&(sg_pass){
            .action.colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = { 0.03f, 0.06f, 0.20f, 1.0f } },
            .attachments.colors[0] = P.att[i],
            .label = "slide-pass",
        });
        if (sl->has_image) {
            float rect[4] = {
                sl->img_rect[0] / CANVAS_W * 2.0f - 1.0f, 1.0f - sl->img_rect[1] / CANVAS_H * 2.0f,
                sl->img_rect[2] / CANVAS_W * 2.0f - 1.0f, 1.0f - sl->img_rect[3] / CANVAS_H * 2.0f,
            };
            sg_apply_pipeline(P.blit_pip);
            sg_apply_bindings(&(sg_bindings){ .views[0] = sl->img_view, .samplers[0] = P.blit_smp });
            sg_apply_uniforms(0, &SG_RANGE(rect));
            sg_draw(0, 4, 1);
        }
        if (count[i] > 0) {
            float canvas[4] = { CANVAS_W, CANVAS_H, 0.0f, 0.0f };
            sg_apply_pipeline(P.canvas_pip);
            sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = P.canvas_buf });
            sg_apply_uniforms(0, &SG_RANGE(canvas));
            sg_draw(first[i], count[i], 1);
        }
        sdtx_context_draw_layer(P.ctx, i);
        sg_end_pass();
    }
    sdtx_set_context(sdtx_default_context());
}

// ---------------------------------------------------------------------------
//  Setup
// ---------------------------------------------------------------------------
void present_init(const char* slides_path, traffic_t* t) {
    memset(&P, 0, sizeof(P));
    P.slides = (slide_t*)calloc(MAX_SLIDES, sizeof(slide_t));
    P.current = -1;
    P.start_timer = 1.0f;
    P.cam_request = -1;
    P.gap = PRESENT_GAP;
    if (slides_path) {
        const char* slash = strrchr(slides_path, '/');
        if (slash) {
            snprintf(P.base_dir, sizeof(P.base_dir), "%.*s/", (int)(slash - slides_path), slides_path);
        }
    }

    if (load_slides(slides_path)) {
        printf("[highway] loaded %d slides from %s\n", P.nslides, slides_path);
    } else {
        printf("[highway] no slides file (%s), using the built-in title slide\n", slides_path ? slides_path : "-");
        P.nslides = 0;
        slide_t* sl = new_slide();
        add_line(sl, K_TITLE, "METR");
        add_line(sl, K_SUB, "task-Completion");
        add_line(sl, K_SUB, "Time Horizons of");
        add_line(sl, K_SUB, "Frontier AI Models");
    }

    P.ctx = sdtx_make_context(&(sdtx_context_desc_t){
        .char_buf_size = 1 << 15,
        .canvas_width = CANVAS_W,
        .canvas_height = CANVAS_H,
        .color_format = SG_PIXELFORMAT_RGBA8,
        .depth_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
    });
    // images
    for (int i = 0; i < P.nslides; i++) {
        slide_t* sl = &P.slides[i];
        if (!sl->image_path[0]) {
            continue;
        }
        uint8_t* px = NULL;
        if (highway_load_image(sl->image_path, MAX_IMAGE_DIM, &sl->img_w, &sl->img_h, &px)) {
            sl->img = sg_make_image(&(sg_image_desc){
                .width = sl->img_w,
                .height = sl->img_h,
                .pixel_format = SG_PIXELFORMAT_RGBA8,
                .data = { px, (size_t)sl->img_w * (size_t)sl->img_h * 4 },
                .label = "slide-image",
            });
            sl->img_view = sg_make_view(&(sg_view_desc){ .texture.image = sl->img });
            sl->has_image = true;
            free(px);
            printf("[highway] slide %d: image %s (%dx%d)\n", i + 1, sl->image_path, sl->img_w, sl->img_h);
        } else {
            printf("[highway] slide %d: could not load image %s\n", i + 1, sl->image_path);
            add_line(sl, K_H3, "missing image:");
            add_line(sl, K_TEXT, sl->image_path);
        }
    }
    sg_shader blit_shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func = { .source = BLIT_MSL, .entry = "vs_blit" },
        .fragment_func = { .source = BLIT_MSL, .entry = "fs_blit" },
        .uniform_blocks[0] = { .stage = SG_SHADERSTAGE_VERTEX, .size = 16, .msl_buffer_n = 0 },
        .views[0].texture = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .image_type = SG_IMAGETYPE_2D,
            .sample_type = SG_IMAGESAMPLETYPE_FLOAT,
            .msl_texture_n = 0,
        },
        .samplers[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .sampler_type = SG_SAMPLERTYPE_FILTERING, .msl_sampler_n = 0 },
        .texture_sampler_pairs[0] = { .stage = SG_SHADERSTAGE_FRAGMENT, .view_slot = 0, .sampler_slot = 0 },
        .label = "blit-shader",
    });
    P.blit_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = blit_shd,
        .primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP,
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
        .label = "blit",
    });
    P.blit_smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

    sg_shader canvas_shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func = { .source = CANVAS_MSL, .entry = "vs_canvas" },
        .fragment_func = { .source = CANVAS_MSL, .entry = "fs_canvas" },
        .uniform_blocks[0] = { .stage = SG_SHADERSTAGE_VERTEX, .size = 16, .msl_buffer_n = 0 },
        .label = "canvas-shader",
    });
    P.canvas_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = canvas_shd,
        .layout = {
            .buffers[0].stride = sizeof(chart_vert_t),
            .attrs = {
                [0] = { .offset = offsetof(chart_vert_t, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(chart_vert_t, color), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = {
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
        .label = "canvas",
    });
    P.canvas_buf = sg_make_buffer(&(sg_buffer_desc){
        .size = CHART_MAX_VERTS * sizeof(chart_vert_t),
        .usage = { .vertex_buffer = true, .write_transient = true },
        .label = "chart-verts",
    });
    P.canvas_verts = (chart_vert_t*)malloc(CHART_MAX_VERTS * sizeof(chart_vert_t));

    for (int i = 0; i < P.nslides; i++) {
        int ts = (P.slides[i].has_image || P.slides[i].chart) ? IMAGE_TEX_SCALE : 1;
        P.img[i] = sg_make_image(&(sg_image_desc){
            .usage.color_attachment = true,
            .width = SLIDE_TEX_W * ts,
            .height = SLIDE_TEX_H * ts,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .sample_count = 1,
            .label = "slide",
        });
        P.att[i] = sg_make_view(&(sg_view_desc){ .color_attachment.image = P.img[i] });
        P.tex[i] = sg_make_view(&(sg_view_desc){ .texture.image = P.img[i] });
    }
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        P.cars[i].car = traffic_add_scripted(t, CAR_SIGN_VAN);
        P.cars[i].state = SC_FREE;
    }
}

void present_shutdown(void) {
    free(P.slides);
    P.slides = NULL;
}

// ---------------------------------------------------------------------------
//  Choreography
// ---------------------------------------------------------------------------
static float ease_out_back(float x) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float t = x - 1.0f;
    return 1.0f + c3 * t * t * t + c1 * t * t;
}

static void show_slide(traffic_t* t, int idx, bool from_back) {
    if (P.slides[idx].chart) {
        chart_anim_reset(&P.slides[idx].anim);
        if (from_back) {   // returning to a chart: show it completed
            P.slides[idx].anim.step = chart_num_steps(P.slides[idx].chart) - 1;
            chart_anim_snap(&P.slides[idx].anim, P.slides[idx].chart);
        }
    }
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        if (P.cars[i].state == SC_APPROACH || P.cars[i].state == SC_PRESENT) {
            P.cars[i].state = SC_LEAVE;
        }
    }
    sign_car_t* sc = NULL;
    for (int i = 0; i < PRESENT_MAX_SIGNS && !sc; i++) {
        if (P.cars[i].state == SC_FREE && P.cars[i].car >= 0) {
            sc = &P.cars[i];
        }
    }
    if (!sc) {   // all busy: recycle the van that is furthest away
        for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
            if (P.cars[i].car >= 0 && (!sc || P.cars[i].rel > sc->rel)) {
                sc = &P.cars[i];
            }
        }
    }
    if (!sc) {
        return;
    }
    const car_t* hero = &t->cars[0];
    car_t* c = &t->cars[sc->car];
    const int ncol = (int)(sizeof(SIGN_VAN_COLORS) / sizeof(SIGN_VAN_COLORS[0]));
    c->active = true;
    c->lane = 0;
    c->lat = LANE_INNER_LAT;
    c->lat_vel = 0.0f;
    c->tint = SIGN_VAN_COLORS[idx % ncol];
    sc->slide = idx;
    sc->state = SC_APPROACH;
    sc->rel = START_REL;
    sc->rel_vel = START_REL_VEL;
    sc->grow = 0.0f;
    c->s = wrapf(hero->s + sc->rel, t->L);
    c->speed = hero->speed + sc->rel_vel;
    P.current = idx;
    if (P.slides[idx].camera >= 0) {
        P.cam_request = P.slides[idx].camera;
    }
}

int present_take_camera_request(void) {
    int c = P.cam_request;
    P.cam_request = -1;
    return c;
}

// the current slide's chart, if its van is still arriving or presenting
static slide_t* active_chart_slide(void) {
    if (P.current < 0 || !P.slides[P.current].chart) return NULL;
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        if (P.cars[i].slide == P.current && (P.cars[i].state == SC_APPROACH || P.cars[i].state == SC_PRESENT)) {
            return &P.slides[P.current];
        }
    }
    return NULL;
}

void present_set_gap(float gap) {
    P.gap = gap;
}

void present_set_autostart(bool on) {
    P.start_timer = on ? 1.0f : 0.0f;
}

void present_next(traffic_t* t) {
    P.start_timer = 0.0f;
    slide_t* cs = active_chart_slide();
    if (cs && cs->anim.step < chart_num_steps(cs->chart) - 1) {
        chart_anim_snap(&cs->anim, cs->chart);   // finish the running step first
        cs->anim.step++;
        return;
    }
    if (P.current + 1 < P.nslides) {
        show_slide(t, P.current + 1, false);
    }
}

void present_prev(traffic_t* t) {
    slide_t* cs = active_chart_slide();
    if (cs && cs->anim.step > 0) {
        cs->anim.step--;
        chart_anim_snap(&cs->anim, cs->chart);
        return;
    }
    if (P.current - 1 >= 0) {
        show_slide(t, P.current - 1, true);
    }
}

void present_update(traffic_t* t, float dt) {
    if (P.start_timer > 0.0f) {
        P.start_timer -= dt;
        if (P.start_timer <= 0.0f && P.current < 0) {
            present_next(t);
        }
    }
    slide_t* cs = active_chart_slide();
    if (cs) {
        chart_anim_update(&cs->anim, cs->chart, dt);
    }
    const car_t* hero = &t->cars[0];
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        sign_car_t* sc = &P.cars[i];
        if (sc->state == SC_FREE) {
            continue;
        }
        car_t* c = &t->cars[sc->car];
        if (sc->state == SC_LEAVE) {
            sc->rel_vel = fminf(sc->rel_vel + 9.0f * dt, 45.0f);
            sc->rel += sc->rel_vel * dt;
            if (sc->rel > 450.0f) {
                sc->state = SC_FREE;
                c->active = false;
                continue;
            }
        } else {
            // critically damped spring towards the presenting position
            const float w = 1.6f;
            float acc = w * w * (P.gap - sc->rel) - 2.0f * w * sc->rel_vel;
            sc->rel_vel += acc * dt;
            sc->rel += sc->rel_vel * dt;
            if (c->lane == 0 && sc->rel > 7.5f) {
                c->lane = 1;   // cut in ahead of the player
            }
            if (sc->state == SC_APPROACH && c->lane == 1 &&
                fabsf(sc->rel - P.gap) < 1.5f && fabsf(c->lat - LANE_OUTER_LAT) < 0.4f) {
                sc->state = SC_PRESENT;
            }
        }
        if (c->lane == 1 || sc->state == SC_LEAVE) {
            sc->grow = fminf(sc->grow + dt / GROW_TIME, 1.0f);
        }
        c->speed = fmaxf(0.0f, hero->speed + sc->rel_vel);
        c->s = wrapf(hero->s + sc->rel, t->L);
        float target_lat = (c->lane == 0) ? LANE_INNER_LAT : LANE_OUTER_LAT;
        const float k = 5.0f;
        c->lat_vel += (k * (target_lat - c->lat) - 2.0f * sqrtf(k) * c->lat_vel) * dt;
        c->lat += c->lat_vel * dt;
    }
}

int present_signs(present_sign_t out[PRESENT_MAX_SIGNS]) {
    int n = 0;
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        const sign_car_t* sc = &P.cars[i];
        if (sc->state == SC_FREE) {
            continue;
        }
        present_sign_t* o = &out[n++];
        o->car = sc->car;
        o->slide = sc->slide;
        o->sign_scale = SMALL_SCALE + (1.0f - SMALL_SCALE) * ease_out_back(sc->grow);
        o->on_top = (sc->state == SC_PRESENT) ||
                    (sc->state == SC_APPROACH && sc->rel > 6.0f) ||
                    (sc->state == SC_LEAVE && sc->rel < 40.0f && sc->rel > 6.0f);
    }
    return n;
}

float present_zoom_target(void) {
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        const sign_car_t* sc = &P.cars[i];
        if (sc->state == SC_PRESENT) {
            const slide_t* sl = &P.slides[sc->slide];
            bool zoom = (sl->zoom >= 0) ? (sl->zoom == 1) : (sl->has_image || sl->chart);
            return zoom ? 1.0f : 0.0f;
        }
    }
    return 0.0f;
}

int present_focus_car(void) {
    for (int i = 0; i < PRESENT_MAX_SIGNS; i++) {
        if (P.cars[i].state == SC_PRESENT) {
            return P.cars[i].car;
        }
    }
    return -1;
}

int present_slide_count(void) { return P.nslides; }
int present_current_slide(void) { return P.current; }
sg_view present_slide_texture(int slide) { return P.tex[slide]; }
