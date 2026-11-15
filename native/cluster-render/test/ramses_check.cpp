/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Runs the compositor on the unit's own libramses (TCP, no renderer) with a
 * display port that grants every request: every layer kind's objects,
 * shaders (compiled by the library's glslang), buffer writes, uniforms,
 * draw-order changes, an exclusive layer, removals and a scene published
 * again must be accepted, with nothing logged as an error by this service
 * or the library. Then the 3D and asset side: the backend's own answers for
 * every new call (uniform arrays, turned quads, views split into passes,
 * parts and their geometry, every kind of animation, and on RAMSES 19 the
 * scene described whole, as to a renderer that subscribes, once animations
 * have stopped), and through the compositor a lit rotating cube, an effect
 * sampling an R8 glyph atlas and an RGBA8 sprite with a vec4 array, a turned
 * needle and an effect on the renderer's clock, published again. On RAMSES
 * 19, a scene file the library saves from a synthetic gauge (a needle on a
 * pivot, a bar on a data object, a plain quad, a camera and two passes):
 * loaded as the panel's scene through the compositor, every edit kind read
 * back from the library, the needle turned at 30 edits a second,
 * unpublished for the HUD's shared scene id, loaded again with every kept
 * edit, replaced in place by a second file, dropped with the resources its
 * loads made; a file of another scene id refused.
 * Run by `make test-ramses ROOTFS=<dir>`;
 * built with CRS_RAMSES27 against iDrive 8's RAMSES 27 (graph_ramses27.h),
 * by `make test-ramses27 ROOTFS=<dir>`. */
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "../src/compositor.h"
#include "fake_port.h"
#include "log.h"
#ifdef CRS_RAMSES27
#include "../src/graph_ramses27.h"
#include "ramses-client-api/RamsesClient.h"
#include "ramses-framework-api/RamsesFramework.h"
#include "ramses-framework-api/RamsesFrameworkConfig.h"
#else
#include "../src/graph_ramses19.h"
#include "ramses19/ramses.h"
#endif

using namespace crs;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static const char *const kBars = R"(#version 100
precision mediump float;
uniform float u_level;
uniform float u_time;
varying vec2 v_uv;
void main() {
    float bar = step(abs(v_uv.y - 0.5) * 2.0, u_level * (0.6 + 0.4 * sin(u_time * 6.0 + v_uv.x * 20.0)));
    gl_FragColor = vec4(1.0, 0.1, 0.1, bar);
})";

static const char *const kLitVs = R"(#version 100
attribute vec3 a_position;
attribute vec3 a_normal;
uniform mat4 u_mvp;
uniform mat4 u_normal;
varying vec3 v_n;
void main() {
    v_n = (u_normal * vec4(a_normal, 0.0)).xyz;
    gl_Position = u_mvp * vec4(a_position, 1.0);
})";

static const char *const kLitFs = R"(#version 100
precision mediump float;
uniform vec4 u_color;
varying vec3 v_n;
void main() {
    float d = max(dot(normalize(v_n), normalize(vec3(0.4, 0.7, 0.6))), 0.0);
    gl_FragColor = vec4(u_color.rgb * (0.22 + 0.78 * d), 1.0);
})";

/* Every semantic and attribute a part's shaders can have. */
static const char *const kAllVs = R"(#version 100
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_texcoord;
attribute vec4 a_color;
uniform mat4 u_mvp;
uniform mat4 u_mv;
uniform mat4 u_m;
uniform mat4 u_v;
uniform mat4 u_p;
uniform mat4 u_normal;
uniform mat4 u_bones[2];
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    v_uv = a_texcoord;
    v_color = a_color * (u_normal * vec4(a_normal, 0.0)).z;
    vec4 p = u_bones[0] * u_bones[1] * vec4(a_position, 1.0);
    gl_Position = u_mvp * p + (u_p * u_v * u_m * p - u_p * u_mv * p);
})";

static const char *const kAllFs = R"(#version 100
precision mediump float;
uniform sampler2D u_atlas;
uniform vec3 u_tint;
uniform float u_fade;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_FragColor = vec4(v_color.rgb * u_tint, texture2D(u_atlas, v_uv).r * u_fade);
})";

/* A glyph of an R8 atlas over an RGBA8 sprite, cells from a vec4 array. */
static const char *const kGlyphs = R"(#version 100
precision mediump float;
uniform sampler2D u_atlas;
uniform sampler2D u_sprite;
uniform vec4 u_cells[4];
varying vec2 v_uv;
void main() {
    float a = texture2D(u_atlas, u_cells[0].xy + v_uv * u_cells[0].zw).r;
    vec4 s = texture2D(u_sprite, v_uv);
    gl_FragColor = vec4(mix(s.rgb, u_cells[1].rgb, a), max(s.a, a)) + u_cells[2] * u_cells[3].x;
})";

/* Plasma on the renderer's clock. */
static const char *const kPlasma = R"(#version 100
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform float u_time;
varying vec2 v_uv;
void main() {
    vec2 p = (v_uv - 0.5) * 6.0;
    float v = sin(p.x + u_time) + sin(p.y * 1.3 - u_time * 0.7) + sin(length(p) * 2.2 - u_time * 1.4);
    gl_FragColor = vec4(0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + v * 1.6), 1.0);
})";

#ifndef CRS_RAMSES27
static bool ok_status(ramses::status_t s) { return s == ramses::StatusOK; }

/* The synthetic scene file's effect: a flat colour, dimmed by u_level. */
static const char *const kGaugeVs = R"(#version 100
attribute vec2 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
})";

static const char *const kGaugeFs = R"(#version 100
precision mediump float;
uniform vec4 u_color;
uniform float u_level;
void main() {
    gl_FragColor = vec4(u_color.rgb * u_level, u_color.a);
})";

/* Saves a synthetic gauge, made here, as a scene file and a resource file:
 * a needle on a pivot node (to turn), a bar whose level is a data object, a
 * plain quad; an orthographic camera, a main and a glow pass. */
static bool save_gauge(ramses::RamsesClient &client, ramses::sceneId_t id, const std::string &file,
                       const std::string &res)
{
    ramses::SceneConfig sc;
    ramses::Scene *s = client.createScene(id, sc, "gauge");
    ramses::EffectDescription d;
    d.setVertexShader(kGaugeVs);
    d.setFragmentShader(kGaugeFs);
    d.setUniformSemantic("u_mvp", ramses::EEffectUniformSemantic_ModelViewProjectionMatrix);
    ramses::Effect *fx = client.createEffect(d, ramses::ResourceCacheFlag_DoNotCache, "gauge_fx");
    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    static const uint16_t tris[6] = {0, 1, 2, 2, 1, 3};
    const ramses::Vector2fArray *pos = client.createConstVector2fArray(4, quad);
    const ramses::UInt16Array *idx = client.createConstUInt16Array(6, tris);
    if (!s || !fx || !pos || !idx)
        return false;
    ramses::OrthographicCamera *cam = s->createOrthographicCamera("main_cam");
    cam->setFrustum(0, 1920, 0, 720, 0.1f, 10);
    cam->setViewport(0, 0, 1920, 720);
    ramses::RenderGroup *group = s->createRenderGroup("main_group");
    ramses::RenderGroup *glow = s->createRenderGroup("glow_group");
    bool good = true;
    const char *const passes[2] = {"main_pass", "glow_pass"};
    for (int i = 0; i < 2; i++) {
        ramses::RenderPass *p = s->createRenderPass(passes[i]);
        good = good && p && ok_status(p->setCamera(*cam)) && ok_status(p->addRenderGroup(i ? *glow : *group)) &&
               ok_status(p->setRenderOrder(i));
    }
    ramses::GroupNode *gauge = s->createGroupNode("gauge");
    ramses::GroupNode *pivot = s->createGroupNode("needle_pivot");
    ramses::DataFloat *level = s->createDataFloat("level");
    good = good && gauge && pivot && level && ok_status(gauge->setTranslation(0, 0, -1)) &&
           ok_status(pivot->setParent(*gauge)) && ok_status(pivot->setTranslation(960, 360, 0));
    struct { const char *name; ramses::Node *parent; float x, y, w, h; } parts[] = {
        {"needle", pivot, -4, 0, 8, 300}, {"bar", gauge, 200, 80, 600, 40}, {"quad", nullptr, 1500, 500, 200, 120}};
    for (const auto &part : parts) {
        ramses::MeshNode *m = s->createMeshNode(part.name);
        ramses::Appearance *app = s->createAppearance(*fx, (std::string(part.name) + "_look").c_str());
        ramses::GeometryBinding *geo = s->createGeometryBinding(*fx);
        ramses::AttributeInput ain;
        ramses::UniformInput color, dim;
        good = good && m && app && geo && ok_status(fx->findAttributeInput("a_position", ain)) &&
               ok_status(geo->setInputBuffer(ain, *pos)) && ok_status(geo->setIndices(*idx)) &&
               ok_status(m->setAppearance(*app)) && ok_status(m->setGeometryBinding(*geo)) &&
               ok_status(m->setTranslation(part.x, part.y, part.parent ? 0.0f : -1.0f)) &&
               ok_status(m->setScaling(part.w, part.h, 1)) &&
               ok_status(fx->findUniformInput("u_color", color)) && ok_status(fx->findUniformInput("u_level", dim)) &&
               ok_status(app->setInputValueVector4f(color, 1, 1, 1, 1)) &&
               ok_status(std::string(part.name) == "bar" ? app->bindInput(dim, *level) : app->setInputValueFloat(dim, 1)) &&
               (!part.parent || ok_status(m->setParent(*part.parent))) &&
               ok_status((std::string(part.name) == "quad" ? glow : group)->addMeshNode(*m));
    }
    ramses::ResourceFileDescription rd(res.c_str());
    rd.add(fx);
    rd.add(pos);
    rd.add(idx);
    ramses::ResourceFileDescriptionSet set;
    set.add(rd);
    good = good && ok_status(level->setValue(0.5f)) && ok_status(s->flush()) &&
           ok_status(client.saveSceneToFile(*s, file.c_str(), set, false));
    client.destroy(*s);
    client.destroy(*fx);
    client.destroy(*pos);
    client.destroy(*idx);
    return good;
}

/* An object of the scene the client knows by the name "gauge". */
template <typename T> static T *gauge_object(ramses::RamsesClient &client, const char *name)
{
    ramses::Scene *s = static_cast<ramses::Scene *>(client.findObjectByName("gauge"));
    return s ? static_cast<T *>(s->findObjectByName(name)) : nullptr;
}

/* The edits shown: the needle turned by turn, red, the level 0.75, the quad
 * hidden, the viewport halved, the glow pass off. */
static bool gauge_edited(ramses::RamsesClient &client, float turn)
{
    ramses::Node *pivot = gauge_object<ramses::Node>(client, "needle_pivot");
    ramses::Node *quad = gauge_object<ramses::Node>(client, "quad");
    ramses::MeshNode *needle = gauge_object<ramses::MeshNode>(client, "needle");
    ramses::DataFloat *level = gauge_object<ramses::DataFloat>(client, "level");
    ramses::LocalCamera *cam = gauge_object<ramses::LocalCamera>(client, "main_cam");
    ramses::RenderPass *glow = gauge_object<ramses::RenderPass>(client, "glow_pass");
    if (!pivot || !quad || !needle || !level || !cam || !glow) {
        std::printf("     the loaded gauge's objects not found\n");
        return false;
    }
    float rx = 0, ry = 0, rz = 0, v = 0, r = 0, g = 0, b = 0, a = 0;
    ramses::UniformInput color;
    const bool read = ok_status(pivot->getRotation(rx, ry, rz)) && ok_status(level->getValue(v)) &&
                      ok_status(needle->getAppearance()->getEffect().findUniformInput("u_color", color)) &&
                      ok_status(needle->getAppearance()->getInputValueVector4f(color, r, g, b, a));
    std::printf("     rotation %g, level %g, colour %g %g %g, quad %s, viewport %ux%u, glow %s\n", rz, v, r, g, b,
                quad->getVisibility() ? "shown" : "hidden", cam->getViewportWidth(), cam->getViewportHeight(),
                glow->isEnabled() ? "on" : "off");
    return read && rz == turn && v == 0.75f && r == 1 && g == 0 && !quad->getVisibility() &&
           cam->getViewportWidth() == 960 && cam->getViewportHeight() == 360 && !glow->isEnabled();
}

static size_t resource_count(ramses::RamsesClient &client, uint32_t type)
{
    size_t n = 0;
    ramses::ResourceIterator it(client, ramses::ERamsesObjectType(type));
    while (it.getNext())
        n++;
    return n;
}

/* The scene called name described whole, as the library describes it to a
 * renderer that subscribes: saved to a file, which is then removed. */
static ramses::RamsesClient *describing;
static bool described(const char *name)
{
    ramses::Scene *s = static_cast<ramses::Scene *>(describing->findObjectByName(name));
    const std::string file = "/tmp/crs-ramses-check-" + std::to_string(getpid()) + "-described.ramses";
    ramses::ResourceFileDescriptionSet none;
    const bool saved = s && ok_status(describing->saveSceneToFile(*s, file.c_str(), none, false));
    unlink(file.c_str());
    return saved;
}
#endif

/* A cube: 6 faces x 4 vertices with their normals. */
static MeshData cube_mesh()
{
    static const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    MeshData d;
    for (int f = 0; f < 6; f++) {
        const float *N = n[f];
        const float a[3] = {N[1], N[2], N[0]};
        const float b[3] = {N[1] * a[2] - N[2] * a[1], N[2] * a[0] - N[0] * a[2], N[0] * a[1] - N[1] * a[0]};
        const uint16_t base = (uint16_t)(d.position.size() / 3);
        for (int v = 0; v < 4; v++) {
            const float sa = (v & 1) ? 1.0f : -1.0f, sb = (v & 2) ? 1.0f : -1.0f;
            for (int c = 0; c < 3; c++) {
                d.position.push_back(N[c] + sa * a[c] + sb * b[c]);
                d.normal.push_back(N[c]);
                d.texcoord.push_back(c == 2 ? 0.0f : (c ? sb : sa) * 0.5f + 0.5f);
            }
            d.texcoord.pop_back();
            d.color.insert(d.color.end(), {1.0f, 0.5f, 0.0f, 1.0f});
        }
        for (uint16_t k : {0, 1, 2, 2, 1, 3})
            d.index.push_back((uint16_t)(base + k));
    }
    d.vertices = (uint32_t)(d.position.size() / 3);
    return d;
}

static Animation keyed(Animated prop, uint32_t width, std::vector<uint32_t> t, std::vector<float> v, bool step = false,
                       const char *uniform = "")
{
    Animation a;
    a.property = prop;
    a.width = width;
    a.t = t;
    a.v = v;
    a.loop = true;
    a.step = step;
    a.uniform = uniform;
    return a;
}

/* The backend's answers to every 3D, asset and animation call, on a scene
 * of its own. */
static void backend(Gpu &gpu)
{
    std::string err;
    TargetSpec t;
    t.name = "check";
    t.scene_id = 0x010004f8;
    t.width = 1920;
    t.height = 720;
    std::unique_ptr<Graph> g = gpu.scene(t, &err);
    const int glyph_fx = gpu.effect(kQuadVertexShader, kGlyphs, &err);
    const int all_fx = gpu.effect(kAllVs, kAllFs, &err);
    const int lit_fx = gpu.effect(kLitVs, kLitFs, &err);
    ok("effects with every semantic, attribute and array compile",
       g && glyph_fx && all_fx && lit_fx &&
           gpu.attributes(all_fx) == (kAttrPosition | kAttrNormal | kAttrTexcoord | kAttrColor) &&
           gpu.attributes(lit_fx) == (kAttrPosition | kAttrNormal));
    if (!g || !glyph_fx || !all_fx || !lit_fx)
        return;
    const int atlas = g->buffer(TexFormat::R8, 64, 32, Sampling::Bilinear, Wrap::Repeat);
    const int sprite = g->buffer(TexFormat::RGBA8, 16, 16, Sampling::Nearest, Wrap::Clamp);
    std::vector<uint8_t> px(64 * 32 * 4, 90);
    Rect r;
    r.x = 100; r.y = 100; r.w = 200; r.h = 80;
    const int q1 = g->quad(glyph_fx, r);
    const float cells[16] = {0, 0, 0.25f, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    Turn turn;
    turn.rotate = 30; turn.sx = 1.5f; turn.px = 100; turn.py = 70;
    ok("repeating and clamped buffers, written, sampled by one quad",
       atlas && sprite && g->write(atlas, 0, 0, 64, 32, px.data()) && g->write(sprite, 0, 0, 16, 16, px.data()) &&
           q1 && g->texture(q1, "u_atlas", atlas) && g->texture(q1, "u_sprite", sprite));
    const std::vector<uint8_t> blocks(tex_bytes(TexFormat::ETC2RGBA, 16, 16), 0x55);
    const int etc2rgba =
        g->immutable_texture(TexFormat::ETC2RGBA, 16, 16, Sampling::Bilinear, Wrap::Clamp, blocks.data(), blocks.size());
    const int etc2rgb = g->immutable_texture(TexFormat::ETC2RGB, 16, 16, Sampling::Nearest, Wrap::Repeat, blocks.data(),
                                             tex_bytes(TexFormat::ETC2RGB, 16, 16));
    const int q0 = g->quad(glyph_fx, r);
    ok("etc2rgba and etc2rgb textures, sampled by one quad",
       etc2rgba && etc2rgb && q0 && g->texture(q0, "u_atlas", etc2rgb) && g->texture(q0, "u_sprite", etc2rgba) &&
           !g->write(etc2rgb, 0, 0, 4, 4, blocks.data()));
    ok("a vec4 array uniform and a turn", g->uniform_array(q1, "u_cells", UniformType::Vec4, cells, 4) &&
                                             g->turn(q1, turn) && g->show(q1, true) && g->order(q1, 0));
    Projection persp;
    Projection ortho;
    ortho.ortho = true;
    ortho.ortho_h = 4;
    Pose cam;
    cam.t[2] = 6;
    cam.r[0] = -10;
    Rect vr;
    vr.x = -100; vr.y = 50; vr.w = 600; vr.h = 400;   /* reaches past the left edge */
    const int v1 = g->view(vr, persp, cam);
    vr.x = 1000; vr.y = 300;
    const int v2 = g->view(vr, ortho, cam);
    const int q2 = g->quad(glyph_fx, r);
    const int q3 = g->quad(glyph_fx, r);
    for (int q : {q2, q3})
        g->texture(q, "u_atlas", atlas), g->texture(q, "u_sprite", sprite),
            g->uniform_array(q, "u_cells", UniformType::Vec4, cells, 4), g->show(q, true);
    ok("perspective and orthographic views between quads: five passes",
       v1 && v2 && g->show(v1, true) && g->show(v2, true) && g->order(v1, 4) && g->order(q2, 8) &&
           g->order(v2, 12) && g->order(q3, 16) && g->flush(1));
    PartLook flat;
    flat.depth = false; flat.cull = Cull::None; flat.blend = Blend::Add;
    PartLook opaque;
    opaque.blend = Blend::None; opaque.cull = Cull::Front;
    const int p1 = g->part(v1, lit_fx, PartLook());
    const int p2 = g->part(v1, all_fx, flat);
    const int p3 = g->part(v2, all_fx, opaque);
    const MeshData cube = cube_mesh();
    Geometry bare = cube.geometry();
    bare.normal = nullptr;
    const float bones[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float tint[3] = {1, 0.5f, 0.2f}, fade = 1.0f, orange[4] = {1, 0.45f, 0, 1};
    Pose pose;
    pose.r[1] = 30; pose.t[2] = -1; pose.s[0] = 1.5f;
    ok("parts: geometry with every attribute, uniforms of every kind, a sampler, a pose",
       p1 && p2 && p3 && g->geometry(p1, cube.geometry()) && g->geometry(p2, cube.geometry()) &&
           g->geometry(p3, cube.geometry()) && g->uniform(p1, "u_color", orange, 4) &&
           g->uniform_array(p2, "u_bones", UniformType::Mat4, bones, 2) &&
           g->uniform_array(p2, "u_tint", UniformType::Vec3, tint, 1) && g->uniform(p2, "u_fade", &fade, 1) &&
           g->texture(p2, "u_atlas", atlas) && g->pose(p1, pose) && g->flush(2));
    ok("geometry lacking what the shader reads is refused", !g->geometry(p1, bare));
    ok("geometry replaced", g->geometry(p1, cube.geometry()) && g->flush(3));
    const int a1 = g->animate(q1, keyed(Animated::Rotate, 1, {0, 1000}, {0, 90}));
    const int a2 = g->animate(q1, keyed(Animated::Translate, 3, {0, 1000}, {0, 0, 0, 40, 20, 0}));
    const int a3 = g->animate(q2, keyed(Animated::Scale, 3, {0, 500, 1000}, {1, 1, 1, 2, 2, 1, 1, 1, 1}, true));
    const int a4 = g->animate(p1, keyed(Animated::Rotate, 3, {0, 4000}, {0, 0, 0, 0, 360, 0}));
    const int a5 = g->animate(p2, keyed(Animated::Translate, 3, {0, 1000}, {0, 0, 0, 0, 1, 0}, true));
    const int a6 = g->animate(v1, keyed(Animated::Translate, 3, {0, 2000}, {0, 0, 6, 0, 0, 8}));
    const int a7 = g->animate(v2, keyed(Animated::Rotate, 3, {0, 2000}, {0, 0, 0, 0, 30, 0}));
    const int a8 = g->animate(p2, keyed(Animated::Uniform, 1, {0, 1000}, {0, 1}, false, "u_fade"));
    const int a9 = g->animate(p2, keyed(Animated::Uniform, 3, {0, 1000}, {1, 0, 0, 0, 0, 1}, true, "u_tint"));
    const int a10 = g->animate(p1, keyed(Animated::Uniform, 4, {0, 1000}, {1, 0, 0, 1, 0, 0, 1, 1}, false, "u_color"));
    const int a11 = g->animate(p1, keyed(Animated::Uniform, 4, {0, 1000}, {1, 0, 0, 1, 0, 0, 1, 1}, true, "u_color"));
    ok("animations of every property, linear and step, on quads, parts and cameras",
       a1 && a2 && a3 && a4 && a5 && a6 && a7 && a8 && a9 && a10 && a11 && g->flush(4));
    const int errors = cld::log_errors;
    ok("refused: a uniform the shader lacks, and one of another width",
       !g->animate(p2, keyed(Animated::Uniform, 1, {0, 1000}, {0, 1}, false, "u_none")) &&
           !g->animate(p2, keyed(Animated::Uniform, 3, {0, 1000}, {0, 0, 0, 1, 1, 1}, false, "u_fade")) &&
           cld::log_errors == errors + 2 && g->flush(4));
    cld::log_errors = errors;
#ifndef CRS_RAMSES27
    ok("and the scene described whole", described("check"));
#endif
    g->stop(a1);
    g->stop(a11);
    g->order(v1, 20);   /* the views together at the top: three passes, a segment fewer */
    g->order(v2, 24);
    ok("stopped, reordered", g->flush(5));
#ifndef CRS_RAMSES27
    ok("with animations stopped, the scene described whole to a renderer that subscribes", described("check"));
#endif
    Projection near = persp;
    near.fov = 50;
    vr.x = 2000;   /* past the right edge: nothing to draw */
    ok("a camera changed, a view moved off the target", g->camera(v1, near, cam) && g->place(v2, vr) && g->flush(6));
    g->drop_part(p2);
    g->drop_view(v2);
    g->drop_quad(q3);
    ok("parts, views and quads dropped with their animations", g->flush(7));
#ifndef CRS_RAMSES27
    ok("and the scene described whole again", described("check"));
#endif
    g.reset();
    gpu.drop_effect(glyph_fx);
    gpu.drop_effect(all_fx);
    gpu.drop_effect(lit_fx);
}

int main()
{
    cld::log_name = "ramses_check";
#ifdef CRS_RAMSES27
    ramses::RamsesFramework::SetConsoleLogLevel(ramses::ELogLevel::Warn);
    ramses::RamsesFrameworkConfig fwc;
    ramses::RamsesFramework framework(fwc);
    ramses::RamsesClient *client = framework.createClient("ramses_check");
    if (!client || framework.connect() != ramses::StatusOK) {
        std::printf("FAIL connect\n");
        return 1;
    }
    Ramses27Gpu gpu(*client);
#else
    const char *args[] = {"ramses_check"};
    ramses::RamsesFrameworkConfig fwc(1, args);
    ramses::RamsesFramework framework(fwc);
    ramses::RamsesClient client("ramses_check", framework);
    if (framework.connect() != ramses::StatusOK) {
        std::printf("FAIL connect\n");
        return 1;
    }
    Ramses19Gpu gpu(client);
    describing = &client;
    /* Saved before any target has the panel's scene id. */
    const std::string dir = "/tmp/crs-ramses-check-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0755);
    const std::string gauge = dir + "/gauge.ramses", gauge_res = dir + "/gauge.ramres";
    const std::string other = dir + "/other.ramses", other_res = dir + "/other.ramres";
    const std::string gauge2 = dir + "/gauge2.ramses", gauge2_res = dir + "/gauge2.ramres";
    ok("a synthetic scene file saved, a second one, and one of another scene id",
       save_gauge(client, 0x01000400, gauge, gauge_res) && save_gauge(client, 0x01000400, gauge2, gauge2_res) &&
           save_gauge(client, 0x010004f9, other, other_res));
#endif
    std::string err;
    ok("the unit quad", gpu.init(&err));

    FakePort port;
    SlotManager slots(&port, nullptr);
    TargetSpec m;
    m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 500;
    m.take_over_stock = true;
    TargetSpec p;
    p.name = "panel"; p.slot = 0x203; p.scene_id = 0x01000400; p.width = 1920; p.height = 720; p.linger_ms = 500;
    p.scene_files = true;
    TargetSpec h;   /* the panel's scene id */
    h.name = "hud"; h.slot = 0x323; h.scene_id = 0x01000400; h.width = 800; h.height = 480; h.linger_ms = 500;
    slots.add(m);
    slots.add(p);
    slots.add(h);
    GovernorConfig gc;
    Compositor comp(&gpu, &slots, gc, 250);
    slots.set_scenes(&comp);
    port.m = &slots;
    ok("the built-in effects compile", comp.init(&err));

    int64_t now = 10000;
    auto run = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 33) {
            now += 33;
            port.tick(now);
            comp.arrange(now);
            slots.tick(now);
            comp.render(now);
        }
    };

    LayerParams lp;
    lp.contend = true;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    const uint32_t vid = comp.add_video(1, 0, lp, vp, now, &err);
    VideoLayer *video = static_cast<VideoLayer *>(comp.layer(1, vid));
    LayerParams ip;
    ip.klass = Klass::Overlay;
    ip.rect.x = 900; ip.rect.y = 380; ip.rect.w = 160; ip.rect.h = 80;
    const uint32_t img = comp.add_image(2, 0, ip, 160, 80, 30, Sampling::Nearest, now, &err);
    ImageLayer *image = static_cast<ImageLayer *>(comp.layer(2, img));
    LayerParams ep;
    ep.klass = Klass::Overlay;
    ep.rect.x = 740; ep.rect.y = 461; ep.rect.w = 440; ep.rect.h = 132;
    const uint32_t fx = comp.add_effect(3, 1, ep, "", kBars, 30, now, &err);
    ok("a video, an image and an effect layer (the effect compiled by glslang)", vid && img && fx);
    const float level = 0.7f;
    static_cast<EffectLayer *>(comp.layer(3, fx))->set_uniform("u_level", &level, 1);

    std::vector<uint8_t> atlas(video->frame_bytes());
    std::vector<uint8_t> px(160 * 80 * 4, 200);
    image->update({}, px.data(), px.size(), &err);
    for (int i = 0; i < 90; i++) {
        /* Still, then a small change, then whole-picture motion (packed). */
        for (size_t k = 0; k < atlas.size(); k++)
            atlas[k] = (uint8_t)(i < 30 ? k / 13 : i < 60 ? (k / 13 + (k % 1088 < 64 ? i : 0)) : k / 13 + i * 9);
        video->offer(atlas.data(), now);
        if (i % 10 == 0) {
            std::vector<uint8_t> patch(16 * 16 * 4, (uint8_t)i);
            image->update({crk::PixelRect{8, 8, 16, 16}}, patch.data(), patch.size(), &err);
        }
        run(33);
    }
    run(1000);
    std::string reason;
    ok("all shown", comp.state(vid, &reason) == LayerState::Shown && comp.state(img, &reason) == LayerState::Shown &&
                        comp.state(fx, &reason) == LayerState::Shown);
    const std::string s = video->stats();
    std::printf("     video: %s\n", s.c_str());
    ok("whole, partial and packed writes all went", s.find(" 0 whole") == std::string::npos &&
                                                       s.find(" 0 partial") == std::string::npos &&
                                                       s.find(" 0 packed") == std::string::npos &&
                                                       s.find(" 0 failed") != std::string::npos);

    LayerParams xp;
    xp.klass = Klass::Alert;
    xp.exclusive = true;
    const uint32_t alert = comp.add_effect(4, 0, xp, "", "#version 100\nvoid main(){gl_FragColor=vec4(0.0,0.0,1.0,1.0);}",
                                           0, now, &err);
    run(200);
    ok("an exclusive alert suspends the rest", comp.state(vid, &reason) == LayerState::Suspended);
    comp.remove_owner(4);
    comp.layer(2, img)->params.z = -5;
    comp.changed(comp.layer(2, img));
    run(200);
    ok("removed, and the order changed: back", alert && comp.state(vid, &reason) == LayerState::Shown);

    port.steal(m);
    run(2500);
    ok("the scene taken and published again: shown", comp.state(vid, &reason) == LayerState::Shown);
    comp.remove_owner(1);
    comp.remove_owner(2);
    comp.remove_owner(3);
    run(1000);
    ok("every layer gone: both targets released", slots.status(0).state == TargetState::Idle &&
                                                      slots.status(1).state == TargetState::Idle);

    backend(gpu);

    /* Sample content through the compositor, on the panel. */
    AssetParams gp;
    gp.format = TexFormat::R8;
    gp.w = 256;
    gp.h = 64;
    gp.sampling = Sampling::Bilinear;
    const uint32_t glyphs = comp.add_asset(5, 1, gp, &err);
    AssetParams sp;
    sp.w = sp.h = 32;
    sp.wrap = Wrap::Repeat;
    const uint32_t sprite = comp.add_asset(5, 1, sp, &err);
    std::vector<uint8_t> gpx(256 * 64), spx(32 * 32 * 4);
    for (size_t i = 0; i < gpx.size(); i++)
        gpx[i] = (uint8_t)(i * 7);
    for (size_t i = 0; i < spx.size(); i++)
        spx[i] = (uint8_t)(i * 3);
    comp.asset(5, glyphs)->update({}, gpx.data(), gpx.size(), &err);
    comp.asset(5, sprite)->update({}, spx.data(), spx.size(), &err);
    LayerParams tp;
    tp.klass = Klass::Overlay;
    tp.rect.x = 60; tp.rect.y = 560; tp.rect.w = 512; tp.rect.h = 128;
    const uint32_t label = comp.add_effect(5, 1, tp, "", kGlyphs, 0, now, &err);
    EffectLayer *ll = static_cast<EffectLayer *>(comp.layer(5, label));
    const float cells[16] = {0, 0, 0.0625f, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    ll->set_uniform("u_cells", UniformType::Vec4, cells, 4);
    const bool bound = comp.bind(5, label, 0, "u_atlas", glyphs, &err) && comp.bind(5, label, 0, "u_sprite", sprite, &err);

    LayerParams cp;
    cp.rect.x = 660; cp.rect.y = 120; cp.rect.w = 600; cp.rect.h = 480;
    MeshParams mp;
    mp.camera.t[2] = 6;
    const uint32_t box = comp.add_mesh(5, 1, cp, mp, now, &err);
    MeshLayer *ml = static_cast<MeshLayer *>(comp.layer(5, box));
    const bool parted = comp.add_part(5, box, 1, kLitVs, kLitFs, PartLook(), &err) &&
                        comp.geometry(5, box, 1, cube_mesh(), now, &err);
    const float orange[4] = {1, 0.45f, 0, 1};
    ml->set_uniform(1, "u_color", orange, 4);
    Pose tilt;
    tilt.r[0] = 25;
    ml->set_pose(1, tilt);
    const bool spinning = ml->animate(1, 1, keyed(Animated::Rotate, 3, {0, 6000}, {25, 0, 0, 25, 360, 0}), &err);

    LayerParams np;
    np.klass = Klass::Overlay;
    np.rect.x = 1500; np.rect.y = 160; np.rect.w = 24; np.rect.h = 240;
    np.rotate = -120;
    np.pivot_x = np.pivot_y = true;
    np.px = 12; np.py = 228;
    const uint32_t needle = comp.add_image(5, 1, np, 24, 240, 30, Sampling::Bilinear, now, &err);
    std::vector<uint8_t> npx(24 * 240 * 4, 220);
    static_cast<ImageLayer *>(comp.layer(5, needle))->update({}, npx.data(), npx.size(), &err);
    const bool sweeping = comp.layer(5, needle)->animate(1, 0, keyed(Animated::Rotate, 1, {0, 3000, 6000}, {-120, 120, -120}), &err);

    LayerParams pp;
    pp.klass = Klass::Ambient;
    const uint32_t plasma = comp.add_effect(5, 1, pp, "", kPlasma, 0, now, &err);
    static_cast<EffectLayer *>(comp.layer(5, plasma))->cluster_time();
    ok("assets, a mesh layer with a lit cube, a turned needle and a clock effect",
       glyphs && sprite && label && bound && box && parted && spinning && needle && sweeping && plasma);
    run(1500);
    auto all_shown = [&]() {
        for (uint32_t id : {label, box, needle, plasma})
            if (comp.state(id, &reason) != LayerState::Shown) {
                std::printf("     layer %u: %s %s\n", id, layer_state_name(comp.state(id, &reason)), reason.c_str());
                return false;
            }
        return true;
    };
    ok("all shown", all_shown());
    run(1100);
    ok("the cube's geometry replaced a second later", comp.geometry(5, box, 1, cube_mesh(), now, &err));
    run(200);
    port.steal(p);
    run(500);
    port.free_slot(p);   /* the HMI's content gone again */
    run(2500);
    ok("the panel taken and published again: assets, binds, parts and animations back", all_shown());
    comp.layer(5, needle)->stop(1);
    comp.touch(comp.layer(5, needle));
    run(100);
    comp.remove_owner(5);
    run(4000);
    ok("all gone: the panel released", slots.status(1).state == TargetState::Idle);

#ifndef CRS_RAMSES27
    /* The synthetic scene file on the panel, through the compositor. */
    SceneSource src;
    src.file = gauge;
    src.res = {gauge_res};
    const uint32_t sc = comp.add_scene(6, 1, src, false, &err);
    SceneEdits *ed = comp.edits(6, sc);
    const std::vector<EditProblem> refused =
        ed->add("--rotate ^needle_pivot$=0,0,45\n--uniform ^needle$:u_color=1,0,0,1\n--set level=0.75\n"
                "--hide ^quad$\n--viewport ^main_cam$=0,0,960,360\n--pass-off ^glow_pass$\n--uniform ^bar$:u_level=1");
    run(1500);
    const size_t resources = resource_count(client, gpu.types().resource);
    const std::vector<EditProblem> problems = ed->take_problems();
    ok("the scene file loaded as the panel's scene, every edit kind applied in its first flush",
       sc && refused.empty() && comp.state(sc, &reason) == LayerState::Shown && gauge_edited(client, 45));
    ok("a uniform bound to a data object refused, reported", problems.size() == 1 &&
                                                                 problems[0].reason.find("bound to a data object") != std::string::npos);
    const std::set<uint32_t> uniforms(gpu.types().uniform, gpu.types().uniform + 11),
        datas(gpu.types().data, gpu.types().data + 5);
    ok("eleven uniform types and five data object types learned, each different",
       uniforms.size() == 11 && datas.size() == 5 && !uniforms.count(0) && !datas.count(0));
    for (int i = 0; i <= 30; i++) {
        ed->add("--rotate ^needle_pivot$=0,0," + std::to_string(i * 3));
        run(33);
    }
    ok("the needle turned at 30 edits a second", gauge_edited(client, 90) && comp.state(sc, &reason) == LayerState::Shown);

    /* Published again: the HUD (the panel's scene id) shown while the HMI holds the panel. */
    port.steal(p);
    run(200);
    LayerParams hp;
    const uint32_t hud = comp.add_effect(7, 2, hp, "", kBars, 0, now, &err);
    run(1500);
    ok("the HUD shown meanwhile: the scene file's scene destroyed",
       slots.status(2).state == TargetState::Shown && !client.findObjectByName("gauge"));
    comp.remove_owner(7);
    run(1000);
    port.free_slot(p);
    run(2500);
    ok("the panel published again: the file loaded again with every kept edit, no resource more than the first time",
       hud && comp.state(sc, &reason) == LayerState::Shown && gauge_edited(client, 90) &&
           resource_count(client, gpu.types().resource) == resources);

    SceneSource second;
    second.file = gauge2;
    second.res = {gauge2_res};
    const int requests = port.requests;
    comp.replace_scene(6, sc, second, false);
    ed->add("--rotate ^needle_pivot$=0,0,60\n--uniform ^needle$:u_color=1,0,0,1\n--set level=0.75\n"
            "--hide ^quad$\n--viewport ^main_cam$=0,0,960,360\n--pass-off ^glow_pass$");
    run(500);
    ok("another file in place: loaded and edited, the slot not asked for again, no resource more",
       comp.state(sc, &reason) == LayerState::Shown && gauge_edited(client, 60) && port.requests == requests &&
           resource_count(client, gpu.types().resource) == resources);

    /* Layers over the file: a video and an effect drawn in its scene, in
     * passes after its own (main_pass 0, glow_pass 1), its edits with them. */
    SceneLayers over;
    over.on = true;
    comp.set_layers(sc, over);
    comp.replace_scene(6, sc, second, false);
    LayerParams hv;
    hv.rect.x = 700; hv.rect.y = 160; hv.rect.w = 560; hv.rect.h = 400;
    VideoParams hvp;
    hvp.w = 560;
    hvp.h = 400;
    const uint32_t hosted = comp.add_video(8, 1, hv, hvp, now, &err);
    LayerParams fp;
    fp.klass = Klass::Overlay;
    fp.rect = hv.rect;
    const uint32_t edge = comp.add_effect(8, 1, fp, "", kBars, 0, now, &err);
    VideoLayer *hvideo = static_cast<VideoLayer *>(comp.layer(8, hosted));
    std::vector<uint8_t> hatlas(hvideo->frame_bytes());
    for (int i = 0; i < 30; i++) {
        for (size_t k = 0; k < hatlas.size(); k++)
            hatlas[k] = (uint8_t)(k / 7 + i * 5);
        hvideo->offer(hatlas.data(), now);
        if (i == 10)
            ed->add("--rotate ^needle_pivot$=0,0,30\n--uniform ^needle$:u_color=1,0,0,1\n--set level=0.75\n"
                    "--hide ^quad$\n--viewport ^main_cam$=0,0,960,360\n--pass-off ^glow_pass$");
        run(33);
    }
    run(500);
    const ramses::RenderPass *lp0 = gauge_object<ramses::RenderPass>(client, "cluster-render layers");
    ok("layers over the file: a video and an effect drawn in its scene from render order 2, its edits flushed with them",
       comp.state(sc, &reason) == LayerState::Shown && comp.state(hosted, &reason) == LayerState::Shown &&
           comp.state(edge, &reason) == LayerState::Shown && lp0 && lp0->getRenderOrder() == 2 &&
           gauge_edited(client, 30));
    comp.remove_owner(8);
    run(500);
    ok("the layers gone, the file stays", comp.state(sc, &reason) == LayerState::Shown && client.findObjectByName("gauge"));
    comp.remove_owner(6);
    run(1500);
    ok("dropped: the panel released, the scene and the resources its load made gone",
       slots.status(1).state == TargetState::Idle && !client.findObjectByName("gauge") &&
           resource_count(client, gpu.types().resource) < resources);

    SceneSource wrong;
    wrong.file = other;
    wrong.res = {other_res};
    const uint32_t bad = comp.add_scene(6, 1, wrong, false, &err);
    run(500);
    ok("a file of another scene id refused, nothing left of it",
       bad && comp.state(bad, &reason) == LayerState::Error && reason.find("scene id is 0x10004f9") != std::string::npos &&
           !client.findObjectByName("gauge"));
    comp.remove_owner(6);
    run(500);
    const int removed = std::system(("rm -rf " + dir).c_str());
    (void)removed;
#endif
    ok("nothing logged as an error", cld::log_errors == 0);
    framework.disconnect();
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
