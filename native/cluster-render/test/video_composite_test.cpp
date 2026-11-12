/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for several videos on one target (video_composite.h,
 * compositor.h) on the fake renderer:
 * - while the display holds every video's buffers (one video, or two on
 *   one target or on two), each video draws itself, with no composite, and
 *   drives the renderer exactly as the build before composites: every call,
 *   every pixel and every flush of a 20 s run match the trace recorded from
 *   that build (video_trace.h), but for an opaque camera over the map,
 *   which leaves the map's tiles under it unwritten (occlusion_test.cpp);
 * - a third video composes, as few targets as it takes, and with one gone
 *   the videos are separate again; a video joining a kept composite is
 *   drawn at its opacity at once;
 * - a video the renderer refused is not tried again until its scene is
 *   published again, and composited, its refusal goes with its objects;
 * - a video sliding every tick for 10 s remakes the composite at most once
 *   a second, never past two video buffers, and it shrinks once that holds;
 * - a video stepped 1 px a set (odd and even x) starts or stops being
 *   composited at most once a second, a bounded number of buffers made;
 * - a video arriving while a composite waits to shrink is priced against
 *   the composite held;
 * - a change held a while is taken only if it fits once real too, and
 *   videos let in past what fits change no buffer; a composite made with a
 *   held video at an odd x starts at the even x it is drawn at;
 * - a moved or shown video's quads go to its new place only in the flush
 *   whose write puts it there on the canvas, never before, refused flushes
 *   included;
 * - the blend and the cell composition: SSE2 and scalar equal, copies where
 *   the top source is opaque or alone, the coverage blend where translucent
 *   sources overlap, cells nothing covers left alone;
 * - an opaque source drawn through the composite's quads shows exactly what
 *   it shows drawing itself (the shaders' CPU twins);
 * - two (on a one-buffer display) to four videos on one target: one
 *   composite, the stack's order,
 *   opacity, overlap and none; the canvas against a float reference;
 * - only the tiles a source changed are composed and written;
 * - a source hidden, shown, moved, faded; the scene taken down and back;
 * - the limits: client videos, display video buffers (a turned, scaled or
 *   odd-placed video keeps its own), memory, a span past 1920x720, a
 *   change of params refused;
 * - the last other video leaving: the remaining one draws itself again,
 *   its newest frame whole;
 * - the display never holds more than two video buffers, whatever comes and
 *   goes, and the bytes stay within the 5500 KB/s budget;
 * - the cost of composing, per frame and per second (printed). */
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../kit/block_pack.h"
#include "../kit/yuv_atlas.h"
#include "../src/compositor.h"
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

/* ---- the display's video buffers, counted over every scene ------------- */

struct Count {
    int live = 0, max = 0, made = 0;   /* atlases on the display, the most at once, made in all */
    /* Atlases each scene holds as last flushed (what the display has), and
     * the most of them together. */
    std::map<const void *, int> flushed;
    int flushed_max = 0;
    void set_flushed(const void *g, int n)
    {
        if (n)
            flushed[g] = n;
        else
            flushed.erase(g);
        int sum = 0;
        for (const auto &kv : flushed)
            sum += kv.second;
        flushed_max = std::max(flushed_max, sum);
    }
};

class CountingGraph : public FakeGraph {
public:
    explicit CountingGraph(Count *c) : c_(c) {}
    int made = 0;   /* atlases made in this scene */
    ~CountingGraph() override
    {
        for (const auto &kv : pending.buffers)
            c_->live -= kv.second.f == TexFormat::R8;
        c_->set_flushed(this, 0);
    }
    bool flush(uint64_t tag) override
    {
        if (!FakeGraph::flush(tag))
            return false;
        int n = 0;
        for (const auto &kv : shown.buffers)
            n += kv.second.f == TexFormat::R8;
        c_->set_flushed(this, n);
        return true;
    }
    using FakeGraph::buffer;
    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap) override
    {
        const int b = FakeGraph::buffer(f, w, h, s, wrap);
        if (f == TexFormat::R8) {
            c_->max = std::max(c_->max, ++c_->live);
            c_->made++;
            made++;
        }
        return b;
    }
    void drop_buffer(int b) override
    {
        auto it = pending.buffers.find(b);
        if (it != pending.buffers.end() && it->second.f == TexFormat::R8)
            c_->live--;
        FakeGraph::drop_buffer(b);
    }

private:
    Count *c_;
};

class CountingGpu : public FakeGpu {
public:
    Count atlases;
    std::unique_ptr<Graph> scene(const TargetSpec &t, std::string *) override
    {
        CountingGraph *g = new CountingGraph(&atlases);
        g->name = t.name;
        g->registry = &live;
        g->clock_us = clock_us;
        g->flush_cost_us = &flush_cost_us;
        g->effect_attributes = &attrs;
        live[t.name] = g;
        scenes++;
        return std::unique_ptr<Graph>(g);
    }
};

struct NullPort : Port {
    bool available() const override { return true; }
    void request(const TargetSpec &) override {}
    void release(const TargetSpec &) override {}
    void take_over(const TargetSpec &, uint64_t) override {}
    void refresh_slots() override {}
};

static const uint32_t W = 1088, H = 480;

struct Rig {
    NullPort port;
    CountingGpu gpu;
    SlotManager slots{&port, nullptr};
    std::unique_ptr<Compositor> comp;
    int64_t now = 50000;
    int map = 0, panel = 1, hud = 2, wide = 3;

    explicit Rig(uint64_t budget = 0, CompositorLimits lim = CompositorLimits())
    {
        const struct { const char *name; uint64_t slot, scene; uint32_t w, h; } t[] = {
            {"map", 0x100, 0x010004f0, W, H},
            {"panel", 0x203, 0x01000400, 1920, 720},
            {"hud", 0x323, 0x01000401, 800, 480},
            {"wide", 0x324, 0x01000402, 2240, 896}};
        for (const auto &x : t) {
            TargetSpec s;
            s.name = x.name; s.slot = x.slot; s.scene_id = x.scene; s.width = x.w; s.height = x.h; s.linger_ms = 2000;
            slots.add(s);
        }
        GovernorConfig gc;
        gc.max_bps = budget;
        gc.floor_bps = budget / 3;
        comp.reset(new Compositor(&gpu, &slots, gc, 250, lim));
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
    void run(int64_t ms, int64_t tick = 1)
    {
        for (int64_t t = 0; t < ms; t += tick)
            step(tick);
    }
    void show(const char *target)
    {
        step(1);
        slots.on_shown(slots.targets()[slots.index(target)].scene_id, now);
        step(1);
    }
    FakeGraph *g(const char *t) { return gpu.graph(t); }
    VideoLayer *video(uint64_t owner, int target, Rect r, uint32_t w, uint32_t h, std::string *err,
                      float opacity = 1.0f, int z = 0, VideoParams vp = VideoParams())
    {
        LayerParams lp;
        lp.rect = r;
        lp.opacity = opacity;
        lp.z = z;
        lp.contend = true;
        vp.w = w;
        vp.h = h;
        const uint32_t id = comp->add_video(owner, target, lp, vp, now, err);
        return id ? static_cast<VideoLayer *>(comp->layer(owner, id)) : nullptr;
    }
};

/* The display holding one video's buffers: two videos on a target
 * compose, for the tests of composing itself. */
static CompositorLimits one_buffer()
{
    CompositorLimits l;
    l.videos = 1;
    return l;
}

static Rect R(int x, int y, uint32_t w, uint32_t h)
{
    Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

/* A w x h atlas: a gradient set by seed, chroma varied. */
static std::vector<uint8_t> frame(uint32_t w, uint32_t h, int seed)
{
    std::vector<uint8_t> f((size_t)w * crk::atlas_height(h));
    for (uint32_t y = 0; y < crk::atlas_height(h); y++)
        for (uint32_t x = 0; x < w; x++)
            f[(size_t)y * w + x] = (uint8_t)(y < h ? (x * 3 + y * 2 + seed * 29) & 0xff
                                                   : 64 + ((x / 3 + y + seed * 11) % 128));
    return f;
}

/* The same with colours that stay inside RGB's range (no clamping): the
 * blend in YUV is then the blend in RGB. */
static std::vector<uint8_t> soft(uint32_t w, uint32_t h, int seed)
{
    std::vector<uint8_t> f((size_t)w * crk::atlas_height(h));
    for (uint32_t y = 0; y < crk::atlas_height(h); y++)
        for (uint32_t x = 0; x < w; x++)
            f[(size_t)y * w + x] = (uint8_t)(y < h ? 80 + (x / 2 + y + seed * 29) % 96 : 112 + (x / 3 + y + seed * 11) % 32);
    return f;
}

static std::vector<uint8_t> rounded(const std::vector<uint8_t> &f)
{
    std::vector<uint8_t> r(f.size());
    crk::round_levels(r.data(), f.data(), f.size(), 2);
    return r;
}

static int buffers(FakeGraph *g, TexFormat f)
{
    int n = 0;
    for (const auto &kv : g->shown.buffers)
        n += kv.second.f == f;
    return n;
}

/* The shown R8 atlas buffer of the scene (one expected). */
static const FakeBuffer *atlas_of(FakeGraph *g)
{
    for (const auto &kv : g->shown.buffers)
        if (kv.second.f == TexFormat::R8)
            return &kv.second;
    return nullptr;
}

/* The shown, visible quads sampling an R8 buffer, by draw order. */
static std::vector<const FakeQuad *> atlas_quads(FakeGraph *g)
{
    std::vector<const FakeQuad *> out;
    for (const auto &kv : g->shown.quads) {
        auto t = kv.second.textures.find("u_image");
        if (!kv.second.visible || t == kv.second.textures.end() || !g->shown.buffers.count(t->second) ||
            g->shown.buffers.at(t->second).f != TexFormat::R8)
            continue;
        out.push_back(&kv.second);
    }
    std::sort(out.begin(), out.end(), [](const FakeQuad *a, const FakeQuad *b) { return a->order < b->order; });
    return out;
}

/* ---- the single video ---------------------------------------------------- */

/* Recorded with video_trace.h from the build before composites (main at
 * 05e38ec): hash, calls, write bytes, flushes, failed flushes, failed writes. */
static const uint64_t kMainTrace[2][6] = {
    {0xe0b20733c2a49aeeull, 1208, 31669248, 227, 5, 0},
    {0x772813a8daa32b9aull, 1315, 33012736, 256, 5, 8},
};
/* A camera joining a shown map, and sets (video_trace.h join_trace). */
static const uint64_t kMainJoinTrace[2][6] = {
    {0x18f19b095ea37fc8ull, 3380, 21038592, 468, 0, 0},
    {0xdf96bf1d74f5b41bull, 3390, 21038592, 541, 0, 0},
};
/* Two videos (video_trace.h two_video_trace), recorded the same way. On the
 * map the camera is opaque over the map, which leaves the 15 tiles wholly
 * under it unwritten (occlusion_test.cpp): that run is recorded from this
 * build. */
static const uint64_t kMainTwoTrace[2][6] = {
    {0x2b96bfc56be05ffaull, 3287, 40503808, 396, 4, 21},
    {0x8f0173ab738d414dull, 3146, 40896000, 507, 5, 0},
};

static void single_video()
{
    for (int which = 0; which < 2; which++) {
        const Trace t = single_video_trace(which);
        char name[200];
        std::snprintf(name, sizeof name,
                      "one video (%s): every renderer call, pixel and flush as before composites (%llu calls, "
                      "%llu KB, %llu flushes)",
                      which ? "panel, under an image" : "map", (unsigned long long)t.calls,
                      (unsigned long long)(t.write_bytes / 1024), (unsigned long long)t.flushes);
        ok(name, t.hash == kMainTrace[which][0] && t.calls == kMainTrace[which][1] &&
                     t.write_bytes == kMainTrace[which][2] && t.flushes == kMainTrace[which][3] &&
                     t.failed_flushes == kMainTrace[which][4] && t.failed_writes == kMainTrace[which][5]);
    }
    for (int which = 0; which < 2; which++) {
        const Trace t = two_video_trace(which);
        char name[260];
        if (which)
            std::snprintf(name, sizeof name,
                          "two videos (a map and a camera on the panel): each draws itself, every renderer call, "
                          "pixel and flush as before composites (%llu calls, %llu KB, %llu flushes)",
                          (unsigned long long)t.calls, (unsigned long long)(t.write_bytes / 1024),
                          (unsigned long long)t.flushes);
        else
            std::snprintf(name, sizeof name,
                          "two videos (a map and an opaque camera over it on the map): each draws itself, as "
                          "recorded with the tiles under the camera left unwritten (%llu calls, %llu KB, %llu "
                          "flushes)",
                          (unsigned long long)t.calls, (unsigned long long)(t.write_bytes / 1024),
                          (unsigned long long)t.flushes);
        ok(name, t.hash == kMainTwoTrace[which][0] && t.calls == kMainTwoTrace[which][1] &&
                     t.write_bytes == kMainTwoTrace[which][2] && t.flushes == kMainTwoTrace[which][3] &&
                     t.failed_flushes == kMainTwoTrace[which][4] && t.failed_writes == kMainTwoTrace[which][5]);
    }
    for (int which = 0; which < 2; which++) {
        const Trace t = join_trace(which);
        char name[200];
        std::snprintf(name, sizeof name,
                      "a camera added %s a shown map between passes, then sets: as before composites (%llu calls, "
                      "%llu flushes)",
                      which ? "on the panel beside" : "over", (unsigned long long)t.calls,
                      (unsigned long long)t.flushes);
        ok(name, t.hash == kMainJoinTrace[which][0] && t.calls == kMainJoinTrace[which][1] &&
                     t.write_bytes == kMainJoinTrace[which][2] && t.flushes == kMainJoinTrace[which][3] &&
                     t.failed_flushes == kMainJoinTrace[which][4] && t.failed_writes == kMainJoinTrace[which][5]);
    }
    Rig r(5500 * 1024);
    std::string err;
    VideoLayer *v = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    v->offer(frame(W, H, 1).data(), r.now);
    r.show("map");
    r.run(100);
    ok("one video: no composite, the layer draws itself", !r.comp->composite(r.map) && !v->source() &&
                                                              buffers(r.g("map"), TexFormat::R8) == 1 &&
                                                              atlas_of(r.g("map"))->px == rounded(frame(W, H, 1)));
    /* Each video alone on its own target: no composite on either. */
    VideoLayer *p = r.video(2, r.panel, R(416, 120, W, H), W, H, &err);
    p->offer(frame(W, H, 2).data(), r.now);
    r.run(50);
    ok("a video on each of two targets: no composite", !r.comp->composite(r.map) && !r.comp->composite(r.panel));
}

/* ---- the blend and the cells ---------------------------------------------- */

static void blend()
{
    std::vector<uint8_t> a(1000), b(1000);
    for (size_t i = 0; i < a.size(); i++) {
        a[i] = (uint8_t)(i * 7 + 3);
        b[i] = (uint8_t)(255 - i * 13);
    }
    bool same = true, ends = true;
    for (uint32_t t = 0; t <= 256; t++) {
        std::vector<uint8_t> x = a, y = a;
        blend_bytes(x.data(), b.data(), 1000, t, true);
        blend_bytes(y.data(), b.data(), 1000, t, false);
        same = same && x == y;
        if (t == 0)
            ends = ends && x == a;
        if (t == 256)
            ends = ends && x == b;
        for (size_t i = 0; i < 1000 && same; i++)
            same = std::abs((int)x[i] - (a[i] + (b[i] - a[i]) * (double)t / 256.0)) <= 0.5 + 1e-9;
    }
    ok("blend: SSE2 and scalar equal, rounded to nearest, 0 and 256 exact", same && ends);
}

/* The canvas over nothing: per pixel and plane, the sources over it bottom
 * first, each blended in by its share of the coverage, in doubles. */
static std::vector<double> reference(uint32_t cw, uint32_t ch, const std::vector<ComposeSource> &src,
                                     const std::vector<uint8_t> &before)
{
    std::vector<double> out(before.begin(), before.end());
    const uint32_t ah = crk::atlas_height(ch);
    for (uint32_t y = 0; y < ah; y++)
        for (uint32_t x = 0; x < cw; x++) {
            /* The luma pixel this byte stands for, and where it reads a source. */
            uint32_t lx, ly;
            int plane;
            if (y < ch) {
                lx = x, ly = y, plane = 0;
            } else {
                plane = x < cw / 2 ? 1 : 2;
                lx = (plane == 1 ? x : x - cw / 2) * 2;
                ly = (y - ch) * 2;
            }
            double c = 0, a = 0;
            bool any = false;
            for (const ComposeSource &s : src) {
                if (!s.alpha || lx < s.clip.x || lx >= s.clip.x + s.clip.w || ly < s.clip.y ||
                    ly >= s.clip.y + s.clip.h)
                    continue;
                const uint32_t fx = lx - s.x, fy = ly - s.y;
                size_t at;
                if (plane == 0)
                    at = (size_t)fy * s.w + fx;
                else
                    at = (size_t)(s.h + fy / 2) * s.w + (plane == 2 ? s.w / 2 : 0) + fx / 2;
                const double v = s.atlas[at], al = s.alpha / 256.0;
                c = any ? (al * v + (1 - al) * a * c) / (al + (1 - al) * a) : v;
                a = any ? al + (1 - al) * a : al;
                any = true;
            }
            if (any)
                out[(size_t)y * cw + x] = c;
        }
    return out;
}

static int worst(const std::vector<uint8_t> &got, const std::vector<double> &want)
{
    double d = 0;
    for (size_t i = 0; i < got.size(); i++)
        d = std::max(d, std::fabs(got[i] - want[i]));
    return (int)std::ceil(d - 1e-9);
}

static void cells()
{
    const uint32_t cw = 256, ch = 128;
    const std::vector<uint8_t> a = frame(128, 64, 1), b = frame(96, 48, 2), c = frame(64, 32, 3);
    auto src = [&](const std::vector<uint8_t> &f, uint32_t w, uint32_t h, int x, int y, uint32_t alpha) {
        ComposeSource s;
        s.atlas = f.data();
        s.w = w;
        s.h = h;
        s.x = x;
        s.y = y;
        s.clip = crk::PixelRect{(uint32_t)x, (uint32_t)y, w, h};
        s.alpha = alpha;
        return s;
    };
    std::vector<uint8_t> blank((size_t)cw * crk::atlas_height(ch), 77);
    const crk::PixelRect all{0, 0, cw, ch};
    struct Case {
        const char *name;
        std::vector<ComposeSource> s;
        int tolerance;
    } cases[] = {
        {"cells: apart, each copied exactly, the rest untouched", {src(a, 128, 64, 0, 0, 256), src(b, 96, 48, 140, 70, 256)}, 0},
        {"cells: an opaque source over another, copied over it exactly", {src(a, 128, 64, 0, 0, 256), src(b, 96, 48, 64, 32, 256)}, 0},
        {"cells: translucent over opaque, blended by its opacity", {src(a, 128, 64, 0, 0, 256), src(b, 96, 48, 64, 32, 128)}, 1},
        {"cells: translucent over translucent, by each one's share of the coverage",
         {src(a, 128, 64, 0, 0, 100), src(b, 96, 48, 64, 32, 180)}, 1},
        {"cells: three, the middle opaque: the bottom is hidden under it",
         {src(a, 128, 64, 0, 0, 200), src(b, 96, 48, 32, 16, 256), src(c, 64, 32, 96, 40, 77)}, 1},
        {"cells: a source at opacity 0 leaves the cell as the ones below make it",
         {src(a, 128, 64, 0, 0, 256), src(b, 96, 48, 64, 32, 0)}, 0},
    };
    for (const Case &k : cases) {
        std::vector<uint8_t> simd = blank, scalar = blank;
        compose_rect(simd.data(), cw, ch, all, k.s.data(), k.s.size(), true);
        compose_rect(scalar.data(), cw, ch, all, k.s.data(), k.s.size(), false);
        /* The same over tile-sized rects one after another. */
        std::vector<uint8_t> tiled = blank;
        for (uint32_t ty = 0; ty < ch; ty += 64)
            for (uint32_t tx = 0; tx < cw; tx += 64)
                compose_rect(tiled.data(), cw, ch, crk::PixelRect{tx, ty, 64, 64}, k.s.data(), k.s.size(), true);
        const int d = worst(simd, reference(cw, ch, k.s, blank));
        char name[200];
        std::snprintf(name, sizeof name, "%s (within %d level%s; SSE2, scalar and tile by tile equal)", k.name, d,
                      d == 1 ? "" : "s");
        ok(name, simd == scalar && simd == tiled && d <= k.tolerance);
    }
}

/* ---- the shaders' CPU twins ------------------------------------------------ */

static double bilinear(const uint8_t *tex, uint32_t w, uint32_t th, double s, double t)
{
    const double x = s - 0.5, y = t - 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const double ax = x - fx, ay = y - fy;
    auto at = [&](long i, long j) {
        i = i < 0 ? 0 : i >= (long)w ? (long)w - 1 : i;
        j = j < 0 ? 0 : j >= (long)th ? (long)th - 1 : j;
        return tex[(size_t)j * w + (size_t)i] / 255.0;
    };
    const long i = (long)fx, j = (long)fy;
    return (1 - ay) * ((1 - ax) * at(i, j) + ax * at(i + 1, j)) + ay * ((1 - ax) * at(i, j + 1) + ax * at(i + 1, j + 1));
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* kAtlasViewFragmentShader where the quad's v_uv is (u, v). */
static void view_rgb(const uint8_t *canvas, uint32_t cw, uint32_t ch, const float view[4], double u, double v,
                     uint8_t rgb[3])
{
    const double W = cw, H = ch, th = crk::atlas_height(ch);
    const double lx = view[0] + u * view[2], ly = view[1] + v * view[3];
    const double cx = clampd(lx * 0.5 + 0.25, view[0] * 0.5 + 0.5, (view[0] + view[2]) * 0.5 - 0.5);
    const double cy = clampd(ly * 0.5, view[1] * 0.5 + 0.5, (view[1] + view[3]) * 0.5 - 0.5);
    const double y = bilinear(canvas, cw, (uint32_t)th, lx, clampd(ly, view[1] + 0.5, view[1] + view[3] - 0.5));
    const double cu = bilinear(canvas, cw, (uint32_t)th, cx, H + cy) - 128.0 / 255.0;
    const double cv = bilinear(canvas, cw, (uint32_t)th, W * 0.5 + cx, H + cy) - 128.0 / 255.0;
    auto level = [](double x) { return (uint8_t)std::lround(clampd(x, 0.0, 1.0) * 255.0); };
    rgb[0] = level(y + 1.402 * cv);
    rgb[1] = level(y - 0.344136 * cu - 0.714136 * cv);
    rgb[2] = level(y + 1.772 * cu);
}

/* ---- two and three videos on a target -------------------------------------- */

static void two_videos()
{
    Rig r(0);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    m->offer(map.data(), r.now);
    r.show("map");
    r.run(50);
    FakeGraph *g = r.g("map");
    VideoLayer *c = r.video(2, r.map, R(704, 192, 320, 240), 320, 240, &err, 1.0f, 1);
    ok("a second video on the map is taken", c != nullptr);
    c->offer(cam.data(), r.now);
    /* A third video, on the panel: three buffers would be one too many. */
    VideoLayer *p = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
    p->offer(cam.data(), r.now);
    r.run(50);
    const CompositeVideo *cv = r.comp->composite(r.map);
    ok("with a third video on the panel: the map's two in one composite, each a source, the panel's alone",
       cv && cv->sources() == 2 && m->source() && c->source() && cv->video().w == W && cv->video().h == H &&
           !r.comp->composite(r.panel) && !p->source());
    ok("the display holds one atlas and two packed buffers, as for one video",
       buffers(g, TexFormat::R8) == 1 && buffers(g, TexFormat::RGBA8) == 2);
    const std::vector<const FakeQuad *> q = atlas_quads(g);
    ok("each drawn by its own quad over its rect, the camera on top (its z)",
       q.size() == 2 && q[0]->r.w == W && q[0]->r.h == H && q[1]->r.x == 704 && q[1]->r.y == 192 &&
           q[1]->r.w == 320 && q[1]->r.h == 240 && q[0]->uniforms.at("u_opacity")[0] == 1.0f &&
           q[1]->uniforms.at("u_opacity")[0] == 1.0f);
    /* The canvas: the map, the camera copied over it. */
    const FakeBuffer *a = atlas_of(g);
    std::vector<ComposeSource> s(2);
    const std::vector<uint8_t> rm = rounded(map), rc = rounded(cam);
    s[0].atlas = rm.data(), s[0].w = W, s[0].h = H, s[0].clip = crk::PixelRect{0, 0, W, H};
    s[1].atlas = rc.data(), s[1].w = 320, s[1].h = 240, s[1].x = 704, s[1].y = 192;
    s[1].clip = crk::PixelRect{704, 192, 320, 240};
    ok("the canvas is the map with the camera copied over it, byte for byte",
       a && worst(a->px, reference(W, H, s, a->px)) == 0);
    /* Drawn by the view shader, each looks exactly as drawn alone. */
    bool exact = true;
    for (int k = 0; k < 2 && a; k++) {
        const FakeQuad *quad = q[k];
        const std::vector<float> &view = quad->uniforms.at("u_view");
        const std::vector<uint8_t> &f = k ? rc : rm;
        const uint32_t w = k ? 320 : W, h = k ? 240 : H;
        for (uint32_t py = 0; py < h; py += k ? 1 : 7)
            for (uint32_t px = 0; px < w; px += k ? 1 : 5) {
                /* The map's pixels under the camera are the camera's. */
                if (!k && px >= 704 && px < 1024 && py >= 192 && py < 432)
                    continue;
                const double u = (px + 0.5) / w, v = (py + 0.5) / h;
                uint8_t want[3], got[3];
                crk::atlas_rgb(f.data(), w, h, u, v, want);
                view_rgb(a->px.data(), W, H, view.data(), u, v, got);
                exact = exact && !std::memcmp(want, got, 3);
            }
    }
    ok("each source drawn through the composite shows exactly what it shows drawn alone", exact);

    /* The camera moved under the map's z: the map then covers it. */
    c->params.z = -1;
    r.comp->changed(c);
    r.run(50);
    const std::vector<const FakeQuad *> q2 = atlas_quads(g);
    s[0].clip = crk::PixelRect{0, 0, W, H};
    std::swap(s[0], s[1]);
    ok("order follows the stack: the camera below the map, the canvas the map's alone",
       q2.size() == 2 && q2[0]->r.w == 320 && atlas_of(g)->px == rm);
}

static void three_videos()
{
    Rig r(0);
    std::string err;
    const std::vector<uint8_t> f1 = soft(W, H, 1), f2 = soft(320, 240, 2), f3 = soft(256, 128, 3);
    VideoLayer *a = r.video(1, r.panel, R(416, 120, W, H), W, H, &err);
    VideoLayer *b = r.video(2, r.panel, R(600, 200, 320, 240), 320, 240, &err, 0.5f, 1);
    VideoLayer *c = r.video(3, r.panel, R(800, 320, 256, 128), 256, 128, &err, 0.75f, 2);
    VideoLayer *d = r.video(4, r.panel, R(1600, 600, 256, 120), 256, 120, &err, 0.6f, 3);
    a->offer(f1.data(), r.now);
    b->offer(f2.data(), r.now);
    c->offer(f3.data(), r.now);
    const std::vector<uint8_t> f4 = soft(256, 120, 4);
    d->offer(f4.data(), r.now);
    r.show("panel");
    r.run(60);
    const CompositeVideo *cv = r.comp->composite(r.panel);
    FakeGraph *g = r.g("panel");
    ok("four videos on the panel: one composite over them together, within the target",
       cv && cv->sources() == 4 && cv->x() == 416 && cv->y() == 120 && cv->video().w == 1440 &&
           cv->video().h == 600 && buffers(g, TexFormat::R8) == 1);
    const std::vector<uint8_t> r1 = rounded(f1), r2 = rounded(f2), r3 = rounded(f3), r4 = rounded(f4);
    std::vector<ComposeSource> s(4);
    const std::vector<uint8_t> *fs[4] = {&r1, &r2, &r3, &r4};
    const Rect rects[4] = {R(416, 120, W, H), R(600, 200, 320, 240), R(800, 320, 256, 128), R(1600, 600, 256, 120)};
    const uint32_t al[4] = {256, 128, 192, 154};
    for (int k = 0; k < 4; k++) {
        s[k].atlas = fs[k]->data();
        s[k].w = rects[k].w;
        s[k].h = rects[k].h;
        s[k].x = rects[k].x - 416;
        s[k].y = rects[k].y - 120;
        s[k].clip = crk::PixelRect{(uint32_t)s[k].x, (uint32_t)s[k].y, rects[k].w, rects[k].h};
        s[k].alpha = al[k];
    }
    const FakeBuffer *at = atlas_of(g);
    const int d1 = at ? worst(at->px, reference(1440, 600, s, at->px)) : 99;
    char name[200];
    std::snprintf(name, sizeof name,
                  "three overlapping (opaque, 0.5, 0.75) and one apart (0.6): the canvas within %d level of the "
                  "reference", d1);
    ok(name, d1 <= 1);
    const std::vector<const FakeQuad *> q = atlas_quads(g);
    ok("each at its opacity, in the stack's order",
       q.size() == 4 && q[0]->uniforms.at("u_opacity")[0] == 1.0f && q[1]->uniforms.at("u_opacity")[0] == 0.5f &&
           q[2]->uniforms.at("u_opacity")[0] == 0.75f && std::fabs(q[3]->uniforms.at("u_opacity")[0] - 0.6f) < 1e-6 &&
           q[0]->order < q[1]->order && q[1]->order < q[2]->order && q[2]->order < q[3]->order);
    /* The drawn result over a backdrop, as the GPU blends the quads, against
     * each video's own quad drawn alone over it: within rounding. */
    int off = 0;
    const uint8_t back[3] = {40, 90, 140};
    for (uint32_t py = 120; py < 720; py += 9)
        for (uint32_t px = 416; px < 1856; px += 11) {
            double want[3] = {(double)back[0], (double)back[1], (double)back[2]};
            double got[3] = {want[0], want[1], want[2]};
            for (int k = 0; k < 4; k++) {
                const Rect &rr = rects[k];
                if ((int)px < rr.x || (int)px >= rr.x + (int)rr.w || (int)py < rr.y || (int)py >= rr.y + (int)rr.h)
                    continue;
                const double u = (px - rr.x + 0.5) / rr.w, v = (py - rr.y + 0.5) / rr.h, op = al[k] / 256.0;
                uint8_t own[3], comp[3];
                crk::atlas_rgb(fs[k]->data(), rr.w, rr.h, u, v, own);
                const std::vector<float> &view = q[k]->uniforms.at("u_view");
                view_rgb(at->px.data(), 1440, 600, view.data(), u, v, comp);
                for (int ch = 0; ch < 3; ch++) {
                    want[ch] = op * own[ch] + (1 - op) * want[ch];
                    got[ch] = op * comp[ch] + (1 - op) * got[ch];
                }
            }
            for (int ch = 0; ch < 3; ch++)
                off = std::max(off, (int)std::ceil(std::fabs(want[ch] - got[ch]) - 1e-9));
        }
    std::snprintf(name, sizeof name,
                  "drawn over a backdrop, the quads give what each video's own quad would, within %d levels", off);
    ok(name, off <= 4);
}

/* ---- only what changed ------------------------------------------------------ */

static void dirty_tiles()
{
    Rig r(0, one_buffer());
    std::string err;
    std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 256, 5);
    VideoParams exact;
    exact.level_bits = 0;
    exact.dirty_threshold = 0;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err, 1.0f, 0, exact);
    VideoLayer *c = r.video(2, r.map, R(128, 64, 320, 256), 320, 256, &err, 1.0f, 1, exact);
    m->offer(map.data(), r.now);
    c->offer(cam.data(), r.now);
    r.show("map");
    r.run(60);
    const CompositeVideo *cv = r.comp->composite(r.map);
    FakeGraph *g = r.g("map");
    const uint64_t t0 = cv->composed_tiles(), b0 = g->flushed_bytes;
    /* The camera's 64x64 block at (64, 64) of its frame: canvas tile (3, 2). */
    for (uint32_t y = 64; y < 128; y++)
        for (uint32_t x = 64; x < 128; x++)
            cam[(size_t)y * 320 + x] ^= 0x55;
    c->offer(cam.data(), r.now);
    r.run(40);
    ok("a source changing one tile's luma: that tile composed again, its bytes alone written",
       cv->composed_tiles() - t0 == 1 && g->flushed_bytes - b0 == 64 * 64);
    const uint64_t t1 = cv->composed_tiles(), b1 = g->flushed_bytes;
    m->offer(map.data(), r.now);
    c->offer(cam.data(), r.now);
    r.run(40);
    ok("the same frames again: nothing composed, nothing written",
       cv->composed_tiles() == t1 && g->flushed_bytes == b1);
    m->repeat(r.now);
    c->repeat(r.now);
    r.run(40);
    ok("repeats: nothing composed, nothing written", cv->composed_tiles() == t1 && g->flushed_bytes == b1);
    /* The map changing a tile under the camera: composed, but the canvas
     * there is the camera's, so nothing is written. */
    for (uint32_t y = 128; y < 192; y++)
        for (uint32_t x = 192; x < 256; x++)
            map[(size_t)y * W + x] ^= 0x33;
    m->offer(map.data(), r.now);
    r.run(40);
    ok("the map changing under the opaque camera: one tile composed, nothing written",
       cv->composed_tiles() - t1 == 1 && g->flushed_bytes == b1);

    /* Hidden: its rect composed from the map again; shown: back. */
    const uint64_t t2 = cv->composed_tiles();
    c->params.visible = false;
    r.comp->changed(c);
    r.run(40);
    const std::vector<uint8_t> canvas = atlas_of(g)->px;
    ok("the camera hidden: its quads hidden, its 5x4 tiles composed from the map (the map's alone again)",
       atlas_quads(g).size() == 1 && cv->composed_tiles() - t2 == 20 && canvas == map);
    c->params.visible = true;
    r.comp->changed(c);
    r.run(40);
    ok("shown again: its quads back, drawn from its newest frame", atlas_quads(g).size() == 2 &&
                                                                    atlas_of(g)->px[(size_t)64 * W + 128] == cam[0]);
    /* Moved by 64: composed where it was and where it is. */
    const uint64_t t3 = cv->composed_tiles();
    c->params.rect.x += 64;
    r.comp->changed(c);
    r.run(40);
    ok("moved by 64: the composite kept, the 6x4 tiles it left and covers composed",
       r.comp->composite(r.map) == cv && cv->composed_tiles() - t3 == 24 && atlas_quads(g)[1]->r.x == 192 &&
           atlas_of(g)->px[(size_t)64 * W + 192] == cam[0] && atlas_of(g)->px[(size_t)64 * W + 128] == map[(size_t)64 * W + 128]);
    /* Its opacity: the overlap blended. */
    c->params.opacity = 0.5f;
    r.comp->changed(c);
    r.run(40);
    const uint8_t want = (uint8_t)((map[(size_t)64 * W + 192] * 128 + cam[0] * 128 + 128) >> 8);
    ok("faded to 0.5 over the opaque map: its quads at 0.5, the canvas under it half each",
       atlas_quads(g)[1]->uniforms.at("u_opacity")[0] == 0.5f && atlas_of(g)->px[(size_t)64 * W + 192] == want);
}

/* ---- leaving, limits, the display's buffers ---------------------------------- */

static void leaving()
{
    Rig r(5500 * 1024);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    m->offer(map.data(), r.now);
    r.show("map");
    r.run(100);
    VideoLayer *c = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
    VideoLayer *d = r.video(3, r.map, R(640, 64, 320, 240), 320, 240, &err, 1.0f, 2);
    c->offer(cam.data(), r.now);
    d->offer(cam.data(), r.now);
    r.run(100);
    ok("with two cameras: composited", r.comp->composite(r.map) != nullptr);
    const std::vector<uint8_t> map2 = frame(W, H, 2), cam2 = frame(320, 240, 6);
    m->offer(map2.data(), r.now);
    c->offer(cam2.data(), r.now);
    r.run(10);
    r.comp->remove_owner(3);
    r.run(100);
    FakeGraph *g = r.g("map");
    std::vector<const FakeQuad *> q = atlas_quads(g);
    const FakeBuffer *cb = nullptr;
    for (const auto &kv : g->shown.buffers)
        if (kv.second.f == TexFormat::R8 && kv.second.w == 320)
            cb = &kv.second;
    ok("one camera gone: two videos, separate again, each its own buffers and its newest frame whole",
       !r.comp->composite(r.map) && !m->source() && !c->source() && buffers(g, TexFormat::R8) == 2 &&
           buffers(g, TexFormat::RGBA8) == 4 && q.size() == 2 && !q[0]->uniforms.count("u_view") &&
           !q[1]->uniforms.count("u_view") && cb && cb->px == rounded(cam2));
    r.comp->remove_owner(2);
    r.run(100);
    q = atlas_quads(g);
    ok("the other gone: the map alone, its newest frame",
       !r.comp->composite(r.map) && buffers(g, TexFormat::R8) == 1 && buffers(g, TexFormat::RGBA8) == 2 &&
           q.size() == 1 && atlas_of(g)->px == rounded(map2));
    /* And it goes on exactly as a single video: a changed tile alone. */
    std::vector<uint8_t> map3 = map2;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            map3[(size_t)y * W + x] ^= 0x40;
    const uint64_t b0 = g->flushed_bytes;
    m->offer(map3.data(), r.now);
    r.run(100);
    ok("and writes only what changes, as before", g->flushed_bytes - b0 == 64 * 64);
}

/* Composing only past the display's two video buffers. */
static void cap_rule()
{
    Rig r(5500 * 1024);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    VideoLayer *c = r.video(2, r.map, R(704, 64, 320, 240), 320, 240, &err, 1.0f, 1);
    m->offer(map.data(), r.now);
    c->offer(cam.data(), r.now);
    r.show("map");
    r.run(100);
    FakeGraph *g = r.g("map");
    ok("two videos on the map: separate, two atlases, each drawing itself",
       !r.comp->composite(r.map) && !m->source() && !c->source() && buffers(g, TexFormat::R8) == 2 &&
           atlas_quads(g).size() == 2);
    VideoLayer *d = r.video(3, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 2);
    d->offer(cam.data(), r.now);
    r.run(100);
    ok("a third: the three composed, one atlas", r.comp->composite(r.map) &&
                                                     r.comp->composite(r.map)->sources() == 3 &&
                                                     buffers(g, TexFormat::R8) == 1 && atlas_quads(g).size() == 3);
    r.comp->remove_owner(3);
    r.run(100);
    ok("the third gone: separate again", !r.comp->composite(r.map) && buffers(g, TexFormat::R8) == 2);
    /* Two on the map and two on the panel: both compose. */
    VideoLayer *p1 = r.video(4, r.panel, R(0, 0, 320, 240), 320, 240, &err);
    ok("a third on the panel: the map's two compose, the panel's draws itself",
       p1 && r.comp->composite(r.map) && !r.comp->composite(r.panel) && !p1->source());
    VideoLayer *p2 = r.video(5, r.panel, R(320, 0, 320, 240), 320, 240, &err);
    ok("a fourth, on the panel: both targets compose", p2 && r.comp->composite(r.map) && r.comp->composite(r.panel));
    r.comp->remove_owner(5);
    p1->offer(cam.data(), r.now);
    r.run(100);   /* the map's composite written: its quads at their opacity */
    const int made = static_cast<CountingGraph *>(g)->made;
    VideoLayer *m3 = r.video(6, r.map, R(64, 64, 320, 240), 320, 240, &err, 0.75f, 3);
    m3->offer(cam.data(), r.now);
    r.run(100);
    ok("three on the map and one on the panel: the map alone composes",
       m3 && r.comp->composite(r.map) && r.comp->composite(r.map)->sources() == 3 && !r.comp->composite(r.panel));
    const FakeQuad *joined = nullptr;
    for (const FakeQuad *q : atlas_quads(g))
        if (q->r.x == 64 && q->r.y == 64 && q->r.w == 320)
            joined = q;
    ok("a video joining the kept composite is drawn at once at its opacity (0.75)",
       static_cast<CountingGraph *>(g)->made == made && joined && joined->uniforms.at("u_opacity")[0] == 0.75f);
    r.comp->remove_owner(4);
    r.comp->remove_owner(6);
    r.comp->remove_owner(2);
    VideoLayer *h = r.video(7, r.hud, R(0, 0, 320, 240), 320, 240, &err);
    VideoLayer *p = r.video(8, r.panel, R(0, 0, 320, 240), 320, 240, &err);
    ok("a video on each of three targets: the third refused, nothing composes",
       !r.comp->composite(r.map) && h && !p && err.find("too many video buffers on the display") == 0);
}

/* A video whose objects the renderer refused stays refused until its scene
 * is published again, whatever else comes and goes; made a source, its own
 * refusal goes with its objects. */
static void refused_attach()
{
    Rig r(0);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    LayerParams fp;
    fp.klass = Klass::Overlay;
    fp.rect = R(0, 0, 64, 64);
    r.comp->add_effect(9, r.map, fp, "", "#version 100\nvoid main() { gl_FragColor = vec4(1.0); }", 0, r.now, &err);
    r.show("map");
    CountingGraph *g = static_cast<CountingGraph *>(r.g("map"));
    g->unknown_uniform = "u_size";   /* the video quads' uniforms refused */
    VideoLayer *v = r.video(2, r.map, R(0, 0, W, H), W, H, &err);
    v->offer(map.data(), r.now);
    r.run(50);
    std::string why;
    const int made = g->made;
    ok("a video the renderer refuses: error", r.comp->state(v->id(), &why) == LayerState::Error && made == 1);
    VideoLayer *q = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
    q->offer(cam.data(), r.now);
    v->params.z = 2;
    r.comp->changed(v);
    r.comp->remove_owner(3);
    r.run(50);
    ok("a video added and removed elsewhere, and a set: not tried again", g->made == made &&
                                                                          r.comp->state(v->id(), &why) ==
                                                                              LayerState::Error);
    g->unknown_uniform.clear();
    VideoLayer *c = r.video(4, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 3);
    VideoLayer *p = r.video(5, r.panel, R(0, 0, 320, 240), 320, 240, &err);   /* a third: the map's compose */
    c->offer(cam.data(), r.now);
    p->offer(cam.data(), r.now);
    r.run(100);
    ok("composited with a second video on the map: its refusal gone with its own objects, it is shown",
       r.comp->composite(r.map) && v->source() && r.comp->state(v->id(), &why) == LayerState::Shown &&
           atlas_quads(g).size() == 2);
}

/* A picture-in-picture sliding across the panel every tick for 10 s, with
 * the map and a third video on the panel: the composite is made again at
 * most once a second, never past two video buffers, and smaller once the
 * slide stops. */
static void sliding()
{
    Rig r(5500 * 1024);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.panel, R(416, 120, W, H), W, H, &err);
    VideoLayer *a = r.video(2, r.panel, R(600, 200, 320, 240), 320, 240, &err, 1.0f, 1);
    VideoLayer *pip = r.video(3, r.panel, R(416, 120, 320, 240), 320, 240, &err, 1.0f, 2);
    for (VideoLayer *v : {m, a, pip})
        v->offer(v == m ? map.data() : cam.data(), r.now);
    r.show("panel");
    CountingGraph *g = static_cast<CountingGraph *>(r.g("panel"));
    int x = 416, dx = 4;
    for (int t = 0; t < 1000; t++) {
        if (t % 3 == 0) {
            m->offer(map.data(), r.now);
            a->offer(cam.data(), r.now);
            pip->offer(cam.data(), r.now);
        }
        x += dx;
        if (x < 0 || x > 1600) {
            dx = -dx;
            x += 2 * dx;
        }
        pip->params.rect.x = x & ~1;
        r.comp->changed(pip);
        r.step(10);
    }
    const int made = g->made;
    char name[200];
    std::snprintf(name, sizeof name,
                  "a video moved every tick for 10 s: the composite made %d times (at most 11), never past two video "
                  "buffers (%d)", made, r.gpu.atlases.max);
    ok(name, made >= 2 && made <= 11 && r.gpu.atlases.max <= 2);
    const CompositeVideo *cv = r.comp->composite(r.panel);
    ok("it covers where the video went", cv && cv->x() <= 416 && cv->video().w >= 1440);
    r.run(5000, 10);
    cv = r.comp->composite(r.panel);
    const uint32_t right = (uint32_t)std::max<int>(416 + (int)W, pip->params.rect.x + 320);
    ok("the slide stopped: made smaller once that held 3 s, over the videos alone",
       g->made == made + 1 && cv && cv->x() == 416 && cv->y() == 120 && cv->video().w == (right - 416 + 3) / 4 * 4 &&
           cv->video().h == H && r.gpu.atlases.max <= 2);
}

/* Whether the shown canvas holds frame f (w x h, exact) where quad q
 * samples it (u_view). */
static bool holds(const FakeGraph *g, const FakeQuad &q, const std::vector<uint8_t> &f, uint32_t w, uint32_t h)
{
    const FakeBuffer *a = nullptr;
    for (const auto &kv : g->shown.buffers)
        if (kv.second.f == TexFormat::R8)
            a = &kv.second;
    const std::vector<float> &v = q.uniforms.at("u_view");
    const uint32_t vx = (uint32_t)v[0], vy = (uint32_t)v[1], vw = (uint32_t)v[2], vh = (uint32_t)v[3];
    if (!a || vw != w || vh != h)
        return false;
    const uint32_t cw = a->w, ch = a->h * 2 / 3;
    for (uint32_t y = 0; y < h; y++)
        if (std::memcmp(&a->px[(size_t)(vy + y) * cw + vx], &f[(size_t)y * w], w))
            return false;
    for (uint32_t y = 0; y < h / 2; y++)
        for (uint32_t k = 0; k < 2; k++)
            if (std::memcmp(&a->px[(size_t)(ch + vy / 2 + y) * cw + k * cw / 2 + vx / 2],
                            &f[(size_t)(h + y) * w + k * w / 2], w / 2))
                return false;
    return true;
}

/* A camera over the map moved by sets faster than the budget writes the
 * canvas, hidden and shown, with refused flushes: no flush ever shows its
 * quads at a place whose canvas does not hold it, and it gets where it is
 * sent. */
static void moves_with_canvas()
{
    Rig r(1500 * 1024, one_buffer());
    std::string err;
    VideoParams exact;
    exact.level_bits = 0;
    exact.dirty_threshold = 0;
    exact.pan_blocks = false;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err, 1.0f, 0, exact);
    VideoLayer *c = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1, exact);
    m->offer(map.data(), r.now);
    c->offer(cam.data(), r.now);
    r.show("map");
    r.run(500);
    FakeGraph *g = r.g("map");
    int stale = 0, flushes = 0, placements = 0, shown_ticks = 0;
    int32_t last_x = -1;
    const int xs[] = {64, 128, 704, 320, 512, 66, 768};
    for (int t = 0; t < 3000; t++) {
        if (t % 33 == 0) {
            m->repeat(r.now);
            c->repeat(r.now);
        }
        if (t % 37 == 0 && t < 2600) {
            c->params.rect.x = xs[(t / 37) % 7];
            c->params.rect.y = 64 + (t / 37) % 3 * 32;
            r.comp->changed(c);
        }
        if (t == 900 || t == 1500) {
            c->params.visible = t == 1500;
            r.comp->changed(c);
        }
        g->fail_flush = t >= 1700 && t < 1760;
        const uint64_t f0 = g->flushes;
        r.step(1);
        if (g->flushes == f0)
            continue;
        flushes++;
        for (const auto &kv : g->shown.quads) {
            const FakeQuad &q = kv.second;
            if (!q.visible || !q.uniforms.count("u_view") || q.r.w != 320 || q.uniforms.at("u_opacity")[0] == 0)
                continue;
            shown_ticks++;
            if (q.r.x != last_x)
                placements++, last_x = q.r.x;
            if (!holds(g, q, cam, 320, 240))
                stale++;
        }
    }
    r.run(1000);
    bool there = false;
    for (const FakeQuad *q : atlas_quads(g))
        there = there || (q->r.x == c->params.rect.x && q->r.y == c->params.rect.y && holds(g, *q, cam, 320, 240));
    char name[220];
    std::snprintf(name, sizeof name,
                  "a camera moved every 37 ms past what the budget writes, hidden, shown, flushes refused: in %d "
                  "flushes (%d moves shown) its quads never over a canvas without it (%d)",
                  flushes, placements, stale);
    ok(name, stale == 0 && placements > 10 && shown_ticks > 0);
    ok("and it ends where it was sent, the canvas holding it there", there);
}

/* Three videos on the map, one stepped a pixel a set, so its x is odd and
 * even by turns, for 10 s: it starts or stops being composited at most once
 * a second, so the display's video buffers are made a bounded number of
 * times; held composited at an odd x it is drawn at the even one. */
static void odd_steps()
{
    Rig r(5500 * 1024);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    VideoLayer *a = r.video(2, r.map, R(704, 64, 320, 240), 320, 240, &err, 1.0f, 1);
    VideoLayer *pip = r.video(3, r.map, R(64, 200, 320, 240), 320, 240, &err, 1.0f, 2);
    for (VideoLayer *v : {m, a, pip})
        v->offer(v == m ? map.data() : cam.data(), r.now);
    r.show("map");
    r.run(1200, 10);
    CountingGraph *g = static_cast<CountingGraph *>(r.g("map"));
    const int made0 = g->made;
    bool rounded = true;
    int odd_composited = 0;
    for (int t = 0; t < 1000; t++) {
        if (t % 3 == 0)
            for (VideoLayer *v : {m, a, pip})
                v->repeat(r.now);
        pip->params.rect.x = 64 + t % 400;
        r.comp->changed(pip);
        r.step(10);
        if (pip->source() && (pip->params.rect.x & 1)) {
            odd_composited++;
            for (const auto &kv : g->pending.quads)
                if (kv.second.r.w == 320 && kv.second.r.y == 200 && kv.second.uniforms.count("u_view") &&
                    (kv.second.r.x & 1))
                    rounded = false;
        }
    }
    char name[220];
    std::snprintf(name, sizeof name,
                  "a video stepped 1 px a set for 10 s, odd and even by turns: %d atlases made (at most 12), never "
                  "past two buffers (%d)", g->made - made0, r.gpu.atlases.max);
    ok(name, g->made - made0 <= 12 && r.gpu.atlases.max <= 2);
    ok("kept composited at an odd x, drawn at the even x left of it", odd_composited > 0 && rounded);
}

/* While a composite waits to shrink, a new video is priced against the
 * composite held, not the smaller one planned. */
static void shrink_memory()
{
    CompositorLimits lim;
    lim.pixel_bytes = 9300000;
    Rig r(0, lim);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.panel, R(416, 120, W, H), W, H, &err);
    VideoLayer *a = r.video(2, r.panel, R(600, 200, 320, 240), 320, 240, &err, 1.0f, 1);
    VideoLayer *pip = r.video(3, r.panel, R(480, 300, 320, 240), 320, 240, &err, 1.0f, 2);
    for (VideoLayer *v : {m, a, pip})
        v->offer(v == m ? map.data() : cam.data(), r.now);
    r.show("panel");
    r.run(1100, 10);
    pip->params.rect.x = 0;
    r.comp->changed(pip);
    r.run(100, 10);
    const uint32_t grown = r.comp->composite(r.panel) ? r.comp->composite(r.panel)->video().w : 0;
    pip->params.rect.x = 480;
    r.comp->changed(pip);
    r.run(100, 10);
    VideoLayer *h = r.video(4, r.hud, R(0, 0, 320, 240), 320, 240, &err);
    ok("a composite grown to 1504 and waiting to shrink to 1088: a video that fits only beside the smaller one "
       "is refused",
       grown == 1504 && r.comp->composite(r.panel)->video().w == 1504 && !h &&
           err == "the service's pixel memory is full");
    r.run(3500, 10);
    h = r.video(4, r.hud, R(0, 0, 320, 240), 320, 240, &err);
    ok("once it has shrunk, taken", r.comp->composite(r.panel)->video().w == W && h);
}

/* Changes held a while (a composited video turned or put at an odd x, a
 * direct one put where it would compose past 1920x720) are taken only when
 * they fit once real too; and videos let in past what fits change no
 * buffer. */
static void held_changes()
{
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    {
        Rig r(0);
        std::string err;
        VideoLayer *a = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
        VideoLayer *b = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
        a->offer(map.data(), r.now);
        b->offer(cam.data(), r.now);
        r.show("map");
        r.run(100, 10);
        /* A third: the map's two composed now, the hold window open. */
        VideoLayer *p = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
        p->offer(cam.data(), r.now);
        r.show("panel");
        LayerParams turned = b->params, odd = b->params;
        turned.rotate = 90;
        odd.rect.x = 65;
        ok("within the hold after composing: turning one of the map's (three buffers once real) refused",
           r.comp->composite(r.map) && !r.comp->check(b, turned, &err) &&
               err.find("too many video buffers on the display") == 0);
        ok("and putting it at an odd x", !r.comp->check(b, odd, &err) &&
                                             err.find("too many video buffers on the display") == 0);
        /* Past check(): held now, three buffers once real. */
        b->params = turned;
        r.comp->changed(b);
        r.run(1500, 10);
        ok("a turn let in past what fits: at the hold's end no buffer changes, the video stays composited",
           r.comp->composite(r.map) && b->source() && r.gpu.atlases.live == 2 && r.gpu.atlases.max <= 2);
        b->params.rotate = 0;
        r.comp->changed(b);
        r.run(100, 10);
        ok("turned back: fits, as before", r.comp->composite(r.map) && b->source() && r.gpu.atlases.max <= 2);
    }
    {
        Rig r(0);
        std::string err;
        VideoLayer *a = r.video(1, r.map, R(0, 0, 320, 240), 320, 240, &err);
        VideoLayer *b = r.video(2, r.map, R(400, 0, 320, 240), 320, 240, &err, 1.0f, 1);
        a->offer(cam.data(), r.now);
        b->offer(cam.data(), r.now);
        r.show("map");
        r.run(100, 10);
        VideoLayer *p = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
        r.show("panel");
        LayerParams out = b->params;
        out.rect.x = 760;
        ok("past the composite within kRemakeMs of its making: taken, its remake waits",
           p && r.comp->composite(r.map) && r.comp->check(b, out, &err));
        b->params = out;
        r.comp->changed(b);
        b->params.rotate = 90;   /* past check(): three buffers once real */
        r.comp->changed(b);
        r.run(1500, 10);
        ok("a turn let in past what fits drops the waiting remake: no wake-up every millisecond",
           r.comp->composite(r.map) && r.gpu.atlases.max <= 2 && r.comp->wake_in_ms(r.now) != 1);
    }
    {
        Rig r(0);
        std::string err;
        VideoLayer *a = r.video(1, r.wide, R(0, 0, W, H), W, H, &err);
        VideoLayer *d = r.video(2, r.wide, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
        r.run(100, 10);
        VideoLayer *b = r.video(3, r.wide, R(1101, 400, W, H), W, H, &err);   /* a and d composed now */
        LayerParams even = b && a && d ? b->params : LayerParams();
        even.rect.x = 1100;
        ok("within the hold: a direct video put where it would compose past 1920x720 once real: refused",
           a && d && b && r.comp->composite(r.wide) && !b->source() && !r.comp->check(b, even, &err) &&
               err == "the target's videos together span more than 1920x720");
    }
}

/* A composited video held at an odd x and leftmost when the composite is
 * made again: the canvas starts at the even x it is drawn at. */
static void even_origin()
{
    Rig r(0);
    std::string err;
    const std::vector<uint8_t> map = frame(W, H, 1), cam = frame(320, 240, 5);
    VideoLayer *m = r.video(1, r.panel, R(416, 120, W, H), W, H, &err);
    VideoLayer *a = r.video(2, r.panel, R(600, 200, 320, 240), 320, 240, &err, 1.0f, 1);
    VideoLayer *pip = r.video(3, r.panel, R(480, 300, 320, 240), 320, 240, &err, 1.0f, 2);
    for (VideoLayer *v : {m, a, pip})
        v->offer(v == m ? map.data() : cam.data(), r.now);
    r.show("panel");
    r.run(1100, 10);
    VideoLayer *q = r.video(4, r.panel, R(800, 300, 320, 240), 320, 240, &err, 1.0f, 3);   /* joins inside: a member change */
    q->offer(cam.data(), r.now);
    pip->params.rect.x = 101;
    pip->params.rect.y = 51;
    r.comp->changed(pip);
    r.run(100, 10);
    const CompositeVideo *cv = r.comp->composite(r.panel);
    ok("a video held composited at 101,51 leftmost and topmost: the composite remade from 100,50, even",
       cv && pip->source() && cv->x() == 100 && cv->y() == 50);
}

static void limits()
{
    {
        CompositorLimits lim;
        lim.client_videos = 3;
        Rig r(0, lim);
        std::string err;
        bool all = true;
        for (int k = 0; k < 3; k++)
            all = all && r.video(k + 1, r.map, R(k * 64, 0, 320, 240), 320, 240, &err);
        ok("three videos on one target within a limit of three", all && r.comp->composite(r.map)->sources() == 3);
        ok("a fourth refused: too many video layers",
           !r.video(9, r.map, R(0, 0, 320, 240), 320, 240, &err) && err == "too many video layers");
    }
    {
        Rig r(0);
        std::string err;
        const int n = 8;
        int made = 0;
        for (int k = 0; k < n + 1; k++)
            made += r.video(k + 1, r.panel, R(k * 128, 0, 128, 64), 128, 64, &err) != nullptr;
        ok("the default: eight client videos, sharing one buffer", made == 8 && r.comp->composite(r.panel)->sources() == 8);
    }
    {
        Rig r(0);
        std::string err;
        VideoLayer *a = r.video(1, r.map, R(0, 0, 640, 480), 320, 240, &err);   /* scaled */
        VideoLayer *b = r.video(2, r.map, R(1, 0, 320, 240), 320, 240, &err);   /* an odd x */
        ok("a scaled video and one at an odd x keep buffers of their own: two", a && b && !r.comp->composite(r.map));
        VideoLayer *c = r.video(3, r.map, R(64, 64, 320, 240), 320, 240, &err);
        ok("a third would need a third buffer: too many video buffers on the display",
           !c && err.find("too many video buffers on the display") == 0);
        VideoLayer *e = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
        ok("and so on another target", !e && err.find("too many video buffers") == 0);
    }
    {
        Rig r(0);
        std::string err;
        VideoLayer *a = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
        VideoLayer *b = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
        VideoLayer *p = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
        ok("two composited on the map and one on the panel: two buffers", a && b && p && r.comp->composite(r.map));
        r.run(1100);   /* past kRemakeMs from the map's videos being composited */
        LayerParams turned = b->params;
        turned.rotate = 90;
        ok("turning one of the map's would need three: refused before it is set",
           !r.comp->check(b, turned, &err) && err.find("too many video buffers") == 0);
        LayerParams moved = b->params;
        moved.rect.x = 128;
        ok("moving it is fine", r.comp->check(b, moved, &err));
        r.comp->remove_owner(3);
        ok("with the panel's gone, the turn is allowed", r.comp->check(b, turned, &err));
        b->params = turned;
        r.comp->changed(b);
        ok("and turned, both draw themselves", !r.comp->composite(r.map) && !a->source() && !b->source());
    }
    {
        Rig r(0);
        std::string err;
        VideoLayer *m = r.video(1, r.map, R(0, 0, 320, 240), 320, 240, &err);
        VideoLayer *a = r.video(2, r.wide, R(0, 0, 1088, 480), 1088, 480, &err);
        VideoLayer *b = r.video(3, r.wide, R(1100, 400, 1088, 480), 1088, 480, &err);
        ok("a third video whose target's would have to compose wider than 1920: refused",
           m && a && !b && err == "the target's videos together span more than 1920x720");
    }
    {
        CompositorLimits lim = one_buffer();
        lim.pixel_bytes = 6815744;   /* the two videos fit, with the composite they do not */
        Rig r(0, lim);
        std::string err;
        VideoLayer *a = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
        VideoLayer *b = r.video(2, r.map, R(0, 0, 320, 240), 320, 240, &err);
        ok("the composite's memory counts: refused past the service's pixel memory",
           a && !b && err == "the service's pixel memory is full");
    }
}

static void two_buffers()
{
    Rig r(5500 * 1024);
    std::string err;
    std::vector<VideoLayer *> v;
    const std::vector<uint8_t> big = frame(W, H, 1), small = frame(320, 240, 2);
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    m->offer(big.data(), r.now);
    VideoLayer *p = r.video(2, r.panel, R(416, 120, W, H), W, H, &err);
    p->offer(big.data(), r.now);
    r.show("map");
    r.show("panel");
    r.run(200, 10);
    /* Joins and leaves on both targets, moves that remake a composite, a
     * scaled video taking a buffer, a scene taken down and back. */
    for (int round = 0; round < 3; round++) {
        VideoLayer *a = r.video(10 + round, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
        VideoLayer *b = r.video(20 + round, r.panel, R(600, 200, 320, 240), 320, 240, &err, 0.5f, 1);
        VideoLayer *c = r.video(30 + round, r.panel, R(0, 0, 320, 240), 320, 240, &err, 1.0f, 2);
        for (VideoLayer *x : {a, b, c})
            if (x)
                x->offer(small.data(), r.now);
        r.run(200, 10);
        if (c) {
            c->params.rect.y = 480;
            r.comp->changed(c);
            r.run(100, 10);
        }
        const TargetSpec &spec = r.slots.targets()[r.panel];
        r.comp->unpublish(spec);
        r.comp->publish(spec, &err);
        r.run(100, 10);
        r.comp->remove_owner(10 + round);
        r.run(100, 10);
        VideoLayer *s = r.video(40 + round, r.hud, R(0, 0, 400, 240), 320, 240, &err);   /* a third buffer: refused */
        ok(round ? "again" : "a video on a third target, both buffers in use: refused", !s);
        r.comp->remove_owner(20 + round);
        r.comp->remove_owner(30 + round);
        r.run(100, 10);
    }
    char name[160];
    std::snprintf(name, sizeof name,
                  "videos joining, leaving, moving and scenes made again: at most %d video buffers on the display "
                  "at any call", r.gpu.atlases.max);
    ok(name, r.gpu.atlases.max <= 2);
    std::snprintf(name, sizeof name, "  and at most %d in the scenes as flushed, across targets",
                  r.gpu.atlases.flushed_max);
    ok(name, r.gpu.atlases.flushed_max <= 2);
    ok("and back to one video on each target: each draws itself", !r.comp->composite(r.map) &&
                                                                   !r.comp->composite(r.panel) && !m->source() &&
                                                                   !p->source());
}

/* Each target's scene reaches the display with its own flush, the map's
 * before the panel's: a target makes video buffers only once the display
 * can hold them with what the other targets' scenes hold as flushed. */
static void across_targets()
{
    const std::vector<uint8_t> big = frame(W, H, 1), small = frame(320, 240, 2);
    {
        Rig r(5500 * 1024);
        std::string err;
        VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
        VideoLayer *a = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
        VideoLayer *p = r.video(3, r.panel, R(0, 0, 320, 240), 320, 240, &err);
        for (VideoLayer *x : {a, p})
            x->offer(small.data(), r.now);
        m->offer(big.data(), r.now);
        r.show("map");
        r.show("panel");
        r.run(300, 10);
        const bool composed = r.comp->composite(r.map) && !r.comp->composite(r.panel);
        r.comp->remove_owner(3);
        r.run(300, 10);
        char name[200];
        std::snprintf(name, sizeof name,
                      "the panel's video leaving while the map composes two: the map's separate again only once the "
                      "panel's buffers went (at most %d on the display)",
                      r.gpu.atlases.flushed_max);
        ok(name, composed && r.gpu.atlases.flushed_max <= 2 && !r.comp->composite(r.map) && m->attached() &&
                     a->attached());
    }
    {
        Rig r(5500 * 1024);
        std::string err;
        LayerParams ip;
        ip.rect = R(0, 0, 64, 64);
        ip.contend = true;
        r.comp->add_image(1, r.map, ip, 64, 64, 30, Sampling::Bilinear, r.now, &err);
        VideoLayer *p = r.video(2, r.panel, R(0, 0, 320, 240), 320, 240, &err);
        VideoLayer *q = r.video(3, r.panel, R(400, 0, 320, 240), 320, 240, &err);
        for (VideoLayer *x : {p, q})
            x->offer(small.data(), r.now);
        r.show("map");
        r.show("panel");
        r.run(300, 10);
        VideoLayer *v = r.video(4, r.map, R(0, 0, W, H), W, H, &err);
        v->offer(big.data(), r.now);
        r.run(300, 10);
        char name[200];
        std::snprintf(name, sizeof name,
                      "a third video on a shown map while the panel draws two: the map's made only once the panel's "
                      "composite took their place (at most %d on the display)",
                      r.gpu.atlases.flushed_max);
        ok(name, r.gpu.atlases.flushed_max <= 2 && r.comp->composite(r.panel) && v->attached());
    }
}

/* 20 s at 30 fps: a moving map and two moving cameras over it on the map, on
 * a 5500 KB/s budget. */
static void budget()
{
    const uint64_t rate = 5500 * 1024, secs = 20;
    Rig r(rate);
    std::string err;
    VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
    VideoLayer *a = r.video(2, r.map, R(64, 64, 320, 240), 320, 240, &err, 1.0f, 1);
    VideoLayer *b = r.video(3, r.map, R(640, 200, 320, 240), 320, 240, &err, 0.7f, 2);
    std::vector<uint8_t> fm, fa, fb;
    for (int k = 0; k < 600; k++) {
        trace_frame(fm, W, H, k * 4, k, 0);
        trace_frame(fa, 320, 240, k * 8, 0, 0);
        trace_frame(fb, 320, 240, 0, k, k);
        m->offer(fm.data(), r.now);
        a->offer(fa.data(), r.now);
        b->offer(fb.data(), r.now);
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0)
                r.slots.on_shown(r.slots.targets()[r.map].scene_id, r.now);
        }
    }
    FakeGraph *g = r.g("map");
    const WriteStats w = r.comp->write_stats(r.map);
    char name[200];
    std::snprintf(name, sizeof name,
                  "three moving videos on the map, 20 s: %llu KB written, within 5500 KB/s (%llu KB), %llu frames",
                  (unsigned long long)(g->flushed_bytes / 1024), (unsigned long long)(rate * secs / 1024),
                  (unsigned long long)w.frames);
    /* The first whole frame goes as debt: at most one atlas past the rate. */
    ok(name, g->flushed_bytes <= rate * secs + rate / 4 + (uint64_t)W * crk::atlas_height(H) && w.frames > 300);
}

/* ---- the cost ------------------------------------------------------------------ */

static double wall_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* compose(): the map and a camera; kind 0 every tile of the map changing, 1
 * one tile of the camera, 2 the camera at 0.5 over the map, both changing. */
static void cost()
{
    const char *names[] = {"full frame (the map changes everywhere, an opaque camera over it)",
                           "partial (one 64x64 tile of the camera changes)",
                           "translucent overlap (a camera at 0.5 over the map, both changing everywhere)"};
    const int frames = 300;
    for (int kind = 0; kind < 3; kind++) {
        Rig r(0, one_buffer());
        std::string err;
        VideoLayer *m = r.video(1, r.map, R(0, 0, W, H), W, H, &err);
        VideoLayer *c = r.video(2, r.map, R(704, 192, 320, 240), 320, 240, &err, kind == 2 ? 0.5f : 1.0f, 1);
        r.show("map");
        std::vector<std::vector<uint8_t>> fm, fc;
        for (int k = 0; k < 2; k++) {
            fm.push_back(rounded(frame(W, H, k)));
            fc.push_back(rounded(frame(320, 240, k)));
        }
        m->offer(fm[0].data(), r.now);
        c->offer(fc[0].data(), r.now);
        r.run(50);
        CompositeVideo *cv = const_cast<CompositeVideo *>(r.comp->composite(r.map));
        std::vector<uint8_t> part = fc[0];
        double wall = 0;
        const uint64_t t0 = cv->composed_tiles();
        for (int k = 1; k <= frames; k++) {
            /* Offered outside the timing: the copy into the layer is the
             * single video's too. */
            if (kind == 1) {
                for (uint32_t y = 64; y < 128; y++)
                    std::memset(&part[(size_t)y * 320 + 64], (uint8_t)(k * 8), 64);
                c->offer(part.data(), r.now);
            } else {
                m->offer(fm[k % 2].data(), r.now);
                c->offer(fc[k % 2].data(), r.now);
            }
            r.now += 34;   /* the composite's 30 fps clock due again */
            const double w0 = wall_ms();
            cv->compose(r.now);
            wall += wall_ms() - w0;
        }
        char name[260];
        std::snprintf(name, sizeof name, "cost, %s: %.3f ms a frame (%.1f ms a second at 30 fps), %.1f tiles a frame",
                      names[kind], wall / frames, wall / frames * 30, (double)(cv->composed_tiles() - t0) / frames);
        const double tiles = (double)(cv->composed_tiles() - t0) / frames;
        ok(name, tiles == (kind == 1 ? 1.0 : 136.0));
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   /* each line out as it is decided */
    single_video();
    blend();
    cells();
    two_videos();
    three_videos();
    dirty_tiles();
    leaving();
    cap_rule();
    refused_attach();
    sliding();
    moves_with_canvas();
    odd_steps();
    shrink_memory();
    held_changes();
    even_origin();
    limits();
    two_buffers();
    across_targets();
    budget();
    cost();
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
