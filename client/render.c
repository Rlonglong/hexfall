#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "util.h"

/* The look follows the browser prototype: pastel hex floors lit by a
 * sky/ground hemisphere and a warm sun, sky-coloured fog, white pillars
 * around a purple slime, and a dark broadcast-style HUD where amber always
 * means "you". Everything this file keeps is for looks only. Where a bean
 * stands, and the moment the whole scene shows (tiles included), come from
 * the interpolation's view (interp.h), except for our own bean and the
 * tiles it has been on, which come from the prediction (predict.h). */

#define SKY ((Color){168, 214, 232, 255})
#define SLIME ((Color){176, 106, 224, 255})
#define PILLAR ((Color){217, 222, 226, 255})
#define TOUCHED ((Color){255, 247, 251, 255})
#define PUPIL ((Color){21, 24, 26, 255})
#define FG ((Color){238, 243, 244, 255})
#define MUTED ((Color){142, 155, 161, 255})
#define AMBER ((Color){242, 177, 52, 255})
#define AMBER_INK ((Color){26, 20, 5, 255})
#define SIGNAL ((Color){229, 72, 77, 255})
#define SIGNAL_INK ((Color){255, 225, 226, 255})
#define GOOD ((Color){126, 224, 129, 255})
#define BOOTH ((Color){10, 14, 16, 209})
#define PANEL ((Color){22, 29, 33, 245})
#define RULE ((Color){43, 55, 61, 255})

static const Color LAYER_COLORS[SIM_LAYERS] = {
    {255, 143, 184, 255},  /* top: pink */
    {255, 211, 107, 255},  /* middle: yellow */
    {122, 215, 201, 255},  /* bottom: teal */
};

static const Color PLAYER_COLORS[16] = {
    {242, 177, 52, 255},  {76, 194, 255, 255},  {255, 111, 168, 255}, {126, 224, 129, 255},
    {180, 140, 255, 255}, {255, 138, 61, 255},  {232, 238, 240, 255}, {61, 214, 198, 255},
    {255, 93, 115, 255},  {154, 208, 255, 255}, {195, 230, 110, 255}, {255, 168, 217, 255},
    {143, 155, 255, 255}, {255, 184, 107, 255}, {100, 227, 161, 255}, {213, 140, 255, 255},
};

/* ---- lighting: hemisphere + sun + fog, one shade per face for the tiles ---- */

static const char LIT_VS[] =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "out vec3 worldPos;\n"
    "out vec3 normal;\n"
    "void main() {\n"
    "    worldPos = vec3(matModel * vec4(vertexPosition, 1.0));\n"
    "    normal = mat3(matNormal) * vertexNormal;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char LIT_FS[] =
    "#version 330\n"
    "in vec3 worldPos;\n"
    "in vec3 normal;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 emissive;\n"
    "uniform vec3 viewPos;\n"
    "out vec4 finalColor;\n"
    "const vec3 SUN_DIR = vec3(0.318, 0.848, 0.424);\n" /* toward (30, 80, 40) */
    "const vec3 SUN = vec3(1.0, 0.953, 0.82) * 0.45;\n"
    "const vec3 SKY_LIGHT = vec3(0.68);\n"
    "const vec3 GROUND_LIGHT = vec3(0.42, 0.31, 0.48) * 0.68;\n"
    "const vec3 FOG = vec3(0.659, 0.839, 0.910);\n"
    "void main() {\n"
    "    vec3 n = normalize(normal);\n"
    "    vec3 light = mix(GROUND_LIGHT, SKY_LIGHT, n.y * 0.5 + 0.5) + SUN * max(dot(n, SUN_DIR), 0.0);\n"
    "    vec3 c = min(colDiffuse.rgb * light + emissive, vec3(1.0));\n"
    "    float fog = clamp((distance(worldPos, viewPos) - 110.0) / 150.0, 0.0, 1.0);\n"
    "    finalColor = vec4(mix(c, FOG, fog), colDiffuse.a);\n"
    "}\n";

static Shader lit;
static int loc_emissive, loc_view;
static Material mat;
static Mesh hex, ball, pillar, slime, disc, cone;
static Texture2D dot;

/* ---- fonts: player names may be in any script, so the atlas grows ---- */

static const char *const BODY_FONTS[] = {
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",  /* macOS; covers CJK */
    "/Library/Fonts/Arial Unicode.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL,
};
static const char *const DISPLAY_FONTS[] = {
    "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
    NULL,
};

#define BODY_SIZE 64
#define DISPLAY_SIZE 160
#define MAX_CODEPOINTS 4096

static unsigned char *body_file;
static int body_file_size;
static Font body_font, display_font;
static int body_loaded, display_loaded;
static int codepoints[MAX_CODEPOINTS], codepoint_count, codepoints_added;

/* ---- cosmetic state ---- */

typedef struct {
    uint16_t id;
    int used, was_alive;
    float face;
} bean_look;

typedef struct {
    float life, x, y, z, vx, vy, vz;
    Color color;
} particle;

#define PARTICLES 512
#define TAG_OTHERS 4      /* name tags besides our own */
#define TAG_RADIUS 14.0f  /* how near another bean must be for its tag */

static Camera3D cam;
static int cam_ready, last_focus, swinging;
static float focus_y, layer_alpha[SIM_LAYERS] = {1, 1, 1};
static float cam_yaw, cam_back, cam_up, cam_ahead, cam_look;
static Vector3 cam_centre;
static uint64_t last_frame_ms, start_ms, phase_seen_ms, count_seen_ms;
static int last_phase = -1, phase_before = -1, last_count;
static bean_look looks[HF_MAX_PLAYERS * 2];
static particle parts[PARTICLES];
static int part_head;
static uint8_t seen_state[SIM_LAYERS][SIM_TILES];
static const predict *pred;  /* this frame's, for the tiles */
static int was_chase;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float rnd(void) { return (float)hf_random01(); }

static Color player_color(const world *w, uint16_t id)
{
    const world_player *p = world_player_by_id(w, id);
    return PLAYER_COLORS[(p ? p->color : id) % 16];
}

static Color faded(Color c, float k) { return ColorAlpha(c, (float)c.a / 255.0f * clampf(k, 0, 1)); }

/* ================================================================ setup */

static void put_triangle(float **v, float **n, Vector3 a, Vector3 b, Vector3 c, Vector3 normal)
{
    Vector3 p[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        *(*v)++ = p[i].x, *(*v)++ = p[i].y, *(*v)++ = p[i].z;
        *(*n)++ = normal.x, *(*n)++ = normal.y, *(*n)++ = normal.z;
    }
}

/* A hex tile with one normal per face, so each face gets a single shade.
 * A corner points along +z, as in the board's layout. */
static Mesh hex_prism(float radius, float height)
{
    Mesh m = {0};
    m.triangleCount = 24;
    m.vertexCount = m.triangleCount * 3;
    m.vertices = MemAlloc(sizeof(float) * 3 * (unsigned)m.vertexCount);
    m.normals = MemAlloc(sizeof(float) * 3 * (unsigned)m.vertexCount);
    m.texcoords = MemAlloc(sizeof(float) * 2 * (unsigned)m.vertexCount);
    float *v = m.vertices, *n = m.normals, h = height / 2;
    Vector3 top[6], bottom[6], up = {0, 1, 0}, down = {0, -1, 0}, ct = {0, h, 0}, cb = {0, -h, 0};
    for (int k = 0; k < 6; k++) {
        float a = (float)k * PI / 3;
        top[k] = (Vector3){sinf(a) * radius, h, cosf(a) * radius};
        bottom[k] = (Vector3){top[k].x, -h, top[k].z};
    }
    for (int k = 0; k < 6; k++) {
        int j = (k + 1) % 6;
        float a = ((float)k + 0.5f) * PI / 3;
        Vector3 side = {sinf(a), 0, cosf(a)};
        put_triangle(&v, &n, ct, top[k], top[j], up);
        put_triangle(&v, &n, cb, bottom[j], bottom[k], down);
        put_triangle(&v, &n, top[k], bottom[k], bottom[j], side);
        put_triangle(&v, &n, top[k], bottom[j], top[j], side);
    }
    UploadMesh(&m, false);
    return m;
}

static const char *find_file(const char *env, const char *const *paths)
{
    const char *p = getenv(env);
    if (p && FileExists(p))
        return p;
    for (; *paths; paths++)
        if (FileExists(*paths))
            return *paths;
    return NULL;
}

static void build_body_font(void)
{
    if (body_loaded)
        UnloadFont(body_font);
    body_loaded = 0;
    body_font = GetFontDefault();
    if (!body_file)
        return;
    Font f = LoadFontFromMemory(".ttf", body_file, body_file_size, BODY_SIZE, codepoints, codepoint_count);
    if (f.texture.id == 0 || f.texture.id == GetFontDefault().texture.id)
        return;
    GenTextureMipmaps(&f.texture);
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
    body_font = f;
    body_loaded = 1;
}

/* Every character the HUD is about to show; new ones rebuild the atlas. */
static void need_text(const char *s)
{
    while (*s) {
        int size = 0, cp = GetCodepointNext(s, &size);
        s += size > 0 ? size : 1;
        if (cp < 0x80)
            continue;
        int known = 0;
        for (int i = 0; i < codepoint_count && !known; i++)
            known = codepoints[i] == cp;
        if (!known && codepoint_count < MAX_CODEPOINTS) {
            codepoints[codepoint_count++] = cp;
            codepoints_added = 1;
        }
    }
}

void render_init(void)
{
    cam.up = (Vector3){0, 1, 0};
    cam.fovy = 45;
    cam.projection = CAMERA_PERSPECTIVE;
    start_ms = hf_now_ms();

    lit = LoadShaderFromMemory(LIT_VS, LIT_FS);
    lit.locs[SHADER_LOC_MATRIX_MVP] = GetShaderLocation(lit, "mvp");
    lit.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(lit, "matModel");
    lit.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(lit, "matNormal");
    lit.locs[SHADER_LOC_COLOR_DIFFUSE] = GetShaderLocation(lit, "colDiffuse");
    loc_emissive = GetShaderLocation(lit, "emissive");
    loc_view = GetShaderLocation(lit, "viewPos");
    mat = LoadMaterialDefault();
    mat.shader = lit;

    hex = hex_prism(SIM_HEX_SIZE * 0.94f, 0.8f);
    ball = GenMeshSphere(1, 14, 18);
    pillar = GenMeshCylinder(1.8f, 44, 10);
    slime = GenMeshPlane(600, 600, 1, 1);
    disc = GenMeshCylinder(1, 0.02f, 18);
    cone = GenMeshCone(1, 1, 8);

    Image img = GenImageGradientRadial(32, 32, 0.35f, WHITE, BLANK);
    dot = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(dot, TEXTURE_FILTER_BILINEAR);

    for (int c = 32; c < 127; c++)
        codepoints[codepoint_count++] = c;
    const char *body_path = find_file("HEXFALL_FONT", BODY_FONTS);
    if (body_path)
        body_file = LoadFileData(body_path, &body_file_size);
    build_body_font();
    const char *display_path = find_file("HEXFALL_DISPLAY_FONT", DISPLAY_FONTS);
    if (display_path) {
        display_font = LoadFontEx(display_path, DISPLAY_SIZE, NULL, 0);
        if (display_font.texture.id != 0 && display_font.texture.id != GetFontDefault().texture.id) {
            GenTextureMipmaps(&display_font.texture);
            SetTextureFilter(display_font.texture, TEXTURE_FILTER_TRILINEAR);
            display_loaded = 1;
        }
    }
}

void render_close(void)
{
    if (body_loaded)
        UnloadFont(body_font);
    if (display_loaded)
        UnloadFont(display_font);
    if (body_file)
        UnloadFileData(body_file);
    UnloadTexture(dot);
    UnloadMesh(hex);
    UnloadMesh(ball);
    UnloadMesh(pillar);
    UnloadMesh(slime);
    UnloadMesh(disc);
    UnloadMesh(cone);
    UnloadShader(lit);
}

/* ================================================================ 3D */

static void draw_mesh(Mesh m, Color c, Matrix t)
{
    mat.maps[MATERIAL_MAP_DIFFUSE].color = c;
    DrawMesh(m, mat, t);
}

static void set_glow(Color c)
{
    float v[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    SetShaderValue(lit, loc_emissive, v, SHADER_UNIFORM_VEC3);
}

static Matrix at(float x, float y, float z, float sx, float sy, float sz)
{
    return MatrixMultiply(MatrixScale(sx, sy, sz), MatrixTranslate(x, y, z));
}

static void emit(float x, float y, float z, float vx, float vy, float vz, float life, Color c)
{
    parts[part_head] = (particle){life, x, y, z, vx, vy, vz, c};
    part_head = (part_head + 1) % PARTICLES;
}

static void burst(float x, float y, float z, const Color *colors, int ncolors, int n, float speed)
{
    for (int k = 0; k < n; k++) {
        float a = rnd() * 2 * PI, s = (0.4f + rnd()) * speed;
        emit(x, y, z, cosf(a) * s, 3 + rnd() * speed, sinf(a) * s, 0.5f + rnd() * 0.5f, colors[k % ncolors]);
    }
}

static bean_look *look_for(uint16_t id, float facing)
{
    bean_look *slot = NULL;
    for (size_t i = 0; i < sizeof looks / sizeof looks[0]; i++) {
        if (looks[i].used && looks[i].id == id)
            return &looks[i];
        if (!looks[i].used && !slot)
            slot = &looks[i];
    }
    if (!slot)
        slot = &looks[id % (sizeof looks / sizeof looks[0])];
    *slot = (bean_look){.id = id, .used = 1, .face = facing};
    return slot;
}

/* The floor under a bean: the highest one at or below its feet, so the
 * camera starts moving down as soon as it falls through a hole. */
static int floor_below(float y)
{
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        if (SIM_LAYER_Y[layer] <= y + 0.5f)
            return layer;
    return SIM_LAYERS - 1;
}

/* The floor to watch from above: the one most players stand on. */
static int busiest_layer(const interp_view *v)
{
    int count[SIM_LAYERS] = {0}, best = 0;
    for (int i = 0; i < v->count; i++) {
        const world_body *b = &v->bodies[i];
        if (b->alive && b->grounded && b->layer >= 0 && b->layer < SIM_LAYERS)
            count[b->layer]++;
    }
    for (int layer = 1; layer < SIM_LAYERS; layer++)
        if (count[layer] > count[best])
            best = layer;
    return count[best] ? best : last_focus;
}

static void update_camera(const world *w, const interp_view *v, const render_options *opt, float dt, float t)
{
    const world_body *me = interp_body(v, w->me);
    int round_on = w->phase == PHASE_PLAYING || w->phase == PHASE_RESULT;
    int chase = !opt->overview && round_on && me && me->alive;
    int focus = w->phase == PHASE_LOBBY || w->phase == PHASE_COUNTDOWN ? 0
                : chase ? floor_below(me->y)
                        : busiest_layer(v);
    last_focus = focus;

    float k = cam_ready ? 1.0f - expf(-dt * 2.2f) : 1.0f;
    focus_y += (SIM_LAYER_Y[focus] - focus_y) * k;
    for (int layer = 0; layer < SIM_LAYERS; layer++) {
        float want = layer < focus ? 0.14f : 1.0f;  /* floors above the one we watch fade out */
        layer_alpha[layer] += (want - layer_alpha[layer]) * (cam_ready ? fminf(1, dt * 4) : 1.0f);
    }

    /* What the camera circles around, how far back and up it sits, and how
     * far past the centre it looks. */
    Vector3 centre = {0, focus_y, 0};
    float back = 40, up = 52, ahead = 4, look = -1, want_yaw = opt->yaw;
    if (chase) {
        /* The mouse tilts it. It stays low enough to be under the floor
         * above, so it comes closer to look down steeply. */
        centre = (Vector3){me->x, me->y, me->z};
        up = fminf(20 * sinf(opt->pitch), 11);
        back = up / tanf(opt->pitch);
        ahead = 3, look = 0.5f;
    } else if (w->phase == PHASE_LOBBY) {
        back = 40, up = 46, ahead = 0, look = -8;  /* inside the ring of pillars */
        want_yaw = t * 0.08f;                      /* a slow orbit while people get ready */
    }

    /* The angle is the player's (mouse or Q/E), and the keys are read
     * relative to it, so it is used as is: what W means on screen and what
     * is sent always agree. Only a big change (leaving the lobby's orbit)
     * swings round smoothly. */
    float r = cam_ready ? 1.0f - expf(-dt * 3) : 1.0f;
    float turn = atan2f(sinf(want_yaw - cam_yaw), cosf(want_yaw - cam_yaw));
    if (fabsf(turn) > 0.35f)
        swinging = 1;
    else if (fabsf(turn) < 0.01f)
        swinging = 0;
    cam_yaw += swinging && cam_ready ? turn * r : turn;

    /* A tilt with the mouse is used at once, like a turn; a change of view
     * (chase, overview, lobby) glides. */
    float glide = chase && was_chase ? 1.0f : r;
    was_chase = chase;
    cam_centre = cam_ready ? Vector3Lerp(cam_centre, centre, r) : centre;
    cam_back += (back - cam_back) * glide;
    cam_up += (up - cam_up) * glide;
    cam_ahead += (ahead - cam_ahead) * glide;
    cam_look += (look - cam_look) * glide;
    float sy = sinf(cam_yaw), cy = cosf(cam_yaw);
    cam.position = (Vector3){cam_centre.x + sy * cam_back, cam_centre.y + cam_up, cam_centre.z + cy * cam_back};
    cam.target = (Vector3){cam_centre.x - sy * cam_ahead, cam_centre.y + cam_look, cam_centre.z - cy * cam_ahead};
    cam.fovy += ((chase ? 52.0f : 45.0f) - cam.fovy) * r;
    cam_ready = 1;
}

/* Tiles are drawn as they were at the moment the view shows. */
/* A tile as it is drawn: the ones our bean has been on at our own moment,
 * the rest at the view's (predict.h). *since: ticks since it was touched. */
static int tile_state(const world *w, const interp_view *v, int layer, int index, sim_tile *t, float *since)
{
    double at;
    predict_tile(pred, w, layer, index, v->tick, t, &at);
    *since = (float)(at - (double)t->touch_tick);
    return interp_tile_state(t, at);
}

static void draw_tile(const world *w, const interp_view *v, int layer, int index, float alpha)
{
    sim_tile tile;
    float since;  /* ticks since it was touched */
    int state = tile_state(w, v, layer, index, &tile, &since);
    const sim_tile *t = &tile;
    int q, r;
    float x, z, y = SIM_LAYER_Y[layer] - 0.4f;
    sim_tile_qr(index, &q, &r);
    sim_tile_center(q, r, &x, &z);
    Color c = LAYER_COLORS[layer];
    Matrix spin = MatrixIdentity();
    if (state == TILE_TOUCHED) {
        /* White at once, shaking harder as its moment comes. */
        float k = clampf(since / SIM_FALL_TICKS, 0, 1);
        c = TOUCHED;
        x += (rnd() - 0.5f) * 0.18f * k;
        z += (rnd() - 0.5f) * 0.18f * k;
    } else if (state == TILE_GONE) {
        float s = (since - SIM_FALL_TICKS) / SIM_TICK_HZ;
        if (t->touch_tick == 0 || s < 0 || s > 0.8f)
            return;  /* long gone, or gone before we joined */
        y -= 15.0f * s * s;
        spin = MatrixMultiply(MatrixRotateX(s * 1.5f), MatrixRotateZ(s * 0.8f));
    }
    draw_mesh(hex, ColorAlpha(c, alpha), MatrixMultiply(spin, MatrixTranslate(x, y, z)));
}

static void draw_floors(const world *w, const interp_view *v, int see_through)
{
    for (int layer = 0; layer < SIM_LAYERS; layer++) {
        float a = layer_alpha[layer];
        if ((a < 0.99f) != see_through || a < 0.02f)
            continue;
        for (int i = 0; i < SIM_TILES; i++)
            draw_tile(w, v, layer, i, see_through ? a : 1.0f);
    }
}

/* Crumbs when a tile goes, the first time we see it go. */
static void watch_tiles(const world *w, const interp_view *v)
{
    for (int layer = 0; layer < SIM_LAYERS; layer++)
        for (int i = 0; i < SIM_TILES; i++) {
            sim_tile t;
            float since;
            int state = tile_state(w, v, layer, i, &t, &since);
            if (state == TILE_GONE && seen_state[layer][i] != TILE_GONE && t.touch_tick != 0 &&
                since - SIM_FALL_TICKS < 3 && rnd() < 0.5f) {
                int q, r;
                float x, z;
                sim_tile_qr(i, &q, &r);
                sim_tile_center(q, r, &x, &z);
                Color colors[2] = {LAYER_COLORS[layer], WHITE};
                burst(x, SIM_LAYER_Y[layer], z, colors, 2, 4, 2);
            }
            seen_state[layer][i] = (uint8_t)state;
        }
}

static void draw_shadow(const world *w, const interp_view *v, const world_body *b)
{
    int q, r;
    if (!sim_world_tile(b->x, b->z, &q, &r))
        return;
    for (int layer = 0; layer < SIM_LAYERS; layer++) {
        float h = SIM_LAYER_Y[layer], since;
        sim_tile t;
        int state = tile_state(w, v, layer, sim_tile_index(q, r), &t, &since);
        if (h > b->y + 0.01f || state == TILE_GONE)
            continue;
        float s = 0.95f * clampf(1.0f - (b->y - h) / 8.0f, 0.4f, 1.0f);
        draw_mesh(disc, ColorAlpha(BLACK, 0.25f), at(b->x, h + 0.03f, b->z, s, 1, s));
        return;
    }
}

static void draw_bean(const world_body *b, const bean_look *lk, Color color, float t)
{
    float speed = sqrtf(b->vx * b->vx + b->vz * b->vz);
    float lean = fminf(0.35f, speed * 0.04f);
    float bob = b->grounded && speed > 1 ? fabsf(sinf(t * 14 + b->x)) * 0.25f : 0;
    Matrix root = MatrixMultiply(MatrixRotateY(lk->face), MatrixTranslate(b->x, b->y, b->z));
    Matrix inner = MatrixMultiply(MatrixMultiply(MatrixRotateX(lean), MatrixTranslate(0, bob, 0)), root);
    draw_mesh(ball, color, MatrixMultiply(at(0, 1.25f, 0, 0.9f, 1.25f, 0.9f), inner));
    for (int side = -1; side <= 1; side += 2) {
        draw_mesh(ball, WHITE, MatrixMultiply(at(0.32f * side, 1.78f, 0.7f, 0.24f, 0.24f, 0.24f), inner));
        draw_mesh(ball, PUPIL, MatrixMultiply(at(0.32f * side, 1.8f, 0.9f, 0.12f, 0.12f, 0.12f), inner));
    }
}

static void draw_crown(const world_body *b, float t)
{
    Matrix spin = MatrixMultiply(MatrixRotateY(t * 1.2f), MatrixTranslate(b->x, b->y + 3.0f, b->z));
    draw_mesh(pillar, AMBER, MatrixMultiply(MatrixScale(0.6f / 1.8f, 0.3f / 44, 0.6f / 1.8f), spin));
    for (int k = 0; k < 5; k++) {
        float a = (float)k * 2 * PI / 5;
        draw_mesh(cone, AMBER, MatrixMultiply(at(sinf(a) * 0.45f, 0.3f, cosf(a) * 0.45f, 0.16f, 0.45f, 0.16f), spin));
    }
}

static void draw_arena(void)
{
    set_glow(SLIME);  /* the slime is lit by nothing but itself */
    draw_mesh(slime, BLACK, MatrixTranslate(0, SIM_SLIME_Y, 0));
    set_glow(BLANK);
    for (int k = 0; k < 12; k++) {
        float a = (float)k / 12 * 2 * PI, x = sinf(a) * 46, z = cosf(a) * 46;
        draw_mesh(pillar, PILLAR, MatrixTranslate(x, SIM_SLIME_Y, z));
        draw_mesh(ball, LAYER_COLORS[k % 3], at(x, SIM_SLIME_Y + 45, z, 2.2f, 2.2f, 2.2f));
    }
}

static void step_particles(float dt)
{
    for (int i = 0; i < PARTICLES; i++) {
        particle *p = &parts[i];
        if (p->life <= 0)
            continue;
        p->life -= dt;
        p->vy -= 25 * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->z += p->vz * dt;
    }
}

static void draw_particles(void)
{
    for (int i = 0; i < PARTICLES; i++)
        if (parts[i].life > 0)
            DrawBillboard(cam, dot, (Vector3){parts[i].x, parts[i].y, parts[i].z}, 0.8f,
                          faded(parts[i].color, parts[i].life * 2));
}

static void draw_world(const world *w, const interp_view *v, float t)
{
    float eye[3] = {cam.position.x, cam.position.y, cam.position.z};
    SetShaderValue(lit, loc_view, eye, SHADER_UNIFORM_VEC3);
    draw_arena();
    if (w->have_board)
        draw_floors(w, v, 0);

    if (w->phase == PHASE_PLAYING || w->phase == PHASE_RESULT) {
        rlDisableDepthMask();
        for (int i = 0; i < v->count; i++)
            if (v->bodies[i].alive)
                draw_shadow(w, v, &v->bodies[i]);
        rlEnableDepthMask();
        for (int i = 0; i < v->count; i++) {
            const world_body *b = &v->bodies[i];
            if (!b->alive)
                continue;
            draw_bean(b, look_for(b->id, b->facing), player_color(w, b->id), t);
            const world_player *p = world_player_by_id(w, b->id);
            if (w->phase == PHASE_RESULT && p && p->place == 1)
                draw_crown(b, t);
        }
    }

    rlDisableDepthMask();  /* see-through floors and sparks do not hide what is behind them */
    if (w->have_board)
        draw_floors(w, v, 1);
    draw_particles();
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
}

/* Beans turn smoothly, and a bean that just went into the slime splashes. */
static void watch_beans(const world *w, const interp_view *v, float dt)
{
    for (int i = 0; i < v->count; i++) {
        const world_body *b = &v->bodies[i];
        bean_look *lk = look_for(b->id, b->facing);
        float diff = atan2f(sinf(b->facing - lk->face), cosf(b->facing - lk->face));
        lk->face += diff * fminf(1, dt * 12);
        /* RESULT too: the last one to fall is shown a moment after the server ends the round. */
        if (lk->was_alive && !b->alive && (w->phase == PHASE_PLAYING || w->phase == PHASE_RESULT)) {
            Color colors[3] = {{217, 166, 255, 255}, SLIME, WHITE};
            burst(b->x, SIM_SLIME_Y + 0.5f, b->z, colors, 3, 22, 6);
        }
        lk->was_alive = b->alive;
    }
}

/* ================================================================ HUD */

static Font display_face(void) { return display_loaded ? display_font : body_font; }

static void text(const char *s, float x, float y, float size, Color c)
{
    DrawTextEx(body_font, s, (Vector2){x, y}, size, size * 0.02f, c);
}

static float text_width(const char *s, float size) { return MeasureTextEx(body_font, s, size, size * 0.02f).x; }

static void label(const char *s, float x, float y, float size, Color c)  /* small caps-style eyebrow */
{
    DrawTextEx(display_face(), s, (Vector2){x, y}, size, size * 0.16f, c);
}

static float label_width(const char *s, float size) { return MeasureTextEx(display_face(), s, size, size * 0.16f).x; }

static void big(const char *s, float cx, float cy, float size, Color fill, Color edge)
{
    Font f = display_face();
    Vector2 m = MeasureTextEx(f, s, size, size * 0.03f);
    Vector2 p = {cx - m.x / 2, cy - m.y / 2};
    float e = fmaxf(2, size / 40);
    for (int k = 0; k < 16; k++) {
        float a = (float)k * PI / 8;
        DrawTextEx(f, s, (Vector2){p.x + cosf(a) * e, p.y + sinf(a) * e}, size, size * 0.03f, edge);
    }
    DrawTextEx(f, s, p, size, size * 0.03f, fill);
}

static void box(float x, float y, float w, float h, float radius, Color c)
{
    float shorter = fminf(w, h);
    DrawRectangleRounded((Rectangle){x, y, w, h}, shorter > 0 ? clampf(2 * radius / shorter, 0, 1) : 0, 8, c);
}

static void card(float x, float y, float w, float h)
{
    box(x, y, w, h, 12, PANEL);
    DrawRectangleRoundedLinesEx((Rectangle){x, y, w, h}, clampf(24 / fminf(w, h), 0, 1), 8, 1, RULE);
}

static void triangle(Vector2 a, Vector2 b, Vector2 c, Color col)
{
    float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0)
        DrawTriangle(a, c, b, col);
    else
        DrawTriangle(a, b, c, col);
}

static void draw_crown_icon(float x, float y, float s)
{
    /* The crown from the prototype's results card, 64 x 48 units. */
    static const float pts[7][2] = {{6, 40}, {10, 12}, {23, 25}, {32, 6}, {41, 25}, {54, 12}, {58, 40}};
    static const int tris[5][3] = {{0, 1, 2}, {2, 3, 4}, {4, 5, 6}, {0, 2, 4}, {0, 4, 6}};
    Vector2 p[7];
    for (int i = 0; i < 7; i++)
        p[i] = (Vector2){x + pts[i][0] * s, y + pts[i][1] * s};
    for (int i = 0; i < 7; i++)
        DrawLineEx(p[i], p[(i + 1) % 7], 4 * s, AMBER_INK);
    for (int i = 0; i < 5; i++)
        triangle(p[tris[i][0]], p[tris[i][1]], p[tris[i][2]], AMBER);
    box(x + 5 * s, y + 39 * s, 54 * s, 7 * s, 2 * s, AMBER_INK);
    box(x + 6 * s, y + 40 * s, 52 * s, 5 * s, 1.5f * s, AMBER);
}

static int in_front(Vector3 p)
{
    Vector3 f = Vector3Subtract(cam.target, cam.position), d = Vector3Subtract(p, cam.position);
    return Vector3DotProduct(f, d) > 0;
}

static void draw_tag(const world *w, const world_body *b)
{
    const world_player *p = world_player_by_id(w, b->id);
    Vector3 head = {b->x, b->y + 3.9f, b->z};
    if (!p || !in_front(head))
        return;
    int mine = b->id == w->me;
    Vector2 s = GetWorldToScreen(head, cam);
    float size = mine ? 18 : 14, tw = text_width(p->name, size), bw = tw + (mine ? 22 : 14), bh = size + 10;
    if (mine) {
        box(s.x - bw / 2, s.y - bh - 8, bw, bh, 6, AMBER);
        triangle((Vector2){s.x - 8, s.y - 9}, (Vector2){s.x, s.y}, (Vector2){s.x + 8, s.y - 9}, AMBER);
        text(p->name, s.x - tw / 2, s.y - bh - 8 + 5, size, AMBER_INK);
    } else {
        box(s.x - bw / 2, s.y - bh, bw, bh, 4, ColorAlpha(BOOTH, 0.6f));
        text(p->name, s.x - tw / 2, s.y - bh + 5, size, FG);
    }
}

/* Our own tag always. Of the others only the few nearest to us (or, while
 * we watch, to where the camera looks), so that a full room stays
 * readable, plus the winner's once the round is over. */
static void draw_tags(const world *w, const interp_view *v)
{
    const world_body *me = interp_body(v, w->me);
    int alive_me = me && me->alive;
    Vector3 from = alive_me ? (Vector3){me->x, me->y, me->z} : cam.target;
    int pick[HF_MAX_PLAYERS], n = 0;
    float dist[HF_MAX_PLAYERS];
    for (int i = 0; i < v->count; i++) {
        const world_body *b = &v->bodies[i];
        const world_player *p = world_player_by_id(w, b->id);
        if (!b->alive || !p || b->id == w->me)
            continue;
        float dx = b->x - from.x, dz = b->z - from.z, d = sqrtf(dx * dx + dz * dz);
        if (w->phase == PHASE_RESULT && p->place == 1)
            d = -1;  /* first in line */
        else if (d > TAG_RADIUS || fabsf(b->y - from.y) > 6)
            continue;
        int k = n++;
        for (; k > 0 && dist[k - 1] > d; k--) {
            pick[k] = pick[k - 1];
            dist[k] = dist[k - 1];
        }
        pick[k] = i;
        dist[k] = d;
    }
    for (int k = 0; k < n && k < TAG_OTHERS; k++)
        draw_tag(w, &v->bodies[pick[k]]);
    if (alive_me)
        draw_tag(w, me);  /* last, on top */
}

static void draw_alive(const interp_view *v)
{
    char count[16], of[24];
    snprintf(count, sizeof count, "%d", interp_alive_count(v));
    snprintf(of, sizeof of, "of %d", v->count);
    float x = 16, y = 16, bw = 104;
    box(x, y, bw, 96, 4, BOOTH);
    label("ALIVE", x + (bw - label_width("ALIVE", 14)) / 2, y + 10, 14, AMBER);
    Vector2 m = MeasureTextEx(display_face(), count, 52, 1);
    DrawTextEx(display_face(), count, (Vector2){x + (bw - m.x) / 2, y + 26}, 52, 1, FG);
    text(of, x + (bw - text_width(of, 14)) / 2, y + 74, 14, MUTED);
}

static void draw_feed(const world *w, const interp_view *v, uint64_t now_ms)
{
    float right = (float)GetScreenWidth() - 16, y = 16;
    for (int i = w->note_count - 1; i >= 0; i--) {  /* newest on top */
        const world_note *n = &w->notes[i];
        float age = (float)(now_ms - n->at_ms) / 1000.0f, a = clampf((5.0f - age) / 0.5f, 0, 1);
        /* Our own knock-out is on our own moment (predict.h), the others' on the view's. */
        double moment = n->player == w->me && pred->shown_valid ? pred->shown_tick : v->tick;
        if (age > 5.0f || (n->knockout && moment < (double)n->tick))
            continue;  /* faded, or the bean has not reached the slime on screen yet */
        float size = 15, h = 26;
        if (n->knockout) {
            const world_player *p = world_player_by_id(w, n->player);
            int mine = n->player == w->me;
            const char *name = mine ? "You" : p ? p->name : "someone";
            char tail[24];
            snprintf(tail, sizeof tail, "out  #%d", n->place);
            float nw = text_width(name, size), tw = text_width(tail, size), bw = 34 + nw + 8 + tw + 10;
            box(right - bw, y, bw, h, 3, faded(mine ? SIGNAL : BOOTH, a));
            DrawCircleV((Vector2){right - bw + 16, y + h / 2}, 5, faded(player_color(w, n->player), a));
            text(name, right - bw + 28, y + 4, size, faded(mine ? WHITE : FG, a));
            text(tail, right - bw + 28 + nw + 8, y + 4, size, faded(mine ? SIGNAL_INK : MUTED, a));
        } else {
            float bw = text_width(n->text, size) + 20;
            box(right - bw, y, bw, h, 3, faded(BOOTH, a));
            text(n->text, right - bw + 10, y + 4, size, faded(FG, a));
        }
        y += h + 4;
    }
}

static void draw_lobby(const world *w, const render_options *opt)
{
    int rows = w->player_count > 0 ? w->player_count : 1, ready = 0;
    float bw = 460, bh = 150 + (float)rows * 30;
    float x = ((float)GetScreenWidth() - bw) / 2, y = fmaxf(24, ((float)GetScreenHeight() - bh) / 2 - 30);
    card(x, y, bw, bh);
    label("LOBBY", x + 26, y + 22, 15, AMBER);
    DrawTextEx(display_face(), "HEX FALL", (Vector2){x + 26, y + 40}, 44, 2, FG);
    char players[32];
    snprintf(players, sizeof players, "%d player%s", w->player_count, w->player_count == 1 ? "" : "s");
    text(players, x + bw - 26 - text_width(players, 15), y + 26, 15, MUTED);
    DrawLineEx((Vector2){x + 26, y + 96}, (Vector2){x + bw - 26, y + 96}, 1, RULE);
    for (int i = 0; i < w->player_count; i++) {
        const world_player *p = &w->players[i];
        int is_ready = (p->flags & RP_READY) != 0;
        float row = y + 108 + (float)i * 30;
        ready += is_ready;
        DrawCircleV((Vector2){x + 34, row + 10}, 7, PLAYER_COLORS[p->color % 16]);
        text(p->name, x + 52, row, 18, p->id == w->me ? AMBER : FG);
        const char *state = is_ready ? "READY" : "...";
        label(state, x + bw - 26 - label_width(state, 14), row + 2, 14, is_ready ? GOOD : MUTED);
    }
    char hint[64];
    if (opt->ready)
        snprintf(hint, sizeof hint, "You are ready. Waiting for %d more.", w->player_count - ready);
    else
        snprintf(hint, sizeof hint, "Press ENTER when you are ready");
    text(hint, x + 26, y + bh - 36, 17, opt->ready ? FG : AMBER);
}

static void draw_result(const world *w, uint64_t now_ms)
{
    float since = (float)(now_ms - phase_seen_ms) / 1000.0f;
    if (since < 1.2f)
        return;  /* first a moment to see the winner with the crown */
    const world_player *winner = NULL, *me = world_player_by_id(w, w->me);
    for (int i = 0; i < w->player_count; i++)
        if (w->players[i].place == 1)
            winner = &w->players[i];
    char title[64], sub[48], next[48];
    if (!winner)
        snprintf(title, sizeof title, "Nobody made it");
    else if (winner->id == w->me)
        snprintf(title, sizeof title, "You take the crown!");
    else
        snprintf(title, sizeof title, "%s takes the crown", winner->name);
    if (me && me->place)
        snprintf(sub, sizeof sub, "You placed #%d of %d", me->place, w->body_count);
    else
        snprintf(sub, sizeof sub, "You watched this round");
    int left = (int)ceilf((float)(int32_t)(w->phase_end_tick - w->tick) / SIM_TICK_HZ);
    snprintf(next, sizeof next, "Back to the lobby in %d s", left < 0 ? 0 : left);
    need_text(title);

    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)sw, (int)sh, (Color){8, 11, 13, 120});
    float bw = fmaxf(360, text_width(title, 26) + 60), bh = 200, x = (sw - bw) / 2, y = (sh - bh) / 2;
    card(x, y, bw, bh);
    draw_crown_icon(sw / 2 - 32, y + 22, 1);
    text(title, (sw - text_width(title, 26)) / 2, y + 86, 26, FG);
    text(title, (sw - text_width(title, 26)) / 2 + 0.7f, y + 86, 26, FG);  /* a touch bolder */
    text(sub, (sw - text_width(sub, 16)) / 2, y + 126, 16, MUTED);
    text(next, (sw - text_width(next, 14)) / 2, y + 158, 14, MUTED);
}

static void pill(const char *s, float y, Color bg, Color fg)
{
    float size = 17, tw = text_width(s, size), bw = tw + 32;
    box(((float)GetScreenWidth() - bw) / 2, y, bw, size + 14, (size + 14) / 2, bg);
    text(s, ((float)GetScreenWidth() - tw) / 2, y + 7, size, fg);
}

static void draw_hud(const world *w, const interp_view *v, const net_status *st, const render_options *opt,
                     uint64_t now_ms)
{
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    /* Whether we play this round is the newest news; whether we are out is
     * what the screen shows. */
    const world_body *in_round = world_body_by_id(w, w->me), *me = interp_body(v, w->me);

    switch (w->phase) {
    case PHASE_LOBBY:
        draw_lobby(w, opt);
        break;
    case PHASE_COUNTDOWN: {
        int left = (int)ceilf((float)(int32_t)(w->phase_end_tick - w->tick) / SIM_TICK_HZ);
        left = left < 1 ? 1 : left;
        if (left != last_count) {
            last_count = left;
            count_seen_ms = now_ms;
        }
        float pop = 1 + 0.35f * clampf(1 - (float)(now_ms - count_seen_ms) / 250.0f, 0, 1);
        char digit[8];
        snprintf(digit, sizeof digit, "%d", left);
        big(digit, sw / 2, sh * 0.32f, 150 * pop, AMBER, AMBER_INK);
        break;
    }
    case PHASE_PLAYING: {
        draw_alive(v);
        float since = (float)(now_ms - phase_seen_ms) / 1000.0f;
        if (phase_before == PHASE_COUNTDOWN && since < 0.8f)
            big("GO!", sw / 2, sh * 0.32f, 150 * (1 + 0.3f * clampf(1 - since * 4, 0, 1)), AMBER, AMBER_INK);
        if (!in_round)
            pill("Watching - you join the next round", sh - 110, BOOTH, FG);
        else if (me && !me->alive)
            pill("You are out - watching the rest", sh - 110, SIGNAL, WHITE);
        break;
    }
    case PHASE_RESULT:
        draw_alive(v);
        draw_result(w, now_ms);
        break;
    }
    draw_feed(w, v, now_ms);

    if (st->state != NET_CONNECTED || !w->welcomed) {
        const char *what = st->state == NET_RESYNCING      ? "Catching up with the server..."
                           : st->state == NET_RECONNECTING ? "Connection lost, reconnecting..."
                                                           : "Connecting...";
        pill(what, 20, SIGNAL, WHITE);
    }

    const char *keys = opt->mouse_look ? "WASD move   SPACE jump   mouse turn   ESC free the mouse   C camera   ENTER ready   F1 network"
                                       : "WASD move   SPACE jump   click to turn with the mouse   C camera   ENTER ready   F1 network";
    float kw = text_width(keys, 13);
    box(sw - kw - 34, sh - 40, kw + 18, 26, 4, BOOTH);
    text(keys, sw - kw - 25, sh - 34, 13, FG);
}

/* The screen has stopped and waits for snapshots (interp.h): a spinner
 * where the connection's own notices go. Faded in, so that a short wait
 * does not flash. */
static void draw_waiting(const interp *ip, float t)
{
    float a = clampf(((float)ip->waiting_ms - 100) / 200, 0, 1);
    if (a <= 0)
        return;
    const char *s = "Waiting for the server";
    float size = 17, tw = text_width(s, size), bh = size + 14, bw = tw + 32 + 26;
    float x = ((float)GetScreenWidth() - bw) / 2, y = 20;
    box(x, y, bw, bh, bh / 2, faded(BOOTH, a));
    Vector2 c = {x + 24, y + bh / 2};
    float start = fmodf(t * 300, 360);
    DrawRing(c, 6, 9, 0, 360, 32, faded(MUTED, 0.5f * a));
    DrawRing(c, 6, 9, start, start + 110, 16, faded(AMBER, a));
    text(s, x + 42, y + 7, size, faded(FG, a));
}

static void draw_network_panel(const world *w, const interp *ip, const predict *pr, const net_status *st)
{
    char lines[9][160];
    double frames = ip->frames ? (double)ip->frames : 1;
    snprintf(lines[0], sizeof lines[0], "Connection  %s   sessions %llu   resyncs %llu   clock %s",
             net_state_name(st->state), (unsigned long long)st->sessions, (unsigned long long)st->resyncs,
             st->clock_valid ? "synced" : "not synced");
    snprintf(lines[1], sizeof lines[1], "RTT  %.1f ms   floor %.1f ms   jitter %.1f ms", st->rtt_ms, st->rtt_min_ms,
             st->jitter_ms);
    snprintf(lines[2], sizeof lines[2], "Packets  in %llu   out %llu   resent %llu", (unsigned long long)st->packets_in,
             (unsigned long long)st->packets_out, (unsigned long long)st->resends);
    snprintf(lines[3], sizeof lines[3], "Snapshots  tick %u   lost %.1f%%   95%% within %.0f ms   too late %llu",
             w->tick, 100 * ip->loss, ip->late_ms, (unsigned long long)ip->too_late);
    if (ip->mode == INTERP_NONE)
        snprintf(lines[4], sizeof lines[4], "Smoothing  none: the newest snapshot as it is");
    else
        snprintf(lines[4], sizeof lines[4], "Smoothing  %s   others drawn %.0f ms in the past", interp_mode_name(ip->mode),
                 ip->delay_ms);
    snprintf(lines[5], sizeof lines[5], "Screen  stutter %.1f ms   guessed %.1f%%   waited %.1f%%   %d fps", ip->stutter_ms,
             100 * (double)ip->frames_guessed / frames, 100 * (double)ip->frames_waiting / frames, GetFPS());
    if (ip->guess_errors)
        snprintf(lines[6], sizeof lines[6], "Guess  %s   off by %.2f on average, %.2f at most", interp_guess_name(ip->guess),
                 ip->guess_error_sum / (double)ip->guess_errors, ip->guess_error_max);
    else
        snprintf(lines[6], sizeof lines[6], "Guess  %s   nothing guessed yet", interp_guess_name(ip->guess));
    /* Without prediction a press shows once the server has used it (the
     * inputs ahead of it) and the snapshot has waited its turn on screen. */
    double felt = pr->ahead * 1000.0 / SIM_TICK_HZ + ip->delay_ms;
    if (!pr->enabled)
        snprintf(lines[7], sizeof lines[7], "Prediction  off: your moves show after ~%.0f ms", felt);
    else if (!pr->snapshots)
        snprintf(lines[7], sizeof lines[7], "Prediction  on: waiting for a round");
    else
        snprintf(lines[7], sizeof lines[7], "Prediction  on: moves show at once, not after ~%.0f ms   corrected %llu times, %.2f on average",
                 felt, (unsigned long long)pr->corrections, pr->corrections ? pr->correction_sum / (double)pr->corrections : 0);
    const netsim_cfg *in = &st->inbound;
    if (in->delay_ms == 0 && in->jitter_ms == 0 && in->loss <= 0)
        snprintf(lines[8], sizeof lines[8], "Simulated bad network  off");
    else
        snprintf(lines[8], sizeof lines[8], "Simulated bad network  +%d..%d ms, %.0f%% lost", in->delay_ms,
                 in->delay_ms + in->jitter_ms, 100 * in->loss);
    int n = 9;
    float size = 14, row = 19, wide = 0;
    for (int i = 0; i < n; i++)
        wide = fmaxf(wide, text_width(lines[i], size));
    const char *keys = "I smoothing   G guess   P prediction   N bad network";
    wide = fmaxf(wide, label_width("NETWORK", 13) + 24 + text_width(keys, 13));
    float h = 34 + (float)n * row, y = (float)GetScreenHeight() - h - 14;
    box(14, y, wide + 28, h, 4, BOOTH);
    label("NETWORK", 28, y + 10, 13, AMBER);
    text(keys, 28 + wide - text_width(keys, 13), y + 9, 13, MUTED);
    for (int i = 0; i < n; i++)
        text(lines[i], 28, y + 28 + (float)i * row, size, FG);
}

/* ================================================================ frame */

void render_frame(const world *w, const interp *ip, const predict *pr, const net_status *st,
                  const render_options *opt, uint64_t now_ms)
{
    /* The others as the interpolation shows them, our own bean where the
     * prediction has it. */
    static interp_view scene;
    scene = ip->view;
    if (pr->shown_valid) {
        int i = 0;
        while (i < scene.count && scene.bodies[i].id != w->me)
            i++;
        if (i == scene.count && scene.count < HF_MAX_PLAYERS)
            scene.count++;
        if (i < scene.count)
            scene.bodies[i] = pr->shown;
    }
    pred = pr;
    const interp_view *v = &scene;
    float dt = last_frame_ms ? (float)(now_ms - last_frame_ms) / 1000.0f : 0.0f;
    float t = (float)(now_ms - start_ms) / 1000.0f;
    last_frame_ms = now_ms;
    dt = fminf(dt, 0.1f);
    if (w->phase != last_phase) {
        phase_before = last_phase;
        last_phase = w->phase;
        phase_seen_ms = now_ms;
    }

    for (int i = 0; i < w->player_count; i++)
        need_text(w->players[i].name);
    for (int i = 0; i < w->note_count; i++)
        need_text(w->notes[i].text);
    if (codepoints_added) {
        codepoints_added = 0;
        build_body_font();
    }

    if (w->have_board)
        watch_tiles(w, v);
    watch_beans(w, v, dt);
    step_particles(dt);
    update_camera(w, v, opt, dt, t);

    BeginDrawing();
    ClearBackground(SKY);
    BeginMode3D(cam);
    draw_world(w, v, t);
    EndMode3D();

    if (w->phase == PHASE_PLAYING || w->phase == PHASE_RESULT)
        draw_tags(w, v);
    draw_hud(w, v, st, opt, now_ms);
    if (st->state == NET_CONNECTED && w->welcomed)
        draw_waiting(ip, t);
    if (opt->show_network)
        draw_network_panel(w, ip, pr, st);
    EndDrawing();
}
