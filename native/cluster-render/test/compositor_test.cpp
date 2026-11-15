/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for the compositor and its layers (compositor.h, layer.h,
 * video_layer.h) on a fake renderer that keeps pending and flushed state
 * apart (fake_graph.h):
 * - a video layer maps its target from its first frame, writes the atlas
 *   whole, then only the tiles that changed, nothing for noise under the
 *   threshold, and shows exactly the newest frame;
 * - a new scene's first frame goes whole and exact at once, past the
 *   budget; under a tight byte budget fast motion then goes as packed blocks
 *   and the exact atlas comes back when the budget allows;
 * - a refused setData holds the tick's flush back, and until the video is
 *   written whole again the target flushes nothing, so the cluster keeps
 *   the last frame and never shows a half-written one; a refused flush is
 *   recovered the same way; an image is hidden until written whole, never
 *   holding the target, unless it is exclusive (it then holds, as the
 *   layers below it are suspended); held rewrites go as the budget allows,
 *   so the bytes tried stay within it, and several held videos rewrite
 *   together;
 * - layers of several clients share one scene: class and z order, an
 *   exclusive layer suspends the ones below and gives them back in one
 *   flush, clients coming and going cause no display traffic, and the
 *   target is released only after the last layer has gone and the linger;
 * - warm scenes: a target released or taken by the display keeps its
 *   scene, writes and flushes nothing while released, and asked for again
 *   flushes in the same tick, a video writing only the tiles that changed
 *   since the scene's last flush; the renderer's resend of the whole scene
 *   is charged to the byte budget, a new scene's not;
 * - layers keep their content across their target's scene going and
 *   coming back (another target with the same scene id taking it); the
 *   panel on its own scene id drawn and shown with the HUD, and after its
 *   id is refused, on the HUD's id, drawn again from its layers in that
 *   id's scene once the HUD lets it go;
 * - effects compile or are refused at creation; uniforms and u_time;
 * - the limits: 64 layers a client and 96 in all; 96 small layers changing
 *   every tick, far past the budget, written within it, each an even share;
 * - assets: written before the layers sampling them are drawn, charged to
 *   the budget, bound and bound again, refused as a drop while sampled;
 * - a scene published again rebuilds assets, binds, uniform arrays, mesh
 *   parts with their geometry, and animations;
 * - quads turned and scaled about their pivot; uniform arrays;
 * - mesh layers: parts with their depth, culling and blending, ready with
 *   their first geometry, geometry replaced at most once a second and
 *   paced; views and quads drawn in passes that follow the stack;
 * - animations on quads, parts and cameras, and the renderer's u_time;
 * - fades: video and image layers drawn at 0 until their target is shown,
 *   then brought to their own opacity over fade_ms in a few flushes; when
 *   nothing wants the target they stay drawn while they fade out, and it is
 *   released after the fade and the linger; a slot the display takes goes
 *   at once; a parked target stages nothing. */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../kit/block_pack.h"
#include "../kit/yuv_atlas.h"
#include "../src/compositor.h"
#include "fake_graph.h"

using namespace crs;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

struct CountingPort : Port {
    int requests = 0, releases = 0;
    bool available() const override { return true; }
    void request(const TargetSpec &) override { requests++; }
    void release(const TargetSpec &) override { releases++; }
    void take_over(const TargetSpec &, uint64_t) override {}
    void refresh_slots() override {}
};

static const uint32_t W = 1088, H = 480, AH = H + H / 2;

/* A map-like frame: smooth gradients, so changes are where the test puts them. */
static std::vector<uint8_t> frame(int seed)
{
    std::vector<uint8_t> f((size_t)W * AH);
    for (uint32_t y = 0; y < AH; y++)
        for (uint32_t x = 0; x < W; x++)
            f[(size_t)y * W + x] = (uint8_t)(y < H ? ((x + y) / 8 + seed * 7) & 0xff : 128);
    return f;
}

static std::vector<uint8_t> rounded(const std::vector<uint8_t> &f)
{
    std::vector<uint8_t> r(f.size());
    crk::round_levels(r.data(), f.data(), f.size(), 2);
    return r;
}

struct Rig {
    CountingPort port;
    FakeGpu gpu;
    SlotManager slots{&port, nullptr};
    Compositor *comp = nullptr;
    int64_t now = 50000;
    int map = 0, panel = 1, hud = 2;

    /* own_panel: the panel on its own scene id, falling back to the HUD's,
     * as cluster-render's main sets it. */
    explicit Rig(uint64_t budget = 0, CompositorLimits lim = CompositorLimits(), bool share_hud = false,
                 bool own_panel = false)
    {
        TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = W; m.height = H; m.linger_ms = 2000;
        TargetSpec p;
        p.name = "panel"; p.slot = 0x203; p.scene_id = 0x01000400; p.width = 1920; p.height = 720; p.linger_ms = 2000;
        if (own_panel) {
            p.scene_id = 0x010004f8;
            p.fallback_scene_id = 0x01000400;
        }
        TargetSpec h;
        h.name = "hud"; h.slot = 0x323; h.scene_id = 0x01000400; h.width = 800; h.height = 480; h.linger_ms = 2000;
        h.share = share_hud;
        slots.add(m);
        slots.add(p);
        slots.add(h);
        GovernorConfig gc;
        gc.max_bps = budget;
        gc.floor_bps = budget / 3;
        comp = new Compositor(&gpu, &slots, gc, 250, lim);
        slots.set_scenes(comp);
        std::string err;
        if (!comp->init(&err))
            std::printf("init: %s\n", err.c_str());
    }
    ~Rig() { delete comp; }
    void step(int64_t ms = 0)
    {
        now += ms;
        comp->arrange(now);
        slots.tick(now);
        comp->render(now);
    }
    void run(int64_t ms, int64_t tick = 10)
    {
        for (int64_t t = 0; t < ms; t += tick)
            step(tick);
    }
    FakeGraph *g(const char *t) { return gpu.graph(t); }
};

static LayerParams params(Klass k, Rect r, bool contend = false);
static Rect R(int x, int y, uint32_t w, uint32_t h);

/* While the panel waits after a loss, the HUD (the panel's scene id) wants
 * its target, is shown, and its client goes: the panel's scene is
 * unpublished for the HUD's, which is released after its linger. */
static void hud_borrows_scene(Rig &r)
{
    std::string err;
    const uint32_t h = r.comp->add_image(9, r.hud, params(Klass::Content, R(0, 0, 8, 8)), 8, 8, 30,
                                         Sampling::Nearest, r.now, &err);
    std::vector<uint8_t> px(8 * 8 * 4, 1);
    static_cast<ImageLayer *>(r.comp->layer(9, h))->update({}, px.data(), px.size(), &err);
    r.step(10);
    r.slots.on_shown(0x01000400, r.now);
    r.comp->remove_owner(9);
}

static LayerParams params(Klass k, Rect r, bool contend)
{
    LayerParams p;
    p.klass = k;
    p.rect = r;
    p.contend = contend;
    return p;
}

static Rect R(int x, int y, uint32_t w, uint32_t h)
{
    Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

/* 20 s of a map at 30 fps on a 5500 KB/s budget, and the HUD's video
 * (452x200 at 10 fps) on its share. hud: with the HUD layer; still: the
 * map's picture stops changing after its first frame.
 * Returns the map's flushes' bytes, one entry a flush, and the HUD's bytes. */
struct ShareRun {
    std::vector<uint64_t> map_flushes;
    uint64_t map_age_ms = 0, map_frames = 0, map_bytes = 0;
    uint64_t hud_bytes = 0;
    int hud_flushes = 0;
};
static uint64_t gm_total(Rig &r)
{
    FakeGraph *g = r.g("map");
    return g ? g->flushed_bytes : 0;
}

/* hud_shown false: a HUD layer that wants nothing (hidden); map false: no
 * map layer, the HUD alone. */
static ShareRun share_run(bool hud, bool still, bool hud_shown = true, bool map = true)
{
    Rig r(5500 * 1024, CompositorLimits(), true);
    std::string err;
    const uint32_t mid = map ? r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H), true), VideoParams(), r.now, &err) : 0;
    VideoParams hp;
    hp.w = 452;
    hp.h = 200;
    hp.fps = 10;
    LayerParams hlp = params(Klass::Content, R(174, 140, 452, 200), true);
    hlp.visible = hud_shown;
    const uint32_t hid = hud ? r.comp->add_video(2, r.hud, hlp, hp, r.now, &err) : 0;
    VideoLayer *mv = map ? static_cast<VideoLayer *>(r.comp->layer(1, mid)) : nullptr;
    VideoLayer *hv = hud ? static_cast<VideoLayer *>(r.comp->layer(2, hid)) : nullptr;
    std::vector<uint8_t> hf((size_t)452 * 300);
    ShareRun out;
    uint64_t map_seen = 0, hud_seen = 0;
    int hud_seen_flushes = 0;
    for (int k = 0; k < 600; k++) {   /* 20 s at 30 fps */
        const std::vector<uint8_t> f = frame(still ? 1 : k);
        if (mv)
            mv->offer(f.data(), r.now);
        if (hv) {
            for (size_t i = 0; i < hf.size(); i++)
                hf[i] = (uint8_t)(i / 7 + k * 5);
            hv->offer(hf.data(), r.now);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0) {
                r.slots.on_shown(0x010004f0, r.now);
                if (hud)
                    r.slots.on_shown(0x01000400, r.now);
            }
        }
        FakeGraph *gm = r.g("map"), *gh = r.g("hud");
        if (gm && gm->flushed_bytes != map_seen) {
            out.map_flushes.push_back(gm->flushed_bytes - map_seen);
            map_seen = gm->flushed_bytes;
        }
        if (gh) {
            hud_seen = gh->flushed_bytes;
            hud_seen_flushes = gh->flushes;
        }
    }
    out.hud_bytes = hud_seen;
    out.hud_flushes = hud_seen_flushes;
    out.map_bytes = gm_total(r);
    out.map_age_ms = r.comp->write_stats(r.map).age_ms;
    out.map_frames = r.comp->write_stats(r.map).frames;
    return out;
}

static void share_hud()
{
    const uint64_t rate = 5500 * 1024, secs = 20;
    char name[220];
    const ShareRun alone = share_run(false, false), hidden = share_run(true, false, false), with = share_run(true, false);
    std::snprintf(name, sizeof name, "HUD share, HUD off (no layer, or one that wants nothing): the map writes the same %zu "
                  "flushes, %llu KB", alone.map_flushes.size(), (unsigned long long)(alone.map_bytes / 1024));
    ok(name, alone.map_flushes == hidden.map_flushes && hidden.hud_bytes == 0);
    std::snprintf(name, sizeof name, "HUD share, moving map: the map at least half (%llu KB), the HUD at most half (%llu KB, "
                  "%d flushes) of %llu KB", (unsigned long long)(with.map_bytes / 1024), (unsigned long long)(with.hud_bytes / 1024),
                  with.hud_flushes, (unsigned long long)(rate * secs / 1024));
    ok(name, with.map_bytes >= rate * secs / 2 - 783360 && with.hud_bytes <= rate * secs / 2 + rate / 8 + 452 * 300);
    ok("HUD share, moving map: the HUD smooth, about its 10 a second", with.hud_flushes >= 150 && with.hud_flushes <= 205);
    ok("HUD share, moving map: what the HUD does not need goes to the map",
       with.map_bytes + with.hud_bytes >= rate * (secs - 1) && with.map_bytes > alone.map_bytes - with.hud_bytes - rate / 4);

    const ShareRun still_with = share_run(true, true);
    std::snprintf(name, sizeof name, "HUD share, still map: the HUD at most 10 updates a second (%d flushes, %llu KB in 20 s)",
                  still_with.hud_flushes, (unsigned long long)(still_with.hud_bytes / 1024));
    ok(name, still_with.hud_flushes >= 150 && still_with.hud_flushes <= 205 && still_with.hud_bytes > 150ull * 452 * 300 / 2);
    const ShareRun hud_alone = share_run(true, false, true, false);
    std::snprintf(name, sizeof name, "HUD share, the HUD alone: no map writes, the HUD at its 10 a second (%d flushes, %llu KB)",
                  hud_alone.hud_flushes, (unsigned long long)(hud_alone.hud_bytes / 1024));
    ok(name, hud_alone.map_bytes == 0 && hud_alone.hud_flushes >= 150 && hud_alone.hud_flushes <= 205 &&
                 hud_alone.hud_bytes <= rate * secs + rate / 4);
}

/* The video layer's atlas buffer and quad, and its packed quad, as shown. */
struct VideoView {
    const FakeBuffer *atlas = nullptr;
    int atlas_quad = 0, block_quad = 0;
};

static VideoView video_view(FakeGraph *g)
{
    VideoView v;
    for (const auto &kv : g->shown.quads) {
        auto t = kv.second.textures.find("u_image");
        if (t == kv.second.textures.end())
            continue;
        const FakeBuffer &b = g->shown.buffers.at(t->second);
        if (b.f == TexFormat::R8) {
            v.atlas = &b;
            v.atlas_quad = kv.first;
        } else if (b.w == crk::block_texels_w(W) && b.h == crk::block_texels_h(H)) {
            v.block_quad = kv.first;
        }
    }
    return v;
}

static void video_basics()
{
    Rig r;
    std::string err;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H), true), vp, r.now, &err);
    r.run(500);
    ok("a video layer with no frame does not ask for its target", id && r.port.requests == 0 && !r.g("map"));
    const std::vector<uint8_t> a = frame(1);
    static_cast<VideoLayer *>(r.comp->layer(1, id))->offer(a.data(), r.now);
    r.step(10);
    FakeGraph *g = r.g("map");
    VideoView v = g ? video_view(g) : VideoView();
    ok("its first frame publishes and requests the target, written whole in the first flush",
       r.port.requests == 1 && g && g->flushes == 1 && v.atlas && v.atlas->px == rounded(a) &&
           g->shown.quads[v.atlas_quad].visible && g->shown.quads[v.atlas_quad].uniforms["u_opacity"][0] == 1.0f);
    ok("the packed quad is hidden", v.block_quad && !g->shown.quads[v.block_quad].visible);
    r.slots.on_shown(0x010004f0, r.now);
    r.step(10);
    std::string why;
    ok("shown once the display maps it", r.comp->state(id, &why) == LayerState::Shown);

    /* One 64x64 tile of Y changes. */
    std::vector<uint8_t> b = a;
    for (uint32_t y = 128; y < 192; y++)
        for (uint32_t x = 256; x < 320; x++)
            b[(size_t)y * W + x] += 40;
    static_cast<VideoLayer *>(r.comp->layer(1, id))->offer(b.data(), r.now);
    const uint64_t f0 = g->flushes;
    r.step(40);
    v = video_view(g);
    ok("a changed tile is written alone, and the shown atlas is the new frame",
       g->flushes == f0 + 1 && v.atlas->px == rounded(b) && g->last_tag == f0 + 1);
    std::vector<uint8_t> c = b;
    for (size_t i = 0; i < (size_t)W * H; i += 3)
        c[i] = (uint8_t)(c[i] + 4);   /* decoder noise, under the threshold of 8 */
    static_cast<VideoLayer *>(r.comp->layer(1, id))->offer(c.data(), r.now);
    r.step(40);
    ok("noise under the threshold is not written: no flush", g->flushes == f0 + 1);
    Layer *vlp = r.comp->layer(1, id);
    vlp->params.rotate = 90;
    vlp->params.sx = 0.5f;
    r.comp->changed(vlp);
    r.step(10);
    v = video_view(g);
    const Turn &ta = g->shown.quads[v.atlas_quad].turn, &tb = g->shown.quads[v.block_quad].turn;
    ok("a turn reaches both of the video's quads, about the rect's centre",
       ta.rotate == 90 && tb.rotate == 90 && ta.sx == 0.5f && tb.sx == 0.5f && ta.px == W / 2.0f &&
           ta.py == H / 2.0f && v.atlas->px == rounded(b));
    r.run(8100);
    ok("frames stopping for stall_ms: stalled, the target lingers, then is released, its scene kept",
       r.comp->state(id, &why) == LayerState::Stalled && r.port.releases == 1 && r.g("map") == g);
    static_cast<VideoLayer *>(r.comp->layer(1, id))->offer(b.data(), r.now);
    r.step(10);
    v = video_view(g);
    ok("a new frame maps it again, in the same scene", r.port.requests == 2 && r.g("map") == g && r.gpu.scenes == 1);
    r.run(1100);
    v = video_view(g);
    ok("and shows that frame once its tiles within the threshold settle", v.atlas && v.atlas->px == rounded(b));
}

static void warm_scene()
{
    Rig r;
    r.comp->set_fade_ms(250);
    std::string err;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H), true), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, id));
    const std::vector<uint8_t> a = frame(1);
    vl->offer(a.data(), r.now);
    r.step(10);
    r.slots.on_shown(0x010004f0, r.now);
    r.run(50);
    FakeGraph *g = r.g("map");
    r.slots.on_lost(0x010004f0, "fetch removed", r.now);
    r.step(10);
    ok("taken by the display: released, the scene kept",
       g && r.port.releases == 1 && r.g("map") == g && r.slots.status(r.map).state == TargetState::Waiting);
    /* Frames keep coming while it waits after the removal (2 s): two tiles
     * change. */
    std::vector<uint8_t> c = a;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            c[(size_t)y * W + x] += 40;
            c[(size_t)(y + 256) * W + x + 512] += 40;
        }
    const uint64_t f0 = g->flushes, b0 = g->flushed_bytes;
    for (int k = 0; k < 19; k++) {
        vl->offer(c.data(), r.now);
        r.run(100);
    }
    ok("released: nothing written or flushed while frames come", g->flushes == f0 && g->written == 0 &&
                                                                     r.port.requests == 1);
    r.run(150);
    const VideoView v = video_view(g);
    const uint64_t bytes = g->flushed_bytes - b0;
    ok("asked for again: the same scene, flushed at once with the newest frame",
       r.port.requests == 2 && r.gpu.scenes == 1 && g->flushes > f0 && v.atlas && v.atlas->px == rounded(c));
    ok("only the tiles that changed since its last flush are written", bytes == 2 * 64 * 64);
    ok("drawn at 0 until shown", v.atlas_quad && g->shown.quads[v.atlas_quad].uniforms["u_opacity"][0] == 0.0f);
}

static void packed_motion()
{
    Rig r(1000 * 1024);   /* 1000 KB/s: a whole atlas (783,360 B) is 0.77 s of budget */
    std::string err;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, id));
    const std::vector<uint8_t> a = frame(2);
    vl->offer(a.data(), r.now);
    r.step(10);
    FakeGraph *g = r.g("map");
    VideoView v = video_view(g);
    ok("a new scene's first frame goes whole and exact at once, past the budget",
       g->flushes == 1 && g->shown.quads[v.atlas_quad].visible && v.atlas->px == rounded(a));
    const std::vector<uint8_t> b = frame(30);
    vl->offer(b.data(), r.now);
    for (int i = 0; i < 150 && g->flushes < 2; i++)
        r.step(10);
    v = video_view(g);
    ok("a whole frame the budget cannot cover goes packed: packed quad shown, atlas hidden",
       g->flushes == 2 && g->shown.quads[v.block_quad].visible && !g->shown.quads[v.atlas_quad].visible);
    std::vector<uint8_t> expect(crk::block_bytes(W, H));
    crk::pack_blocks(rounded(b).data(), W, H, expect.data(), 1);
    const int bank = g->shown.quads[v.block_quad].textures["u_image"];
    ok("the packed bank holds the frame's packing", g->shown.buffers[bank].px == expect);
    r.run(1500);
    v = video_view(g);
    ok("with no newer frame, the exact atlas comes back once the budget allows, in one flush",
       g->shown.quads[v.atlas_quad].visible && !g->shown.quads[v.block_quad].visible && v.atlas->px == rounded(b));
    const std::string s = vl->stats();
    ok("counted: one packed write, one return to exact", s.find("1 packed, 1 back to exact") != std::string::npos);
}

static void failure_hold()
{
    Rig r;
    std::string err;
    VideoParams vp;
    const uint32_t vid = r.comp->add_video(1, r.panel, params(Klass::Content, R(416, 120, W, H)), vp, r.now, &err);
    const uint32_t img = r.comp->add_image(2, r.panel, params(Klass::Overlay, R(0, 0, 64, 64)), 64, 64, 30,
                                           Sampling::Nearest, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, vid));
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(2, img));
    const std::vector<uint8_t> a = frame(3), b = frame(9);
    std::vector<uint8_t> px(64 * 64 * 4, 200);
    vl->offer(a.data(), r.now);
    il->update({}, px.data(), px.size(), &err);
    r.step(10);
    FakeGraph *g = r.g("panel");
    VideoView v = video_view(g);
    ok("video and image drawn in one flush", g->flushes == 1 && g->visible().size() == 2);
    g->write_budget = 1000;   /* the next video write is refused part way */
    vl->offer(b.data(), r.now);
    const uint64_t f0 = g->flushes;
    r.step(40);
    ok("a refused setData holds the tick's flush back", g->flushes == f0);
    px.assign(px.size(), 90);
    il->update({}, px.data(), px.size(), &err);
    g->written = 0;   /* the video's whole rewrites are refused too */
    r.run(100);
    v = video_view(g);
    ok("until the video's whole rewrite goes the target holds: the cluster keeps the last frame",
       g->flushes == f0 && g->shown.quads[v.atlas_quad].visible && v.atlas->px == rounded(a));
    g->write_budget = -1;
    r.run(100);
    v = video_view(g);
    const int ibuf = g->shown.quads[g->visible().back()].textures["u_image"];
    ok("the video's next write is whole and shows the newest frame; the image follows",
       g->shown.quads[v.atlas_quad].visible && v.atlas->px == rounded(b) &&
           g->shown.buffers[ibuf].px == px);
    g->fail_flush = true;
    vl->offer(a.data(), r.now);
    r.run(100);
    v = video_view(g);
    ok("a refused flush: the target holds, still showing the last frame",
       g->shown.quads[v.atlas_quad].visible && v.atlas->px == rounded(b));
    g->fail_flush = false;
    r.run(100);
    v = video_view(g);
    ok("and then shows the newest frame", g->shown.quads[v.atlas_quad].visible && v.atlas->px == rounded(a));
}

/* The panel on its own scene id: drawn and shown with the HUD, each in its
 * own scene; its id refused, on the HUD's id, its scene unpublished, and
 * drawn again from its layers in a scene of that id once the HUD lets it
 * go. */
static void panel_and_hud_together()
{
    Rig r(0, CompositorLimits(), false, true);
    r.comp->set_fade_ms(0);
    std::string err;
    const uint32_t pi = r.comp->add_image(2, r.panel, params(Klass::Overlay, R(0, 0, 64, 64)), 64, 64, 30,
                                          Sampling::Nearest, r.now, &err);
    const uint32_t hi = r.comp->add_image(3, r.hud, params(Klass::Content, R(0, 0, 8, 8), true), 8, 8, 30,
                                          Sampling::Nearest, r.now, &err);
    std::vector<uint8_t> ppx(64 * 64 * 4, 120), hpx(8 * 8 * 4, 33);
    static_cast<ImageLayer *>(r.comp->layer(2, pi))->update({}, ppx.data(), ppx.size(), &err);
    static_cast<ImageLayer *>(r.comp->layer(3, hi))->update({}, hpx.data(), hpx.size(), &err);
    r.step(10);
    r.slots.on_shown(0x010004f8, r.now);
    r.slots.on_shown(0x01000400, r.now);
    r.run(100);
    FakeGraph *pg = r.g("panel"), *hg = r.g("hud");
    const auto drawn = [](FakeGraph *g, const std::vector<uint8_t> &px) {
        if (!g || g->visible().empty())
            return false;
        const int buf = g->shown.quads[g->visible().back()].textures["u_image"];
        return g->shown.buffers[buf].px == px;
    };
    ok("own ids: the panel and the HUD shown at once, each drawn in its own scene",
       pg && hg && pg != hg && r.port.requests == 2 && drawn(pg, ppx) && drawn(hg, hpx) &&
           r.slots.status(r.panel).state == TargetState::Shown && r.slots.status(r.hud).state == TargetState::Shown);
    r.slots.on_lost(0x010004f8, "fetch removed", r.now);
    r.run(2100);   /* asked for again after the removal */
    r.slots.on_refused(0x010004f8, Refusal::UnknownContent, "E_DCSM_CONTENTID_UNKNOWN", r.now);
    r.run(3000);
    ok("fallen back: the panel's scene unpublished, it waits for the HUD's id, the HUD drawn on",
       !r.g("panel") && r.slots.targets()[r.panel].scene_id == 0x01000400 && r.g("hud") == hg &&
           r.slots.status(r.hud).state == TargetState::Shown && !r.slots.requested(r.panel) && drawn(hg, hpx));
    r.comp->remove_owner(3);
    r.run(2100);
    r.run(1500);
    pg = r.g("panel");
    ok("the HUD's client gone: the HUD released and its scene unpublished, the panel drawn again in a scene of "
       "the fallback id",
       !r.g("hud") && pg && r.slots.requested(r.panel) && drawn(pg, ppx) && r.gpu.scenes == 3);
}

static void image_failure_hides()
{
    Rig r;
    std::string err;
    VideoParams vp;
    const uint32_t vid = r.comp->add_video(1, r.panel, params(Klass::Content, R(416, 120, W, H)), vp, r.now, &err);
    const uint32_t img = r.comp->add_image(2, r.panel, params(Klass::Overlay, R(0, 0, 64, 64)), 64, 64, 30,
                                           Sampling::Nearest, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, vid));
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(2, img));
    const std::vector<uint8_t> a = frame(3), b = frame(9);
    std::vector<uint8_t> px(64 * 64 * 4, 200);
    vl->offer(a.data(), r.now);
    il->update({}, px.data(), px.size(), &err);
    r.step(10);
    FakeGraph *g = r.g("panel");
    px.assign(px.size(), 90);
    il->update({}, px.data(), px.size(), &err);
    g->write_budget = 1000;   /* the image's write is refused part way */
    r.step(40);
    g->write_budget = -1;
    vl->offer(b.data(), r.now);
    r.step(1);
    VideoView v = video_view(g);
    ok("a refused image write hides the image; the video's next frame still goes",
       g->visible().size() == 1 && g->shown.quads[v.atlas_quad].visible && v.atlas->px == rounded(b));
    r.run(100);
    const int ibuf = g->shown.quads[g->visible().back()].textures["u_image"];
    ok("the image's whole rewrite shows it again", g->visible().size() == 2 && g->shown.buffers[ibuf].px == px);
}

static void exclusive_image_holds()
{
    Rig r;
    std::string err;
    VideoParams vp;
    const uint32_t vid = r.comp->add_video(1, r.panel, params(Klass::Content, R(416, 120, W, H)), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, vid));
    const std::vector<uint8_t> a = frame(5);
    vl->offer(a.data(), r.now);
    r.step(10);
    FakeGraph *g = r.g("panel");
    LayerParams ap = params(Klass::Alert, R(0, 0, 64, 64));
    ap.exclusive = true;
    const uint32_t img = r.comp->add_image(2, r.panel, ap, 64, 64, 1, Sampling::Nearest, r.now, &err);
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(2, img));
    std::vector<uint8_t> px(64 * 64 * 4, 200);
    il->update({}, px.data(), px.size(), &err);
    g->write_budget = 1000;   /* the alert's first write is refused part way */
    r.run(50);
    VideoView v = video_view(g);
    ok("an exclusive image whose write failed holds the target: the video stays shown",
       g->shown.quads[v.atlas_quad].visible && g->visible().size() == 1);
    g->write_budget = -1;
    r.run(20);
    v = video_view(g);
    const int top = g->visible().empty() ? 0 : g->visible().back();
    ok("its whole rewrite goes as soon as it can, at 1 fps too: the alert alone, the video suspended",
       g->visible().size() == 1 && top != v.atlas_quad && g->shown.buffers[g->shown.quads[top].textures["u_image"]].px == px);
}

static void image_made_exclusive_while_parked()
{
    Rig r;
    std::string err;
    VideoParams vp;
    const uint32_t vid = r.comp->add_video(1, r.panel, params(Klass::Content, R(416, 120, W, H)), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, vid));
    vl->offer(frame(6).data(), r.now);
    LayerParams ap = params(Klass::Alert, R(0, 0, 64, 64));
    const uint32_t img = r.comp->add_image(2, r.panel, ap, 64, 64, 1, Sampling::Nearest, r.now, &err);
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(2, img));
    std::vector<uint8_t> px(64 * 64 * 4, 200);
    il->update({}, px.data(), px.size(), &err);
    r.step(10);
    FakeGraph *g = r.g("panel");
    px.assign(px.size(), 90);
    il->update({}, px.data(), px.size(), &err);
    r.run(1000);   /* its 1 fps clock is due again */
    g->write_budget = 1000;   /* its write is refused part way: hidden, not holding */
    r.run(20);
    g->write_budget = -1;
    ap.exclusive = true;   /* made exclusive before its next deadline */
    il->params = ap;
    const uint64_t f0 = g->flushes;
    r.run(20);
    VideoView v = video_view(g);
    ok("an image made exclusive while hidden after a failure holds the target: never an empty flush",
       g->flushes > f0 && g->visible().size() == 1 && !g->shown.quads[v.atlas_quad].visible &&
           g->shown.buffers[g->shown.quads[g->visible().back()].textures["u_image"]].px == px);
}

static void hold_within_budget()
{
    const uint64_t budget = 6000 * 1024;
    Rig r(budget);
    std::string err;
    VideoParams vp;
    LayerParams lp = params(Klass::Content, R(0, 0, W, H));
    lp.prewarm = true;   /* the scene is up before the first frame */
    const uint32_t vid = r.comp->add_video(1, r.map, lp, vp, r.now, &err);
    const uint32_t img = r.comp->add_image(2, r.map, params(Klass::Overlay, R(0, 0, 64, 64)), 64, 64, 30,
                                           Sampling::Nearest, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, vid));
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(2, img));
    r.step(10);
    FakeGraph *g = r.g("map");
    g->fail_flush = true;   /* the first frame's flush is refused for 300 ms */
    std::vector<uint8_t> px(64 * 64 * 4, 200);
    il->update({}, px.data(), px.size(), &err);
    const std::vector<uint8_t> a = frame(4);
    vl->offer(a.data(), r.now);
    r.run(300, 1);
    const size_t tried = g->written;
    ok("a held first frame is rewritten as the budget allows: the bytes tried stay within it",
       tried <= budget * 300 / 1000 + budget / 4 + (size_t)W * AH + px.size());
    g->fail_flush = false;
    r.run(400, 1);
    VideoView v = video_view(g);
    ok("the display taking flushes again: the frame shows", v.atlas_quad && g->shown.quads[v.atlas_quad].visible &&
                                                                 v.atlas->px == rounded(a));
    r.run(200, 1);
    ok("and the image, never holding the target, follows", g->visible().size() == 2);
}

static void held_videos_together()
{
    Rig r;   /* no limit: nothing lines the frame clocks up */
    std::string err;
    VideoParams vp;
    const uint32_t v1 = r.comp->add_video(1, r.panel, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err);
    VideoLayer *l1 = static_cast<VideoLayer *>(r.comp->layer(1, v1));
    l1->offer(frame(1).data(), r.now);
    r.step(1);
    r.run(10, 1);
    const uint32_t v2 = r.comp->add_video(2, r.panel, params(Klass::Content, R(800, 240, W, H)), vp, r.now, &err);
    VideoLayer *l2 = static_cast<VideoLayer *>(r.comp->layer(2, v2));
    l2->offer(frame(2).data(), r.now);
    r.run(100, 1);
    FakeGraph *g = r.g("panel");
    g->fail_flush = true;   /* both videos' next frames fail */
    for (int i = 0; i < 3; i++) {
        l1->offer(frame(10 + i).data(), r.now);
        l2->offer(frame(20 + i).data(), r.now);
        r.run(33, 1);
    }
    g->fail_flush = false;
    const uint64_t f0 = g->flushes;
    r.run(100, 1);
    ok("two held videos with frame clocks apart rewrite together and the target flushes again",
       g->flushes > f0 && g->visible().size() == 2 && !r.comp->composite(r.panel));
}

static void refusals_lower_the_cap()
{
    Rig r(6000 * 1024);
    std::string err;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, id));
    int seed = 0;
    auto motion = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 33) {
            const std::vector<uint8_t> f = frame(++seed);
            vl->offer(f.data(), r.now);
            r.run(33, 11);
        }
    };
    motion(2000);
    r.slots.on_shown(0x010004f0, r.now);
    FakeGraph *g = r.g("map");
    g->fail_flush = true;
    motion(20000);
    ok("flushes refused for 20 s: evidence while the writes are tried, two steps down past the cooldown",
       r.comp->governor().cap() == 5000u * 1024 && r.g("map") == g);
}

static const char *const kFill = "#version 100\nprecision mediump float;\nuniform vec4 u_color;\n"
                                 "void main() { gl_FragColor = u_color; }";

/* Steps until the target's flushes carry more bytes; the ms it took. */
static int64_t until_written(Rig &r, FakeGraph *g, int64_t limit = 3000)
{
    const uint64_t b0 = g->flushed_bytes;
    const int64_t t0 = r.now;
    while (g->flushed_bytes == b0 && r.now - t0 < limit)
        r.step(10);
    return r.now - t0;
}

static void warm_scene_charged()
{
    const uint64_t rate = 2000 * 1024;   /* a 250 ms burst: 512,000 B */
    Rig r(rate);
    std::string err;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H), true), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, id));
    std::vector<uint8_t> a = frame(1);
    vl->offer(a.data(), r.now);
    r.step(10);
    FakeGraph *g = r.g("map");
    const size_t atlas = (size_t)W * AH, banks = 2 * crk::block_bytes(W, H);
    ok("a new scene: its first frame written whole at once", g && g->flushed_bytes == atlas);
    r.slots.on_shown(0x010004f0, r.now);
    a[0] += 40;
    vl->offer(a.data(), r.now);
    const int64_t fresh = until_written(r, g);
    /* (783,360 - 512,000) B of debt at 2,048,000 B/s: 133 ms. */
    ok("its debt is the frame's alone: the next write within 140 ms", fresh >= 120 && fresh <= 140);
    r.run(2000);
    r.slots.on_lost(0x010004f0, "fetch removed", r.now);
    a[W * 300] += 40;
    vl->offer(a.data(), r.now);
    const uint64_t b1 = g->flushed_bytes;
    while (r.port.requests < 2)
        r.step(10);
    r.step(10);
    ok("a warm scene asked for again: its newest frame flushed at once, nothing charged until the display takes it",
       g->flushed_bytes > b1);
    /* Taken with nearly a full burst: the resend of atlas and banks
     * (1,305,600 B) leaves about 800,000 B of debt, about 390 ms. */
    r.slots.on_shown(0x010004f0, r.now);
    a[W * 100] += 40;
    vl->offer(a.data(), r.now);
    const int64_t warm = until_written(r, g);
    ok("taken: its resend charged, the next write after 380-400 ms",
       atlas + banks == 1305600 && warm >= 380 && warm <= 400);
}

static void stack_and_lifecycle()
{
    Rig r;
    std::string err;
    const uint32_t base = r.comp->add_effect(1, r.panel, params(Klass::Content, R(0, 0, 100, 100)), "", kFill, 0,
                                             r.now, &err);
    const uint32_t over = r.comp->add_image(2, r.panel, params(Klass::Overlay, R(10, 10, 32, 32)), 32, 32, 30,
                                            Sampling::Nearest, r.now, &err);
    std::vector<uint8_t> px(32 * 32 * 4, 255);
    static_cast<ImageLayer *>(r.comp->layer(2, over))->update({}, px.data(), px.size(), &err);
    r.step(10);
    r.slots.on_shown(0x01000400, r.now);
    r.step(10);
    FakeGraph *g = r.g("panel");
    std::vector<int> vis = g->visible();
    ok("two clients' layers in one scene, the overlay above the content",
       vis.size() == 2 && g->shown.quads[vis[0]].effect != g->shown.quads[vis[1]].effect &&
           g->shown.quads[vis[1]].textures.count("u_image"));
    LayerParams ap = params(Klass::Alert, R(0, 0, 1920, 720));
    ap.exclusive = true;
    const uint32_t alert = r.comp->add_effect(3, r.panel, ap, "", kFill, 0, r.now, &err);
    r.step(10);
    std::string why;
    ok("an exclusive alert suspends the layers below it in one flush",
       g->visible().size() == 1 && r.comp->state(base, &why) == LayerState::Suspended &&
           r.comp->state(over, &why) == LayerState::Suspended && r.comp->state(alert, &why) == LayerState::Shown);
    r.comp->remove_owner(3);
    r.step(10);
    ok("the alert gone: both back in the next flush", g->visible().size() == 2);
    r.comp->remove_owner(2);
    r.run(5000);
    ok("clients coming and going while one stays: one request, no release",
       r.port.requests == 1 && r.port.releases == 0 && g->visible().size() == 1);
    Layer *l = r.comp->layer(1, base);
    l->params.visible = false;
    r.comp->changed(l);
    r.run(1990);
    ok("the last layer hidden: still mapped through the linger", r.port.releases == 0 && r.g("panel"));
    r.run(30);
    FakeGraph *g0 = r.g("panel");
    ok("then released, its scene kept", r.port.releases == 1 && g0);
    l->params.visible = true;
    r.comp->changed(l);
    r.step(10);
    ok("shown again: the same scene, requested once", r.port.requests == 2 && r.g("panel") == g0);
    ok("a layer of another client cannot be reached", r.comp->layer(2, base) == nullptr);
}

static void republish_keeps_content()
{
    Rig r;
    std::string err;
    const uint32_t img = r.comp->add_image(1, r.panel, params(Klass::Overlay, R(0, 0, 16, 16)), 16, 16, 30,
                                           Sampling::Nearest, r.now, &err);
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(1, img));
    std::vector<uint8_t> px(16 * 16 * 4, 7);
    il->update({}, px.data(), px.size(), &err);
    std::vector<uint8_t> patch(4 * 4 * 4, 99);
    il->update({crk::PixelRect{4, 4, 4, 4}}, patch.data(), patch.size(), &err);
    r.step(10);
    r.slots.on_lost(0x01000400, "fetch removed", r.now);
    r.step(10);
    hud_borrows_scene(r);
    ok("the display took it, and the HUD its scene id: the panel's scene down", !r.g("panel") && r.g("hud"));
    r.run(2100);
    FakeGraph *g = r.g("panel");
    bool same = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            if (kv.second.f == TexFormat::RGBA8 && kv.second.w == 16) {
                same = kv.second.px[0] == 7 && kv.second.px[((4 * 16) + 4) * 4] == 99;
            }
    ok("requested again: the image written whole as it was", g && same);
    std::vector<uint8_t> wrong(3, 0);
    ok("a short update is refused", !il->update({crk::PixelRect{0, 0, 1, 1}}, wrong.data(), wrong.size(), &err));
    ok("a rectangle outside the image is refused",
       !il->update({crk::PixelRect{15, 15, 2, 2}}, patch.data(), 16, &err));
}

static void effects()
{
    Rig r;
    std::string err;
    r.gpu.reject = "broken";
    ok("an effect that does not compile is refused at creation",
       r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), "", "broken", 0, r.now, &err) == 0 &&
           !err.empty());
    const uint32_t id = r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), "", kFill, 10,
                                           r.now, &err);
    EffectLayer *e = static_cast<EffectLayer *>(r.comp->layer(1, id));
    const float red[4] = {1, 0, 0, 1};
    e->set_uniform("u_color", red, 4);
    r.step(10);
    FakeGraph *g = r.g("panel");
    const int q = g->visible().at(0);
    ok("a uniform set before the scene reaches it with the first flush", g->shown.quads[q].uniforms["u_color"][0] == 1);
    r.run(500);
    ok("u_time advances at time_fps", g->shown.quads[q].uniforms.count("u_time") &&
                                          g->shown.quads[q].uniforms["u_time"][0] > 0.3f);
    const uint64_t f0 = g->flushes;
    g->unknown_uniform = "u_missing";
    e->set_uniform("u_missing", red, 1);
    r.step(1);
    r.step(1);
    ok("a uniform the shader lacks is dropped, not retried", g->flushes <= f0 + 1);
}

static void limits()
{
    CompositorLimits lim;
    lim.per_client = 2;
    lim.client_videos = 1;
    Rig r(0, lim);
    std::string err;
    VideoParams vp;
    ok("video layers are limited",
       r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err) &&
           !r.comp->add_video(2, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err));
    r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), "", kFill, 0, r.now, &err);
    ok("and layers per client",
       !r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), "", kFill, 0, r.now, &err));
    vp.w = 1090;
    ok("a video size the atlas cannot take is refused",
       !r.comp->add_video(3, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err));
    ok("an unknown target is refused",
       !r.comp->add_effect(3, 9, params(Klass::Overlay, R(0, 0, 8, 8)), "", kFill, 0, r.now, &err));
}

/* The default limits: 64 layers a client, 96 in all. */
static void layer_limits()
{
    Rig r;
    std::string err;
    auto add = [&](uint64_t owner, uint32_t i) {
        return r.comp->add_effect(owner, i % 2 ? r.map : r.panel,
                                  params(Klass::Overlay, R((int)(i % 16) * 16, (int)(i / 16) * 16, 16, 16)), "", kFill,
                                  0, r.now, &err);
    };
    uint32_t added = 0;
    for (uint32_t i = 0; i < 64; i++)
        added += add(1, i) != 0;
    ok("a client's 64 layers are taken", added == 64);
    ok("its 65th is refused: too many layers", !add(1, 64) && err == "too many layers");
    added = 0;
    for (uint32_t i = 0; i < 32; i++)
        added += add(2, i) != 0;
    ok("another client's 32 make 96 in all, taken", added == 32 && r.comp->layers().size() == 96);
    ok("a 97th, of a third client, is refused: too many layers", !add(3, 0) && err == "too many layers");
    r.run(100);
    ok("all 96 drawn, on two targets", r.g("panel") && r.g("map") &&
                                           r.g("panel")->visible().size() + r.g("map")->visible().size() == 96);
    r.comp->remove_owner(2);
    ok("a client gone frees its share", add(3, 0) != 0);
}

/* 96 small image layers changing every tick, far past the 5500 KB/s ceiling
 * (UploadPacer: the layers staged in a tick are charged after its flush, so
 * one flush may overdraw the allowance): in any window of t, at most t of the
 * rate, the burst and one flush, a flush at most one write of each layer; in
 * 10 s at most 10 s of the rate and the burst. Every layer gets an even
 * share, and the panel stays shown. */
static void many_layers_within_budget()
{
    const uint64_t budget = 5500 * 1024, burst = budget / 4;   /* the Rig's 250 ms */
    const uint32_t side = 48, layers = 96;
    const size_t frame = (size_t)side * side * 4;              /* 9 KB a write */
    Rig r(budget);
    std::string err;
    std::vector<ImageLayer *> imgs;
    for (uint32_t i = 0; i < layers; i++) {
        const uint64_t owner = i < 64 ? 1 : 2;
        const uint32_t id = r.comp->add_image(owner, r.panel,
                                              params(Klass::Overlay, R((int)(i % 24) * 80, (int)(i / 24) * 80, side, side)),
                                              side, side, 30, Sampling::Nearest, r.now, &err);
        imgs.push_back(id ? static_cast<ImageLayer *>(r.comp->layer(owner, id)) : nullptr);
    }
    ok("96 image layers taken", std::find(imgs.begin(), imgs.end(), nullptr) == imgs.end());
    std::vector<uint8_t> px(frame);
    auto change = [&](int n) {
        for (size_t i = 0; i < imgs.size(); i++) {
            std::fill(px.begin(), px.end(), (uint8_t)(n * 7 + i));
            imgs[i]->update({}, px.data(), px.size(), &err);
        }
    };
    change(0);
    r.step(10);
    r.slots.on_shown(0x01000400, r.now);
    FakeGraph *g = r.g("panel");
    /* The bytes each 10 ms tick flushes over 10 s. */
    const int ticks = 1000;
    std::vector<uint64_t> per_tick;
    uint64_t before = g ? g->flushed_bytes : 0;
    bool shown = g != nullptr;
    for (int n = 1; g && n <= ticks; n++) {
        change(n);
        r.step(10);
        per_tick.push_back(g->flushed_bytes - before);
        before = g->flushed_bytes;
        shown = shown && r.slots.status(r.panel).state == TargetState::Shown && g->visible().size() == layers;
    }
    const uint64_t flush = per_tick.empty() ? 0 : *std::max_element(per_tick.begin(), per_tick.end());
    uint64_t worst = 0, total = 0;
    bool within = per_tick.size() == (size_t)ticks && flush <= layers * frame;
    for (int w : {10, 100, 250, 1000}) {
        const size_t n = (size_t)w / 10;
        const uint64_t bound = budget * (uint64_t)w / 1000 + burst + layers * frame;
        uint64_t sum = 0;
        for (size_t i = 0; i < per_tick.size(); i++) {
            sum += per_tick[i];
            if (i >= n)
                sum -= per_tick[i - n];
            if (i + 1 >= n) {
                worst = std::max(worst, sum * 1000 / bound);
                within = within && sum <= bound;
            }
        }
    }
    for (uint64_t b : per_tick)
        total += b;
    char name[240];
    std::snprintf(name, sizeof name,
                  "96 layers past the budget: every window of 10 ms to 1 s within the rate, the burst and one "
                  "flush (%llu KB; worst %llu.%llu%% of that)",
                  (unsigned long long)(flush / 1024), (unsigned long long)(worst / 10), (unsigned long long)(worst % 10));
    ok(name, within);
    std::snprintf(name, sizeof name, "  10 s within 10 s of the 5500 KB/s ceiling and the burst (%llu KB), and at least "
                  "90%% of it", (unsigned long long)(total / 1024));
    ok(name, total <= budget * 10 + burst && total >= budget * 10 * 9 / 10);
    uint64_t least = ~0ull, writes = 0;
    for (const ImageLayer *l : imgs) {
        least = std::min(least, l->write_stats().frames);
        writes += l->write_stats().frames;
    }
    std::snprintf(name, sizeof name, "  every layer an even share: the least %llu writes, on average %llu",
                  (unsigned long long)least, (unsigned long long)(writes / layers));
    ok(name, least * 10 >= writes / layers * 9);
    ok("  the panel shown throughout, all 96 drawn", shown);
}


static const char *const kSample = "#version 100\nprecision mediump float;\nuniform sampler2D u_atlas;\n"
                                   "uniform vec4 u_glyphs[3];\nvarying vec2 v_uv;\n"
                                   "void main() { gl_FragColor = texture2D(u_atlas, v_uv) * u_glyphs[0]; }";
static const char *const kLitVs = "#version 100\nattribute vec3 a_position;\nattribute vec3 a_normal;\n"
                                  "uniform mat4 u_mvp;\nuniform mat4 u_normal;\nvarying vec3 v_n;\n"
                                  "void main() { v_n = (u_normal * vec4(a_normal, 0.0)).xyz;\n"
                                  "  gl_Position = u_mvp * vec4(a_position, 1.0); }";
static const char *const kFlatVs = "#version 100\nattribute vec3 a_position;\nuniform mat4 u_mvp;\n"
                                   "void main() { gl_Position = u_mvp * vec4(a_position, 1.0); }";
static const char *const kPartFs = "#version 100\nprecision mediump float;\nuniform vec4 u_color;\n"
                                   "void main() { gl_FragColor = u_color; }";

/* A triangle fan of n triangles, normals when asked. */
static MeshData fan(uint32_t n, bool normals)
{
    MeshData d;
    d.vertices = n + 2;
    for (uint32_t i = 0; i < d.vertices; i++) {
        d.position.insert(d.position.end(), {(float)i, (float)(i % 2), -5.0f});
        if (normals)
            d.normal.insert(d.normal.end(), {0.0f, 0.0f, 1.0f});
    }
    for (uint32_t i = 0; i < n; i++)
        d.index.insert(d.index.end(), {0, (uint16_t)(i + 1), (uint16_t)(i + 2)});
    return d;
}

static Animation keys(Animated prop, uint32_t width, std::vector<uint32_t> t, std::vector<float> v,
                      const std::string &uniform = "")
{
    Animation a;
    a.property = prop;
    a.width = width;
    a.t = t;
    a.v = v;
    a.uniform = uniform;
    a.loop = true;
    return a;
}

static const FakeBuffer *buffer_of(FakeGraph *g, int handle)
{
    auto it = g->shown.buffers.find(handle);
    return it == g->shown.buffers.end() ? nullptr : &it->second;
}

static void assets()
{
    Rig r;
    std::string err;
    AssetParams ap;
    ap.format = TexFormat::R8;
    ap.w = 64;
    ap.h = 32;
    ap.sampling = Sampling::Bilinear;
    ap.wrap = Wrap::Repeat;
    const uint32_t atlas = r.comp->add_asset(1, r.panel, ap, &err);
    AssetParams sp;
    sp.w = sp.h = 16;
    const uint32_t sprite = r.comp->add_asset(1, r.panel, sp, &err);
    const uint32_t elsewhere = r.comp->add_asset(1, r.map, sp, &err);
    r.run(100);
    ok("assets alone ask for nothing", atlas && sprite && elsewhere && r.port.requests == 0);
    const uint32_t fx = r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 64, 32)), "", kSample, 0,
                                           r.now, &err);
    ok("an effect layer samples an asset on its target", r.comp->bind(1, fx, 0, "u_atlas", atlas, &err));
    r.run(100);
    std::string why;
    ok("a layer sampling an asset with no pixels yet is stalled and asks for nothing",
       r.comp->state(fx, &why) == LayerState::Stalled && r.port.requests == 0);
    std::vector<uint8_t> glyphs(64 * 32);
    for (size_t i = 0; i < glyphs.size(); i++)
        glyphs[i] = (uint8_t)i;
    r.comp->asset(1, atlas)->update({}, glyphs.data(), glyphs.size(), &err);
    r.step(10);
    FakeGraph *g = r.g("panel");
    int abuf = 0;
    for (const auto &kv : g ? g->shown.buffers : std::map<int, FakeBuffer>())
        if (kv.second.f == TexFormat::R8)
            abuf = kv.first;
    const FakeBuffer *ab = g ? buffer_of(g, abuf) : nullptr;
    ok("its pixels: the target is published and the asset written whole in the first flush, as asked",
       r.port.requests == 1 && g && g->flushes == 1 && ab && ab->px == glyphs && ab->wrap == Wrap::Repeat &&
           ab->sampling == Sampling::Bilinear);
    ok("the layer sampling it is drawn from the next flush, bound to it",
       g && g->visible().empty() && (r.step(10), g->visible().size() == 1) &&
           g->shown.quads[g->visible()[0]].textures["u_atlas"] == abuf);
    std::vector<uint8_t> patch(4 * 2, 255);
    r.comp->asset(1, atlas)->update({crk::PixelRect{8, 4, 4, 2}}, patch.data(), patch.size(), &err);
    r.step(10);
    ab = buffer_of(g, abuf);
    ok("a change is written in place", ab && ab->px[4 * 64 + 8] == 255 && ab->px[4 * 64 + 7] == glyphs[4 * 64 + 7]);
    std::vector<uint8_t> px(16 * 16 * 4, 9);
    r.comp->asset(1, sprite)->update({}, px.data(), px.size(), &err);
    ok("bound again to another asset", r.comp->bind(1, fx, 0, "u_atlas", sprite, &err));
    r.step(10);
    r.step(10);
    const int q = g->visible().at(0);
    ok("the layer samples the new one", buffer_of(g, g->shown.quads[q].textures["u_atlas"])->f == TexFormat::RGBA8);
    ok("a dropped asset no layer samples goes", r.comp->remove_asset(1, atlas, &err));
    ok("one a layer samples is refused", !r.comp->remove_asset(1, sprite, &err) && err == "asset in use");
    ok("an asset of another target cannot be bound", !r.comp->bind(1, fx, 0, "u_atlas", elsewhere, &err));
    ok("nor one of another client", !r.comp->bind(2, fx, 0, "u_atlas", sprite, &err));
    g->unknown_sampler = "u_nope";
    ok("nor a sampler the shader lacks", !r.comp->bind(1, fx, 0, "u_nope", sprite, &err));
    const uint32_t img = r.comp->add_image(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), 8, 8, 30,
                                           Sampling::Nearest, r.now, &err);
    ok("image layers bind nothing", !r.comp->bind(1, img, 0, "u_image", sprite, &err));
    r.comp->remove_owner(1);
    ok("a client gone takes its layers, then its assets", r.comp->layers().empty() && r.comp->assets().empty());

    /* The budget: an asset's bytes are charged like a layer's. */
    Rig b(100 * 1024);   /* 100 KB/s, 25 KB of burst */
    AssetParams big;
    big.w = big.h = 128;   /* 64 KB */
    const uint32_t a2 = b.comp->add_asset(1, b.panel, big, &err);
    std::vector<uint8_t> apx(128 * 128 * 4, 3), ipx(16 * 16 * 4, 4);
    b.comp->asset(1, a2)->update({}, apx.data(), apx.size(), &err);
    const uint32_t i2 = b.comp->add_image(1, b.panel, params(Klass::Overlay, R(0, 0, 16, 16)), 16, 16, 30,
                                          Sampling::Nearest, b.now, &err);
    static_cast<ImageLayer *>(b.comp->layer(1, i2))->update({}, ipx.data(), ipx.size(), &err);
    b.step(10);
    FakeGraph *bg = b.g("panel");
    auto image_written = [&]() {
        for (const auto &kv : bg->shown.buffers)
            if (kv.second.w == 16 && kv.second.px[0] == 4)
                return true;
        return false;
    };
    ok("an asset is staged before the layers: it takes the budget first", bg && !image_written() &&
                                                                             bg->flushed_bytes == apx.size());
    b.run(300);
    ok("the layer waits until its bytes are repaid", !image_written());
    b.run(300);
    ok("then is written", image_written());
}

static void republish_rebuilds()
{
    Rig r;
    std::string err;
    AssetParams ap;
    ap.format = TexFormat::R8;
    ap.w = ap.h = 8;
    const uint32_t atlas = r.comp->add_asset(1, r.panel, ap, &err);
    std::vector<uint8_t> px(64, 77);
    r.comp->asset(1, atlas)->update({}, px.data(), px.size(), &err);
    const uint32_t fx = r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 64, 32)), "", kSample, 0,
                                           r.now, &err);
    EffectLayer *e = static_cast<EffectLayer *>(r.comp->layer(1, fx));
    e->cluster_time();
    const float glyphs[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    e->set_uniform("u_glyphs", UniformType::Vec4, glyphs, 3);
    r.comp->bind(1, fx, 0, "u_atlas", atlas, &err);
    e->animate(1, 0, keys(Animated::Rotate, 1, {0, 1000}, {0, 90}), &err);
    MeshParams mp;
    mp.projection.fov = 40;
    const uint32_t mesh = r.comp->add_mesh(1, r.panel, params(Klass::Content, R(100, 100, 400, 300)), mp, r.now, &err);
    MeshLayer *m = static_cast<MeshLayer *>(r.comp->layer(1, mesh));
    r.comp->add_part(1, mesh, 1, kLitVs, kSample, PartLook(), &err);
    r.comp->geometry(1, mesh, 1, fan(4, true), r.now, &err);
    Pose pose;
    pose.r[1] = 30;
    m->set_pose(1, pose);
    const float red[4] = {1, 0, 0, 1};
    m->set_uniform(1, "u_color", red, 4);
    r.comp->bind(1, mesh, 1, "u_atlas", atlas, &err);
    m->animate(2, 1, keys(Animated::Rotate, 3, {0, 3000}, {0, 0, 0, 0, 360, 0}), &err);
    r.run(50);
    r.slots.on_shown(0x01000400, r.now);
    r.run(50);
    r.slots.on_lost(0x01000400, "fetch removed", r.now);
    r.step(10);
    hud_borrows_scene(r);
    ok("the display took the panel, and the HUD its scene id: the panel's scene down", !r.g("panel") && r.g("hud"));
    r.run(5100);
    FakeGraph *g = r.g("panel");
    if (!g) {
        ok("published again", false);
        return;
    }
    r.run(50);
    int quad = 0, view = 0, part = 0;
    for (const auto &kv : g->shown.quads)
        quad = kv.first;
    for (const auto &kv : g->shown.views)
        view = kv.first;
    for (const auto &kv : g->shown.parts)
        part = kv.first;
    const FakeQuad &fq = g->shown.quads[quad];
    const int abuf = fq.textures.count("u_atlas") ? fq.textures.at("u_atlas") : 0;
    const FakeBuffer *ab = buffer_of(g, abuf);
    ok("published again: the asset written whole and the effect bound to it",
       ab && ab->px == px && fq.visible);
    ok("its uniform array", fq.types.count("u_glyphs") && fq.types.at("u_glyphs") == UniformType::Vec4 &&
                               fq.uniforms.at("u_glyphs") == std::vector<float>(glyphs, glyphs + 12));
    std::vector<Animation> qa = g->playing(quad);
    bool spin = false, clock = false;
    for (const Animation &a : qa) {
        spin = spin || (a.property == Animated::Rotate && a.v == std::vector<float>{0, 90});
        clock = clock || (a.property == Animated::Uniform && a.uniform == "u_time" &&
                          a.t == std::vector<uint32_t>{0, 3600000} && a.v == std::vector<float>{0, 3600} && a.loop);
    }
    ok("its animation and the renderer's u_time started again", qa.size() == 2 && spin && clock);
    const FakePart &fp = g->shown.parts[part];
    ok("the mesh layer's view, camera and part with its geometry",
       view && g->shown.views[view].visible && g->shown.views[view].p.fov == 40 && fp.view == view &&
           fp.geometry && fp.vertices == 6 && fp.indices == 12 && (fp.attributes & kAttrNormal));
    ok("the part's pose, uniform, bind and animation", fp.pose.r[1] == 30 && fp.uniforms.at("u_color")[0] == 1 &&
                                                          fp.textures.at("u_atlas") == abuf &&
                                                          g->playing(part).size() == 1);
}

/* An etc2rgba asset: w and h multiples of 4, sent whole once,
 * an immutable texture each scene made from it, its real bytes charged; a
 * texture the renderer refuses puts the layers sampling it in error. */
static void compressed_assets()
{
    std::string err;
    AssetParams ap;
    ap.format = TexFormat::ETC2RGBA;
    ap.w = 250;
    ap.h = 280;
    ok("etc2rgba: w and h are multiples of 4",
       Asset::size_error(ap.format, 250, 280) && Asset::size_error(ap.format, 252, 278) &&
           !Asset::size_error(ap.format, 252, 280));
    ok("its bytes: 16 a 4x4 block", tex_bytes(TexFormat::ETC2RGBA, 252, 280) == 63u * 70u * 16u);
    AssetParams big;
    big.format = TexFormat::ETC2RGBA;
    big.w = 2048;
    big.h = 1024;
    ok("a 2048x1024 atlas fits one frame message and takes 2 MiB of the pixel memory",
       !Asset::size_error(big.format, big.w, big.h) && Asset::memory(big) == 2u << 20);
    const size_t n = 63 * 70 * 16;
    CompositorLimits lim;
    lim.pixel_bytes = n + 1024;   /* its blocks, and room for an 8x8 image */
    Rig r(0, lim);
    ok("refused at add", !r.comp->add_asset(1, r.panel, ap, &err) && err.find("multiples of 4") != std::string::npos);
    ap.w = 252;
    const uint32_t a = r.comp->add_asset(1, r.panel, ap, &err);
    AssetParams sq;
    sq.w = sq.h = 16;   /* 2 KB charged, with its scratch */
    ok("charged its real bytes, no scratch", a && !r.comp->add_asset(1, r.panel, sq, &err) &&
                                                 err.find("pixel memory") != std::string::npos);
    Asset *as = r.comp->asset(1, a);
    std::vector<uint8_t> blocks(n);
    for (size_t i = 0; i < n; i++)
        blocks[i] = (uint8_t)(i * 7);
    ok("a short upload is refused", !as->update({}, blocks.data(), n - 16, &err) && !as->have());
    std::vector<uint8_t> four(4 * 4 * 4);
    ok("rects are refused", !as->update({crk::PixelRect{0, 0, 4, 4}}, four.data(), four.size(), &err) &&
                                err.find("no rects") != std::string::npos);
    const uint32_t fx = r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 252, 280)), "", kSample, 0,
                                           r.now, &err);
    ok("an effect layer samples it before it has pixels", r.comp->bind(1, fx, 0, "u_atlas", a, &err));
    ok("whole: taken", as->update({}, blocks.data(), n, &err));
    ok("once: sent again, refused", !as->update({}, blocks.data(), n, &err) && err.find("once") != std::string::npos);
    r.step(10);
    r.slots.on_shown(0x01000400, r.now);
    r.run(50);
    FakeGraph *g = r.g("panel");
    int tbuf = 0;
    for (const auto &kv : g ? g->shown.buffers : std::map<int, FakeBuffer>())
        if (kv.second.f == TexFormat::ETC2RGBA)
            tbuf = kv.first;
    const FakeBuffer *tb = g ? buffer_of(g, tbuf) : nullptr;
    ok("an immutable texture made from its blocks", tb && tb->w == 252 && tb->h == 280 && tb->px == blocks);
    std::string why;
    ok("the layer bound to it once made, and shown",
       g && g->visible().size() == 1 && g->shown.quads[g->visible()[0]].textures["u_atlas"] == tbuf &&
           r.comp->state(fx, &why) == LayerState::Shown);
    r.slots.on_lost(0x01000400, "fetch removed", r.now);
    r.step(10);
    hud_borrows_scene(r);
    r.run(5100);
    r.run(50);
    g = r.g("panel");
    int again = 0;
    for (const auto &kv : g ? g->shown.buffers : std::map<int, FakeBuffer>())
        if (kv.second.f == TexFormat::ETC2RGBA && kv.second.px == blocks)
            again = kv.first;
    ok("a new scene: made again and bound", again && g->visible().size() == 1 &&
                                                g->shown.quads[g->visible()[0]].textures["u_atlas"] == again);

    Rig no;
    no.gpu.refuse_textures = true;
    const uint32_t b = no.comp->add_asset(1, no.panel, ap, &err);
    no.comp->asset(1, b)->update({}, blocks.data(), n, &err);
    const uint32_t fx2 = no.comp->add_effect(1, no.panel, params(Klass::Overlay, R(0, 0, 252, 280)), "", kSample, 0,
                                             no.now, &err);
    no.comp->bind(1, fx2, 0, "u_atlas", b, &err);
    no.step(10);
    no.slots.on_shown(0x01000400, no.now);
    no.run(50);
    ok("refused by the renderer: the asset says so",
       no.comp->asset(1, b)->refused().find("refused") != std::string::npos);
    ok("and the layer sampling it is in error, with why",
       no.comp->state(fx2, &why) == LayerState::Error && why.find("etc2rgba") != std::string::npos);
    no.run(6000);
    ok("and no longer wants its target: released after the linger", no.port.releases == 1);

    /* etc2rgb: the same rules at 8 bytes a block, made as an etc2rgb texture. */
    AssetParams rgb;
    rgb.format = TexFormat::ETC2RGB;
    rgb.w = 252;
    rgb.h = 280;
    const size_t m = 63 * 70 * 8;
    ok("etc2rgb: 8 bytes a 4x4 block, charged once",
       tex_bytes(rgb.format, 252, 280) == m && Asset::memory(rgb) == m && Asset::size_error(rgb.format, 250, 280));
    Rig r3;
    const uint32_t c = r3.comp->add_asset(1, r3.panel, rgb, &err);
    std::vector<uint8_t> rgb_blocks(m, 0x5a);
    ok("etc2rgb: whole once", r3.comp->asset(1, c)->update({}, rgb_blocks.data(), m, &err) &&
                                  !r3.comp->asset(1, c)->update({}, rgb_blocks.data(), m, &err));
    const uint32_t fx3 = r3.comp->add_effect(1, r3.panel, params(Klass::Overlay, R(0, 0, 252, 280)), "", kSample, 0,
                                             r3.now, &err);
    r3.comp->bind(1, fx3, 0, "u_atlas", c, &err);
    r3.step(10);
    r3.slots.on_shown(0x01000400, r3.now);
    r3.run(50);
    FakeGraph *g3 = r3.g("panel");
    int rbuf = 0;
    for (const auto &kv : g3 ? g3->shown.buffers : std::map<int, FakeBuffer>())
        if (kv.second.f == TexFormat::ETC2RGB && kv.second.px == rgb_blocks)
            rbuf = kv.first;
    ok("etc2rgb: an etc2rgb texture made from its blocks, bound and shown",
       rbuf && g3->shown.quads[g3->visible()[0]].textures["u_atlas"] == rbuf &&
           r3.comp->state(fx3, &why) == LayerState::Shown);
}

static void transforms_and_uniforms()
{
    Rig r;
    std::string err;
    const uint32_t img = r.comp->add_image(1, r.panel, params(Klass::Overlay, R(100, 50, 40, 200)), 40, 200, 30,
                                           Sampling::Bilinear, r.now, &err);
    ImageLayer *il = static_cast<ImageLayer *>(r.comp->layer(1, img));
    std::vector<uint8_t> px(40 * 200 * 4, 255);
    il->update({}, px.data(), px.size(), &err);
    il->params.rotate = 30;
    il->params.sx = 2;
    r.comp->changed(il);
    r.step(10);
    FakeGraph *g = r.g("panel");
    const Turn t = g->shown.quads[g->visible().at(0)].turn;
    ok("a quad turns and scales about its centre by default",
       t.rotate == 30 && t.sx == 2 && t.sy == 1 && t.px == 20 && t.py == 100);
    il->params.py = 190;
    il->params.pivot_y = true;
    r.comp->changed(il);
    r.step(10);
    const Turn t2 = g->shown.quads[g->visible().at(0)].turn;
    ok("or about its pivot", t2.px == 20 && t2.py == 190);

    const uint32_t fx = r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), "", kSample, 0,
                                           r.now, &err);
    EffectLayer *e = static_cast<EffectLayer *>(r.comp->layer(1, fx));
    std::vector<float> m(16 * 4, 0.5f);
    ok("a mat4 array uniform", e->set_uniform("u_bones", UniformType::Mat4, m.data(), 4));
    r.step(10);
    const FakeQuad *fq = nullptr;
    for (const auto &kv : g->shown.quads)
        if (kv.second.uniforms.count("u_bones"))
            fq = &kv.second;
    ok("reaches the scene as one array of its type",
       fq && fq->uniforms.at("u_bones").size() == 64 && fq->types.at("u_bones") == UniformType::Mat4);
    std::vector<float> big(4096, 1.0f);
    ok("a quad's uniforms hold at most 4096 values",
       !e->set_uniform("u_big", UniformType::Vec4, big.data(), 1024) &&
           e->set_uniform("u_big", UniformType::Vec4, big.data(), 1000));
}

static void meshes()
{
    Rig r(100 * 1024);
    std::string err;
    MeshParams mp;
    const uint32_t id = r.comp->add_mesh(1, r.panel, params(Klass::Content, R(0, 0, 640, 480)), mp, r.now, &err);
    ok("a part's vertex shader must read a_position",
       !r.comp->add_part(1, id, 1, "#version 100\nvoid main(){gl_Position=vec4(0.0);}", kPartFs, PartLook(), &err));
    PartLook glow;
    glow.depth = false;
    glow.cull = Cull::None;
    glow.blend = Blend::Add;
    ok("parts with their own shaders and looks",
       r.comp->add_part(1, id, 1, kLitVs, kPartFs, PartLook(), &err) &&
           r.comp->add_part(1, id, 2, kFlatVs, kPartFs, glow, &err));
    ok("a part number once", !r.comp->add_part(1, id, 2, kFlatVs, kPartFs, glow, &err));
    r.run(100);
    std::string why;
    ok("no geometry yet: stalled, nothing asked", r.comp->state(id, &why) == LayerState::Stalled &&
                                                      r.port.requests == 0);
    ok("geometry lacking an attribute the shader reads is refused",
       !r.comp->geometry(1, id, 1, fan(2, false), r.now, &err) && err.find("a_normal") != std::string::npos);
    /* 30 KB each: the 25 KB burst takes the first only. */
    ok("geometry for both parts",
       r.comp->geometry(1, id, 1, fan(1200, true), r.now, &err) && r.comp->geometry(1, id, 2, fan(2400, false), r.now, &err));
    r.step(10);
    FakeGraph *g = r.g("panel");
    std::map<int, FakePart> parts = g ? g->shown.parts : std::map<int, FakePart>();
    int p1 = 0, p2 = 0;
    for (const auto &kv : parts)
        (kv.second.look.depth ? p1 : p2) = kv.first;
    ok("ready with its first geometry: published, the view shown",
       g && g->drawn().size() == 1 && g->is_view(g->drawn()[0]));
    ok("each part drawn as asked", p1 && p2 && parts[p1].look.cull == Cull::Back &&
                                        parts[p1].look.blend == Blend::Alpha && parts[p2].look.cull == Cull::None &&
                                        parts[p2].look.blend == Blend::Add && !parts[p2].look.depth);
    ok("geometry goes within the budget: one part in the first flush", parts[p1].geometry && !parts[p2].geometry);
    r.run(400);
    ok("the other once the budget allows", g->shown.parts[p2].geometry);
    ok("a part's geometry is replaced at most once a second",
       !r.comp->geometry(1, id, 1, fan(3, true), r.now, &err) && err.find("once a second") != std::string::npos);
    r.run(1000);
    ok("and then is", r.comp->geometry(1, id, 1, fan(3, true), r.now, &err));
    r.run(500);
    ok("made again, once", g->shown.parts[p1].geometries == 2 && g->shown.parts[p1].vertices == 5);
    MeshLayer *m = static_cast<MeshLayer *>(r.comp->layer(1, id));
    m->mesh.projection.ortho = false;
    m->mesh.camera.t[2] = 12;
    m->params.rect = R(10, 20, 300, 200);
    r.comp->changed(m);
    r.step(10);
    const FakeView &v = g->shown.views.begin()->second;
    ok("the camera and the rect move the view", v.camera.t[2] == 12 && v.r.x == 10 && v.r.w == 300);
    CompositorLimits lim;
    lim.pixel_bytes = 64 * 1024;
    Rig small(0, lim);
    const uint32_t sid = small.comp->add_mesh(1, small.panel, params(Klass::Content, R(0, 0, 64, 64)), mp, small.now, &err);
    small.comp->add_part(1, sid, 1, kFlatVs, kPartFs, PartLook(), &err);
    ok("geometry counts in the service's memory", !small.comp->geometry(1, sid, 1, fan(6000, false), small.now, &err) &&
                                                     err.find("memory") != std::string::npos);
}

static void pass_order()
{
    Rig r;
    std::string err;
    MeshParams mp;
    auto mesh = [&](Klass k, int z) {
        LayerParams lp = params(k, R(0, 0, 400, 300));
        lp.z = z;
        const uint32_t id = r.comp->add_mesh(1, r.panel, lp, mp, r.now, &err);
        r.comp->add_part(1, id, 1, kFlatVs, kPartFs, PartLook(), &err);
        r.comp->geometry(1, id, 1, fan(2, false), r.now, &err);
        return id;
    };
    const uint32_t a = r.comp->add_effect(1, r.panel, params(Klass::Content, R(0, 0, 8, 8)), "", kFill, 0, r.now, &err);
    const uint32_t m1 = mesh(Klass::Content, 1);
    const uint32_t img = r.comp->add_image(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), 8, 8, 30,
                                           Sampling::Nearest, r.now, &err);
    std::vector<uint8_t> px(8 * 8 * 4, 1);
    static_cast<ImageLayer *>(r.comp->layer(1, img))->update({}, px.data(), px.size(), &err);
    const uint32_t m2 = mesh(Klass::Overlay, 1);
    r.step(10);
    FakeGraph *g = r.g("panel");
    auto kinds = [&]() {
        std::string k;
        for (int h : g->drawn())
            k += g->is_view(h) ? 'v' : 'q';
        return k;
    };
    auto passes = [&]() {
        std::vector<bool> v;
        for (int h : g->drawn())
            v.push_back(g->is_view(h));
        return plan_passes(v);
    };
    ok("views and quads in the stack's order", kinds() == "qvqv" && passes() == std::vector<int>({0, 1, 2, 3}));
    const uint32_t b = r.comp->add_effect(1, r.panel, params(Klass::Content, R(0, 0, 8, 8)), "", kFill, 0, r.now, &err);
    r.step(10);
    ok("quads between the same views share a pass", kinds() == "qqvqv" &&
                                                      passes() == std::vector<int>({0, 0, 1, 2, 3}));
    LayerParams xp = params(Klass::Alert, R(0, 0, 400, 300));
    xp.exclusive = true;
    const uint32_t alert = r.comp->add_mesh(2, r.panel, xp, mp, r.now, &err);
    r.comp->add_part(2, alert, 1, kFlatVs, kPartFs, PartLook(), &err);
    r.comp->geometry(2, alert, 1, fan(2, false), r.now, &err);
    r.step(10);
    std::string why;
    ok("an exclusive mesh layer suspends the rest", kinds() == "v" && passes() == std::vector<int>({1}) &&
                                                       r.comp->state(m1, &why) == LayerState::Suspended);
    r.comp->remove_owner(2);
    r.step(10);
    ok("and gives them back in order", kinds() == "qqvqv");
    LayerParams top = r.comp->layer(1, m1)->params;
    top.klass = Klass::Alert;
    r.comp->layer(1, m1)->params = top;
    r.comp->changed(r.comp->layer(1, m1));
    r.step(10);
    ok("a view moved to the top of the stack is drawn last", kinds() == "qqqvv" &&
                                                               passes() == std::vector<int>({0, 0, 0, 1, 2}));
    (void)a;
    (void)b;
    (void)m2;
}

static void animations()
{
    Rig r;
    std::string err;
    const uint32_t fx = r.comp->add_effect(1, r.panel, params(Klass::Overlay, R(0, 0, 8, 8)), "", kFill, 0, r.now, &err);
    Layer *e = r.comp->layer(1, fx);
    r.step(10);
    FakeGraph *g = r.g("panel");
    const int q = g->visible().at(0);
    /* A change outside stage(): the server touches the layer, so it is
     * flushed. */
    auto flushed = [&]() {
        r.comp->touch(e);
        r.step(10);
        return true;
    };
    ok("a quad plays a rotation", e->animate(1, 0, keys(Animated::Rotate, 1, {0, 500, 1000}, {0, 45, 0}), &err) &&
                                       flushed() && g->playing(q).size() == 1);
    ok("a quad's rotation is one value", !e->animate(2, 0, keys(Animated::Rotate, 3, {0}, {0, 0, 0}), &err));
    ok("a quad has no parts", !e->animate(2, 1, keys(Animated::Scale, 3, {0}, {1, 1, 1}), &err));
    ok("the same number replaces it", e->animate(1, 0, keys(Animated::Scale, 3, {0, 100}, {1, 1, 1, 2, 2, 1}), &err) &&
                                           flushed() && g->playing(q).size() == 1 &&
                                           g->playing(q)[0].property == Animated::Scale);
    for (uint32_t k = 2; k <= Layer::kMaxAnimations; k++)
        e->animate(k, 0, keys(Animated::Uniform, 4, {0, 100}, {0, 0, 0, 1, 1, 1, 1, 1}, "u_color"), &err);
    ok("at most 16 a layer", !e->animate(99, 0, keys(Animated::Rotate, 1, {0}, {0}), &err));
    ok("stopped, it goes from the scene", e->stop(1) && flushed() && g->playing(q).size() == 15 && !e->stop(1));
    g->unknown_uniform = "u_missing";
    ok("one the renderer refuses is refused",
       !e->animate(1, 0, keys(Animated::Uniform, 1, {0, 1}, {0, 1}, "u_missing"), &err));

    MeshParams mp;
    const uint32_t mid = r.comp->add_mesh(1, r.panel, params(Klass::Content, R(0, 0, 64, 64)), mp, r.now, &err);
    Layer *m = r.comp->layer(1, mid);
    r.comp->add_part(1, mid, 1, kFlatVs, kPartFs, PartLook(), &err);
    ok("a mesh layer's camera moves", m->animate(1, 0, keys(Animated::Translate, 3, {0, 1000}, {0, 0, 0, 0, 0, 5}), &err));
    ok("but has no scale", !m->animate(2, 0, keys(Animated::Scale, 3, {0}, {1, 1, 1}), &err));
    ok("a part turns in 3D", m->animate(2, 1, keys(Animated::Rotate, 3, {0, 1000}, {0, 0, 0, 0, 360, 0}), &err));
    ok("not one that does not exist", !m->animate(3, 7, keys(Animated::Rotate, 3, {0}, {0, 0, 0}), &err));
    VideoParams vp;
    const uint32_t vid = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err);
    ok("a video layer plays none", !r.comp->layer(1, vid)->animate(1, 0, keys(Animated::Rotate, 1, {0}, {0}), &err));
}

static int64_t g_clock_us;
static int64_t fake_clock() { return g_clock_us; }

/* The adaptive budget closes the loop: flushes the renderer is slow to take
 * lower the shared cap a step at a time, what is written follows the cap,
 * and healthy upload raises it again. */
static void adaptive_budget()
{
    Rig r(6000 * 1024);
    r.comp->set_clock(fake_clock);
    r.gpu.clock_us = &g_clock_us;
    std::string err;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H)), vp, r.now, &err);
    VideoLayer *vl = static_cast<VideoLayer *>(r.comp->layer(1, id));
    int seed = 0;
    auto motion = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 33) {
            const std::vector<uint8_t> f = frame(++seed);
            vl->offer(f.data(), r.now);
            r.run(33, 11);
        }
    };
    motion(2000);
    r.slots.on_shown(0x010004f0, r.now);
    ok("starts at the 6000 KB/s ceiling", r.comp->governor().cap() == 6000u * 1024);
    r.gpu.flush_cost_us = 300000;   /* every flush now takes 300 ms */
    motion(3000);
    ok("slow flushes lower the cap one step", r.comp->governor().cap() == 5500u * 1024 &&
                                               r.comp->governor().reason().find("slow flush") != std::string::npos);
    motion(60000);
    ok("still slow: further steps, not below the floor",
       r.comp->governor().cap() < 5500u * 1024 && r.comp->governor().cap() >= 2000u * 1024);
    r.gpu.flush_cost_us = 0;
    motion(35000);   /* the evidence still in the window drains */
    const uint64_t cap = r.comp->governor().cap();
    FakeGraph *g = r.g("map");
    const uint64_t b0 = g->flushed_bytes;
    motion(10000);
    const uint64_t rate = (g->flushed_bytes - b0) / 10;
    ok("what is written follows the lowered cap", rate <= cap + cap / 10 && rate > cap / 2);
    std::printf("     wrote %llu KB/s at a %llu KB/s cap\n", (unsigned long long)(rate / 1024),
                (unsigned long long)(cap / 1024));
    motion(50000);
    ok("healthy upload raises it a step after a minute", r.comp->governor().cap() == cap + 500u * 1024);
    ok("and says why", r.comp->governor().reason().find("raised") != std::string::npos);
}

static float opacity(FakeGraph *g, int quad)
{
    auto it = g->shown.quads.find(quad);
    if (it == g->shown.quads.end() || it->second.uniforms["u_opacity"].empty())
        return -1.0f;
    return it->second.uniforms["u_opacity"][0];
}

static void fades()
{
    Rig r;
    r.comp->set_fade_ms(250);
    std::string err;
    const uint32_t id = r.comp->add_video(1, r.map, params(Klass::Content, R(0, 0, W, H), true), VideoParams(), r.now,
                                          &err);
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, id));
    const std::vector<uint8_t> a = frame(1);
    v->offer(a.data(), r.now);
    r.step(10);
    FakeGraph *g = r.g("map");
    const int quad = g ? video_view(g).atlas_quad : 0;
    ok("acquiring: the first frame drawn at opacity 0", quad && g->shown.quads[quad].visible && opacity(g, quad) == 0.0f);
    r.slots.on_shown(0x010004f0, r.now);
    const uint64_t f0 = g->flushes;
    const int64_t t0 = r.now;
    std::vector<float> seen;
    int64_t full_at = -1;
    for (int k = 0; k < 40; k++) {
        r.step(10);
        const float o = opacity(g, quad);
        if (seen.empty() || seen.back() != o)
            seen.push_back(o);
        if (o == 1.0f && full_at < 0)
            full_at = r.now - t0;
    }
    bool rising = true;
    for (size_t k = 1; k < seen.size(); k++)
        rising = rising && seen[k] > seen[k - 1];
    ok("shown: fades in from 0 to its opacity over 250 ms", full_at >= 250 && full_at <= 270 && rising);
    ok("in a few flushes, a step at most every 50 ms", seen.size() >= 4 && seen.size() <= 7 && g->flushes - f0 <= 6);

    Layer *l = r.comp->layer(1, id);
    l->params.visible = false;
    r.comp->changed(l);
    const int64_t t1 = r.now;
    int64_t hidden_at = -1;
    float last = 1.0f;
    bool falling = true;
    for (int k = 0; k < 40; k++) {
        r.step(10);
        if (!g->shown.quads[quad].visible) {
            if (hidden_at < 0)
                hidden_at = r.now - t1;
            continue;
        }
        const float o = opacity(g, quad);
        falling = falling && o <= last;
        last = o;
    }
    ok("hidden by its client, the last layer: drawn while it fades out over 250 ms, then hidden",
       hidden_at >= 250 && hidden_at <= 270 && falling && last < 0.5f);
    ok("the target kept through the fade, then lingering", r.port.releases == 0 &&
                                                              r.slots.status(r.map).state == TargetState::Lingering);
    r.run(2000);
    ok("released after the linger, its scene kept", r.port.releases == 1 && r.g("map") == g);

    Rig q;
    q.comp->set_fade_ms(250);
    LayerParams ip = params(Klass::Overlay, R(0, 0, 32, 32));
    ip.opacity = 0.5f;
    const uint32_t img = q.comp->add_image(1, q.panel, ip, 32, 32, 30, Sampling::Nearest, q.now, &err);
    std::vector<uint8_t> px(32 * 32 * 4, 200);
    static_cast<ImageLayer *>(q.comp->layer(1, img))->update({}, px.data(), px.size(), &err);
    q.step(10);
    q.slots.on_shown(0x01000400, q.now);
    q.run(300);
    FakeGraph *pg = q.g("panel");
    const int iq = pg && !pg->visible().empty() ? pg->visible()[0] : 0;
    ok("an image fades in to its own opacity", iq && opacity(pg, iq) == 0.5f);
    const uint64_t qf = pg ? pg->flushes : 0;
    q.slots.on_lost(0x01000400, "fetch removed", q.now);
    q.run(1000);
    ok("the display took the slot: released at once, nothing flushed, no fade",
       !q.slots.requested(q.panel) && pg && pg->flushes == qf && q.g("panel") == pg);
    q.run(4100);
    pg = q.g("panel");
    const int iq2 = pg && !pg->visible().empty() ? pg->visible()[0] : 0;
    ok("asked for again: drawn at 0 until shown", iq2 && opacity(pg, iq2) == 0.0f);
    q.slots.on_shown(0x01000400, q.now);
    q.run(300);
    ok("then fades in again", opacity(pg, iq2) == 0.5f);

    Rig e;
    e.comp->set_fade_ms(250);
    const uint32_t fx = e.comp->add_effect(1, e.panel, params(Klass::Content, R(0, 0, 100, 100)), "", kFill, 0,
                                           e.now, &err);
    e.step(10);
    e.slots.on_shown(0x01000400, e.now);
    e.run(300);
    Layer *el = e.comp->layer(1, fx);
    el->params.visible = false;
    e.comp->changed(el);
    e.step(10);
    ok("only effect layers: nothing to fade, lingering at once",
       e.slots.status(e.panel).state == TargetState::Lingering);

    Rig p;
    p.comp->set_fade_ms(250);
    const uint32_t pv = p.comp->add_video(1, p.map, params(Klass::Content, R(0, 0, W, H), true), VideoParams(), p.now,
                                          &err);
    VideoLayer *pvl = static_cast<VideoLayer *>(p.comp->layer(1, pv));
    pvl->offer(a.data(), p.now);
    p.step(10);
    p.slots.on_parked(0x010004f0, p.now);
    FakeGraph *mg = p.g("map");
    const uint64_t pf = mg ? mg->flushes : 0;
    const std::vector<uint8_t> b = frame(5);
    for (int k = 0; k < 30; k++) {
        pvl->offer((k & 1 ? a : b).data(), p.now);
        p.step(33);
    }
    ok("parked: frames keep coming, nothing is staged or flushed",
       mg && mg->flushes == pf && p.slots.status(p.map).state == TargetState::Parked);
    p.slots.on_shown(0x010004f0, p.now);
    pvl->offer(b.data(), p.now);
    p.run(300);
    const VideoView pvv = video_view(mg);
    ok("shown: the newest frame written, faded in", mg->flushes > pf && pvv.atlas && pvv.atlas->px == rounded(b) &&
                                                        opacity(mg, pvv.atlas_quad) == 1.0f);
}

/* A repeat is the frame it repeats: one layer given each frame whole and
 * another given repeats for the frames that are the last one again draw
 * the same, flush for flush, and count the same, with the budget to spare
 * (unchanged, within the threshold, settled) and short of it (held,
 * packed, back to exact). Returns the first layer's counts. */
static std::string repeats_are_frames(uint64_t budget, const char *name)
{
    Rig ra(budget), rb(budget);
    std::string err;
    VideoParams vp;
    VideoLayer *va = static_cast<VideoLayer *>(
        ra.comp->layer(1, ra.comp->add_video(1, ra.map, params(Klass::Content, R(0, 0, W, H)), vp, ra.now, &err)));
    VideoLayer *vb = static_cast<VideoLayer *>(
        rb.comp->layer(1, rb.comp->add_video(1, rb.map, params(Klass::Content, R(0, 0, W, H)), vp, rb.now, &err)));
    std::vector<uint8_t> last;
    bool alike = true;
    auto check = [&]() {
        FakeGraph *ga = ra.g("map"), *gb = rb.g("map");
        if (!ga || !gb) {
            alike = alike && !ga && !gb;
            return;
        }
        const VideoView a = video_view(ga), b = video_view(gb);
        const bool same = va->stats() == vb->stats() && ga->flushes == gb->flushes && a.atlas && b.atlas &&
                          a.atlas->px == b.atlas->px &&
                          ga->shown.quads[a.atlas_quad].visible == gb->shown.quads[b.atlas_quad].visible &&
                          ga->shown.quads[a.block_quad].visible == gb->shown.quads[b.block_quad].visible &&
                          ga->shown.quads[a.block_quad].textures == gb->shown.quads[b.block_quad].textures;
        if (!same && alike)
            std::printf("     %s\n     %s\n", va->stats().c_str(), vb->stats().c_str());
        alike = alike && same;
    };
    auto offer = [&](const std::vector<uint8_t> &f) {
        last = f;
        va->offer(f.data(), ra.now);
        vb->offer(f.data(), rb.now);
    };
    auto run = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 10) {
            ra.step(10);
            rb.step(10);
            check();
        }
    };
    /* the frame again every 30 ms for ms, the first at once */
    auto again = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 30) {
            va->offer(last.data(), ra.now);
            vb->repeat(rb.now);
            run(30);
        }
    };
    const std::vector<uint8_t> a = frame(1);
    offer(a);
    run(20);
    ra.slots.on_shown(0x010004f0, ra.now);
    rb.slots.on_shown(0x010004f0, rb.now);
    run(50);
    again(1000);
    std::vector<uint8_t> b = a;
    for (uint32_t y = 128; y < 192; y++)
        for (uint32_t x = 256; x < 320; x++)
            b[(size_t)y * W + x] += 40;
    offer(b);
    again(300);   /* the first while b waits */
    std::vector<uint8_t> c = b;
    for (size_t i = 0; i < (size_t)W * H; i += 3)
        c[i] = (uint8_t)(c[i] + 4);
    offer(c);
    again(1500);
    offer(frame(30));
    again(900);
    run(1500);
    ok(name, alike);
    return va->stats();
}

static bool counted(const std::string &stats, const char *what)
{
    unsigned long long n = 0;
    const size_t at = stats.find(what);
    if (at == std::string::npos)
        return false;
    size_t from = stats.rfind(", ", at);
    from = from == std::string::npos ? 0 : from + 2;
    return std::sscanf(stats.c_str() + from, "%llu", &n) == 1 && n > 0;
}

static void repeats()
{
    const std::string spare = repeats_are_frames(0, "repeats, the budget to spare: drawn and counted as the frames");
    ok("  with frames unchanged, within the threshold and settled",
       counted(spare, " unchanged") && counted(spare, " within threshold") && counted(spare, " settled") &&
           counted(spare, " superseded"));
    const std::string tight = repeats_are_frames(1000 * 1024, "repeats, short of budget: drawn and counted as the frames");
    ok("  with frames held, packed and back to exact",
       counted(tight, " budget holds") && counted(tight, " packed") && counted(tight, " back to exact"));
}

int main()
{
    repeats();
    share_hud();
    video_basics();
    packed_motion();
    failure_hold();
    panel_and_hud_together();
    image_failure_hides();
    exclusive_image_holds();
    image_made_exclusive_while_parked();
    hold_within_budget();
    held_videos_together();
    refusals_lower_the_cap();
    stack_and_lifecycle();
    warm_scene();
    warm_scene_charged();
    republish_keeps_content();
    effects();
    limits();
    layer_limits();
    many_layers_within_budget();
    adaptive_budget();
    assets();
    republish_rebuilds();
    compressed_assets();
    transforms_and_uniforms();
    meshes();
    pass_order();
    animations();
    fades();
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
