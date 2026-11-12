/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for hidden tiles (occlusion.h, video_layer.h, compositor.h) on
 * the fake renderer:
 * - the coverage's cells and a tile's footprint: luma, chroma, straddling
 *   rows, scaled;
 * - tiles wholly under an opaque camera are not written while hidden: the
 *   buffer keeps an older frame there, the rest is the newest;
 * - nothing is hidden under a translucent, turned or scaled-turned camera,
 *   an effect layer, an image with a transparent pixel there, an animated
 *   image, or an image whose waiting pixels are transparent;
 * - with a camera, an image, a fade, a composite occluded and a composite
 *   occluding moved, hidden, shown, made translucent and turned by sets,
 *   faster than a tight budget writes, with refused flushes and writes, and
 *   with packed blocks: no flush ever shows a part of the map that is not
 *   from the frame the rest of it shows (the check is the test's own, from
 *   what the display shows: every visible luma pixel, and every chroma
 *   sample that reaches one);
 * - the map panning past the budget on its target (and after resent's
 *   whole-scene charge) while an image over a panel video moves and hides:
 *   the panel's effect flushes every tick, the image's change waits only
 *   until the budget's debt is repaid, and nothing stale shows;
 * - a hidden tile changed within the threshold: a scene published again
 *   starts from the newest frame there;
 * - a camera as large as the map hides every map tile (the grid's edge and
 *   chroma tiles included), the map writes nothing while covered and the
 *   camera has the budget; moved aside, back and removed, nothing stale;
 *   an 800x480 camera hides exactly the tiles it covers on screen;
 * - an exclusive alert dismissed during a pan, made before or after the
 *   video: no flush shows neither it nor the video it suspended;
 * - an image animated and stopped every tick: no write while the budget is
 *   in debt; removed and made again: writes past the budget never back to
 *   back;
 * - a composite the renderer refuses over a scaled map: ticks run, the map
 *   shows and hides nothing;
 * - a held opacity change meeting a fade step: nothing stale;
 * - a camera sliding or jumping over a panning map, a panel change
 *   uncovering: within the debt and one map frame;
 * - a full-slot camera's client gone mid-pan: the map's forced write
 *   packed, every window within the rate, the burst, a frame and a packed
 *   frame;
 * - a hole made transparent in an image under a saturated budget: never a
 *   forced write;
 * - a waiting panel busy flushing its other changes keeps its wait's start;
 * - a video whose move waits composed with a joining camera: held no more;
 * - a forced write skipped when the newest frame no longer differs from
 *   the held tiles;
 * - with no opaque layer over a video, everything the renderer gets is
 *   what the build before hidden tiles sent (video_trace.h: a camera at 0.9
 *   or turned over the map, Waze on the cluster and the HUD, the map under
 *   an overlay with transparent pixels);
 * - bytes a minute, Waze-like, with and without the feature: an opaque
 *   camera, one at 0.9, a moving one (printed; the 0.9 one equal);
 * - the cost of the coverage test a tick (printed). */
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../kit/block_pack.h"
#include "../kit/yuv_atlas.h"
#include "../src/compositor.h"
#include "../src/occlusion.h"
#include "../src/video_composite.h"
#include "fake_graph.h"
#include "video_trace.h"

using namespace crs;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

struct NullPort : Port {
    bool available() const override { return true; }
    void request(const TargetSpec &) override {}
    void release(const TargetSpec &) override {}
    void take_over(const TargetSpec &, uint64_t) override {}
    void refresh_slots() override {}
};

static const uint32_t W = 1088, H = 480;

static Rect R(int x, int y, uint32_t w, uint32_t h)
{
    Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

struct Rig {
    NullPort port;
    FakeGpu gpu;
    SlotManager slots{&port, nullptr};
    std::unique_ptr<Compositor> comp;
    int64_t now = 50000;
    int map = 0, panel = 1;

    explicit Rig(uint64_t budget = 0, uint32_t videos = 2, int fade_ms = 0)
    {
        const struct { const char *name; uint64_t slot, scene; uint32_t w, h; } t[] = {
            {"map", 0x100, 0x010004f0, W, H}, {"panel", 0x203, 0x01000400, 1920, 720}};
        for (const auto &x : t) {
            TargetSpec s;
            s.name = x.name; s.slot = x.slot; s.scene_id = x.scene; s.width = x.w; s.height = x.h; s.linger_ms = 2000;
            slots.add(s);
        }
        GovernorConfig gc;
        gc.max_bps = budget;
        gc.floor_bps = budget / 3;
        CompositorLimits lim;
        lim.videos = videos;
        comp.reset(new Compositor(&gpu, &slots, gc, 250, lim));
        comp->set_fade_ms(fade_ms);
        slots.set_scenes(comp.get());
        std::string err;
        if (!comp->init(&err))
            std::printf("init: %s\n", err.c_str());
    }
    ~Rig() { comp.reset(); }
    void step(int64_t ms = 1)
    {
        now += ms;
        comp->arrange(now);
        slots.tick(now);
        comp->render(now);
    }
    void run(int64_t ms)
    {
        for (int64_t t = 0; t < ms; t++)
            step(1);
    }
    void show(const char *target)
    {
        step(1);
        slots.on_shown(slots.targets()[slots.index(target)].scene_id, now);
        step(1);
    }
    FakeGraph *g(const char *t) { return gpu.graph(t); }
    VideoLayer *video(uint64_t owner, int target, Rect r, uint32_t w, uint32_t h, int z = 0, float opacity = 1.0f,
                      VideoParams vp = VideoParams())
    {
        LayerParams lp;
        lp.rect = r;
        lp.opacity = opacity;
        lp.z = z;
        lp.contend = true;
        vp.w = w;
        vp.h = h;
        std::string err;
        const uint32_t id = comp->add_video(owner, target, lp, vp, now, &err);
        if (!id)
            std::printf("video: %s\n", err.c_str());
        return id ? static_cast<VideoLayer *>(comp->layer(owner, id)) : nullptr;
    }
    ImageLayer *image(uint64_t owner, int target, Rect r, uint32_t w, uint32_t h, uint32_t fps = 30)
    {
        LayerParams lp;
        lp.rect = r;
        lp.klass = Klass::Overlay;
        std::string err;
        const uint32_t id = comp->add_image(owner, target, lp, w, h, fps, Sampling::Bilinear, now, &err);
        return id ? static_cast<ImageLayer *>(comp->layer(owner, id)) : nullptr;
    }
};

static VideoParams exact(bool blocks = false)
{
    VideoParams vp;
    vp.level_bits = 0;
    vp.dirty_threshold = 0;
    vp.pan_blocks = blocks;
    return vp;
}

/* Frame k of a w x h video in which every pixel changes each frame, and
 * from which a pixel's k (mod 256) can be read back. */
static uint8_t luma(uint32_t x, uint32_t y, int k) { return (uint8_t)(x + 2 * y + (uint32_t)k); }
static uint8_t chroma(uint32_t c, uint32_t r, int plane, int k) { return (uint8_t)(c + 3 * r + 100 * plane + (uint32_t)k); }
static std::vector<uint8_t> counting(uint32_t w, uint32_t h, int k)
{
    std::vector<uint8_t> f((size_t)w * crk::atlas_height(h));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            f[(size_t)y * w + x] = luma(x, y, k);
    for (uint32_t r = 0; r < h / 2; r++)
        for (int p = 0; p < 2; p++)
            for (uint32_t c = 0; c < w / 2; c++)
                f[(size_t)(h + r) * w + p * (w / 2) + c] = chroma(c, r, p, k);
    return f;
}

/* A still w x h atlas. */
static std::vector<uint8_t> still(uint32_t w, uint32_t h, int seed)
{
    std::vector<uint8_t> f((size_t)w * crk::atlas_height(h));
    for (size_t i = 0; i < f.size(); i++)
        f[i] = (uint8_t)(i * 7 + (size_t)seed * 31);
    return f;
}

/* ---- the display as shown: what of a video can be seen ------------------- */

static bool unturned_quad(const FakeQuad &q) { return q.turn.rotate == 0 && q.turn.sx == 1 && q.turn.sy == 1; }

static float opacity_of(const FakeQuad &q)
{
    auto it = q.uniforms.find("u_opacity");
    return it == q.uniforms.end() ? 1.0f : it->second[0];
}

/* Target pixels drawn opaquely by the shown quads above order: videos'
 * and composites' quads (they sample an R8 atlas or packed blocks, u_size
 * set) over their rect at u_opacity 1, images' (RGBA8 buffer, no u_size)
 * where its texel is opaque, drawn at its size. */
static std::vector<uint8_t> covered(const FakeGraph *g, uint32_t tw, uint32_t th, int32_t order)
{
    std::vector<uint8_t> c((size_t)tw * th, 0);
    for (const auto &kv : g->shown.quads) {
        const FakeQuad &q = kv.second;
        if (!q.visible || q.order <= order || opacity_of(q) < 1.0f || !unturned_quad(q))
            continue;
        auto t = q.textures.find("u_image");
        if (t == q.textures.end() || !g->shown.buffers.count(t->second))
            continue;
        const FakeBuffer &b = g->shown.buffers.at(t->second);
        const bool image = b.f == TexFormat::RGBA8 && !q.uniforms.count("u_size");
        if (image && (b.w != q.r.w || b.h != q.r.h))
            continue;
        for (uint32_t y = 0; y < q.r.h; y++)
            for (uint32_t x = 0; x < q.r.w; x++) {
                const int32_t X = q.r.x + (int32_t)x, Y = q.r.y + (int32_t)y;
                if (X < 0 || Y < 0 || X >= (int32_t)tw || Y >= (int32_t)th)
                    continue;
                if (image && b.px[((size_t)y * b.w + x) * 4 + 3] != 255)
                    continue;
                c[(size_t)Y * tw + X] = 1;
            }
    }
    return c;
}

/* A video of vw x vh counting() frames as the display shows it: through
 * its own quad (u_size vw x vh) or a composite's (u_view vw x vh), the
 * lowest such quad in the draw order. Whether
 * its atlas quad shows; then *k the one frame every seen luma pixel and
 * chroma sample of it holds, or *k -1 when they hold different frames. */
struct Look {
    bool shown = false;
    int k = -1;
    int frames = 0;   /* how many different frames were seen (1: consistent) */
};

static Look look(const FakeGraph *g, uint32_t tw, uint32_t th, uint32_t vw, uint32_t vh)
{
    Look out;
    const FakeQuad *q = nullptr;
    int32_t ox = 0, oy = 0, qx = 0, qy = 0;
    for (const auto &kv : g->shown.quads) {
        const FakeQuad &c = kv.second;
        if (!c.visible || opacity_of(c) <= 0.0f)
            continue;
        auto t = c.textures.find("u_image");
        if (t == c.textures.end() || !g->shown.buffers.count(t->second) ||
            g->shown.buffers.at(t->second).f != TexFormat::R8)
            continue;
        auto view = c.uniforms.find("u_view");
        if (view != c.uniforms.end()) {
            if ((uint32_t)view->second[2] != vw || (uint32_t)view->second[3] != vh)
                continue;
            ox = (int32_t)view->second[0];
            oy = (int32_t)view->second[1];
        } else {
            const std::vector<float> &s = c.uniforms.at("u_size");
            if ((uint32_t)s[0] != vw || (uint32_t)s[1] != vh)
                continue;
            ox = oy = 0;
        }
        if (!q || c.order < q->order) {
            q = &c;
            qx = ox, qy = oy;
        }
    }
    if (!q)
        return out;
    ox = qx, oy = qy;
    out.shown = true;
    const FakeBuffer &a = g->shown.buffers.at(q->textures.at("u_image"));
    const uint32_t aw = a.w, ah = a.h * 2 / 3;   /* the atlas's frame (or canvas) size */
    const std::vector<uint8_t> cov = covered(g, tw, th, q->order);
    const Rect &r = q->r;
    /* Seen target pixels of the quad, in frame pixels: [sx0, sx1) of frame
     * pixel x across (scaled: a pixel more each way). */
    auto seen = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        const int64_t X0 = r.x + (int64_t)x0 * r.w / vw - (r.w != vw), X1 = r.x + ((int64_t)x1 * r.w + vw - 1) / vw + (r.w != vw);
        const int64_t Y0 = r.y + (int64_t)y0 * r.h / vh - (r.h != vh), Y1 = r.y + ((int64_t)y1 * r.h + vh - 1) / vh + (r.h != vh);
        for (int64_t Y = std::max<int64_t>(Y0, std::max<int64_t>(r.y, 0)); Y < std::min<int64_t>(Y1, std::min<int64_t>(r.y + r.h, th)); Y++)
            for (int64_t X = std::max<int64_t>(X0, std::max<int64_t>(r.x, 0)); X < std::min<int64_t>(X1, std::min<int64_t>(r.x + r.w, tw)); X++)
                if (!cov[(size_t)Y * tw + X])
                    return true;
        return false;
    };
    bool got[256] = {};
    auto saw = [&](int k) {
        k &= 255;
        if (!got[k]) {
            got[k] = true;
            out.frames++;
            out.k = k;
        }
    };
    for (uint32_t y = 0; y < vh; y++)
        for (uint32_t x = 0; x < vw; x++)
            if (seen((int32_t)x, (int32_t)y, (int32_t)x + 1, (int32_t)y + 1))
                saw(a.px[(size_t)(oy + y) * aw + ox + x] - luma(x, y, 0));
    for (uint32_t rr = 0; rr < vh / 2; rr++)
        for (uint32_t c = 0; c < vw / 2; c++)
            if (seen(2 * (int32_t)c - 2, 2 * (int32_t)rr - 1, 2 * (int32_t)c + 2, 2 * (int32_t)rr + 3))
                for (int p = 0; p < 2; p++)
                    saw(a.px[(size_t)(ah + oy / 2 + rr) * aw + p * (aw / 2) + ox / 2 + c] - chroma(c, rr, p, 0));
    if (out.frames != 1)
        out.k = -1;
    return out;
}

/* ---- the cells and the footprints ----------------------------------------- */

static void cells()
{
    Coverage c;
    c.reset(W, H);
    ok("coverage: empty covers nothing", c.empty() && !c.covers(Box{0, 0, 8, 8}));
    c.add(Box{13, 13, 77, 77});
    ok("coverage: only cells wholly inside a rect are covered",
       c.covers(Box{16, 16, 72, 72}) && !c.covers(Box{13, 13, 20, 20}) && !c.covers(Box{16, 16, 73, 72}));
    c.add(Box{72, 16, 200, 72});
    ok("coverage: two rects side by side cover across their seam", c.covers(Box{16, 16, 200, 72}));
    Coverage edge;
    edge.reset(W, H);
    edge.add(Box{0, 0, 64, 64});
    ok("coverage: pixels off the target do not count",
       edge.covers(Box{-8, -8, 64, 64}) && edge.covers(Box{-8, -8, 0, 0}) && !edge.covers(Box{-8, -8, 72, 64}));
    c.reset(W, H);
    ok("coverage: reset clears it", c.empty() && !c.covers(Box{16, 16, 24, 24}));
    c.add_if(Box{0, 0, 64, 64}, [](const Box &b) { return b.x0 >= 32; });
    ok("coverage: add_if takes the cells it is told", c.covers(Box{32, 0, 64, 64}) && !c.covers(Box{24, 0, 33, 8}));

    Box b[3];
    int n = tile_footprint(1, 1, W, H, R(0, 0, W, H), b);
    ok("footprint: a luma tile drawn at its size is its own pixels",
       n == 1 && b[0].x0 == 64 && b[0].y0 == 64 && b[0].x1 == 128 && b[0].y1 == 128);
    n = tile_footprint(1, 1, W, H, R(100, 50, W, H), b);
    ok("footprint: placed", n == 1 && b[0].x0 == 164 && b[0].y0 == 114 && b[0].x1 == 228 && b[0].y1 == 178);
    /* Row 7 (atlas rows 448-511): luma rows 448-479 and chroma rows 0-31;
     * column 8 (atlas columns 512-575) holds U 512-543 and V 0-31. */
    n = tile_footprint(8, 7, W, H, R(0, 0, W, H), b);
    ok("footprint: a tile over luma and both chroma planes has three parts",
       n == 3 && b[0].y0 == 448 && b[0].y1 == 480 && b[1].x0 == 1022 && b[1].x1 == 1088 && b[1].y0 == 0 &&
           b[1].y1 == 65 && b[2].x0 == 0 && b[2].x1 == 64);
    n = tile_footprint(3, 8, W, H, R(0, 0, W, H), b);
    ok("footprint: a chroma tile reaches the luma pixels its samples are read for",
       n == 1 && b[0].x0 == 382 && b[0].x1 == 512 && b[0].y0 == 63 && b[0].y1 == 193);
    n = tile_footprint(1, 1, W / 2, H / 2, R(0, 0, W, H), b);
    ok("footprint: scaled twice, wider for the filter",
       n == 1 && b[0].x0 <= 125 && b[0].x1 >= 259 && b[0].x0 >= 120 && b[0].x1 <= 264);
}

/* ---- tiles left unwritten --------------------------------------------------- */

static size_t hidden_count(const VideoLayer *v)
{
    const uint8_t *m = v->hidden_mask();
    if (!m)
        return 0;
    const size_t tiles = (size_t)((v->video().w + 63) / 64) * ((crk::atlas_height(v->video().h) + 63) / 64);
    return (size_t)std::count(m, m + tiles, 1);
}

static void skipped()
{
    Rig r;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, exact());
    VideoLayer *c = r.video(2, r.map, R(704, 64, 320, 240), 320, 240, 1, 1.0f, exact());
    m->offer(counting(W, H, 0).data(), r.now);
    c->offer(still(320, 240, 1).data(), r.now);
    r.show("map");
    r.run(100);
    char name[200];
    std::snprintf(name, sizeof name, "an opaque 320x240 camera at 704,64 hides 15 of the map's 204 tiles (%zu)",
                  hidden_count(m));
    ok(name, hidden_count(m) == 15);
    for (int k = 1; k <= 20; k++) {
        m->offer(counting(W, H, k).data(), r.now);
        r.run(34);
    }
    const Look l = look(r.g("map"), W, H, W, H);
    /* Under the camera the buffer keeps the frame of before. */
    const FakeBuffer *a = nullptr;
    for (const auto &kv : r.g("map")->shown.buffers)
        if (kv.second.f == TexFormat::R8 && kv.second.w == W)
            a = &kv.second;
    const int under = a ? (a->px[(size_t)160 * W + 800] - luma(800, 160, 0)) & 255 : -1;
    std::snprintf(name, sizeof name,
                  "tiles under it are not written: what shows is frame %d, under the camera frame %d", l.k, under);
    ok(name, l.frames == 1 && l.k == 20 && under == 0);
    ok("its status counts them", m->stats().find("tiles left hidden") != std::string::npos);
}

/* What hides the map's tiles and what does not. */
static void opaque_rules()
{
    struct Case {
        const char *name;
        float opacity, rotate, sx;
        bool hides;
    } cases[] = {
        {"an opaque camera hides the tiles under it", 1.0f, 0, 1, true},
        {"a camera at 0.9 hides nothing", 0.9f, 0, 1, false},
        {"a camera turned 1 degree hides nothing", 1.0f, 1, 1, false},
        {"a camera scaled by its turn hides nothing", 1.0f, 0, 1.05f, false},
    };
    for (const Case &k : cases) {
        Rig r;
        VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0);
        VideoLayer *c = r.video(2, r.map, R(704, 64, 320, 240), 320, 240, 1, k.opacity);
        c->params.rotate = k.rotate;
        c->params.sx = k.sx;
        r.comp->changed(c);
        m->offer(counting(W, H, 0).data(), r.now);
        c->offer(still(320, 240, 1).data(), r.now);
        r.show("map");
        r.run(100);
        ok(k.name, (hidden_count(m) > 0) == k.hides);
    }
    {
        Rig r;
        VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0);
        LayerParams lp;
        lp.rect = R(704, 64, 320, 240);
        lp.klass = Klass::Overlay;
        std::string err;
        r.comp->add_effect(2, r.map, lp, kQuadVertexShader,
                           "#version 100\nprecision mediump float;\nvoid main() { gl_FragColor = vec4(1.0); }", 0,
                           r.now, &err);
        m->offer(counting(W, H, 0).data(), r.now);
        r.show("map");
        r.run(100);
        ok("an effect layer hides nothing, whatever it draws", hidden_count(m) == 0);
    }
    {
        Rig r;
        VideoLayer *c = r.video(2, r.map, R(704, 64, 320, 240), 320, 240, 1);
        VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0);
        m->offer(counting(W, H, 0).data(), r.now);
        r.show("map");
        r.run(100);
        ok("a camera with no frame yet (drawn at opacity 0) hides nothing", hidden_count(m) == 0);
        c->offer(still(320, 240, 1).data(), r.now);
        r.run(100);
        ok("once its first frame is written it does", hidden_count(m) == 15);
    }
    /* Images: opaque where every pixel the filter reads has alpha 255, in
     * what the buffer holds and in what waits to be written. */
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255);
    Rig r;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0);
    ImageLayer *img = r.image(2, r.map, R(704, 64, 320, 240), 320, 240, 30);
    std::string err;
    img->update({}, px.data(), px.size(), &err);
    m->offer(counting(W, H, 0).data(), r.now);
    r.show("map");
    r.run(100);
    ok("an opaque image hides the tiles under it as a camera does", hidden_count(m) == 15);
    /* A transparent 8x8 block at image 100,50: target 804,114, map tile
     * (12, 1); the tile at (13, 2) stays hidden. */
    const size_t t12_1 = 1 * 17 + 12, t13_2 = 2 * 17 + 13;
    std::vector<uint8_t> hole((size_t)8 * 8 * 4, 0);
    img->update({crk::PixelRect{100, 50, 8, 8}}, hole.data(), hole.size(), &err);
    r.step(1);
    ok("pixels waiting to be written with alpha 0 count at once: their tile shows",
       m->hidden_mask() && !m->hidden_mask()[t12_1] && m->hidden_mask()[t13_2]);
    r.run(100);
    ok("and once written", m->hidden_mask() && !m->hidden_mask()[t12_1] && m->hidden_mask()[t13_2]);
    std::vector<uint8_t> solid((size_t)8 * 8 * 4, 255);
    img->update({crk::PixelRect{100, 50, 8, 8}}, solid.data(), solid.size(), &err);
    r.step(1);
    ok("opaque pixels waiting to be written over transparent ones do not count until written",
       m->hidden_mask() && !m->hidden_mask()[t12_1]);
    r.run(100);
    ok("once written they do", m->hidden_mask() && m->hidden_mask()[t12_1]);
    for (size_t i = 3; i < px.size(); i += 4)
        px[i] = 254;
    img->update({}, px.data(), px.size(), &err);
    r.run(100);
    ok("an image at alpha 254 hides nothing", hidden_count(m) == 0);
    for (size_t i = 3; i < px.size(); i += 4)
        px[i] = 255;
    img->update({}, px.data(), px.size(), &err);
    r.run(100);
    Animation a;
    a.property = Animated::Translate;
    a.width = 3;
    a.t = {0, 1000};
    a.v = {0, 0, 0, 100, 0, 0};
    img->animate(1, 0, a, &err);
    r.step(1);
    ok("an animated image hides nothing", hidden_count(m) == 0);
    img->stop(1);
    img->params.opacity = 0.99f;
    r.comp->changed(img);
    r.step(1);
    ok("an image at 0.99 hides nothing", hidden_count(m) == 0);
    img->params.opacity = 1.0f;
    img->params.rect = R(704, 64, 400, 300);
    r.comp->changed(img);
    r.step(1);
    ok("a scaled opaque image hides what it covers", hidden_count(m) > 15);
}

/* ---- never stale ----------------------------------------------------------- */

enum class Over { Camera, Image, Blocks, Fade };

/* The map (counting frames every 33 ms, exact) under a 320x240 camera or
 * image moved every 37 ms to places on and off the tile grid and partly off
 * the target, hidden and shown, made translucent and opaque again, turned
 * and back, the target faded out and in (Fade), with refused flushes and a
 * refused write, on a budget the writes outrun: after every flush, every
 * part of the map that shows holds one frame. */
static void never_stale(Over over)
{
    Rig r(over == Over::Blocks ? 3000 * 1024 : 1500 * 1024, 2, over == Over::Fade ? 300 : 0);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, exact(over == Over::Blocks));
    Layer *top = nullptr;
    VideoLayer *cam = nullptr;
    ImageLayer *img = nullptr;
    std::string err;
    if (over == Over::Image) {
        img = r.image(2, r.map, R(64, 64, 320, 240), 320, 240, 30);
        std::vector<uint8_t> px((size_t)320 * 240 * 4, 255);
        img->update({}, px.data(), px.size(), &err);
        top = img;
    } else {
        cam = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, 1, 1.0f, exact());
        cam->offer(still(320, 240, 3).data(), r.now);
        top = cam;
    }
    int k = 0;
    m->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.run(500);
    FakeGraph *g = r.g("map");
    const int xs[] = {64, 704, 768, 320, 900, 66, 512, -40};
    const int ys[] = {64, 0, 192, 300, 63};
    int flushes = 0, checked = 0, stale = 0, newest = -1;
    for (int t = 0; t < 7000; t++) {
        /* With packed blocks, a pause every 1.2 s lets the exact atlas back. */
        if (t % 33 == 0 && (over != Over::Blocks || t % 1000 < 500)) {
            m->offer(counting(W, H, ++k).data(), r.now);
            if (cam)
                cam->repeat(r.now);
        }
        if (t % 37 == 0 && t < 6000) {
            top->params.rect.x = xs[(t / 37) % 8];
            top->params.rect.y = ys[(t / 37) % 5];
            r.comp->changed(top);
        }
        if (t == 1500 || t == 2000) {
            top->params.visible = t == 2000;
            r.comp->changed(top);
        }
        if (t == 2500 || t == 2800) {
            top->params.opacity = t == 2500 ? 0.9f : 1.0f;
            r.comp->changed(top);
        }
        if (t == 3100 || t == 3400) {
            top->params.rotate = t == 3100 ? 2.0f : 0.0f;
            r.comp->changed(top);
        }
        if (over == Over::Image && t == 3600) {
            /* Transparent pixels in the middle of it. */
            std::vector<uint8_t> hole((size_t)64 * 64 * 4, 0);
            img->update({crk::PixelRect{128, 96, 64, 64}}, hole.data(), hole.size(), &err);
        }
        if (over == Over::Fade && (t == 4300 || t == 5000)) {
            m->params.visible = top->params.visible = t == 5000;
            r.comp->changed(m);
            r.comp->changed(top);
        }
        g->fail_flush = t >= 3700 && t < 3760;
        if (t == 4000)
            g->write_budget = (long)g->written + 30000;
        if (t == 4100)
            g->write_budget = -1;
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        flushes++;
        const Look l = look(g, W, H, W, H);
        if (!l.shown)
            continue;
        checked++;
        if (l.frames != 1)
            stale++;
        newest = std::max(newest, l.k);
    }
    const char *names[] = {"a camera", "an image", "a camera, the map with packed blocks", "a camera, faded out and in"};
    char name[300];
    std::snprintf(name, sizeof name,
                  "%s over the map moved every 37 ms, hidden, shown, translucent, turned, flushes and a write refused: "
                  "in %d flushes (%d checked) no part of the map shown from another frame (%d)",
                  names[(int)over], flushes, checked, stale);
    ok(name, stale == 0 && checked > 20);
    const std::string st = m->stats();
    if (over == Over::Blocks) {
        std::snprintf(name, sizeof name, "  and the map went packed and back to exact (frame %d of %d shown)", newest,
                      k & 255);
        ok(name, newest >= 0 && st.find(" 0 packed") == std::string::npos &&
                     st.find(" 0 back to exact") == std::string::npos && st.find("tiles left hidden") != std::string::npos);
        return;
    }
    std::snprintf(name, sizeof name, "  and the map kept up (frame %d of %d shown), hidden tiles used and uncovered",
                  newest, k & 255);
    ok(name, newest >= 0 && st.find("tiles left hidden") != std::string::npos &&
                 st.find(", 0 uncovered") == std::string::npos);
}

/* A composite (on a display holding one video's buffers) of a 768x480 map
 * and a camera beside it, under an opaque image moved about, hidden and
 * shown: the map's part of the canvas that shows always holds one frame.
 * (Beside, not over: where sources overlap, the lower one's chroma near the
 * edge is drawn from the upper one's part of the canvas, whatever is
 * hidden.) */
static void composite_occluded()
{
    const uint32_t MW = 768;
    Rig r(1500 * 1024, 1);
    VideoLayer *m = r.video(1, r.map, R(0, 0, MW, H), MW, H, 0, 1.0f, exact());
    VideoLayer *c = r.video(2, r.map, R(768, 64, 320, 240), 320, 240, 1, 1.0f, exact());
    ImageLayer *img = r.image(3, r.map, R(64, 64, 320, 240), 320, 240, 30);
    std::string err;
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255);
    img->update({}, px.data(), px.size(), &err);
    int k = 0;
    m->offer(counting(MW, H, k).data(), r.now);
    c->offer(still(320, 240, 3).data(), r.now);
    r.show("map");
    r.run(500);
    FakeGraph *g = r.g("map");
    const CompositeVideo *cv = r.comp->composite(r.map);
    const int xs[] = {64, 512, 768, 320, 0, 600};
    int checked = 0, stale = 0, hid = 0, newest = -1;
    for (int t = 0; t < 5000; t++) {
        if (t % 33 == 0) {
            m->offer(counting(MW, H, ++k).data(), r.now);
            c->repeat(r.now);
        }
        if (t % 41 == 0 && t < 4000) {
            img->params.rect.x = xs[(t / 41) % 6];
            img->params.rect.y = 64 + (t / 41) % 3 * 64;
            r.comp->changed(img);
        }
        if (t == 2000 || t == 2300) {
            img->params.visible = t == 2300;
            r.comp->changed(img);
        }
        g->fail_flush = t >= 2700 && t < 2760;
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (cv && cv->hidden_mask())
            hid++;
        if (g->flushes == f0)
            continue;
        const Look l = look(g, W, H, MW, H);
        if (!l.shown)
            continue;
        checked++;
        stale += l.frames != 1;
        newest = std::max(newest, l.k);
    }
    char name[260];
    std::snprintf(name, sizeof name,
                  "a composite under an opaque image moved about: canvas tiles hidden in %d ticks, in %d checked "
                  "flushes no part of the map from another frame (%d)",
                  hid, checked, stale);
    ok(name, cv && hid > 1000 && checked > 20 && stale == 0 && newest >= 0);
}

/* A scaled map (544x240 frames over 1088x480: buffers of its own) under two
 * cameras its target composites: the composite's quads hide the map's
 * tiles, and a camera moved (its quads moving only with the canvas write)
 * never shows the map from another frame where it was. */
static void composite_occluding()
{
    Rig r(1500 * 1024);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W / 2, H / 2, 0, 1.0f, exact());
    VideoLayer *a = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, 1, 1.0f, exact());
    VideoLayer *b = r.video(3, r.map, R(704, 160, 320, 240), 320, 240, 2, 1.0f, exact());
    int k = 0;
    m->offer(counting(W / 2, H / 2, k).data(), r.now);
    a->offer(still(320, 240, 3).data(), r.now);
    b->offer(still(320, 240, 4).data(), r.now);
    r.show("map");
    r.run(500);
    FakeGraph *g = r.g("map");
    const int xs[] = {64, 384, 704, 128, 512};
    int checked = 0, stale = 0, hid = 0;
    for (int t = 0; t < 5000; t++) {
        if (t % 33 == 0) {
            m->offer(counting(W / 2, H / 2, ++k).data(), r.now);
            a->repeat(r.now);
            b->repeat(r.now);
        }
        if (t % 53 == 0 && t < 4000) {
            a->params.rect.x = xs[(t / 53) % 5];
            r.comp->changed(a);
        }
        if (t == 2000 || t == 2400) {
            b->params.visible = t == 2400;
            r.comp->changed(b);
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        hid += m->hidden_mask() != nullptr;
        if (g->flushes == f0)
            continue;
        const Look l = look(g, W, H, W / 2, H / 2);
        if (!l.shown)
            continue;
        checked++;
        stale += l.frames != 1;
    }
    char name[260];
    std::snprintf(name, sizeof name,
                  "a scaled map under two composited cameras, one moved: the map's tiles hidden in %d ticks, in %d "
                  "checked flushes none of it shown from another frame (%d)",
                  hid, checked, stale);
    ok(name, r.comp->composite(r.map) && hid > 1000 && checked > 50 && stale == 0);
}

/* ---- an uncover waiting for the budget ------------------------------------ */

static const char *const kClockFs =
    "#version 100\nprecision mediump float;\nuniform float u_time;\n"
    "void main() { gl_FragColor = vec4(fract(u_time), 0.0, 0.0, 1.0); }";

/* The map panning past the 5500 KB/s budget on map, and on the panel a
 * video (counting frames) under an opaque image moved or hidden every 300
 * ms, beside an effect whose u_time the service sets 30 times a second
 * (with resend, both scenes charged whole by resent() at 1.7 s, the map
 * paused before so that the panel video writes under the image). The
 * panel flushes as
 * often as its effect changes, the image's change shows once the panel
 * video's uncovered tiles are written, within the time the budget takes to
 * repay what it owes, and no panel flush shows part of the video from
 * another frame. */
static void uncover_waits(bool resend)
{
    Rig r(5500 * 1024);
    std::string err;
    LayerParams mp;
    mp.rect = R(0, 0, W, H);
    mp.contend = true;
    VideoParams mvp;
    VideoLayer *m = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, r.map, mp, mvp, r.now, &err)));
    VideoLayer *p = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *img = r.image(3, r.panel, R(800, 200, 320, 240), 320, 240, 30);
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255);
    img->update({}, px.data(), px.size(), &err);
    LayerParams ep;
    ep.rect = R(1600, 40, 200, 60);
    ep.klass = Klass::Overlay;
    r.comp->add_effect(4, r.panel, ep, kQuadVertexShader, kClockFs, 30, r.now, &err);
    std::vector<uint8_t> f;
    int k = 0, pan = 0;
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    p->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    int effect_quad = 0, image_quad = 0;
    for (const auto &kv : g->shown.quads) {
        if (kv.second.uniforms.count("u_time"))
            effect_quad = kv.first;
        auto t = kv.second.textures.find("u_image");
        if (t != kv.second.textures.end() && g->shown.buffers.count(t->second) &&
            g->shown.buffers.at(t->second).f == TexFormat::RGBA8 && !kv.second.uniforms.count("u_size"))
            image_quad = kv.first;
    }
    const int xs[] = {800, 512, 1200, 700, 960};
    int64_t last_time_change = r.now, longest_gap = 0, longest_wait = 0, waits = 0, landed = 0;
    float shown_time = -1;
    int checked = 0, stale = 0;
    int64_t asked = -1;
    Rect want = img->params.rect;
    bool want_visible = true;
    for (int t = 0; t < 6000; t++) {
        if (t % 33 == 0) {
            /* With resend, the map pauses just before the charge, so the
             * panel video writes with tiles under the image. */
            if (!resend || t < 1500 || t >= 1700) {
                pan += 24;
                trace_frame(f, W, H, pan, 0, 0);
                m->offer(f.data(), r.now);
            }
            p->offer(counting(W, H, ++k).data(), r.now);
        }
        if (resend && t == 1700) {
            r.comp->resent(r.slots.targets()[r.map], r.now);
            r.comp->resent(r.slots.targets()[r.panel], r.now);
        }
        if (t % 300 == 150 && t < 5400) {
            if ((t / 300) % 4 == 3) {
                img->params.visible = !img->params.visible;
            } else {
                img->params.visible = true;
                img->params.rect.x = xs[(t / 300) % 5];
            }
            r.comp->changed(img);
            want = img->params.rect;
            want_visible = img->params.visible;
            asked = r.now;
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        const FakeQuad &e = g->shown.quads.at(effect_quad);
        if (e.uniforms.count("u_time") && e.uniforms.at("u_time")[0] != shown_time) {
            shown_time = e.uniforms.at("u_time")[0];
            longest_gap = std::max(longest_gap, r.now - last_time_change);
            last_time_change = r.now;
        }
        const FakeQuad &iq = g->shown.quads.at(image_quad);
        if (asked >= 0 && iq.visible == want_visible && (!want_visible || iq.r == want)) {
            longest_wait = std::max(longest_wait, r.now - asked);
            waits += r.now - asked > 1;
            landed++;
            asked = -1;
        }
        const Look l = look(g, 1920, 720, W, H);
        if (!l.shown)
            continue;
        checked++;
        stale += l.frames != 1;
    }
    longest_gap = std::max(longest_gap, r.now - last_time_change);
    char name[400];
    std::snprintf(name, sizeof name,
                  "the map panning past the budget%s: the panel's effect flushed at most %lld ms apart",
                  resend ? ", after both scenes were charged whole (resent)" : "", (long long)longest_gap);
    ok(name, longest_gap <= 40);
    /* What the budget owes at most: a resent scene's buffers, or a packed
     * map frame and a panel frame written in one tick, repaid at 5500 KB/s. */
    const double owed = resend ? 2.0 * (783360 + 2 * 261120) + 783360 + 307200 : 783360.0 + 261120 + 783360;
    const double bound_ms = owed / (5500.0 * 1024) * 1000 + 34;
    std::snprintf(name, sizeof name,
                  "  the image's moves and hides over the panel video shown once its uncovered tiles were written: "
                  "%lld of %d waited, the longest %lld ms (what the budget owes, repaid: %.0f ms)",
                  (long long)waits, (int)landed, (long long)longest_wait, bound_ms);
    ok(name, landed >= 15 && waits > 0 && longest_wait <= bound_ms && longest_wait >= (resend ? 100 : 20));
    std::snprintf(name, sizeof name, "  in %d checked panel flushes no part of the video from another frame (%d)",
                  checked, stale);
    ok(name, checked > 50 && stale == 0);
}

/* A hidden tile changed within the threshold, then the scene taken down and
 * published again: the new scene's first write is the newest frame, that
 * tile included. */
static void quiet_hidden_reattach()
{
    Rig r;
    VideoParams vp = exact();
    vp.dirty_threshold = 8;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, vp);
    VideoLayer *c = r.video(2, r.map, R(704, 64, 320, 240), 320, 240, 1, 1.0f, exact());
    std::vector<uint8_t> a = counting(W, H, 0), b = a;
    for (uint32_t y = 128; y < 192; y++)
        for (uint32_t x = 768; x < 832; x++)
            b[(size_t)y * W + x] += 4;
    m->offer(a.data(), r.now);
    c->offer(still(320, 240, 1).data(), r.now);
    r.show("map");
    r.run(200);
    m->offer(b.data(), r.now);
    r.run(200);
    const TargetSpec &spec = r.slots.targets()[r.map];
    r.comp->unpublish(spec);
    std::string err;
    r.comp->publish(spec, &err);
    r.run(300);
    const FakeBuffer *atlas = nullptr;
    for (const auto &kv : r.g("map")->shown.buffers)
        if (kv.second.f == TexFormat::R8 && kv.second.w == W)
            atlas = &kv.second;
    bool newest = atlas != nullptr;
    for (uint32_t y = 128; y < 192 && newest; y++)
        newest = !std::memcmp(&atlas->px[(size_t)y * W + 768], &b[(size_t)y * W + 768], 64);
    ok("a hidden tile changed within the threshold: a scene published again starts from the newest frame there",
       m->hidden_mask() && m->hidden_mask()[2 * 17 + 12] && newest);
}

/* ---- large cameras ---------------------------------------------------------- */

/* Which atlas tiles of a vw x vh video drawn unturned at its size over vr,
 * on a tw x th target, change only pixels inside cover: each pixel of a
 * tile, luma or chroma, by the target pixels it reaches (a chroma sample
 * the luma pixels 2c-2 to 2c+1 across and 2r-1 to 2r+2 down), pixels off
 * the video's rect or the target not counted. */
static std::vector<uint8_t> hidden_oracle(uint32_t vw, uint32_t vh, Rect vr, Box cover, uint32_t tw, uint32_t th)
{
    const uint32_t ah = crk::atlas_height(vh), cols = (vw + 63) / 64, rows = (ah + 63) / 64;
    std::vector<uint8_t> out((size_t)cols * rows, 1);
    const Box clip = intersect(box_of(vr), Box{0, 0, (int32_t)tw, (int32_t)th});
    for (uint32_t y = 0; y < ah; y++)
        for (uint32_t x = 0; x < vw; x++) {
            uint8_t &o = out[(size_t)(y / 64) * cols + x / 64];
            if (!o)
                continue;
            Box b;
            if (y < vh) {
                b = Box{vr.x + (int32_t)x, vr.y + (int32_t)y, vr.x + (int32_t)x + 1, vr.y + (int32_t)y + 1};
            } else {
                const int32_t c = (int32_t)(x < vw / 2 ? x : x - vw / 2), rr = (int32_t)(y - vh);
                b = Box{vr.x + 2 * c - 2, vr.y + 2 * rr - 1, vr.x + 2 * c + 2, vr.y + 2 * rr + 3};
            }
            b = intersect(b, clip);
            if (!b.empty() && (b.x0 < cover.x0 || b.y0 < cover.y0 || b.x1 > cover.x1 || b.y1 > cover.y1))
                o = 0;
        }
    return out;
}

static bool chroma_only_hidden(const std::vector<uint8_t> &m, uint32_t vh)
{
    const uint32_t cols = (uint32_t)(m.size() / ((crk::atlas_height(vh) + 63) / 64));
    for (size_t i = 0; i < m.size(); i++)
        if (m[i] && (i / cols) * 64 >= vh)
            return true;
    return false;
}

/* An opaque camera as large as the map over it: every map tile hidden,
 * luma and chroma, those at the grid's right and bottom edge included; the
 * map writes nothing while covered and the camera has the budget; moved
 * aside, back and removed, no flush shows any part of the map from another
 * frame than the rest. */
static void full_slot()
{
    const uint64_t budget = 5500 * 1024;
    Rig r(budget);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, exact());
    VideoLayer *c = r.video(2, r.map, R(0, 0, W, H), W, H, 1, 1.0f, exact());
    int k = 0;
    m->offer(counting(W, H, k).data(), r.now);
    c->offer(still(W, H, 0).data(), r.now);
    r.show("map");
    FakeGraph *g = r.g("map");
    int stale = 0, checked = 0, newest = -1;
    auto run = [&](int ms) {
        for (int t = 0; t < ms; t++) {
            if (t % 33 == 0) {
                m->offer(counting(W, H, ++k).data(), r.now);
                c->offer(still(W, H, k).data(), r.now);
            }
            const uint64_t f0 = g->flushes;
            r.step(1);
            if (g->flushes == f0)
                continue;
            const Look l = look(g, W, H, W, H);
            if (!l.shown || !l.frames)
                continue;
            checked++;
            stale += l.frames > 1;
            newest = l.k;
        }
    };
    run(1000);
    char name[300];
    std::snprintf(name, sizeof name, "a camera as large as the map hides all 204 of its tiles, luma and chroma (%zu)",
                  hidden_count(m));
    ok(name, hidden_count(m) == 204);
    const WriteStats m0 = m->write_stats(), c0 = c->write_stats();
    run(3000);
    const WriteStats m1 = m->write_stats(), c1 = c->write_stats();
    std::snprintf(name, sizeof name,
                  "  covered for 3 s, the map writes %llu bytes and the camera %.0f KB/s of the %llu KB/s budget",
                  (unsigned long long)(m1.bytes - m0.bytes), (c1.bytes - c0.bytes) / 1024.0 / 3,
                  (unsigned long long)(budget / 1024));
    ok(name, m1.bytes == m0.bytes && c1.bytes - c0.bytes >= budget * 3 * 85 / 100);
    c->params.rect.x = 288;
    r.comp->changed(c);
    run(1500);
    c->params.rect.x = 0;
    r.comp->changed(c);
    run(1500);
    r.comp->remove(2, c->id());
    run(1500);
    std::snprintf(name, sizeof name,
                  "  moved aside, back and removed: in %d flushes showing the map, none of it from another frame (%d); "
                  "it shows frame %d of %d",
                  checked, stale, newest, k & 255);
    ok(name, stale == 0 && checked > 10 && newest >= 0 && ((k & 255) - newest + 256) % 256 <= 3);
}

/* An 800x480 opaque camera at 0,0 over the map: exactly the tiles every
 * pixel of which it covers on screen are hidden, chroma tiles included,
 * and those keep an older frame. */
static void camera_800()
{
    Rig r;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, exact());
    VideoLayer *c = r.video(2, r.map, R(0, 0, 800, 480), 800, 480, 1, 1.0f, exact());
    m->offer(counting(W, H, 0).data(), r.now);
    c->offer(still(800, 480, 0).data(), r.now);
    r.show("map");
    r.run(100);
    const std::vector<uint8_t> want = hidden_oracle(W, H, R(0, 0, W, H), Box{0, 0, 800, 480}, W, H);
    const uint8_t *got = m->hidden_mask();
    const size_t n = (size_t)std::count(want.begin(), want.end(), 1);
    char name[200];
    std::snprintf(name, sizeof name,
                  "an 800x480 camera at 0,0: the %zu map tiles all of whose pixels it covers are hidden, chroma tiles "
                  "among them, and no other",
                  n);
    ok(name, got && std::equal(want.begin(), want.end(), got) && chroma_only_hidden(want, H));
    for (int k = 1; k <= 20; k++) {
        m->offer(counting(W, H, k).data(), r.now);
        r.run(34);
    }
    const FakeBuffer *a = nullptr;
    for (const auto &kv : r.g("map")->shown.buffers)
        if (kv.second.f == TexFormat::R8 && kv.second.w == W)
            a = &kv.second;
    /* Atlas row 600 (V row 120), column 600 (V 56): a hidden chroma tile;
     * luma 1000,100: shown. */
    const bool kept = a && a->px[(size_t)600 * W + 600] == chroma(56, 120, 1, 0);
    const bool fresh = a && a->px[(size_t)100 * W + 1000] == luma(1000, 100, 20);
    ok("  a hidden chroma tile keeps the older frame while the shown part is the newest", kept && fresh);
}

/* ---- held changes --------------------------------------------------------- */

static const FakeQuad *quad_of(const FakeGraph *g, TexFormat f, uint32_t w)
{
    for (const auto &kv : g->shown.quads) {
        auto t = kv.second.textures.find("u_image");
        if (t != kv.second.textures.end() && g->shown.buffers.count(t->second) &&
            g->shown.buffers.at(t->second).f == f && g->shown.buffers.at(t->second).w == w &&
            (f == TexFormat::R8 || !kv.second.uniforms.count("u_size")))
            return &kv.second;
    }
    return nullptr;
}

/* The map panning past the budget, and on the panel a video under an opaque
 * image, with an exclusive alert elsewhere on the panel shown and dismissed
 * again and again: while the alert shows, the image is hidden, so its
 * dismissal resumes the video with tiles to uncover. No flush shows neither
 * the alert nor the video; some dismissals wait; nothing stale. The alert
 * is made before the video (alert_first) or after it. */
static void exclusive_dismissal(bool alert_first, bool removed = false)
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H);
    ImageLayer *e = nullptr;
    auto make_alert = [&]() {
        LayerParams lp;
        lp.rect = R(100, 100, 300, 200);
        lp.klass = Klass::Alert;
        lp.exclusive = true;
        lp.visible = false;
        lp.contend = true;
        e = static_cast<ImageLayer *>(
            r.comp->layer(4, r.comp->add_image(4, r.panel, lp, 300, 200, 30, Sampling::Nearest, r.now, &err)));
        std::vector<uint8_t> px((size_t)300 * 200 * 4, 255);
        e->update({}, px.data(), px.size(), &err);
    };
    if (alert_first)
        make_alert();
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *c = r.image(3, r.panel, R(800, 200, 320, 240), 320, 240, 30);
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255);
    c->update({}, px.data(), px.size(), &err);
    if (!alert_first)
        make_alert();
    std::vector<uint8_t> f;
    int k = 0, pan = 0;
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    int holes = 0, waited = 0, dismissed = 0, checked = 0, stale = 0;
    int64_t asked = -1;
    for (int t = 0; t < 14000; t++) {
        if (t % 33 == 0) {
            pan += 24;
            trace_frame(f, W, H, pan, 0, 0);
            m->offer(f.data(), r.now);
            v->offer(counting(W, H, ++k).data(), r.now);
        }
        const int p = t % 700;
        if (removed && p == 400) {
            r.comp->remove(4, e->id());
            e = nullptr;
            asked = r.now;
        } else if (p == 100 || p == 400) {
            if (!e)
                make_alert();
            e->params.visible = p == 100;
            r.comp->changed(e);
            if (p == 400)
                asked = r.now;
        }
        if (p == 250 || p == 600) {
            c->params.visible = p == 600;
            r.comp->changed(c);
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        const FakeQuad *eq = quad_of(g, TexFormat::RGBA8, 300), *vq = quad_of(g, TexFormat::R8, W);
        const bool e_shown = eq && eq->visible, v_shown = vq && vq->visible && opacity_of(*vq) > 0;
        if (asked >= 0 || (p > 400 && p < 700)) {
            holes += !e_shown && !v_shown;
            if (asked >= 0 && !e_shown) {
                dismissed++;
                waited += r.now - asked > 1;
                asked = -1;
            }
        }
        const Look l = look(g, 1920, 720, W, H);
        if (l.shown && l.frames) {
            checked++;
            stale += l.frames > 1;
        }
    }
    char name[300];
    std::snprintf(name, sizeof name,
                  "an exclusive alert %s during a pan, made %s the video: %d dismissals (%d waited for the "
                  "video's uncovered tiles), no flush showing neither (%d), none stale in %d (%d)",
                  removed ? "removed" : "dismissed", alert_first ? "before" : "after", dismissed, waited, holes,
                  checked, stale);
    ok(name, dismissed >= 15 && (removed || waited >= 1) && holes == 0 && stale == 0 && checked > 30);
}

/* An exclusive alert removed while, above it, an image beside a video
 * hides and an opaque one over the video moves onto where the hidden one
 * was, the budget in debt: the move cannot wait without the hiding, which
 * goes with the removal, so all go and the video writes what they
 * uncover. */
static void alert_removed_with_moves()
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H);
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    v->params.klass = Klass::Alert;
    v->params.z = 4;
    r.comp->changed(v);
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255);
    auto above = [&](uint64_t owner, Rect at, int z) {
        ImageLayer *i = r.image(owner, r.panel, at, 320, 240, 30);
        i->update({}, px.data(), px.size(), &err);
        i->params.klass = Klass::Alert;
        i->params.z = z;
        r.comp->changed(i);
        return i;
    };
    ImageLayer *x = above(3, R(800, 200, 320, 240), 5);
    ImageLayer *d = above(5, R(1100, 610, 320, 100), 6);
    ImageLayer *a = nullptr;
    auto make_alert = [&]() {
        LayerParams lp;
        lp.rect = R(100, 100, 300, 200);
        lp.klass = Klass::Alert;
        lp.exclusive = true;
        lp.contend = true;
        a = static_cast<ImageLayer *>(
            r.comp->layer(4, r.comp->add_image(4, r.panel, lp, 300, 200, 30, Sampling::Nearest, r.now, &err)));
        std::vector<uint8_t> ap((size_t)300 * 200 * 4, 255);
        a->update({}, ap.data(), ap.size(), &err);
    };
    std::vector<uint8_t> f;
    int k = 0, pan = 0;
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    int checked = 0, stale = 0, removals = 0;
    for (int t = 0; t < 14000; t++) {
        if (t % 33 == 0) {
            pan += 24;
            trace_frame(f, W, H, pan, 0, 0);
            m->offer(f.data(), r.now);
            v->offer(counting(W, H, ++k).data(), r.now);
        }
        const int p = t % 700;
        if (p == 100)
            make_alert();
        if (p == 400) {
            r.comp->remove(4, a->id());
            a = nullptr;
            removals++;
            d->params.visible = false;
            r.comp->changed(d);
            x->params.rect.x = 1100;
            x->params.rect.y = 400;
            r.comp->changed(x);
        }
        if (p == 600) {
            d->params.visible = true;
            r.comp->changed(d);
            x->params.rect.x = 800;
            x->params.rect.y = 200;
            r.comp->changed(x);
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        const Look l = look(g, 1920, 720, W, H);
        if (l.shown && l.frames) {
            checked++;
            stale += l.frames > 1;
        }
    }
    char name[300];
    std::snprintf(name, sizeof name,
                  "an exclusive alert removed while above it an image hides and another moves onto it: %d "
                  "removals, none stale in %d (%d)",
                  removals, checked, stale);
    ok(name, removals == 20 && stale == 0 && checked > 30);
}

/* An exclusive alert added on the panel again and again while the map pans
 * past the budget: its first write goes with the next allowance, and until
 * then the video below stays drawn, so no flush shows neither. */
static void alert_behind_busy_map()
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H);
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *a = nullptr;
    std::vector<uint8_t> f, ap((size_t)300 * 200 * 4, 255);
    int k = 0, pan = 0;
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    int holes = 0, shows = 0;
    int64_t added = -1, longest = 0;
    for (int t = 0; t < 14000; t++) {
        if (t % 33 == 0) {
            pan += 24;
            trace_frame(f, W, H, pan, 0, 0);
            m->offer(f.data(), r.now);
            v->offer(counting(W, H, ++k).data(), r.now);
        }
        const int p = t % 700;
        if (p == 100) {
            LayerParams lp;
            lp.rect = R(100, 100, 300, 200);
            lp.klass = Klass::Alert;
            lp.exclusive = true;
            lp.contend = true;
            a = static_cast<ImageLayer *>(
                r.comp->layer(4, r.comp->add_image(4, r.panel, lp, 300, 200, 30, Sampling::Nearest, r.now, &err)));
            a->update({}, ap.data(), ap.size(), &err);
            added = r.now;
        }
        if (p == 600) {
            r.comp->remove(4, a->id());
            a = nullptr;
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        const FakeQuad *aq = quad_of(g, TexFormat::RGBA8, 300), *vq = quad_of(g, TexFormat::R8, W);
        bool a_shown = false;
        if (aq && aq->visible) {
            const FakeBuffer &b = g->shown.buffers.at(aq->textures.at("u_image"));
            a_shown = std::any_of(b.px.begin(), b.px.end(), [](uint8_t x) { return x != 0; });
        }
        const bool v_shown = vq && vq->visible && opacity_of(*vq) > 0;
        holes += !a_shown && !v_shown;
        if (added >= 0 && a_shown) {
            longest = std::max(longest, r.now - added);
            shows++;
            added = -1;
        }
    }
    char name[300];
    std::snprintf(name, sizeof name,
                  "an exclusive alert added during a pan past the budget, 20 times: shown %d times, the longest %lld ms "
                  "after it was added; no flush showing neither (%d)",
                  shows, (long long)longest, holes);
    ok(name, shows == 20 && longest <= 300 && holes == 0);
}

/* A video (no packed blocks) under an opaque 1024x448 image a client
 * animates and stops every tick for 10 s: no write goes while the budget
 * is in debt (each tick's writes start from a positive allowance), and
 * every window of 10 ms to 1 s stays within the rate, the burst and one
 * frame. */
static void animate_toggle()
{
    const uint64_t budget = 5500 * 1024, burst = budget / 4, frame = (uint64_t)W * crk::atlas_height(H);
    Rig r(budget);
    std::string err;
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *img = r.image(3, r.panel, R(448, 136, 1024, 448), 1024, 448, 30);
    std::vector<uint8_t> px((size_t)1024 * 448 * 4, 255);
    img->update({}, px.data(), px.size(), &err);
    int k = 0;
    v->offer(counting(W, H, k).data(), r.now);
    r.show("panel");
    r.run(1000);
    Animation a;
    a.property = Animated::Translate;
    a.width = 3;
    a.t = {0, 1000};
    a.v = {0, 0, 0, 4, 0, 0};
    FakeGraph *g = r.g("panel");
    std::vector<uint64_t> per_tick;
    uint64_t last = g->flushed_bytes;
    int past = 0;
    for (int t = 0; t < 10000; t++) {
        if (t % 33 == 0)
            v->offer(counting(W, H, ++k).data(), r.now);
        if (t % 2 == 0)
            img->animate(1, 0, a, &err);
        else
            img->stop(1);
        r.step(1);
        const uint64_t now = g->flushed_bytes;
        per_tick.push_back(now - last);
        /* A write the budget allowed started from a positive allowance: what
         * is left after it is above minus its bytes. */
        past += now > last && r.comp->pacer().allowance() <= -(int64_t)(now - last);
        last = now;
    }
    bool within = true;
    uint64_t worst = 0;
    int worst_w = 0;
    for (int w : {10, 20, 50, 100, 200, 500, 1000}) {
        const uint64_t bound = budget * (uint64_t)w / 1000 + burst + frame;
        uint64_t sum = 0;
        for (size_t i = 0; i < per_tick.size(); i++) {
            sum += per_tick[i];
            if (i >= (size_t)w)
                sum -= per_tick[i - w];
            if (sum * 1000 / bound > worst) {
                worst = sum * 1000 / bound;
                worst_w = w;
            }
            within = within && sum <= bound;
        }
    }
    char name[300];
    std::snprintf(name, sizeof name,
                  "an image over a video animated and stopped every tick for 10 s: no write while the budget is in "
                  "debt (%d), %llu frames written",
                  past, (unsigned long long)v->write_stats().frames);
    ok(name, past == 0 && v->write_stats().frames > 100);
    std::snprintf(name, sizeof name,
                  "  every window of 10 ms to 1 s within the rate, the burst and one frame (the fullest %.1f %% of "
                  "it, over %d ms)",
                  worst / 10.0, worst_w);
    ok(name, within);
}

/* An opaque 512x256 image over a video removed and made again every 400 ms
 * for 10 s: a removal cannot wait, so the video's uncovered tiles go past the
 * budget, but never twice without a write the budget allowed between. */
static void removal_forced()
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    std::vector<uint8_t> px((size_t)512 * 256 * 4, 255);
    ImageLayer *img = nullptr;
    int k = 0;
    v->offer(counting(W, H, k).data(), r.now);
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    uint64_t last = g->flushed_bytes;
    int past = 0, twice = 0;
    bool paced_since = true;
    for (int t = 0; t < 10000; t++) {
        if (t % 33 == 0)
            v->offer(counting(W, H, ++k).data(), r.now);
        if (t % 400 == 0) {
            if (img) {
                r.comp->remove(3, img->id());
                img = nullptr;
            } else {
                img = r.image(3, r.panel, R(640, 200, 512, 256), 512, 256, 30);
                img->update({}, px.data(), px.size(), &err);
            }
        }
        r.step(1);
        const uint64_t now = g->flushed_bytes;
        if (now > last) {
            const bool debt = r.comp->pacer().allowance() <= -(int64_t)(now - last);
            past += debt;
            twice += debt && !paced_since;
            paced_since = !debt;
        }
        last = now;
    }
    const std::string st = v->stats();
    const size_t f = st.find(" forced)");
    const size_t b0 = f == std::string::npos ? std::string::npos : st.rfind('(', f);
    const int forced = b0 == std::string::npos ? -1 : std::atoi(st.c_str() + b0 + 1);
    char name[300];
    std::snprintf(name, sizeof name,
                  "an opaque image over a video removed and made again every 400 ms: %d writes went past the budget "
                  "for a removal, never two without a write the budget allowed between (%d)",
                  forced, twice);
    ok(name, forced >= 1 && past == forced && twice == 0);
}

/* The number before name in a status line; 0 when it has none. */
static unsigned long long count_of(const std::string &st, const char *name)
{
    const size_t p = st.find(name);
    if (p == std::string::npos || p == 0)
        return 0;
    size_t b = p;
    while (b > 0 && st[b - 1] >= '0' && st[b - 1] <= '9')
        b--;
    return std::strtoull(st.c_str() + b, nullptr, 10);
}

/* ---- refused composites, fades, fair uncovers, forced writes ---------------- */

/* A scaled map (buffers of its own) under two cameras its target composes,
 * the composite's objects refused by the renderer: the cameras show an
 * error, nothing is hidden by them, and the map is written and shown. */
static void refused_composite()
{
    Rig r;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W / 2, H / 2, 0, 1.0f, exact());
    m->offer(counting(W / 2, H / 2, 0).data(), r.now);
    r.show("map");
    r.run(100);
    r.g("map")->unknown_uniform = "u_size";   /* new video quads refused: the composite's */
    VideoLayer *a = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, 1, 1.0f, exact());
    VideoLayer *b = r.video(3, r.map, R(704, 160, 320, 240), 320, 240, 2, 1.0f, exact());
    a->offer(still(320, 240, 1).data(), r.now);
    b->offer(still(320, 240, 2).data(), r.now);
    int k = 0;
    for (int t = 0; t < 2000; t++) {
        if (t % 33 == 0) {
            m->offer(counting(W / 2, H / 2, ++k).data(), r.now);
            a->repeat(r.now);
            b->repeat(r.now);
        }
        r.step(1);
    }
    std::string why;
    const CompositeVideo *cv = r.comp->composite(r.map);
    const Look l = look(r.g("map"), W, H, W / 2, H / 2);
    ok("a composite the renderer refuses, two cameras over a scaled map: 2 s of ticks, the cameras in error, the "
       "map hiding nothing and showing its newest frame",
       cv && !cv->attached() && r.comp->state(a->id(), &why) == LayerState::Error && !m->hidden_mask() &&
           l.shown && l.frames == 1 && ((k & 255) - l.k + 256) % 256 <= 2);
}

/* The map panning past the budget, and on the panel a video under an opaque
 * image; again and again the image is set to 0.5 while the target starts to
 * fade out: no flush shows part of the video from another frame. */
static void held_opacity_fade()
{
    Rig r(5500 * 1024, 2, 400);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H);
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *img = r.image(3, r.panel, R(800, 200, 320, 240), 320, 240, 30);
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255), f;
    img->update({}, px.data(), px.size(), &err);
    int k = 0, pan = 0;
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1500);
    FakeGraph *g = r.g("panel");
    int checked = 0, stale = 0, fades = 0;
    for (int t = 0; t < 15000; t++) {
        if (t % 33 == 0) {
            pan += 24;
            trace_frame(f, W, H, pan, 0, 0);
            m->offer(f.data(), r.now);
            v->offer(counting(W, H, ++k).data(), r.now);
        }
        const int p = t % 1500;
        if (p == 0 || p == 900) {
            v->params.visible = img->params.visible = p == 0;
            img->params.opacity = p == 0 ? 1.0f : 0.5f;
            r.comp->changed(img);
            r.comp->changed(v);
            fades += p == 900;
        }
        if (!g)
            g = r.g("panel");
        const uint64_t f0 = g ? g->flushes : 0;
        r.step(1);
        if (!g || g->flushes == f0)
            continue;
        const Look l = look(g, 1920, 720, W, H);
        if (l.shown && l.frames) {
            checked++;
            stale += l.frames > 1;
        }
    }
    char name[300];
    std::snprintf(name, sizeof name,
                  "an opaque image over a video set to 0.5 as its target starts to fade out, %d times during a pan: "
                  "in %d flushes no part of the video from another frame (%d)",
                  fades, checked, stale);
    ok(name, fades == 10 && checked > 100 && stale == 0);
}

/* An opaque camera picture over the map while the map pans past the budget, sliding 4 px a
 * frame (camera_minute_trace kind 2) or jumping between two places every
 * tick, the latter also with the panel busy (an effect it flushes every
 * tick while its uncover waits, its hide made with the budget in debt), so
 * the map has tiles to uncover again and again, and on the panel an image over a video. In each 2.5 s cycle
 * the map pauses while the panel video writes a frame under the image,
 * then pans with the camera sliding, and the image is hidden: the panel
 * shows it gone once the budget has repaid what it owed then and written
 * at most one map frame. */
enum class Slide { Slide, Jump, JumpBusy };

static void fair_uncovers(Slide how)
{
    const bool busy = how == Slide::JumpBusy, jump = how == Slide::Jump || busy;
    const uint64_t budget = 5500 * 1024;
    Rig r(budget);
    std::string err;
    VideoParams whole;
    whole.pan_blocks = false;   /* the map's writes exact: the budget saturated */
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, whole);
    /* The camera an opaque picture (an image): two videos, each its own
     * buffers, so the map's tiles under it are hidden. */
    ImageLayer *c = r.image(2, r.map, R(704, 64, 320, 240), 320, 240, 30);
    VideoLayer *v = r.video(3, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *img = r.image(4, r.panel, R(800, 200, 320, 240), 320, 240, 30);
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255), f;
    img->update({}, px.data(), px.size(), &err);
    c->update({}, px.data(), px.size(), &err);
    if (busy) {
        LayerParams ep;
        ep.rect = R(1600, 40, 200, 60);
        ep.klass = Klass::Overlay;
        r.comp->add_effect(5, r.panel, ep, kQuadVertexShader, kClockFs, 1000, r.now, &err);
    }
    int k = 0, kv = 0, pan = 0, x = 704;
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(counting(W, H, kv).data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    int64_t asked = -1, bound = 0, worst = 0, worst_bound = 0;
    int over = 0, landed = 0, waited = 0, stale = 0, since_moved = 0;
    int64_t first_since = -1;
    bool hide_due = false, sampled = false;
    for (int t = 0; t < 25000; t++) {
        const int p = t % 2500;
        if (p == 100)
            v->offer(counting(W, H, ++kv).data(), r.now);
        if (p >= 800 && t % 33 == 0) {
            k++;
            pan += 24;
            trace_frame(f, W, H, pan, 0, k);
            m->offer(f.data(), r.now);
        }
        if (p >= 800 && (jump || t % 33 == 0)) {
            if (jump)
                x = x == 704 ? 64 : 704;
            else
                x = x - 4 < 64 ? 704 : x - 4;
            c->params.rect.x = x;
            r.comp->changed(c);
        }
        /* With the panel busy, the hide is made with the budget in debt, so
         * that it waits. */
        if (p == 1600)
            hide_due = true;
        const bool hide_now = hide_due && (!busy || r.comp->pacer().allowance() <= -200000 || p == 2300);
        if (hide_now || p == 2400) {
            img->params.visible = p == 2400;
            r.comp->changed(img);
            if (hide_now) {
                hide_due = false;
                sampled = false;
                asked = r.now;
                /* What the budget owes now, and one map frame written. */
                const int64_t owed = std::max<int64_t>(0, -r.comp->pacer().allowance());
                bound = (owed + (int64_t)W * crk::atlas_height(H)) * 1000 / (int64_t)budget + 2;
            }
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        const FakeQuad *iq = quad_of(g, TexFormat::RGBA8, 320);
        /* While its hide waits, the panel's wait keeps its start, however
         * often the panel flushes meanwhile. */
        const int64_t since = r.comp->uncover_waiting_since(r.panel);
        if (asked >= 0 && !(iq && !iq->visible)) {
            if (!sampled)
                first_since = since;
            else
                since_moved += since < 0 || since != first_since;
            sampled = true;
        }
        if (g->flushes == f0)
            continue;
        if (asked >= 0 && iq && !iq->visible) {
            const int64_t w = r.now - asked;
            waited += w > 1;
            over += w > bound;
            if (w > worst) {
                worst = w;
                worst_bound = bound;
            }
            landed++;
            asked = -1;
        }
        const Look l = look(g, 1920, 720, W, H);
        stale += l.shown && l.frames > 1;
    }
    char name[600];
    std::snprintf(name, sizeof name,
                  "a camera %s over the map panning past the budget, an image over a panel video hidden: %d "
                  "hides (%d waited), each within the debt and one map frame (%d over; the longest %lld ms, its bound %lld ms), "
                  "the wait's start kept (%d moves), nothing stale (%d)",
                  busy ? "jumping every tick, the panel busy" : jump ? "jumping every tick" : "sliding 4 px a frame", landed, waited, over, (long long)worst, (long long)worst_bound, since_moved, stale);
    ok(name, landed == 10 && waited >= 5 && over == 0 && since_moved == 0 && stale == 0);
}

/* The map panning with a camera as large as it over it, the camera's
 * client gone mid-pan with the budget in debt (paused: the map's newest
 * frame taken 300 ms before, nothing new waiting): the map's write past the
 * budget is packed, nothing
 * stale shows, and every window of 10 ms to 1 s stays within the rate, the
 * burst and one packed frame past what the base allows (one exact frame). */
static void full_slot_disconnect(bool paused)
{
    const uint64_t budget = 5500 * 1024, burst = budget / 4;
    const uint64_t frame = (uint64_t)W * crk::atlas_height(H), packed = crk::block_bytes(W, H);
    Rig r(budget);
    VideoParams vp = exact(true);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, 0, 1.0f, vp);
    VideoLayer *c = r.video(2, r.map, R(0, 0, W, H), W, H, 1, 1.0f, vp);
    int k = 0;
    m->offer(counting(W, H, k).data(), r.now);
    c->offer(still(W, H, 0).data(), r.now);
    r.show("map");
    r.run(1000);
    FakeGraph *g = r.g("map");
    std::vector<uint64_t> per_tick;
    uint64_t last = g->flushed_bytes;
    int stale = 0, checked = 0;
    uint64_t packed_at = 0, packed_after = 0;
    for (int t = 0; t < 4000; t++) {
        /* The pan stops 500 ms after the camera goes: the exact atlas comes
         * back and is read. */
        if (t % 33 == 0 && t < 2500 && !(paused && t >= 1700 && t < 2000)) {
            m->offer(counting(W, H, ++k).data(), r.now);
            if (c)
                c->offer(still(W, H, k).data(), r.now);
        } else if (t % 33 == 0) {
            m->repeat(r.now);
        }
        /* The camera goes at the first tick from 2 s with the budget in
         * debt. */
        const bool gone = c && t >= 2000 && r.comp->pacer().allowance() <= 0;
        if (gone) {
            packed_at = count_of(m->stats(), " packed");
            r.comp->remove(2, c->id());
            c = nullptr;
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (gone)
            packed_after = count_of(m->stats(), " packed");
        per_tick.push_back(g->flushed_bytes - last);
        last = g->flushed_bytes;
        if (g->flushes == f0)
            continue;
        const Look l = look(g, W, H, W, H);
        if (l.shown && l.frames) {
            checked++;
            stale += l.frames > 1;
        }
    }
    bool within = true;
    uint64_t fullest = 0;
    for (int w : {10, 20, 50, 100, 200, 500, 1000}) {
        const uint64_t lim = budget * (uint64_t)w / 1000 + burst + frame + packed;
        uint64_t sum = 0;
        for (size_t i = 0; i < per_tick.size(); i++) {
            sum += per_tick[i];
            if (i >= (size_t)w)
                sum -= per_tick[i - w];
            fullest = std::max(fullest, sum * 1000 / lim);
            within = within && sum <= lim;
        }
    }
    const unsigned long long forced = count_of(m->stats(), " forced)");
    char name[300];
    std::snprintf(name, sizeof name,
                  "a full-slot camera's client gone mid-pan%s: the map's forced write packed (%llu forced, packed %llu "
                  "-> %llu), nothing stale in %d flushes (%d), every window within rate, burst, a frame and a packed "
                  "frame (the fullest %.1f %%)",
                  paused ? ", nothing new from the map" : "", forced, (unsigned long long)packed_at, (unsigned long long)packed_after, checked, stale,
                  fullest / 10.0);
    ok(name, forced == 1 && packed_after == packed_at + 1 && stale == 0 && checked >= 1 && within);
}

/* An opaque image over a video saturating the budget, a 64x64 hole in it
 * made transparent and opaque again every 300 ms: the hole shows once the
 * image's write and the video's tiles under it go together, never with a
 * write past the budget, and nothing stale shows. */
static void transparent_pixels()
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *img = r.image(3, r.panel, R(448, 136, 1024, 448), 1024, 448, 30);
    std::vector<uint8_t> px((size_t)1024 * 448 * 4, 255), hole((size_t)64 * 64 * 4, 0), solid((size_t)64 * 64 * 4, 255);
    img->update({}, px.data(), px.size(), &err);
    int k = 0;
    v->offer(counting(W, H, k).data(), r.now);
    r.show("panel");
    r.run(1000);
    FakeGraph *g = r.g("panel");
    int checked = 0, stale = 0, toggles = 0;
    for (int t = 0; t < 9000; t++) {
        if (t % 33 == 0)
            v->offer(counting(W, H, ++k).data(), r.now);
        if (t % 300 == 0) {
            const bool open = (t / 300) % 2 == 0;
            img->update({crk::PixelRect{256, 128, 64, 64}}, (open ? hole : solid).data(), hole.size(), &err);
            toggles++;
        }
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        const Look l = look(g, 1920, 720, W, H);
        if (l.shown && l.frames) {
            checked++;
            stale += l.frames > 1;
        }
    }
    const unsigned long long forced = count_of(v->stats(), " forced)"), up = count_of(v->stats(), " uncovered");
    char name[300];
    std::snprintf(name, sizeof name,
                  "a hole made transparent and opaque again in an opaque image over a video, %d times, the budget "
                  "saturated: %llu uncovering writes, none forced (%llu), nothing stale in %d flushes (%d)",
                  toggles, up, forced, checked, stale);
    ok(name, up >= 5 && forced == 0 && stale == 0 && checked > 50);
}

/* The map panning past the budget, and on the panel a video under an opaque
 * image, moved while the budget is in debt so that its move waits; then a
 * camera joins the panel (the map's pan stopping) and the display's two
 * video buffers make the panel compose the video and the camera: the video is drawn by the
 * composite, no layer of the panel is held, and no flush shows part of the
 * video from another frame. */
static void source_held()
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H);
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    ImageLayer *img = r.image(3, r.panel, R(800, 200, 320, 240), 320, 240, 30);
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255), f;
    img->update({}, px.data(), px.size(), &err);
    int k = 0, pan = 0;
    bool panning = true;
    auto tick = [&](int t) {
        if (t % 33 == 0) {
            if (panning) {
                pan += 24;
                trace_frame(f, W, H, pan, 0, 0);
                m->offer(f.data(), r.now);
            }
            v->offer(counting(W, H, ++k).data(), r.now);
        }
    };
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(counting(W, H, k).data(), r.now);
    r.show("map");
    r.show("panel");
    for (int t = 0; t < 1500; t++) {
        tick(t);
        r.step(1);
    }
    /* Moved at a tick with the budget in debt, till its move is held. */
    uint32_t held = 0;
    for (int t = 0; t < 3000 && !held; t++) {
        tick(t);
        if (r.comp->pacer().allowance() <= 0 && t % 7 == 0) {
            v->params.rect.x = v->params.rect.x == 416 ? 352 : 416;
            r.comp->changed(v);
        }
        r.step(1);
        held = r.comp->held_layers(r.panel);
    }
    VideoLayer *c = r.video(4, r.panel, R(1560, 440, 320, 240), 320, 240, 1, 1.0f, exact());
    c->offer(still(320, 240, 1).data(), r.now);
    panning = false;   /* the panel's composite then has the budget */
    FakeGraph *g = r.g("panel");
    int checked = 0, stale = 0;
    for (int t = 0; t < 3000; t++) {
        tick(t);
        if (t % 33 == 0)
            c->repeat(r.now);
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        const Look l = look(g, 1920, 720, W, H);
        if (l.shown && l.frames) {
            checked++;
            stale += l.frames > 1;
        }
    }
    char name[300];
    std::snprintf(name, sizeof name,
                  "a video whose move waits (%u held) composed with a camera joining: drawn by the composite, "
                  "nothing held (%u), none stale in %d flushes (%d)",
                  held, r.comp->held_layers(r.panel), checked, stale);
    ok(name, held == 1 && v->source() && r.comp->composite(r.panel) && r.comp->held_layers(r.panel) == 0 &&
                 checked > 10 && stale == 0);
}

/* A video with tiles held under an opaque image, its newest frame back to
 * what those tiles hold, the image removed with the budget in debt: no
 * write past the budget, and the uncovered tiles show the newest frame. */
static void forced_skipped()
{
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H);
    VideoLayer *v = r.video(2, r.panel, R(416, 120, W, H), W, H, 0, 1.0f, exact());
    std::vector<uint8_t> px((size_t)320 * 240 * 4, 255), f;
    const std::vector<uint8_t> a = counting(W, H, 7), b = counting(W, H, 8);
    int pan = 0;
    auto pan_map = [&](int t) {
        if (t % 33 == 0) {
            pan += 24;
            trace_frame(f, W, H, pan, 0, 0);
            m->offer(f.data(), r.now);
        }
    };
    trace_frame(f, W, H, pan, 0, 0);
    m->offer(f.data(), r.now);
    v->offer(a.data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(1000);
    ImageLayer *img = r.image(3, r.panel, R(800, 200, 320, 240), 320, 240, 30);
    img->update({}, px.data(), px.size(), &err);
    for (int t = 0; t < 1000; t++) {
        pan_map(t);
        r.step(1);
    }
    v->offer(b.data(), r.now);   /* written around the image: its tiles held */
    for (int t = 0; t < 1500; t++) {
        pan_map(t);
        r.step(1);
    }
    bool gone = false;
    for (int t = 0; t < 3000 && !gone; t++) {
        pan_map(t);
        if (r.comp->pacer().allowance() <= -200000) {
            v->offer(a.data(), r.now);
            r.comp->remove(3, img->id());
            gone = true;
        }
        r.step(1);
    }
    const FakeBuffer *atlas = nullptr;
    for (const auto &kv : r.g("panel")->shown.buffers)
        if (kv.second.f == TexFormat::R8 && kv.second.w == W)
            atlas = &kv.second;
    /* Map pixel 900,300 under where the image was: panel 484,180 of the video. */
    const bool newest = atlas && atlas->px[(size_t)180 * W + 484] == luma(484, 180, 7);
    const std::string st = v->stats();
    char name[300];
    std::snprintf(name, sizeof name,
                  "a video's newest frame back to what its held tiles hold, the image over them removed in debt: "
                  "%llu writes past the budget, the uncovered tiles the newest frame",
                  count_of(st, " forced)"));
    ok(name, gone && st.find("tiles left hidden") != std::string::npos && count_of(st, " forced)") == 0 && newest);
}

/* ---- as before with nothing opaque over a video ----------------------------- */

/* Recorded with video_trace.h from the build before hidden tiles
 * (cluster-render-video-composite at 83a34d0): hash, calls, write bytes,
 * flushes, failed flushes, failed writes. */
static const uint64_t kBaseTranslucent[6] = {0x65f389e6faaa92beull, 3087, 40754688, 379, 4, 30};
static const uint64_t kBaseTurned[6] = {0xe3d2afd834497d9eull, 3087, 40754688, 379, 4, 30};
static const uint64_t kBaseHud[6] = {0xd034cccd82a2b14full, 1460, 30050304, 334, 0, 0};
static const uint64_t kBaseOverlay[6] = {0x02cf39cafba3de0eull, 726, 99647488, 168, 0, 0};
/* A minute (camera_minute_trace), kinds 0 to 4: the trace, then the map's
 * frames, bytes and packed frames and the camera's. */
static const uint64_t kBaseMinute[5][12] = {
    {0x8c91ce7f810aafc2ull, 9278, 187145216, 1363, 0, 0, 896, 146554880, 370, 846, 40590336, 208},
    {0xa55b718dab651bb6ull, 9278, 187145216, 1363, 0, 0, 896, 146554880, 370, 846, 40590336, 208},
    {0x30ea992b354a52a2ull, 21242, 187145216, 2533, 0, 0, 896, 146554880, 370, 846, 40590336, 208},
    {0xfffc61acf2fc7cf1ull, 8239, 336015360, 1061, 0, 0, 540, 74789888, 261, 1058, 261225472, 912},
    {0x1d2b2dcf75e6169bull, 12066, 309237760, 1394, 0, 0, 683, 105604096, 284, 1380, 203633664, 339},
};

static bool same(const Trace &t, const uint64_t v[6])
{
    return t.hash == v[0] && t.calls == v[1] && t.write_bytes == v[2] && t.flushes == v[3] &&
           t.failed_flushes == v[4] && t.failed_writes == v[5];
}

static void unchanged()
{
    struct {
        const char *name;
        Trace t;
        const uint64_t *v;
    } runs[] = {
        {"a camera at 0.9 over the map", two_video_trace(0, 0.9f), kBaseTranslucent},
        {"a camera turned 3 degrees over the map", two_video_trace(0, 1.0f, 3.0f), kBaseTurned},
        {"Waze on the cluster and the HUD", hud_trace(), kBaseHud},
        {"the map under an overlay with transparent pixels", overlay_trace(), kBaseOverlay},
    };
    for (const auto &x : runs) {
        char name[220];
        std::snprintf(name, sizeof name,
                      "%s: every renderer call, pixel and flush as before hidden tiles (%llu calls, %llu KB)", x.name,
                      (unsigned long long)x.t.calls, (unsigned long long)(x.t.write_bytes / 1024));
        ok(name, same(x.t, x.v));
    }
}

/* ---- what it saves ---------------------------------------------------------- */

static void minute()
{
    const char *names[] = {"an opaque 320x240 camera", "a 320x240 camera at 0.9",
                           "an opaque 320x240 camera sliding 4 px a frame", "an opaque camera as large as the map",
                           "an opaque 800x480 camera at 0,0"};
    for (int kind = 0; kind < 5; kind++) {
        const Trace t = camera_minute_trace(kind);
        const uint64_t *b = kBaseMinute[kind];
        const unsigned long long cp = count_of(t.camera_stats, " packed"), up = count_of(t.video_stats, " uncovered");
        char name[600];
        std::snprintf(name, sizeof name,
                      "a Waze-like map a minute under %s: %.1f MB written (before %.1f MB); the map %.1f MB (before "
                      "%.1f MB), the camera %.1f MB (before %.1f MB) at %.1f exact and %.1f packed frames a second "
                      "(before %.1f and %.1f); %llu uncovering writes",
                      names[kind], t.write_bytes / 1e6, b[2] / 1e6, t.video.bytes / 1e6, b[7] / 1e6,
                      t.camera.bytes / 1e6, b[10] / 1e6, (t.camera.frames - cp) / 60.0, cp / 60.0,
                      (b[9] - b[11]) / 60.0, b[11] / 60.0, up);
        ok(name, kind == 1 ? same(t, b) : t.video.bytes < b[7]);
        if (kind >= 2) {
            std::snprintf(name, sizeof name, "  within the 5500 KB/s budget: %.0f KB/s", t.write_bytes / 1024.0 / 60);
            ok(name, t.write_bytes / 1024.0 / 60 <= 5500);
        }
    }
}

/* ---- cost -------------------------------------------------------------------- */

static double wall_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void cost()
{
    /* A tick's coverage test for the map under a camera: the cells cleared,
     * the camera's added, the map's 204 tiles tested. */
    const int n = 20000;
    Coverage cov;
    std::vector<uint8_t> hidden;
    size_t any = 0;
    double w0 = wall_ms();
    for (int i = 0; i < n; i++) {
        cov.reset(W, H);
        cov.add(box_of(R(704, 64, 320, 240)));
        any += hidden_tiles(cov, W, H, R(0, 0, W, H), &hidden);
    }
    const double opaque = (wall_ms() - w0) / n;
    /* The camera image: its opaque blocks consulted cell by cell. */
    std::vector<uint8_t> blocks(40 * 30, 1);
    w0 = wall_ms();
    for (int i = 0; i < n; i++) {
        cov.reset(W, H);
        cov.add_if(box_of(R(704, 64, 320, 240)), [&](const Box &c) {
            for (int32_t by = (c.y0 - 64) / 8; by <= (c.y1 - 65) / 8; by++)
                for (int32_t bx = (c.x0 - 704) / 8; bx <= (c.x1 - 705) / 8; bx++)
                    if (!blocks[(size_t)by * 40 + bx])
                        return false;
            return true;
        });
        any += hidden_tiles(cov, W, H, R(0, 0, W, H), &hidden);
    }
    const double image = (wall_ms() - w0) / n;
    /* A composite of the map and a camera (its canvas 1088x480) under an
     * opaque image: the canvas's tiles tested for each source. */
    crk::UploadPacer pacer;
    VideoParams mp, cp;
    mp.w = W, mp.h = H, cp.w = 320, cp.h = 240;
    VideoLayer map(1, 0, 1, pacer, 0, 0, mp, 0), cam(2, 0, 2, pacer, 0, 0, cp, 0);
    map.params.rect = R(0, 0, W, H);
    cam.params.rect = R(704, 64, 320, 240);
    map.to_source();
    cam.to_source();
    CompositeVideo cv(0, pacer, 0, 0, mp, 0, 0, R(0, 0, W, H), 0);
    cv.add(1, &map);
    cv.add(2, &cam);
    cv.arrange_source(1, true, 0);
    cv.arrange_source(2, true, 4);
    w0 = wall_ms();
    for (int i = 0; i < n; i++) {
        cov.reset(W, H);
        cv.begin_hidden();
        cov.add(box_of(R(64, 64, 320, 240)));
        cv.see_source(2, cov);
        cv.see_source(1, cov);
        cv.end_hidden();
        any += cv.hidden_mask() != nullptr;
    }
    const double composite = (wall_ms() - w0) / n;
    char name[300];
    std::snprintf(name, sizeof name,
                  "cost of the coverage test a tick, the map under a camera: %.4f ms (%.2f ms a second at 30 fps); "
                  "under an image: %.4f ms; a composite of the map and a camera under an image: %.4f ms",
                  opaque, opaque * 30, image, composite);
    ok(name, any == 3u * n);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    cells();
    skipped();
    opaque_rules();
    never_stale(Over::Camera);
    never_stale(Over::Image);
    never_stale(Over::Blocks);
    never_stale(Over::Fade);
    composite_occluded();
    composite_occluding();
    uncover_waits(false);
    uncover_waits(true);
    quiet_hidden_reattach();
    full_slot();
    camera_800();
    exclusive_dismissal(true);
    exclusive_dismissal(false);
    exclusive_dismissal(true, true);
    alert_removed_with_moves();
    alert_behind_busy_map();
    animate_toggle();
    removal_forced();
    refused_composite();
    held_opacity_fade();
    fair_uncovers(Slide::Slide);
    fair_uncovers(Slide::Jump);
    fair_uncovers(Slide::JumpBusy);
    source_held();
    forced_skipped();
    full_slot_disconnect(false);
    full_slot_disconnect(true);
    transparent_pixels();
    unchanged();
    minute();
    cost();
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
