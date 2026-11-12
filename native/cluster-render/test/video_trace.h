/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Everything the compositor sends a renderer for one or two videos, as a trace:
 * every Graph call with its arguments, the pixels of every write and each
 * flush's outcome, folded into one hash, with the calls, write bytes and
 * flushes counted. Two builds whose traces match drive the display the same
 * way, byte for byte.
 *
 * The scenarios use only the service's layer interface, so the same file
 * builds against an older tree to give the expected values (video_composite_test.cpp). */
#ifndef CRS_VIDEO_TRACE_H
#define CRS_VIDEO_TRACE_H

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../kit/yuv_atlas.h"
#include "../src/compositor.h"
#include "fake_graph.h"

namespace crs {

struct Trace {
    uint64_t hash = 1469598103934665603ull;
    uint64_t calls = 0, write_bytes = 0, flushes = 0, failed_flushes = 0, failed_writes = 0;
    /* A scenario's first and second videos' write statistics and status
     * lines. */
    WriteStats video, camera;
    std::string video_stats, camera_stats;

    void mix(const void *p, size_t n)
    {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; i++) {
            hash ^= b[i];
            hash *= 1099511628211ull;
        }
    }
    template <class T> void val(const T &v) { mix(&v, sizeof v); }
    void str(const char *s) { mix(s, std::strlen(s) + 1); }
    void call(const char *name)
    {
        calls++;
        str(name);
    }
};

/* Forwards to a FakeGraph and traces. */
class TraceGraph : public Graph {
public:
    TraceGraph(std::unique_ptr<Graph> g, Trace *t) : g_(std::move(g)), t_(t) {}

    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s) override
    {
        t_->call("buffer"), t_->val(f), t_->val(w), t_->val(h), t_->val(s);
        return ret(g_->buffer(f, w, h, s));
    }
    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap) override
    {
        t_->call("buffer2"), t_->val(f), t_->val(w), t_->val(h), t_->val(s), t_->val(wrap);
        return ret(g_->buffer(f, w, h, s, wrap));
    }
    bool write(int buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px) override
    {
        t_->call("write"), t_->val(buf), t_->val(x), t_->val(y), t_->val(w), t_->val(h);
        const bool ok = g_->write(buf, x, y, w, h, px);
        auto it = static_cast<FakeGraph *>(g_.get())->pending.buffers.find(buf);
        if (ok && it != static_cast<FakeGraph *>(g_.get())->pending.buffers.end()) {
            const size_t n = (size_t)w * h * (it->second.f == TexFormat::R8 ? 1 : 4);
            t_->mix(px, n);
            t_->write_bytes += n;
        }
        t_->failed_writes += !ok;
        return okv(ok);
    }
    int quad(int effect, const Rect &r) override
    {
        t_->call("quad"), t_->val(effect), rect(r);
        return ret(g_->quad(effect, r));
    }
    bool place(int q, const Rect &r) override
    {
        t_->call("place"), t_->val(q), rect(r);
        return okv(g_->place(q, r));
    }
    bool turn(int q, const Turn &tn) override
    {
        t_->call("turn"), t_->val(q), t_->val(tn.rotate), t_->val(tn.sx), t_->val(tn.sy), t_->val(tn.px),
            t_->val(tn.py);
        return okv(g_->turn(q, tn));
    }
    bool texture(int q, const char *u, int buf) override
    {
        t_->call("texture"), t_->val(q), t_->str(u), t_->val(buf);
        return okv(g_->texture(q, u, buf));
    }
    bool uniform(int q, const char *name, const float *v, int n) override
    {
        t_->call("uniform"), t_->val(q), t_->str(name), t_->val(n), t_->mix(v, sizeof(float) * n);
        return okv(g_->uniform(q, name, v, n));
    }
    bool uniform_array(int q, const char *name, UniformType ty, const float *v, uint32_t e) override
    {
        t_->call("uniform_array"), t_->val(q), t_->str(name), t_->val(ty), t_->val(e);
        t_->mix(v, sizeof(float) * uniform_width(ty) * e);
        return okv(g_->uniform_array(q, name, ty, v, e));
    }
    bool show(int q, bool on) override
    {
        t_->call("show"), t_->val(q), t_->val(on);
        return okv(g_->show(q, on));
    }
    bool order(int q, int32_t o) override
    {
        t_->call("order"), t_->val(q), t_->val(o);
        return okv(g_->order(q, o));
    }
    void drop_quad(int q) override
    {
        t_->call("drop_quad"), t_->val(q);
        g_->drop_quad(q);
    }
    void drop_buffer(int b) override
    {
        t_->call("drop_buffer"), t_->val(b);
        g_->drop_buffer(b);
    }
    int view(const Rect &r, const Projection &p, const Pose &c) override
    {
        t_->call("view"), rect(r);
        return ret(g_->view(r, p, c));
    }
    bool camera(int v, const Projection &p, const Pose &c) override
    {
        t_->call("camera"), t_->val(v);
        return okv(g_->camera(v, p, c));
    }
    void drop_view(int v) override
    {
        t_->call("drop_view"), t_->val(v);
        g_->drop_view(v);
    }
    int part(int v, int e, const PartLook &l) override
    {
        t_->call("part"), t_->val(v), t_->val(e);
        return ret(g_->part(v, e, l));
    }
    bool geometry(int p, const Geometry &g) override
    {
        t_->call("geometry"), t_->val(p);
        return okv(g_->geometry(p, g));
    }
    bool pose(int p, const Pose &ps) override
    {
        t_->call("pose"), t_->val(p);
        return okv(g_->pose(p, ps));
    }
    void drop_part(int p) override
    {
        t_->call("drop_part"), t_->val(p);
        g_->drop_part(p);
    }
    int animate(int n, const Animation &a) override
    {
        t_->call("animate"), t_->val(n);
        return ret(g_->animate(n, a));
    }
    void stop(int a) override
    {
        t_->call("stop"), t_->val(a);
        g_->stop(a);
    }
    bool flush(uint64_t tag) override
    {
        t_->call("flush"), t_->val(tag);
        const bool ok = g_->flush(tag);
        t_->flushes++;
        t_->failed_flushes += !ok;
        return okv(ok);
    }

private:
    void rect(const Rect &r) { t_->val(r.x), t_->val(r.y), t_->val(r.w), t_->val(r.h); }
    int ret(int v)
    {
        t_->val(v);
        return v;
    }
    bool okv(bool v)
    {
        t_->val(v);
        return v;
    }
    std::unique_ptr<Graph> g_;
    Trace *t_;
};

class TraceGpu : public Gpu {
public:
    FakeGpu fake;
    Trace trace;

    int effect(const char *vs, const char *fs, std::string *err) override
    {
        trace.call("effect"), trace.str(vs), trace.str(fs);
        const int e = fake.effect(vs, fs, err);
        trace.val(e);
        return e;
    }
    void drop_effect(int e) override
    {
        trace.call("drop_effect"), trace.val(e);
        fake.drop_effect(e);
    }
    unsigned attributes(int e) const override { return fake.attributes(e); }
    std::unique_ptr<Graph> scene(const TargetSpec &t, std::string *err) override
    {
        trace.call("scene"), trace.str(t.name.c_str());
        return std::unique_ptr<Graph>(new TraceGraph(fake.scene(t, err), &trace));
    }
    FakeGraph *graph(const char *t) { return fake.graph(t); }
};

struct TracePort : Port {
    bool available() const override { return true; }
    void request(const TargetSpec &) override {}
    void release(const TargetSpec &) override {}
    void take_over(const TargetSpec &, uint64_t) override {}
    void refresh_slots() override {}
};

/* The map (1088x480) and the panel (1920x720), and with hud the HUD (800x480,
 * writing from the HUD's share), a 5500 KB/s budget. */
struct TraceRig {
    TracePort port;
    TraceGpu gpu;
    SlotManager slots{&port, nullptr};
    std::unique_ptr<Compositor> comp;
    int64_t now = 50000;

    explicit TraceRig(uint64_t budget = 5500 * 1024, bool hud = false)
    {
        TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 2000;
        TargetSpec p;
        p.name = "panel"; p.slot = 0x203; p.scene_id = 0x01000400; p.width = 1920; p.height = 720; p.linger_ms = 2000;
        slots.add(m);
        slots.add(p);
        if (hud) {
            TargetSpec h;
            h.name = "hud"; h.slot = 0x323; h.scene_id = 0x01000401; h.width = 800; h.height = 480; h.linger_ms = 3000;
            h.share = true;
            slots.add(h);
        }
        GovernorConfig gc;
        gc.max_bps = budget;
        gc.floor_bps = budget / 3;
        comp.reset(new Compositor(&gpu, &slots, gc, 250));
        slots.set_scenes(comp.get());
        std::string err;
        comp->init(&err);
    }
    ~TraceRig()
    {
        comp.reset();
    }
    void step(int64_t ms)
    {
        now += ms;
        comp->arrange(now);
        slots.tick(now);
        comp->render(now);
    }
};

/* A 1088x480 map-like atlas: gradients moved by pan, a 64x64 patch changed
 * by k, noise of up to 2 levels by seed. */
inline void trace_frame(std::vector<uint8_t> &f, uint32_t w, uint32_t h, int pan, int k, int noise)
{
    const uint32_t ah = h + h / 2;
    f.resize((size_t)w * ah);
    uint32_t s = (uint32_t)noise * 2654435761u + 1;
    for (uint32_t y = 0; y < ah; y++)
        for (uint32_t x = 0; x < w; x++) {
            int v = y < h ? (int)(((x + (uint32_t)pan) + y) / 6) & 0xff : 128 + (int)((x / 32 + y / 16) % 7) - 3;
            if (y < h && x >= 320 && x < 384 && y >= 128 && y < 192)
                v = (v + k * 37) & 0xff;
            if (noise) {
                s = s * 1103515245u + 12345u;
                v += (int)((s >> 16) % 5) - 2;
                v = v < 0 ? 0 : v > 255 ? 255 : v;
            }
            f[(size_t)y * w + x] = (uint8_t)v;
        }
}

/* One video, as waze-on-cluster drives it: on the map at 0,0 (which 0) or on
 * the panel at 416,120 under an overlay image (which 1), 30 fps for 20 s:
 * still stretches with noise and repeats, a changing patch, pans fast enough
 * for packed blocks, a refused write and a refused flush, and the scene
 * taken down and published again. */
inline Trace single_video_trace(int which)
{
    TraceRig r;
    std::string err;
    LayerParams lp;
    lp.contend = true;
    const int target = which ? 1 : 0;
    lp.rect.x = which ? 416 : 0;
    lp.rect.y = which ? 120 : 0;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    const uint32_t id = r.comp->add_video(1, target, lp, vp, r.now, &err);
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, id));
    ImageLayer *img = nullptr;
    if (which) {
        LayerParams ip;
        ip.klass = Klass::Overlay;
        ip.rect.x = 600; ip.rect.y = 200; ip.rect.w = 128; ip.rect.h = 64;
        const uint32_t iid = r.comp->add_image(2, target, ip, 128, 64, 30, Sampling::Nearest, r.now, &err);
        img = static_cast<ImageLayer *>(r.comp->layer(2, iid));
    }
    std::vector<uint8_t> f, px(128 * 64 * 4);
    const uint32_t scene = which ? 0x01000400 : 0x010004f0;
    int pan = 0;
    for (int k = 0; k < 600; k++) {
        const int phase = (k / 60) % 5;
        if (phase == 1 && k % 2)
            v->repeat(r.now);
        else {
            if (phase == 3)
                pan += 24;
            trace_frame(f, 1088, 480, pan, phase == 2 ? k : 0, phase == 0 || phase == 4 ? k : 0);
            v->offer(f.data(), r.now);
        }
        if (img && k % 15 == 0) {
            for (size_t i = 0; i < px.size(); i++)
                px[i] = (uint8_t)(i + k);
            img->update({}, px.data(), px.size(), &err);
        }
        FakeGraph *g = r.gpu.graph(which ? "panel" : "map");
        if (g && k == 250)
            g->write_budget = (long)g->written + 20000;   /* the next big write is refused part way */
        if (g && k == 262)
            g->write_budget = -1;
        if (g && k == 190)
            g->fail_flush = true;
        if (g && k == 193)
            g->fail_flush = false;
        if (k == 470) {
            const TargetSpec &spec = r.slots.targets()[target];
            r.comp->unpublish(spec);
            r.comp->publish(spec, &err);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0)
                r.slots.on_shown(scene, r.now);
        }
    }
    r.comp->remove_owner(1);
    r.comp->remove_owner(2);
    r.step(11);
    return r.gpu.trace;
}

/* Two videos, as on the display's two video buffers: a map as above on
 * the map at 0,0 and a 320x240 camera moving every frame, on the map over
 * it at 704,64 (which 0) or on the panel at 96,96 (which 1), at opacity and
 * turned by rotate degrees. 30 fps for 20 s with the map's still, noisy,
 * repeated, patched and panning stretches, a refused write and a refused
 * flush on the map, and the map's scene taken down and published again. */
inline Trace two_video_trace(int which, float opacity = 1.0f, float rotate = 0)
{
    TraceRig r;
    std::string err;
    LayerParams lp;
    lp.contend = true;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, 0, lp, vp, r.now, &err)));
    LayerParams cp;
    cp.contend = true;
    cp.z = 1;
    cp.rect.x = which ? 96 : 704;
    cp.rect.y = which ? 96 : 64;
    cp.rect.w = 320;
    cp.rect.h = 240;
    cp.opacity = opacity;
    cp.rotate = rotate;
    VideoParams cvp;
    cvp.w = 320;
    cvp.h = 240;
    VideoLayer *c =
        static_cast<VideoLayer *>(r.comp->layer(2, r.comp->add_video(2, which ? 1 : 0, cp, cvp, r.now, &err)));
    std::vector<uint8_t> f, cf;
    int pan = 0;
    for (int k = 0; k < 600; k++) {
        const int phase = (k / 60) % 5;
        if (phase == 1 && k % 2)
            v->repeat(r.now);
        else {
            if (phase == 3)
                pan += 24;
            trace_frame(f, 1088, 480, pan, phase == 2 ? k : 0, phase == 0 || phase == 4 ? k : 0);
            v->offer(f.data(), r.now);
        }
        trace_frame(cf, 320, 240, k * 8, k, k % 3 ? 0 : k);
        c->offer(cf.data(), r.now);
        FakeGraph *g = r.gpu.graph("map");
        if (g && k == 250)
            g->write_budget = (long)g->written + 20000;
        if (g && k == 262)
            g->write_budget = -1;
        if (g && k == 190)
            g->fail_flush = true;
        if (g && k == 193)
            g->fail_flush = false;
        if (k == 470) {
            const TargetSpec &spec = r.slots.targets()[0];
            r.comp->unpublish(spec);
            r.comp->publish(spec, &err);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0) {
                r.slots.on_shown(0x010004f0, r.now);
                if (which)
                    r.slots.on_shown(0x01000400, r.now);
            }
        }
    }
    r.comp->remove_owner(2);
    r.comp->remove_owner(1);
    r.step(11);
    return r.gpu.trace;
}

/* The map as above, alone and shown, then a 320x240 camera added at 2 s, on
 * the map over it (which 0) or on the panel (which 1), between two passes
 * as a socket message is; sets at 5 s (the
 * camera moved and at 0.8), 8 s and 9 s (the map hidden and shown) and 12 s
 * (the map moved by 4 pixels). */
inline Trace join_trace(int which)
{
    TraceRig r;
    std::string err;
    LayerParams lp;
    lp.contend = true;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, 0, lp, vp, r.now, &err)));
    VideoLayer *c = nullptr;
    std::vector<uint8_t> f, cf;
    for (int k = 0; k < 450; k++) {
        const int phase = (k / 60) % 4;
        trace_frame(f, 1088, 480, phase == 3 ? k * 8 : 0, phase == 1 ? k : 0, phase == 2 ? k : 0);
        if (phase == 0 && k % 2)
            v->repeat(r.now);
        else
            v->offer(f.data(), r.now);
        if (k == 60) {
            LayerParams cp;
            cp.contend = true;
            cp.z = 1;
            cp.rect.x = which ? 96 : 704;
            cp.rect.y = which ? 96 : 64;
            cp.rect.w = 320;
            cp.rect.h = 240;
            VideoParams cvp;
            cvp.w = 320;
            cvp.h = 240;
            /* As from the socket: between two passes. */
            c = static_cast<VideoLayer *>(
                r.comp->layer(2, r.comp->add_video(2, which ? 1 : 0, cp, cvp, r.now + 5, &err)));
        }
        if (c) {
            trace_frame(cf, 320, 240, k * 8, k, 0);
            c->offer(cf.data(), r.now);
        }
        if (k == 150) {
            c->params.rect.x += 64;
            c->params.opacity = 0.8f;
            r.comp->changed(c);
        }
        if (k == 240 || k == 270) {
            v->params.visible = k == 270;
            r.comp->changed(v);
        }
        if (k == 360) {
            v->params.rect.x += 4;
            r.comp->changed(v);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0)
                r.slots.on_shown(0x010004f0, r.now);
            if (which && k == 60 && t == 0)
                r.slots.on_shown(0x01000400, r.now);
        }
    }
    r.comp->remove_owner(2);
    r.comp->remove_owner(1);
    r.step(11);
    return r.gpu.trace;
}

/* Waze on the cluster and the HUD at once: the map as above on the map at
 * 0,0, and a 448x208 picture of it on the HUD at 176,136 at 15 fps with no
 * packed blocks, writing from the HUD's share, for 20 s; nothing over
 * either. */
inline Trace hud_trace()
{
    TraceRig r(5500 * 1024, true);
    std::string err;
    LayerParams lp;
    lp.contend = true;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, 0, lp, vp, r.now, &err)));
    LayerParams hp;
    hp.contend = true;
    hp.rect.x = 176;
    hp.rect.y = 136;
    hp.rect.w = 448;
    hp.rect.h = 208;
    VideoParams hvp;
    hvp.w = 448;
    hvp.h = 208;
    hvp.fps = 15;
    hvp.pan_blocks = false;
    VideoLayer *h = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, 2, hp, hvp, r.now, &err)));
    std::vector<uint8_t> f, hf;
    int pan = 0;
    for (int k = 0; k < 600; k++) {
        const int phase = (k / 60) % 5;
        if (phase == 1 && k % 2) {
            v->repeat(r.now);
            h->repeat(r.now);
        } else {
            if (phase == 3)
                pan += 24;
            trace_frame(f, 1088, 480, pan, phase == 2 ? k : 0, phase == 0 || phase == 4 ? k : 0);
            v->offer(f.data(), r.now);
            trace_frame(hf, 448, 208, pan / 2, phase == 2 ? k : 0, phase == 0 || phase == 4 ? k : 0);
            h->offer(hf.data(), r.now);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0) {
                r.slots.on_shown(0x010004f0, r.now);
                r.slots.on_shown(0x01000401, r.now);
            }
        }
    }
    r.comp->remove_owner(1);
    r.step(11);
    return r.gpu.trace;
}

/* The map as above on the map at 0,0 under a 1088x480 overlay image at
 * opacity 1, redrawn every half second: opaque everywhere but one pixel in
 * every 8x8 block (alpha 254), so no part of it is wholly opaque. 20 s. */
inline Trace overlay_trace()
{
    TraceRig r;
    std::string err;
    LayerParams lp;
    lp.contend = true;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, 0, lp, vp, r.now, &err)));
    LayerParams ip = lp;
    ip.klass = Klass::Overlay;
    ImageLayer *img = static_cast<ImageLayer *>(
        r.comp->layer(2, r.comp->add_image(2, 0, ip, 1088, 480, 30, Sampling::Bilinear, r.now, &err)));
    std::vector<uint8_t> f, px((size_t)1088 * 480 * 4);
    int pan = 0;
    for (int k = 0; k < 600; k++) {
        const int phase = (k / 60) % 5;
        if (phase == 1 && k % 2)
            v->repeat(r.now);
        else {
            if (phase == 3)
                pan += 24;
            trace_frame(f, 1088, 480, pan, phase == 2 ? k : 0, phase == 0 || phase == 4 ? k : 0);
            v->offer(f.data(), r.now);
        }
        if (k % 15 == 0) {
            for (size_t i = 0; i < px.size(); i += 4) {
                const size_t x = (i / 4) % 1088, y = (i / 4) / 1088;
                px[i] = (uint8_t)(x + k), px[i + 1] = (uint8_t)y, px[i + 2] = (uint8_t)k;
                px[i + 3] = x % 8 == (size_t)(k / 15) % 8 && y % 8 == 3 ? 254 : 255;
            }
            img->update({}, px.data(), px.size(), &err);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0)
                r.slots.on_shown(0x010004f0, r.now);
        }
    }
    r.comp->remove_owner(2);
    r.comp->remove_owner(1);
    r.step(11);
    return r.gpu.trace;
}

/* A Waze-like map for a minute, with a camera over it: the map 1088x480 at
 * 30 fps on the map at 0,0, in 6 s stretches: still with decoder noise, the
 * position marker moving (a changing 64x64 patch), a fast pan (packed
 * blocks), a slow pan, still again; a 320x240 camera changing every frame
 * at 30 fps over it at 704,64 at opacity (kind 0: 1, kind 1: 0.9), or
 * (kind 2) opaque, sliding 4 px a frame between x 64 and 704; or an opaque
 * camera as large as the map at 0,0 (kind 3) or 800x480 at 0,0 (kind 4),
 * changing everywhere every frame. The bytes written are the trace's
 * write_bytes. */
inline Trace camera_minute_trace(int kind)
{
    TraceRig r;
    std::string err;
    LayerParams lp;
    lp.contend = true;
    lp.rect.w = 1088;
    lp.rect.h = 480;
    VideoParams vp;
    VideoLayer *v = static_cast<VideoLayer *>(r.comp->layer(1, r.comp->add_video(1, 0, lp, vp, r.now, &err)));
    LayerParams cp;
    cp.contend = true;
    cp.z = 1;
    const uint32_t cw = kind == 3 ? 1088 : kind == 4 ? 800 : 320, ch = kind >= 3 ? 480 : 240;
    cp.rect.x = kind >= 3 ? 0 : 704;
    cp.rect.y = kind >= 3 ? 0 : 64;
    cp.rect.w = cw;
    cp.rect.h = ch;
    cp.opacity = kind == 1 ? 0.9f : 1.0f;
    VideoParams cvp;
    cvp.w = cw;
    cvp.h = ch;
    VideoLayer *c = static_cast<VideoLayer *>(r.comp->layer(2, r.comp->add_video(2, 0, cp, cvp, r.now, &err)));
    std::vector<uint8_t> f, cf;
    int pan = 0, x = 704, dx = -4;
    for (int k = 0; k < 1800; k++) {
        const int phase = (k / 180) % 5;
        if (phase == 2)
            pan += 24;
        if (phase == 3)
            pan += 4;
        trace_frame(f, 1088, 480, pan, phase == 1 ? k : 0, k);
        v->offer(f.data(), r.now);
        trace_frame(cf, cw, ch, k * 8, k, k);
        c->offer(cf.data(), r.now);
        if (kind == 2) {
            if (x + dx < 64 || x + dx > 704)
                dx = -dx;
            x += dx;
            c->params.rect.x = x;
            r.comp->changed(c);
        }
        for (int t = 0; t < 33; t += 11) {
            r.step(11);
            if (k == 0 && t == 0)
                r.slots.on_shown(0x010004f0, r.now);
        }
    }
    r.gpu.trace.video = v->write_stats();
    r.gpu.trace.video_stats = v->stats();
    r.gpu.trace.camera = c->write_stats();
    r.gpu.trace.camera_stats = c->stats();
    r.comp->remove_owner(2);
    r.comp->remove_owner(1);
    r.step(11);
    return r.gpu.trace;
}

}  // namespace crs

#endif
