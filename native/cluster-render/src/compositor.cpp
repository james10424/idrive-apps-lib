/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "compositor.h"

#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "block_pack.h"
#include "log.h"
#include "yuv_atlas.h"

namespace crs {

/* A quad placed in the target in pixels by u_mvp; v_uv runs from the quad's
 * top-left. GLSL ES 1.00, like the HMI's own effects. */
const char *const kQuadVertexShader = R"(#version 100
attribute vec2 a_position;
uniform mat4 u_mvp;
varying vec2 v_uv;
void main() {
    v_uv = vec2(a_position.x, 1.0 - a_position.y);
    gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
})";

std::vector<int> plan_passes(const std::vector<bool> &views)
{
    std::vector<int> pass(views.size());
    int p = 0;
    bool after_view = false;
    for (size_t i = 0; i < views.size(); i++) {
        if (views[i] || after_view)
            p++;
        after_view = views[i];
        pass[i] = p;
    }
    return pass;
}

namespace {

/* An image layer's pixels, straight alpha, at u_opacity. */
const char *const kImageFragmentShader = R"(#version 100
precision mediump float;
uniform sampler2D u_image;
uniform float u_opacity;
varying vec2 v_uv;
void main() {
    vec4 c = texture2D(u_image, v_uv);
    gl_FragColor = vec4(c.rgb, c.a * u_opacity);
})";

}  // namespace

const char *layer_state_name(LayerState s)
{
    switch (s) {
    case LayerState::Hidden: return "hidden";
    case LayerState::Stalled: return "stalled";
    case LayerState::Shown: return "shown";
    case LayerState::Suspended: return "suspended";
    case LayerState::Error: return "error";
    default: return "waiting";
    }
}

namespace {

/* Drawn by a composite: unturned, at its frame's size, at even x and y, so
 * its planes line up with the canvas's. */
bool composable(const LayerParams &p, const VideoParams &vp)
{
    return p.rotate == 0 && p.sx == 1 && p.sy == 1 && p.rect.w == vp.w && p.rect.h == vp.h && !(p.rect.x & 1) &&
           !(p.rect.y & 1);
}

int64_t steady_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

}  // namespace

Compositor::Compositor(Gpu *gpu, SlotManager *slots, const GovernorConfig &budget, int upload_burst_ms,
                       const CompositorLimits &limits)
    : gpu_(gpu), slots_(slots), limits_(limits), governor_(budget), pacer_(budget.max_bps, upload_burst_ms),
      clock_us_(steady_us)
{
    share_.share_of(&pacer_);
}

Compositor::~Compositor()
{
    for (auto &kv : layers_) {
        kv.second.layer->detach(false);
        drop_effects(kv.second);
    }
    for (auto &kv : assets_)
        kv.second.asset->detach(false);
    for (Target &t : targets_) {
        if (t.composite)
            t.composite->detach(false);
        t.file.reset();    /* before a graph that borrows its scene */
        t.graph.reset();
    }
    if (atlas_view_fx_) gpu_->drop_effect(atlas_view_fx_);
    if (block_view_fx_) gpu_->drop_effect(block_view_fx_);
    if (atlas_fx_) gpu_->drop_effect(atlas_fx_);
    if (block_fx_) gpu_->drop_effect(block_fx_);
    if (image_fx_) gpu_->drop_effect(image_fx_);
}

bool Compositor::init(std::string *err)
{
    targets_.resize(slots_->targets().size());
    atlas_fx_ = gpu_->effect(kQuadVertexShader, crk::kAtlasFragmentShader, err);
    if (atlas_fx_)
        block_fx_ = gpu_->effect(kQuadVertexShader, crk::kBlockFragmentShader, err);
    if (block_fx_)
        image_fx_ = gpu_->effect(kQuadVertexShader, kImageFragmentShader, err);
    return image_fx_ != 0;
}

uint32_t Compositor::admit(uint64_t owner, int target, int64_t pixel_bytes, std::string *err)
{
    if (target < 0 || target >= (int)targets_.size()) {
        *err = "unknown target";
        return 0;
    }
    uint32_t mine = 0;
    for (const auto &kv : layers_)
        mine += kv.second.owner == owner;
    if (layers_.size() >= limits_.layers || mine >= limits_.per_client) {
        *err = "too many layers";
        return 0;
    }
    if ((int64_t)memory() + pixel_bytes > (int64_t)limits_.pixel_bytes) {
        *err = "the service's pixel memory is full";
        return 0;
    }
    return next_id_++;
}

uint64_t Compositor::memory() const
{
    uint64_t bytes = 0;
    for (const auto &kv : layers_)
        bytes += kv.second.pixel_bytes;
    for (const auto &kv : assets_)
        bytes += kv.second.asset->memory();
    for (const Target &t : targets_)
        bytes += t.composite_bytes;
    return bytes;
}

void Compositor::drop_effects(Entry &e)
{
    if (e.effect)
        gpu_->drop_effect(e.effect);
    if (std::string(e.layer->kind()) == "mesh")
        for (int fx : static_cast<MeshLayer *>(e.layer.get())->effects())
            gpu_->drop_effect(fx);
}

bool Compositor::assets_have(const Entry &e) const
{
    for (const auto &kv : e.binds) {
        auto a = assets_.find(kv.second);
        if (a == assets_.end() || !a->second.asset->have())
            return false;
    }
    return true;
}

uint32_t Compositor::asset_refused(const Entry &e) const
{
    for (const auto &kv : e.binds) {
        auto a = assets_.find(kv.second);
        if (a != assets_.end() && !a->second.asset->refused().empty())
            return kv.second;
    }
    return 0;
}

bool Compositor::assets_clean(const Entry &e) const
{
    for (const auto &kv : e.binds) {
        auto a = assets_.find(kv.second);
        if (a == assets_.end() || !a->second.asset->clean())
            return false;
    }
    return true;
}

void Compositor::apply_binds(Entry &e)
{
    Target &t = targets_[e.layer->target()];
    if (!t.graph || !e.layer->attached())
        return;
    for (const auto &kv : e.binds) {
        const int d = e.layer->drawable(kv.first.first);
        auto a = assets_.find(kv.second);
        const int buf = a == assets_.end() ? 0 : a->second.asset->buffer();
        if (!buf && a != assets_.end() && a->second.asset->compressed())
            continue;   /* bound once its texture is made */
        if (!d || !buf || !t.graph->texture(d, kv.first.second.c_str(), buf))
            log_warn("layer %u: %s not bound to asset %u", e.layer->id(), kv.first.second.c_str(), kv.second);
    }
    t.dirty = true;
}

void Compositor::attach(Entry &e, Target &t)
{
    std::string err;
    e.drawn = false;
    e.order = -1;   /* arranged again: shown and ordered in the same flush */
    e.flushed = Look();
    if (e.held) {
        e.held = false;
        t.held_n--;
        e.layer->hold_scene(nullptr);
    }
    mark(e, e.layer->id(), t);
    if (!e.layer->attach(t.graph.get(), now_ms_, &err)) {
        e.error = err;
        log_error("layer %u (%s): %s", e.layer->id(), e.layer->kind(), err.c_str());
    } else {
        e.error.clear();
    }
    t.dirty = true;
}

uint32_t Compositor::add_video(uint64_t owner, int target, const LayerParams &lp, const VideoParams &vp,
                               int64_t now_ms, std::string *err)
{
    if (const char *e = VideoLayer::size_error(vp.w, vp.h)) {
        *err = e;
        return 0;
    }
    if (target < 0 || target >= (int)targets_.size()) {
        *err = "unknown target";
        return 0;
    }
    uint32_t videos = 0;
    for (const auto &kv : layers_)
        videos += std::string(kv.second.layer->kind()) == "video";
    if (videos >= limits_.client_videos) {
        *err = "too many video layers";
        return 0;
    }
    std::vector<VideoSpec> specs = video_specs(), real = video_specs(true);
    VideoSpec mine;
    mine.target = target;
    mine.params = lp;
    mine.vp = vp;
    mine.composable = composable(lp, vp);
    specs.push_back(mine);
    real.push_back(mine);
    uint64_t composites = 0;
    if (!videos_fit_both(specs, real, &composites, err))
        return 0;
    uint64_t now_composites = 0;
    for (const Target &t : targets_)
        now_composites += t.composite_bytes;
    const size_t atlas = (size_t)vp.w * crk::atlas_height(vp.h);
    const size_t bytes = atlas * 4 + (vp.pan_blocks ? crk::block_bytes(vp.w, vp.h) : 0);
    const uint32_t id = admit(owner, target, (int64_t)bytes + (int64_t)composites - (int64_t)now_composites, err);
    if (!id)
        return 0;
    Entry &e = layers_[id];
    e.layer.reset(new VideoLayer(id, target, ++seq_, pacer_for(target), atlas_fx_, block_fx_, vp, now_ms));
    e.layer->params = lp;
    e.owner = owner;
    e.pixel_bytes = bytes;
    reconcile();
    return id;
}

uint32_t Compositor::add_image(uint64_t owner, int target, const LayerParams &lp, uint32_t w, uint32_t h,
                               uint32_t fps, Sampling s, int64_t now_ms, std::string *err)
{
    if (const char *e = ImageLayer::size_error(w, h)) {
        *err = e;
        return 0;
    }
    const size_t bytes = (size_t)w * h * 4 * 2;
    const uint32_t id = admit(owner, target, bytes, err);
    if (!id)
        return 0;
    Entry &e = layers_[id];
    e.layer.reset(new ImageLayer(id, target, ++seq_, pacer_for(target), image_fx_, w, h, fps, s, now_ms));
    e.layer->params = lp;
    e.owner = owner;
    e.pixel_bytes = bytes;
    if (targets_[target].graph)
        attach(e, targets_[target]);
    return id;
}

uint32_t Compositor::add_effect(uint64_t owner, int target, const LayerParams &lp, const std::string &vs,
                                const std::string &fs, uint32_t time_fps, int64_t now_ms, std::string *err)
{
    if (target < 0 || target >= (int)targets_.size()) {
        *err = "unknown target";
        return 0;
    }
    const int fx = gpu_->effect(vs.empty() ? kQuadVertexShader : vs.c_str(), fs.c_str(), err);
    if (!fx)
        return 0;
    const uint32_t id = admit(owner, target, 0, err);
    if (!id) {
        gpu_->drop_effect(fx);
        return 0;
    }
    Entry &e = layers_[id];
    e.layer.reset(new EffectLayer(id, target, ++seq_, pacer_for(target), fx, time_fps, now_ms));
    e.layer->params = lp;
    e.owner = owner;
    e.effect = fx;
    if (targets_[target].graph)
        attach(e, targets_[target]);
    return id;
}

uint32_t Compositor::add_mesh(uint64_t owner, int target, const LayerParams &lp, const MeshParams &mp, int64_t,
                              std::string *err)
{
    const uint32_t id = admit(owner, target, 0, err);
    if (!id)
        return 0;
    Entry &e = layers_[id];
    e.layer.reset(new MeshLayer(id, target, ++seq_, pacer_for(target), mp));
    e.layer->params = lp;
    e.owner = owner;
    if (targets_[target].graph)
        attach(e, targets_[target]);
    return id;
}

bool Compositor::add_part(uint64_t owner, uint32_t layer, uint32_t part, const std::string &vs,
                          const std::string &fs, const PartLook &look, std::string *err)
{
    auto it = layers_.find(layer);
    if (it == layers_.end() || it->second.owner != owner) {
        *err = "no such layer";
        return false;
    }
    if (std::string(it->second.layer->kind()) != "mesh") {
        *err = "parts are for mesh layers";
        return false;
    }
    MeshLayer *m = static_cast<MeshLayer *>(it->second.layer.get());
    if (m->has_part(part)) {
        *err = "part " + std::to_string(part) + " exists";
        return false;
    }
    const int fx = gpu_->effect(vs.c_str(), fs.c_str(), err);
    if (!fx)
        return false;
    const unsigned attrs = gpu_->attributes(fx);
    if (!(attrs & kAttrPosition)) {
        gpu_->drop_effect(fx);
        *err = "a part's vertex shader reads attribute vec3 a_position";
        return false;
    }
    if (!m->add_part(part, fx, attrs, look, err)) {
        gpu_->drop_effect(fx);
        return false;
    }
    touch(m);
    return true;
}

bool Compositor::geometry(uint64_t owner, uint32_t layer, uint32_t part, MeshData &&data, int64_t now_ms,
                          std::string *err)
{
    auto it = layers_.find(layer);
    if (it == layers_.end() || it->second.owner != owner) {
        *err = "no such layer";
        return false;
    }
    Entry &e = it->second;
    if (std::string(e.layer->kind()) != "mesh") {
        *err = "geometry is for mesh layers";
        return false;
    }
    MeshLayer *m = static_cast<MeshLayer *>(e.layer.get());
    const size_t bytes = e.pixel_bytes - m->part_bytes(part) + data.bytes();
    if (memory() - e.pixel_bytes + bytes > limits_.pixel_bytes) {
        *err = "the service's pixel memory is full";
        return false;
    }
    if (!m->set_geometry(part, std::move(data), now_ms, err))
        return false;
    e.pixel_bytes = bytes;
    return true;
}

uint32_t Compositor::add_asset(uint64_t owner, int target, const AssetParams &ap, std::string *err)
{
    if (target < 0 || target >= (int)targets_.size()) {
        *err = "unknown target";
        return 0;
    }
    if (const char *e = Asset::size_error(ap.format, ap.w, ap.h)) {
        *err = e;
        return 0;
    }
    uint32_t mine = 0;
    for (const auto &kv : assets_)
        mine += kv.second.owner == owner;
    if (assets_.size() >= limits_.assets || mine >= limits_.assets_per_client) {
        *err = "too many assets";
        return 0;
    }
    const uint64_t bytes = Asset::memory(ap);
    if (memory() + bytes > limits_.pixel_bytes) {
        *err = "the service's pixel memory is full";
        return 0;
    }
    const uint32_t id = next_id_++;
    AssetEntry &a = assets_[id];
    a.asset.reset(new Asset(id, target, pacer_for(target), ap));
    a.owner = owner;
    Target &t = targets_[target];
    if (t.graph) {
        a.asset->attach(t.graph.get());
        t.dirty = true;
    }
    return id;
}

Asset *Compositor::asset(uint64_t owner, uint32_t id)
{
    auto it = assets_.find(id);
    return it == assets_.end() || it->second.owner != owner ? nullptr : it->second.asset.get();
}

bool Compositor::remove_asset(uint64_t owner, uint32_t id, std::string *err)
{
    auto it = assets_.find(id);
    if (it == assets_.end() || it->second.owner != owner) {
        *err = "no such asset";
        return false;
    }
    for (const auto &kv : layers_)
        for (const auto &b : kv.second.binds)
            if (b.second == id) {
                *err = "asset in use";
                return false;
            }
    Target &t = targets_[it->second.asset->target()];
    it->second.asset->detach(true);
    if (t.graph)
        t.dirty = true;
    assets_.erase(it);
    return true;
}

bool Compositor::bind(uint64_t owner, uint32_t layer, uint32_t part, const std::string &sampler, uint32_t asset,
                      std::string *err)
{
    auto it = layers_.find(layer);
    auto a = assets_.find(asset);
    if (it == layers_.end() || it->second.owner != owner) {
        *err = "no such layer";
        return false;
    }
    if (a == assets_.end() || a->second.owner != owner) {
        *err = "no such asset";
        return false;
    }
    Entry &e = it->second;
    const std::string kind = e.layer->kind();
    if (kind == "effect" && part) {
        *err = "effect layers have no parts";
        return false;
    }
    if (kind == "mesh" && !static_cast<MeshLayer *>(e.layer.get())->has_part(part)) {
        *err = "no such part";
        return false;
    }
    if (kind != "effect" && kind != "mesh") {
        *err = "binds are for effect layers and mesh parts";
        return false;
    }
    if (a->second.asset->target() != e.layer->target()) {
        *err = "the asset is on another target";
        return false;
    }
    const std::pair<uint32_t, std::string> key(part, sampler);
    if (!e.binds.count(key) && e.binds.size() >= kMaxBinds) {
        *err = "at most " + std::to_string(kMaxBinds) + " binds a layer";
        return false;
    }
    Target &t = targets_[e.layer->target()];
    const int d = e.layer->drawable(part);
    const int buf = a->second.asset->buffer();
    if (t.graph && d && buf) {
        if (!t.graph->texture(d, sampler.c_str(), buf)) {
            *err = "the shader has no sampler " + sampler;
            return false;
        }
        t.dirty = true;
    }
    e.binds[key] = asset;
    return true;
}

void Compositor::touch(Layer *l)
{
    Target &t = targets_[l->target()];
    if (t.graph)
        t.dirty = true;
    auto it = layers_.find(l->id());
    if (it != layers_.end())
        mark(it->second, l->id(), t);
}

void Compositor::mark(Entry &e, uint32_t id, Target &t)
{
    if (e.touched)
        return;
    e.touched = true;
    t.touched.push_back(id);
}

uint32_t Compositor::add_scene(uint64_t owner, int target, const SceneSource &src, bool contend, std::string *err)
{
    if (target < 0 || target >= (int)targets_.size()) {
        *err = "unknown target";
        return 0;
    }
    for (const auto &kv : scenes_)
        if (kv.second.target == target) {
            *err = "the target has a scene file already";
            return 0;
        }
    const uint32_t id = next_id_++;
    SceneEntry &s = scenes_[id];
    s.owner = owner;
    s.target = target;
    s.src = src;
    s.contend = contend;
    return id;
}

bool Compositor::replace_scene(uint64_t owner, uint32_t id, const SceneSource &src, bool contend)
{
    auto it = scenes_.find(id);
    if (it == scenes_.end() || it->second.owner != owner)
        return false;
    SceneEntry &s = it->second;
    s.src = src;
    s.contend = contend;
    s.error.clear();
    s.edits = SceneEdits();
    s.serial++;
    /* its hosts go with its edits; the display decides on its layers again */
    s.hosts.clear();
    s.layers_error.clear();
    host_serial_++;
    return true;
}

void Compositor::set_layers(uint32_t id, const SceneLayers &l)
{
    auto it = scenes_.find(id);
    if (it == scenes_.end())
        return;
    it->second.layers = l;
    if (!l.on)
        it->second.hosts.clear();
    host_serial_++;
}

bool Compositor::host(uint64_t owner, uint32_t id, int hosted, const Host &h, std::string *err)
{
    auto it = scenes_.find(id);
    if (it == scenes_.end() || it->second.owner != owner) {
        *err = "no such scene";
        return false;
    }
    SceneEntry &s = it->second;
    const std::vector<TargetSpec> &specs = slots_->targets();
    /* A refusal ends the scene's host of that target, if it had one: the
     * clients forget a refused host too (PROTOCOL.md "host"). */
    auto refuse = [&](const std::string &why) {
        *err = why;
        if (s.hosts.erase(hosted))
            host_serial_++;
        return false;
    };
    if (!s.layers.on)
        return refuse("a scene hosts only with layers 1");
    if (!s.layers_error.empty())
        return refuse("the display draws no layers over this scene: " + s.layers_error);
    if (!s.error.empty())
        return refuse("the scene's file did not load: " + s.error);
    if (hosted == s.target)
        return refuse("a scene hosts other targets, not its own");
    for (const auto &kv : scenes_)
        if (kv.first != id && kv.second.error.empty() && kv.second.hosts.count(hosted))
            return refuse("target " + specs[hosted].name + " is hosted by another scene");
    auto inside = [](const Rect &r, const TargetSpec &t) {
        return r.w && r.h && r.x >= 0 && r.y >= 0 && (uint64_t)r.x + r.w <= t.width && (uint64_t)r.y + r.h <= t.height;
    };
    if (!inside(h.rect, specs[s.target]))
        return refuse("x, y, w, h must lie inside " + specs[s.target].name);
    if (!inside(h.src, specs[hosted]))
        return refuse("src_x, src_y, src_w, src_h must lie inside " + specs[hosted].name);
    s.hosts[hosted] = h;
    host_serial_++;
    return true;
}

void Compositor::unhost(uint64_t owner, uint32_t id, int hosted)
{
    auto it = scenes_.find(id);
    if (it != scenes_.end() && it->second.owner == owner && it->second.hosts.erase(hosted))
        host_serial_++;
}

std::vector<Compositor::Hosted> Compositor::hosts() const
{
    std::vector<Hosted> out;
    for (const auto &kv : scenes_) {
        const SceneEntry &s = kv.second;
        const Target &t = targets_[s.target];
        /* Its layers drawable: its file published with the layers' graph. */
        if (s.hosts.empty() || !s.error.empty() || !t.file || !t.graph || t.file_of != kv.first ||
            t.file_serial != s.serial)
            continue;
        for (const auto &h : s.hosts) {
            Hosted x;
            x.target = h.first;
            x.into = s.target;
            x.host = h.second;
            out.push_back(x);
        }
    }
    return out;
}

SceneEdits *Compositor::edits(uint64_t owner, uint32_t id)
{
    auto it = scenes_.find(id);
    return it == scenes_.end() || it->second.owner != owner ? nullptr : &it->second.edits;
}

std::string Compositor::scene_text(uint32_t id) const
{
    auto it = scenes_.find(id);
    if (it == scenes_.end())
        return std::string();
    const SceneEntry &s = it->second;
    std::string text = "on " + slots_->targets()[s.target].name + ": " + s.src.file + " and " +
                       std::to_string(s.src.res.size()) + " resource files, " + std::to_string(s.src.bytes / 1024) +
                       " KB; " + std::to_string(s.edits.kept()) + " edit lines kept";
    if (s.layers.on)
        text += "; layers over it" + (s.layers.order_set ? " from render order " + std::to_string(s.layers.order) : "");
    for (const auto &kv : s.hosts) {
        const Host &h = kv.second;
        char b[160];
        std::snprintf(b, sizeof b, "; hosts %s at %ux%u+%d+%d from %ux%u+%d+%d%s",
                      slots_->targets()[kv.first].name.c_str(), h.rect.w, h.rect.h, h.rect.x, h.rect.y, h.src.w,
                      h.src.h, h.src.x, h.src.y, h.shown ? "" : ", not shown");
        text += b;
    }
    return text;
}

uint32_t Compositor::content(int target) const
{
    for (const auto &kv : scenes_)
        if (kv.second.target == target && kv.second.error.empty())
            return kv.first;
    return 0;
}

bool Compositor::layers_wait(int target) const
{
    const uint32_t sc = content(target);
    if (!sc)
        return false;
    const Target &t = targets_[target];
    return !scenes_.at(sc).layers.on || (t.file && !t.graph);
}

bool Compositor::stale(const TargetSpec &spec) const
{
    const int ti = slots_->index(spec.name);
    const Target &t = targets_[ti];
    return (t.graph || t.file) && content(ti) != (t.file ? t.file_of : 0);
}

bool Compositor::outdated(const TargetSpec &spec) const
{
    const Target &t = targets_[slots_->index(spec.name)];
    auto s = scenes_.find(t.file_of);
    return t.file && s != scenes_.end() && s->second.serial != t.file_serial;
}

Layer *Compositor::layer(uint64_t owner, uint32_t id)
{
    auto it = layers_.find(id);
    return it == layers_.end() || it->second.owner != owner ? nullptr : it->second.layer.get();
}

uint64_t Compositor::owner_of(uint32_t id) const
{
    auto s = scenes_.find(id);
    if (s != scenes_.end())
        return s->second.owner;
    auto it = layers_.find(id);
    return it == layers_.end() ? 0 : it->second.owner;
}

void Compositor::changed(Layer *l)
{
    Target &t = targets_[l->target()];
    auto it = layers_.find(l->id());
    /* A held layer takes the change when it is released (commit). */
    if (it == layers_.end() || !it->second.held) {
        l->place();
        if (t.graph)
            t.dirty = true;
    }
    if (it != layers_.end())
        mark(it->second, l->id(), t);
    if (std::string(l->kind()) == "video")
        reconcile();
}

void Compositor::remove(uint64_t owner, uint32_t id)
{
    /* A scene content: its scene is stale now (the slot manager). */
    auto s = scenes_.find(id);
    if (s != scenes_.end()) {
        if (s->second.owner == owner) {
            scenes_.erase(s);
            host_serial_++;
        }
        return;
    }
    auto it = layers_.find(id);
    if (it == layers_.end() || it->second.owner != owner)
        return;
    const int ti = it->second.layer->target();
    Target &t = targets_[ti];
    const bool video = std::string(it->second.layer->kind()) == "video";
    if (it->second.composited && t.composite)
        t.composite->remove(id);
    if (it->second.held)
        t.held_n--;
    if (it->second.flushed.drawn && it->second.flushed.exclusive)
        t.exclusive_gone = true;
    it->second.layer->detach(true);
    if (t.graph)
        t.dirty = true;
    drop_effects(it->second);
    layers_.erase(it);
    if (video)
        reconcile();
}

void Compositor::remove_owner(uint64_t owner)
{
    /* Layers first: then no layer samples the assets. */
    std::vector<uint32_t> ids;
    for (const auto &kv : layers_)
        if (kv.second.owner == owner)
            ids.push_back(kv.first);
    for (uint32_t id : ids)
        remove(owner, id);
    ids.clear();
    for (const auto &kv : assets_)
        if (kv.second.owner == owner)
            ids.push_back(kv.first);
    std::string err;
    for (uint32_t id : ids)
        remove_asset(owner, id, &err);
    for (auto it = scenes_.begin(); it != scenes_.end();) {
        if (it->second.owner != owner) {
            ++it;
            continue;
        }
        it = scenes_.erase(it);
        host_serial_++;
    }
}

void Compositor::arrange(int64_t now_ms)
{
    now_ms_ = now_ms;
    for (const Target &t : targets_)
        if ((t.remake_ms >= 0 && now_ms >= t.remake_ms) || (t.hold_ms >= 0 && now_ms >= t.hold_ms)) {
            reconcile();
            break;
        }
    for (size_t ti = 0; ti < targets_.size(); ti++) {
        Target &t = targets_[ti];
        std::vector<StackItem> items;
        for (const auto &kv : layers_) {
            const Entry &e = kv.second;
            const Layer &l = *e.layer;
            if (l.target() != (int)ti)
                continue;
            StackItem it;
            it.id = kv.first;
            it.klass = l.params.klass;
            it.z = l.params.z;
            it.seq = l.seq();
            it.visible = l.params.visible && e.error.empty() && l.refusal().empty() && !asset_refused(e);
            it.ready = l.ready(now_ms) && assets_have(e);
            it.exclusive = l.params.exclusive;
            it.prewarm = l.params.prewarm;
            it.contend = l.params.contend;
            it.guide_hold = l.params.guide_hold;
            it.yield = l.params.yield;
            it.shown = l.on_display();
            items.push_back(it);
        }
        t.arr = crs::arrange(items);
        if (const uint32_t sc = content((int)ti)) {
            /* The target's scene is the content's file: it wants the
             * target, and every layer waits (state()) unless they are
             * drawn over the file. */
            if (layers_wait((int)ti))
                t.arr.drawn.clear();
            t.arr.wanted = true;
            t.arr.contend = t.arr.contend || scenes_[sc].contend;
        }
        fade((int)ti, t, now_ms);
        slots_->want((int)ti, t.arr.wanted, t.arr.contend, t.arr.guide_hold, t.arr.yield);
        if (!t.graph)
            continue;
        const bool cv = t.composite && t.composite->attached();
        t.videos = 0;
        t.lowest = INT32_MAX;
        t.top = INT32_MIN;
        for (size_t pos = 0; pos < t.arr.order.size(); pos++) {
            Entry &e = layers_[t.arr.order[pos]];
            if (!e.composited && !e.layer->attached())
                continue;
            const bool drawn =
                std::find(t.arr.drawn.begin(), t.arr.drawn.end(), t.arr.order[pos]) != t.arr.drawn.end() &&
                assets_clean(e);
            const int32_t order = (int32_t)pos * 4;
            /* A composite not attached draws none of its sources. */
            if (e.composited ? cv : !std::strcmp(e.layer->kind(), "video")) {
                t.videos++;
                t.lowest = std::min(t.lowest, order);
            }
            if (drawn && (!e.composited || cv))
                t.top = std::max(t.top, order);
            if (e.composited) {
                if (t.composite)
                    t.composite->arrange_source(t.arr.order[pos], drawn, order);
                e.drawn = drawn;
                e.order = order;
                continue;
            }
            if (e.held) {
                e.drawn = drawn;   /* the scene keeps the last flush's until it is released (commit) */
                e.order = order;
                continue;
            }
            if (drawn != e.drawn || order != e.order) {
                e.layer->arrange(drawn, order);
                e.drawn = drawn;
                e.order = order;
                t.dirty = true;
                mark(e, t.arr.order[pos], t);
            }
        }
        if (t.composite && t.composite->arranged())
            t.dirty = true;
    }
}

void Compositor::fade(int ti, Target &t, int64_t now_ms)
{
    const int64_t dt = std::max<int64_t>(0, now_ms - t.fade_ms);
    t.fade_ms = now_ms;
    const TargetState st = slots_->status(ti).state;
    const bool on_screen = t.graph && (st == TargetState::Shown || st == TargetState::Lingering);
    const float step = fade_ms_ > 0 ? (float)dt / (float)fade_ms_ : 1.0f;
    t.fading_out = false;
    if (fade_ms_ <= 0) {
        t.fade = 1.0f;
    } else if (!on_screen) {
        t.fade = 0.0f;   /* nothing of it is on screen, or the display took it: no fade */
    } else if (t.arr.wanted) {
        t.fade = std::min(1.0f, t.fade + step);
    } else {
        /* Nothing wants the target: the layers drawn when something last
         * did stay drawn, and the target wanted, while they fade out. */
        std::vector<uint32_t> held;
        bool fades = false;
        for (uint32_t id : t.last_drawn) {
            auto it = layers_.find(id);
            if (it == layers_.end() || !on_scene(it->second) || it->second.layer->target() != ti)
                continue;
            held.push_back(id);
            fades = fades || it->second.layer->fades();
        }
        t.fade = fades ? std::max(0.0f, t.fade - step) : 0.0f;
        if (t.fade > 0.0f) {
            t.fading_out = true;
            t.arr.drawn = held;
            t.arr.wanted = true;
            t.arr.contend = t.last_contend;
            t.arr.guide_hold = t.last_guide_hold;
            t.arr.yield = t.last_yield;
        }
    }
    if (t.arr.wanted && !t.fading_out) {
        t.last_drawn = t.arr.drawn;
        t.last_contend = t.arr.contend;
        t.last_guide_hold = t.arr.guide_hold;
        t.last_yield = t.arr.yield;
    }
    /* The layers get it by steps, each in the target's next flush; 0 and 1
     * go at once. */
    if (t.fade != t.applied &&
        (t.fade == 0.0f || t.fade == 1.0f || now_ms - t.applied_ms >= kFadeStepMs)) {
        t.applied = t.fade;
        t.applied_ms = now_ms;
    }
    /* A fade step held to keep tiles covered reaches the layers when it is
     * released (commit). */
    if (t.fade_held)
        return;
    for (auto &kv : layers_) {
        Layer &l = *kv.second.layer;
        if (l.target() != ti || l.fade() == t.applied)
            continue;
        l.set_fade(t.applied);
        if (l.attached() && l.fades())
            t.dirty = true;
    }
    if (t.composite && t.composite->fade() != t.applied) {
        t.composite->set_fade(t.applied);
        if (t.composite->attached())
            t.dirty = true;
    }
}

void Compositor::render(int64_t now_ms)
{
    now_ms_ = now_ms;
    /* The HUD's share of the budget, while a target that writes from it is
     * asked for and has a layer: up to kSharePct of each refill, what it does
     * not spend going back at once. */
    bool sharing = false;
    for (const auto &kv : layers_) {
        const int ti = kv.second.layer->target();
        sharing = sharing || (slots_->targets()[ti].share && slots_->requested(ti));
    }
    pacer_.set_share(sharing ? kSharePct : 0);
    /* Released: nothing of a target is asked for. Parked: nothing of it is
     * on screen, and the link carries the HMI's content too. It stages again
     * once shown. Each target staging is told what covers its videos; one
     * with held tiles to uncover, or an alert waiting for its first write,
     * goes first, so that its write takes the next allowance before other
     * targets' writes. */
    render_order_.clear();
    render_later_.clear();
    for (size_t ti = 0; ti < targets_.size(); ti++) {
        Target &t = targets_[ti];
        if ((!t.graph && !t.file) || !slots_->requested((int)ti) ||
            slots_->status((int)ti).state == TargetState::Parked)
            continue;
        t.uncover = false;
        if (t.file && !t.graph) {
            /* A scene file without layers over it: nothing to uncover. */
            t.uncover_since = t.alert_since = -1;
            render_later_.push_back((int)ti);
            continue;
        }
        if (occluding(t)) {
            occlude((int)ti, t);
            t.uncover = uncovers(t);
            t.quiet = t.quiet && !t.uncover && !t.held_n && !t.fade_held;
        }
        /* Since when its uncover has waited: kept while what the client set
         * still uncovers tiles (its changes held back), until the write that
         * carries them goes (render_target). */
        if (!t.uncover)
            t.uncover_since = -1;
        else if (t.uncover_since < 0)
            t.uncover_since = now_ms;
        const bool alert = alert_waits((int)ti);
        if (!alert)
            t.alert_since = -1;
        else if (t.alert_since < 0)
            t.alert_since = now_ms;
        (t.uncover || alert ? render_order_ : render_later_).push_back((int)ti);
    }
    /* The target that has waited longest first: none waits behind another's
     * uncover or alert more than once. */
    auto since = [this](int ti) {
        const Target &t = targets_[ti];
        return t.uncover_since < 0 ? t.alert_since
               : t.alert_since < 0 ? t.uncover_since
                                   : std::min(t.uncover_since, t.alert_since);
    };
    std::stable_sort(render_order_.begin(), render_order_.end(),
                     [&since](int a, int b) { return since(a) < since(b); });
    render_order_.insert(render_order_.end(), render_later_.begin(), render_later_.end());
    for (int ti : render_order_) {
        if (targets_[ti].file && !targets_[ti].graph)
            render_file(ti, targets_[ti], now_ms);
        else
            render_target(ti, targets_[ti], now_ms);
    }
    /* Video objects waiting for another target's flush to drop buffers. */
    for (const Target &t : targets_)
        if (t.make_waiting) {
            reconcile();
            break;
        }
    if (governor_.tick(now_ms))
        pacer_.set_rate(governor_.cap());
}

void Compositor::render_target(int ti, Target &t, int64_t now_ms)
{
    /* Held tiles a change uncovers go in the flush that carries the change.
     * While the budget does not allow that write, the change waits (the
     * scene keeps what the last flush showed for it) and everything else
     * on the target goes as usual. */
    bool uncover = t.uncover;
    if (uncover && !pacer_for(ti).ready(now_ms))
        uncover = !defer_uncover(ti, t);
    commit(ti, t);
    /* A layer whose failed write left its buffer half written holds the
     * target: only its whole rewrite stages, and nothing flushes until
     * that goes, so the cluster keeps showing the last frame. */
    const bool hold = holds(t);
    std::vector<Layer *> staged;
    std::vector<Asset *> staged_assets;
    bool wrote = false, failed = false;
    /* Assets first: the layers sampling them are drawn from them. A
     * held target writes none: no flush would carry them. */
    for (auto &kv : assets_) {
        Asset &a = *kv.second.asset;
        if (hold || a.target() != ti || !a.attached())
            continue;
        const int had = a.buffer();
        const Stage s = a.stage(now_ms);
        if (a.buffer() != had)   /* a compressed asset's texture, made now */
            for (auto &lk : layers_)
                for (const auto &b : lk.second.binds)
                    if (b.second == a.id()) {
                        apply_binds(lk.second);
                        break;
                    }
        if (s == Stage::Idle)
            continue;
        staged_assets.push_back(&a);
        wrote = wrote || s == Stage::Wrote;
        failed = failed || s == Stage::Failed;
    }
    bool composite = false;
    for (size_t pos = t.arr.order.size(); pos-- > 0;) {
        auto it = layers_.find(t.arr.order[pos]);
        if (it == layers_.end())
            continue;
        /* The composite stages at its top source's place: its sources'
         * new frames composed first. */
        Layer *l = it->second.layer.get();
        if (it->second.composited) {
            if (composite || !t.composite || !t.composite->attached())
                continue;
            composite = true;
            t.composite->compose(now_ms);
            l = t.composite.get();
        } else if (!l->attached()) {
            continue;
        }
        if (hold && !l->holds_flush() && !l->uncovering())
            continue;
        const Stage s = l->stage(now_ms);
        if (l == t.composite.get() && t.composite->placed_now())
            t.dirty = true;   /* quads moved over a canvas the display holds */
        if (s == Stage::Idle)
            continue;
        staged.push_back(l);
        wrote = wrote || s == Stage::Wrote;
        failed = failed || s == Stage::Failed;
    }
    /* Layers over a scene file: its due edits go in the same flush. */
    const bool edits = t.file && stage_edits(t, now_ms);
    if (!wrote && !failed && !t.dirty && !edits) {
        settle(ti, t, false);
        return;
    }
    uint64_t bytes = 0;
    for (const Layer *l : staged)
        bytes += l->staged_bytes();
    for (const Asset *a : staged_assets)
        bytes += a->staged_bytes();
    const TargetSpec &spec = slots_->targets()[ti];
    if (!spec.share)
        load(bytes, now_ms);
    bool ok = false;
    if (failed)
        evidence(spec, Evidence::WriteRefused, now_ms, spec.name);
    const bool clear = !failed && !holds(t) && !(uncover && uncovers(t));
    if (clear)
        ok = flush(ti, t, now_ms);
    /* A scene file's edits never wait for the layers over it: when its
     * layers hold the flush (a half-written buffer, a write refused now, an
     * uncover the budget defers), it goes for the edits all the same, and
     * the layers take their writes as not gone (a video's next write is
     * whole). What the layers wrote may show half done until then; the
     * file's owner's content does not freeze behind another client's. */
    const bool went = clear ? ok : edits && flush(ti, t, now_ms);
    for (Asset *a : staged_assets)
        a->committed(ok);
    for (Layer *l : staged)
        l->committed(ok, now_ms);
    if (edits && went)
        t.file_flush_ms = now_ms;
    /* What the layers undid for a flush that did not go goes with the
     * next one. */
    t.dirty = !ok;
    /* The uncover went with this flush: a later one waits from its own
     * start. */
    if (uncover && ok)
        t.uncover_since = -1;
    settle(ti, t, ok);
}

bool Compositor::flush(int ti, Target &t, int64_t now_ms)
{
    const TargetSpec &spec = slots_->targets()[ti];
    const int64_t t0 = clock_us_();
    /* A graph over a file flushes the file's scene, edits and all. */
    const bool ok = t.graph ? t.graph->flush(++t.tag) : t.file->flush(++t.tag);
    const int64_t took = clock_us_() - t0;
    if (!ok)
        evidence(spec, Evidence::FlushFailure, now_ms, spec.name);
    else if (took >= slow_flush_us_)
        evidence(spec, Evidence::SlowFlush, now_ms, spec.name + " " + std::to_string(took / 1000) + " ms");
    if (!ok && !t.flush_failing)
        log_error("target %s: flush %llu refused", spec.name.c_str(), (unsigned long long)t.tag);
    else if (ok && t.flush_failing)
        log_info("target %s: flushing again from %llu", spec.name.c_str(), (unsigned long long)t.tag);
    t.flush_failing = !ok;
    if (ok)
        t.flushed_videos = video_buffers(ti);
    return ok;
}

void Compositor::render_file(int ti, Target &t, int64_t now_ms)
{
    if (stage_edits(t, now_ms) || t.dirty) {
        t.dirty = !flush(ti, t, now_ms);
        t.file_flush_ms = now_ms;
    }
}

bool Compositor::stage_edits(Target &t, int64_t now_ms)
{
    /* The content gone, its scene stays as last flushed until released. */
    auto s = scenes_.find(t.file_of);
    if (s == scenes_.end() || s->second.serial != t.file_serial)
        return false;
    /* Once a tick: lines coming faster stay due (wake_in_ms). */
    if (!t.dirty && file_tick_ms_ > 0 && t.file_flush_ms >= 0 && now_ms - t.file_flush_ms < file_tick_ms_)
        return false;
    return s->second.edits.stage(t.file.get());
}

void Compositor::charge(int ti, uint64_t bytes, int64_t now_ms)
{
    crk::UploadPacer &pacer = pacer_for(ti);
    pacer.ready(now_ms);
    pacer.spend(bytes);
    if (!slots_->targets()[ti].share)
        load(bytes, now_ms);
}

crk::UploadPacer &Compositor::pacer_for(int target)
{
    return target >= 0 && target < (int)slots_->targets().size() && slots_->targets()[target].share ? share_ : pacer_;
}

void Compositor::evidence(const TargetSpec &t, Evidence e, int64_t now_ms, const std::string &detail)
{
    if (t.share)
        log_info("target %s: %s (the HUD share's: the upload cap stays)", t.name.c_str(), detail.c_str());
    else
        governor_.evidence(e, now_ms, detail);
}

/* The link counts as carrying load (for the governor) from kLoadBytes a
 * second written, whether or not the flush goes. */
void Compositor::load(uint64_t bytes, int64_t now_ms)
{
    if (now_ms - second_ms_ >= 1000) {
        second_ms_ = now_ms;
        second_bytes_ = 0;
    }
    second_bytes_ += bytes;
    if (second_bytes_ >= kLoadBytes)
        governor_.activity(now_ms);
}

void Compositor::resent(const TargetSpec &spec, int64_t now_ms)
{
    const int ti = slots_->index(spec.name);
    uint64_t bytes = targets_[ti].file ? targets_[ti].file_bytes : 0;
    for (const auto &kv : layers_)
        if (kv.second.layer->target() == ti)
            bytes += kv.second.layer->buffer_bytes();
    for (const auto &kv : assets_)
        if (kv.second.asset->target() == ti && kv.second.asset->attached())
            bytes += kv.second.asset->bytes();
    if (targets_[ti].composite)
        bytes += targets_[ti].composite->buffer_bytes();
    if (!bytes)
        return;
    charge(ti, bytes, now_ms);
    log_info("target %s: its scene goes whole to the renderer again: %llu KB charged to the upload budget",
             spec.name.c_str(), (unsigned long long)(bytes / 1024));
}

bool Compositor::holds(const Target &t) const
{
    if (t.composite && t.composite->attached() && t.composite->holds_flush())
        return true;
    for (uint32_t id : t.arr.order) {
        auto it = layers_.find(id);
        if (it != layers_.end() && it->second.layer->attached() && it->second.layer->holds_flush())
            return true;
    }
    return false;
}

Shown Compositor::shown_of(const Entry &e, const Target &t) const
{
    const Look k = e.eval_held ? e.flushed : look_of(e);
    Shown s;
    s.drawn = k.drawn;
    s.rect = k.rect;
    s.turned = k.rotate != 0 || k.sx != 1 || k.sy != 1;
    s.opacity = k.opacity;
    s.fade = t.eval_fade_held ? t.flushed_fade : t.applied;
    s.animating = k.animating;
    s.budget_ready = t.budget_ready;
    return s;
}

bool Compositor::alert_waits(int ti) const
{
    for (uint32_t id : targets_[ti].arr.drawn) {
        auto it = layers_.find(id);
        if (it == layers_.end())
            continue;
        const Layer &l = *it->second.layer;
        if ((l.params.klass == Klass::Alert || l.params.exclusive) && l.attached() && !l.on_display())
            return true;
    }
    return false;
}

bool Compositor::occluding(const Target &t) const
{
    return !t.quiet || (t.videos && t.top > t.lowest);
}

void Compositor::occlude(int ti, Target &t)
{
    /* A composite not attached draws none of its sources: they are neither
     * seen nor cover anything. */
    CompositeVideo *cv = t.composite && t.composite->attached() ? t.composite.get() : nullptr;
    t.budget_ready = pacer_for(ti).ready(now_ms_);
    /* The lowest video's place in the draw order the next flush shows, and
     * whether any layer is drawn above it. With none above, nothing is
     * hidden: the videos are told so, and no walk is made. */
    int32_t lowest = INT32_MAX, top = INT32_MIN;
    bool videos = false;
    for (uint32_t id : t.arr.order) {
        auto it = layers_.find(id);
        if (it == layers_.end())
            continue;
        const Entry &e = it->second;
        if (e.composited ? !cv : !e.layer->attached())
            continue;
        const int32_t order = e.composited || !e.eval_held ? e.order : e.flushed.order;
        const bool drawn = e.composited || !e.eval_held ? e.drawn : e.flushed.drawn;
        if (e.composited || !std::strcmp(e.layer->kind(), "video")) {
            videos = true;
            lowest = std::min(lowest, order);
        }
        if (drawn)
            top = std::max(top, order);
    }
    t.quiet = true;
    if (!videos)
        return;
    const TargetSpec &spec = slots_->targets()[ti];
    t.coverage.reset(spec.width, spec.height);
    if (top <= lowest) {
        for (uint32_t id : t.arr.order) {
            auto it = layers_.find(id);
            if (it == layers_.end() || it->second.composited || !it->second.layer->attached() ||
                std::strcmp(it->second.layer->kind(), "video"))
                continue;
            VideoLayer *v = static_cast<VideoLayer *>(it->second.layer.get());
            v->occluded(t.coverage, shown_of(it->second, t));
            t.quiet = t.quiet && !v->holds_tiles();
        }
        if (cv) {
            cv->hide(nullptr);
            t.quiet = t.quiet && !cv->holds_tiles();
        }
        return;
    }
    t.quiet = false;
    t.walk.clear();
    for (uint32_t id : t.arr.order) {
        auto it = layers_.find(id);
        if (it == layers_.end())
            continue;
        const Entry &e = it->second;
        if (e.composited ? !cv : !e.layer->attached())
            continue;
        const int32_t order = e.composited || !e.eval_held ? e.order : e.flushed.order;
        if (order >= lowest)
            t.walk.push_back(std::make_pair(order, id));
    }
    std::sort(t.walk.begin(), t.walk.end(),
              [](const std::pair<int32_t, uint32_t> &a, const std::pair<int32_t, uint32_t> &b) {
                  return a.first > b.first;
              });
    const float fade = t.eval_fade_held ? t.flushed_fade : t.applied;
    if (cv)
        cv->begin_hidden();
    /* Top first. Layers at one place in the draw order (a held layer's old
     * place and another's new one) are drawn in an order the renderer
     * chooses: none of them covers another. */
    for (size_t i = 0; i < t.walk.size();) {
        size_t j = i;
        while (j < t.walk.size() && t.walk[j].first == t.walk[i].first)
            j++;
        for (size_t k = i; k < j; k++) {
            Entry &e = layers_[t.walk[k].second];
            if (e.composited)
                cv->see_source(t.walk[k].second, t.coverage);
            else
                e.layer->occluded(t.coverage, shown_of(e, t));
        }
        if (t.walk[i].first > lowest)
            for (size_t k = i; k < j; k++) {
                Entry &e = layers_[t.walk[k].second];
                if (e.composited)
                    cv->occlude_source(t.walk[k].second, &t.coverage, fade);
                else
                    e.layer->occlude(&t.coverage, shown_of(e, t));
            }
        i = j;
    }
    if (cv)
        cv->end_hidden();
}

bool Compositor::uncovers(const Target &t) const
{
    if (t.composite && t.composite->attached() && t.composite->uncovering())
        return true;
    for (uint32_t id : t.arr.order) {
        auto it = layers_.find(id);
        if (it != layers_.end() && it->second.layer->attached() && it->second.layer->uncovering())
            return true;
    }
    return false;
}

Compositor::Look Compositor::look_of(const Entry &e)
{
    Look k;
    const Layer &l = *e.layer;
    k.drawn = l.attached() && e.drawn;
    k.order = e.order;
    k.rect = l.params.rect;
    k.opacity = l.params.opacity;
    k.rotate = l.params.rotate;
    k.sx = l.params.sx;
    k.sy = l.params.sy;
    k.px = l.params.px;
    k.py = l.params.py;
    k.pivot_x = l.params.pivot_x;
    k.pivot_y = l.params.pivot_y;
    k.animating = l.animating();
    k.exclusive = l.params.exclusive;
    return k;
}

bool Compositor::same(const Look &a, const Look &b)
{
    return a.drawn == b.drawn && a.order == b.order && a.rect == b.rect && a.opacity == b.opacity &&
           a.rotate == b.rotate && a.sx == b.sx && a.sy == b.sy && a.px == b.px && a.py == b.py &&
           a.pivot_x == b.pivot_x && a.pivot_y == b.pivot_y && a.animating == b.animating &&
           a.exclusive == b.exclusive;
}

bool Compositor::evaluate(int ti, Target &t)
{
    occlude(ti, t);
    return uncovers(t);
}

namespace {

/* Whether a layer drawn as a may show pixels of b's place: their rects
 * overlap, or either is turned (its place is then not its rect). */
template <class L> bool may_overlap(const L &a, const L &b)
{
    const bool turned = a.rotate != 0 || a.sx != 1 || a.sy != 1 || b.rotate != 0 || b.sx != 1 || b.sy != 1;
    return turned || !intersect(box_of(a.rect), box_of(b.rect)).empty();
}

}  // namespace

bool Compositor::defer_uncover(int ti, Target &t)
{
    /* The changes since the last flush that went, as units that wait or go
     * together: each layer of its own whose look changed, the fade, the
     * composite's moves, shows and opacities. */
    t.decided = true;
    struct Unit {
        std::vector<Entry *> layers;
        bool fade = false, composite = false, held = true;
        bool goes = false;   /* never held */
    };
    std::vector<Unit> units;
    for (uint32_t id : t.arr.order) {
        auto it = layers_.find(id);
        if (it == layers_.end())
            continue;
        Entry &e = it->second;
        if (!e.composited && e.layer->attached() && !same(look_of(e), e.flushed)) {
            units.push_back(Unit());
            units.back().layers.push_back(&e);
        }
    }
    /* An exclusive layer coming or going and the layers it suspends or
     * resumes go together: every change of what is drawn joins one unit
     * with it, so no flush shows neither. One removed has gone already:
     * what it resumes goes too. */
    std::vector<Unit> merged;
    Unit drawn_changes;
    bool exclusive = false;
    for (Unit &u : units) {
        Entry &e = *u.layers[0];
        const Look k = look_of(e);
        if (k.drawn != e.flushed.drawn) {
            exclusive = exclusive || k.exclusive || e.flushed.exclusive;
            drawn_changes.layers.push_back(&e);
        } else {
            merged.push_back(u);
        }
    }
    if ((exclusive || t.exclusive_gone) && !drawn_changes.layers.empty()) {
        drawn_changes.goes = t.exclusive_gone;
        merged.push_back(drawn_changes);
    } else {
        for (Entry *e : drawn_changes.layers) {
            merged.push_back(Unit());
            merged.back().layers.push_back(e);
        }
    }
    units.swap(merged);
    if (t.applied != t.flushed_fade) {
        units.push_back(Unit());
        units.back().fade = true;
    }
    if (t.composite && t.composite->attached() && t.composite->held_changes()) {
        units.push_back(Unit());
        units.back().composite = true;
    }
    auto apply = [&](Unit &u, bool held) {
        u.held = held && !u.goes;
        for (Entry *e : u.layers)
            e->eval_held = u.held;
        if (u.fade)
            t.eval_fade_held = u.held;
        if (u.composite)
            t.composite->hold(u.held);
    };
    for (Unit &u : units)
        apply(u, true);
    if (!evaluate(ti, t)) {
        /* Only the units that uncover wait. */
        for (Unit &u : units) {
            apply(u, false);
            if (evaluate(ti, t))
                apply(u, true);
        }
        /* A change going while one it would reveal waits would show a
         * place neither the client's state nor the last flush shows: a
         * layer moved or hidden off a place where a waiting layer is to be
         * drawn waits with it. One that cannot wait (goes) makes them all
         * go. */
        bool must_go = false;
        for (bool more = true; more && !must_go;) {
            more = false;
            for (Unit &h : units) {
                if (!h.held)
                    continue;
                for (Entry *he : h.layers) {
                    const Look want = look_of(*he);
                    if (!want.drawn)
                        continue;
                    for (Unit &a : units) {
                        if (a.held)
                            continue;
                        for (Entry *ae : a.layers)
                            if (ae->flushed.drawn && may_overlap(ae->flushed, want)) {
                                if (a.goes) {
                                    must_go = true;
                                } else {
                                    apply(a, true);
                                    more = true;
                                }
                                break;
                            }
                    }
                }
            }
        }
        if (!must_go) {
            if (evaluate(ti, t)) {
                for (Unit &u : units)
                    apply(u, true);
                evaluate(ti, t);
            }
            return true;
        }
    }
    /* Not held back by that (a layer removed, an image hidden after a
     * failed write, video objects made again, or a change that waits with
     * one of these): the changes go, and the uncovering videos write now. */
    for (Unit &u : units)
        apply(u, false);
    evaluate(ti, t);
    if (t.composite && t.composite->attached() && t.composite->uncovering())
        t.composite->force(true);
    for (auto &kv : layers_)
        if (kv.second.layer->target() == ti && kv.second.layer->attached() && kv.second.layer->uncovering())
            static_cast<VideoLayer *>(kv.second.layer.get())->force(true);
    return false;
}

void Compositor::commit(int ti, Target &t)
{
    if (t.decided || t.held_n) {
        for (auto &kv : layers_) {
            Entry &e = kv.second;
            Layer &l = *e.layer;
            if (l.target() != ti || e.composited || !l.attached())
                continue;
            if (e.eval_held && !e.held) {
                /* The scene back to what the last flush showed, its fade
                 * included; params stay what the client set. */
                LayerParams p = l.params;
                const Look &f = e.flushed;
                p.rect = f.rect;
                p.opacity = f.opacity;
                p.rotate = f.rotate;
                p.sx = f.sx;
                p.sy = f.sy;
                p.px = f.px;
                p.py = f.py;
                p.pivot_x = f.pivot_x;
                p.pivot_y = f.pivot_y;
                l.hold_scene(&p);
                l.place();
                l.arrange(f.drawn, f.order);
                e.held = true;
                t.held_n++;
            } else if (!e.eval_held && e.held) {
                l.hold_scene(nullptr);
                l.place();
                l.arrange(e.drawn, e.order);
                e.held = false;
                t.held_n--;
                t.dirty = true;
            }
            if (!e.held && l.start_pending())
                t.dirty = true;
        }
    } else {
        for (uint32_t id : t.touched) {
            auto it = layers_.find(id);
            if (it != layers_.end() && !it->second.composited && it->second.layer->start_pending())
                t.dirty = true;
        }
    }
    if (t.eval_fade_held != t.fade_held) {
        const float f = t.eval_fade_held ? t.flushed_fade : t.applied;
        for (auto &kv : layers_)
            if (kv.second.layer->target() == ti)
                kv.second.layer->set_fade(f);
        if (t.composite)
            t.composite->set_fade(f);
        t.fade_held = t.eval_fade_held;
        t.dirty = t.dirty || !t.fade_held;
    }
    if (t.composite && t.composite->attached() && !t.composite->held() && t.composite->show_opacity())
        t.dirty = true;
}

void Compositor::settle(int ti, Target &t, bool ok)
{
    if (t.decided) {
        for (auto &kv : layers_) {
            Entry &e = kv.second;
            if (e.layer->target() != ti || e.composited)
                continue;
            e.eval_held = false;
            if (!std::strcmp(e.layer->kind(), "video"))
                static_cast<VideoLayer *>(e.layer.get())->force(false);
        }
        if (t.composite) {
            t.composite->force(false);
            t.composite->hold(false);
        }
        t.decided = false;
    }
    t.eval_fade_held = false;
    if (!ok)
        return;
    t.exclusive_gone = false;
    /* What the flush showed of the layers whose look changed: their look,
     * but a held one's (it stays in the list). */
    size_t kept = 0;
    for (uint32_t id : t.touched) {
        auto it = layers_.find(id);
        if (it == layers_.end() || it->second.layer->target() != ti)
            continue;
        Entry &e = it->second;
        if (e.held) {
            t.touched[kept++] = id;
            continue;
        }
        e.flushed = look_of(e);
        e.flushed.animating = e.layer->animations_started();
        e.touched = false;
    }
    t.touched.resize(kept);
    if (!t.fade_held)
        t.flushed_fade = t.applied;
}

int64_t Compositor::wake_in_ms(int64_t now_ms) const
{
    int64_t best = -1;
    auto at = [&](int64_t w) {
        if (w > 0 && (best < 0 || w < best))
            best = w;
    };
    for (const auto &kv : layers_)
        if (slots_->requested(kv.second.layer->target()))
            at(kv.second.layer->wake_in_ms(now_ms));
    for (size_t ti = 0; ti < targets_.size(); ti++) {
        const Target &t = targets_[ti];
        if (t.composite && slots_->requested((int)ti))
            at(t.composite->wake_in_ms(now_ms));
        if (t.remake_ms >= 0)
            at(std::max<int64_t>(1, t.remake_ms - now_ms));
        if (t.hold_ms >= 0)
            at(std::max<int64_t>(1, t.hold_ms - now_ms));
        if (t.graph && (t.fading_out || (t.fade > 0.0f && t.fade < 1.0f) || t.fade != t.applied))
            at(std::max<int64_t>(1, t.applied_ms + kFadeStepMs - now_ms));
        /* due edits of a scene render() stages (requested, not parked) */
        auto s = t.file ? scenes_.find(t.file_of) : scenes_.end();
        if (s != scenes_.end() && s->second.serial == t.file_serial && slots_->requested((int)ti) &&
            slots_->status((int)ti).state != TargetState::Parked && s->second.edits.due())
            at(std::max<int64_t>(1, t.file_flush_ms + file_tick_ms_ - now_ms));
    }
    return best;
}

LayerState Compositor::state(uint32_t id, std::string *reason) const
{
    reason->clear();
    auto sc = scenes_.find(id);
    if (sc != scenes_.end()) {
        const SceneEntry &s = sc->second;
        if (!s.error.empty()) {
            *reason = s.error;
            return LayerState::Error;
        }
        const TargetStatus &ts = slots_->status(s.target);
        const Target &t = targets_[s.target];
        const bool loaded = t.file && t.file_of == id && t.file_serial == s.serial;
        /* Where the service sees the renderer's subscriptions: once it has
         * this scene (a file replaced in place is taken again). */
        if (ts.state == TargetState::Shown && loaded && ts.rendering != Rendering::Checking)
            return LayerState::Shown;
        *reason = ts.state == TargetState::Shown && loaded
                      ? "target shown, its renderer not subscribed to the scene yet"
                      : std::string("target ") + target_state_name(ts.state) + (ts.reason.empty() ? "" : ": " + ts.reason);
        return LayerState::Waiting;
    }
    auto it = layers_.find(id);
    if (it == layers_.end())
        return LayerState::Hidden;
    const Entry &e = it->second;
    const Layer &l = *e.layer;
    if (!e.error.empty() || !l.refusal().empty()) {
        *reason = e.error.empty() ? l.refusal() : e.error;
        return LayerState::Error;
    }
    if (e.composited && !targets_[l.target()].composite_error.empty()) {
        *reason = targets_[l.target()].composite_error;
        return LayerState::Error;
    }
    if (const uint32_t refused = asset_refused(e)) {
        *reason = "asset " + std::to_string(refused) + ": " + assets_.at(refused).asset->refused();
        return LayerState::Error;
    }
    if (!l.params.visible)
        return LayerState::Hidden;
    if (!l.ready(now_ms_) || !assets_have(e))
        return LayerState::Stalled;
    if (layers_wait(l.target())) {
        *reason = "scene";
        return LayerState::Waiting;
    }
    const Target &t = targets_[l.target()];
    if (t.graph && std::string(l.kind()) == "video" &&
        (e.composited ? !(t.composite && t.composite->attached()) : !l.attached())) {
        *reason = "the display's video buffers: another target's are still going";
        return LayerState::Waiting;
    }
    if (std::find(t.arr.suspended.begin(), t.arr.suspended.end(), id) != t.arr.suspended.end())
        return LayerState::Suspended;
    const TargetStatus &ts = slots_->status(l.target());
    if (ts.state == TargetState::Shown && e.drawn)
        return LayerState::Shown;
    if (ts.state == TargetState::Shown && t.graph && !assets_clean(e)) {
        *reason = "an asset it samples is not written yet";
        return LayerState::Waiting;
    }
    *reason = std::string("target ") + target_state_name(ts.state) + (ts.reason.empty() ? "" : ": " + ts.reason);
    return LayerState::Waiting;
}

std::vector<const Layer *> Compositor::layers() const
{
    std::vector<const Layer *> out;
    for (const auto &kv : layers_)
        out.push_back(kv.second.layer.get());
    return out;
}

WriteStats Compositor::write_stats(int target) const
{
    WriteStats sum;
    for (const auto &kv : layers_) {
        if (kv.second.layer->target() != target)
            continue;
        const WriteStats w = kv.second.layer->write_stats();
        sum.frames += w.frames;
        sum.age_ms += w.age_ms;
        sum.bytes += w.bytes;
        sum.failed += w.failed;
        sum.holds += w.holds;
    }
    if (target >= 0 && target < (int)targets_.size() && targets_[target].composite) {
        const WriteStats w = targets_[target].composite->write_stats();
        sum.frames += w.frames;
        sum.age_ms += w.age_ms;
        sum.bytes += w.bytes;
        sum.failed += w.failed;
        sum.holds += w.holds;
    }
    return sum;
}

std::vector<const Asset *> Compositor::assets() const
{
    std::vector<const Asset *> out;
    for (const auto &kv : assets_)
        out.push_back(kv.second.asset.get());
    return out;
}

bool Compositor::publish(const TargetSpec &spec, std::string *err)
{
    const int ti = slots_->index(spec.name);
    Target &t = targets_[ti];
    if (const uint32_t id = content(ti)) {
        SceneEntry &s = scenes_[id];
        t.file = gpu_->load(spec, s.src.file, s.src.res, err);
        if (!t.file) {
            /* The client's file: it wants nothing more; the layers come back,
             * and what it hosted ends. */
            s.error = *err;
            if (!s.hosts.empty()) {
                s.hosts.clear();
                host_serial_++;
            }
            log_warn("target %s: scene file %s refused: %s", spec.name.c_str(), s.src.file.c_str(), err->c_str());
            return false;
        }
        t.file_of = id;
        t.file_serial = s.serial;
        t.file_bytes = s.src.bytes;
        t.tag = 0;
        t.flush_failing = false;
        t.dirty = true;
        s.edits.reset();   /* every kept line in the first flush (render()) */
        charge(ti, t.file_bytes, now_ms_);
        log_info("target %s: scene file %s published, %llu KB with its resource files, %zu edit lines kept",
                 spec.name.c_str(), s.src.file.c_str(), (unsigned long long)(t.file_bytes / 1024), s.edits.kept());
        if (!s.layers.on)
            return true;
        /* The layers over the file, in its scene; without that graph they
         * wait, the file shown all the same, and it hosts nothing. */
        host_serial_++;
        t.graph = t.file->layers(s.layers.order_set ? &s.layers.order : nullptr, err);
        if (!t.graph) {
            log_warn("target %s: no layers over scene file %s: %s", spec.name.c_str(), s.src.file.c_str(),
                     err->c_str());
            s.layers_error = *err;
            s.hosts.clear();
            err->clear();
            return true;
        }
    } else {
        t.graph = gpu_->scene(spec, err);
        if (!t.graph)
            return false;
    }
    t.tag = 0;
    t.flush_failing = false;
    /* A new scene shows nothing until its first flush. */
    t.flushed_fade = t.applied;
    t.fade_held = false;
    t.quiet = false;
    t.uncover_since = t.alert_since = -1;
    for (auto &kv : assets_)
        if (kv.second.asset->target() == ti)
            kv.second.asset->attach(t.graph.get());
    for (auto &kv : layers_) {
        Entry &e = kv.second;
        if (e.layer->target() != ti || e.composited)
            continue;
        if (std::string(e.layer->kind()) == "video" && !may_make_video(ti)) {
            e.placed = false;   /* reconcile_make attaches it */
            t.make_waiting = true;
            continue;
        }
        attach(e, t);
    }
    if (t.composite) {
        if (may_make_video(ti))
            attach_composite(t);
        else
            t.make_waiting = true;
    }
    for (auto &kv : layers_)
        if (kv.second.layer->target() == ti)
            apply_binds(kv.second);
    /* Drawn in the scene's first flush, before the display fetches it;
     * layers sampling assets once those are written. */
    for (size_t pos = 0; pos < t.arr.order.size(); pos++) {
        Entry &e = layers_[t.arr.order[pos]];
        if (!e.composited && !e.layer->attached())
            continue;
        const bool drawn =
            std::find(t.arr.drawn.begin(), t.arr.drawn.end(), t.arr.order[pos]) != t.arr.drawn.end() &&
            assets_clean(e);
        if (e.composited && t.composite)
            t.composite->arrange_source(t.arr.order[pos], drawn, (int32_t)pos * 4);
        else
            e.layer->arrange(drawn, (int32_t)pos * 4);
        e.drawn = drawn;
        e.order = (int32_t)pos * 4;
    }
    if (t.composite)
        t.composite->arranged();
    return true;
}

void Compositor::unpublish(const TargetSpec &spec)
{
    const int ti = slots_->index(spec.name);
    Target &t = targets_[ti];
    for (auto &kv : layers_)
        if (kv.second.layer->target() == ti) {
            kv.second.layer->detach(false);
            kv.second.drawn = false;
            kv.second.order = -1;
        }
    for (auto &kv : assets_)
        if (kv.second.asset->target() == ti)
            kv.second.asset->detach(false);
    if (t.composite)
        t.composite->detach(false);
    if (t.file)
        host_serial_++;   /* what it hosted is not drawable any more */
    t.file.reset();    /* before a graph that borrows its scene */
    t.graph.reset();
    t.file_of = 0;
    t.dirty = false;
    t.flushed_videos = 0;
}

const CompositeVideo *Compositor::composite(int target) const
{
    return target >= 0 && target < (int)targets_.size() ? targets_[target].composite.get() : nullptr;
}

bool Compositor::on_scene(const Entry &e) const
{
    if (e.composited) {
        const Target &t = targets_[e.layer->target()];
        return t.composite && t.composite->attached();
    }
    return e.layer->attached();
}

bool Compositor::composable_now(const Entry &e, const LayerParams &p, const VideoParams &vp, bool *held) const
{
    const bool is = composable(p, vp);
    *held = e.placed && is != e.composable && now_ms_ - targets_[e.layer->target()].members_ms < kRemakeMs;
    return *held ? e.composable : is;
}

std::vector<Compositor::VideoSpec> Compositor::video_specs(bool real) const
{
    std::vector<VideoSpec> out;
    for (const auto &kv : layers_) {
        if (std::string(kv.second.layer->kind()) != "video")
            continue;
        VideoSpec s;
        s.id = kv.first;
        s.target = kv.second.layer->target();
        s.params = kv.second.layer->params;
        s.vp = static_cast<const VideoLayer *>(kv.second.layer.get())->video();
        s.composable = real ? composable(s.params, s.vp) : composable_now(kv.second, s.params, s.vp, &s.held);
        out.push_back(s);
    }
    return out;
}

namespace {

uint32_t up(uint32_t v, uint32_t m) { return (v + m - 1) / m * m; }

}  // namespace

Compositor::VideoPlan Compositor::compose_plan(int ti, const std::vector<VideoSpec> &specs, const Rect *with) const
{
    VideoPlan pl;
    std::vector<const VideoSpec *> group;
    for (const VideoSpec &s : specs) {
        if (s.target != ti)
            continue;
        if (s.composable)
            group.push_back(&s);
        else
            pl.buffers++;
    }
    if (group.size() < 2) {
        pl.buffers += (uint32_t)group.size();
        return pl;
    }
    pl.buffers++;
    /* The canvas: the sources' rectangles together, within the target (to
     * even pixels), widened to what an atlas and packing take. */
    const TargetSpec &spec = slots_->targets()[ti];
    int64_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    VideoParams vp;
    vp.level_bits = 0;
    vp.dirty_threshold = UINT32_MAX;
    vp.dirty_settle_ms = 0;
    vp.fps = 0;
    vp.pan_blocks = true;
    vp.stall_ms = 0;
    bool unclocked = false;
    for (const VideoSpec *s : group) {
        /* What is drawn: its frame at its size, at even x and y (a video
         * held composited after it stopped being composable included). */
        Rect r;
        r.x = s->params.rect.x & ~1;
        r.y = s->params.rect.y & ~1;
        r.w = s->vp.w;
        r.h = s->vp.h;
        x0 = std::min<int64_t>(x0, r.x);
        y0 = std::min<int64_t>(y0, r.y);
        x1 = std::max<int64_t>(x1, (int64_t)r.x + r.w);
        y1 = std::max<int64_t>(y1, (int64_t)r.y + r.h);
        vp.dirty_threshold = std::min(vp.dirty_threshold, s->vp.dirty_threshold);
        if (s->vp.dirty_settle_ms > 0 && (vp.dirty_settle_ms == 0 || s->vp.dirty_settle_ms < vp.dirty_settle_ms))
            vp.dirty_settle_ms = s->vp.dirty_settle_ms;
        unclocked = unclocked || s->vp.fps == 0;
        vp.fps = std::max(vp.fps, s->vp.fps);
        vp.pan_blocks = vp.pan_blocks && s->vp.pan_blocks;
        vp.stall_ms = std::max(vp.stall_ms, s->vp.stall_ms);
        pl.composited.push_back(s->id);
    }
    if (unclocked)
        vp.fps = 0;
    if (with && with->w && with->h) {
        x0 = std::min<int64_t>(x0, with->x);
        y0 = std::min<int64_t>(y0, with->y);
        x1 = std::max<int64_t>(x1, (int64_t)with->x + with->w);
        y1 = std::max<int64_t>(y1, (int64_t)with->y + with->h);
    }
    x0 = std::max<int64_t>(x0, 0);
    y0 = std::max<int64_t>(y0, 0);
    x1 = std::min<int64_t>(x1, spec.width & ~1u);
    y1 = std::min<int64_t>(y1, spec.height & ~1u);
    if (x0 >= x1 || y0 >= y1)
        x0 = y0 = x1 = y1 = 0;   /* nothing of them is on the target */
    pl.x = (int32_t)x0;
    pl.y = (int32_t)y0;
    pl.clip.x = pl.x;
    pl.clip.y = pl.y;
    pl.clip.w = (uint32_t)(x1 - x0);
    pl.clip.h = (uint32_t)(y1 - y0);
    vp.w = up(std::max<uint32_t>(pl.clip.w, 64), 4);
    vp.h = up(std::max<uint32_t>(pl.clip.h, 32), 4);
    if (VideoLayer::size_error(vp.w, vp.h))
        pl.error = "the target's videos together span more than 1920x720";
    /* Its videos are drawn anywhere on the canvas within the target. */
    pl.clip.w = (uint32_t)std::min<int64_t>(vp.w, (int64_t)(spec.width & ~1u) - pl.x);
    pl.clip.h = (uint32_t)std::min<int64_t>(vp.h, (int64_t)(spec.height & ~1u) - pl.y);
    pl.vp = vp;
    pl.bytes = (uint64_t)vp.w * crk::atlas_height(vp.h) * 4 + (vp.pan_blocks ? crk::block_bytes(vp.w, vp.h) : 0);
    return pl;
}

std::vector<Compositor::VideoPlan> Compositor::plan_videos(const std::vector<VideoSpec> &specs,
                                                            std::string *err) const
{
    /* Every video separate while the display holds them all. */
    std::vector<VideoPlan> plans(targets_.size());
    uint32_t buffers = 0;
    for (const VideoSpec &s : specs) {
        plans[s.target].buffers++;
        buffers++;
    }
    err->clear();
    if (buffers <= limits_.videos)
        return plans;
    /* Past it, the targets whose composite saves the most buffers compose,
     * as few as it takes. */
    std::vector<VideoPlan> composed(targets_.size());
    std::vector<std::pair<uint32_t, int>> saves;
    const char *span = nullptr;
    for (size_t ti = 0; ti < targets_.size(); ti++) {
        composed[ti] = compose_plan((int)ti, specs);
        if (composed[ti].composited.size() < 2)
            continue;
        if (composed[ti].error)
            span = composed[ti].error;
        else
            saves.push_back(std::make_pair(plans[ti].buffers - composed[ti].buffers, (int)ti));
    }
    std::stable_sort(saves.begin(), saves.end(),
                     [](const std::pair<uint32_t, int> &a, const std::pair<uint32_t, int> &b) { return a.first > b.first; });
    for (const auto &sv : saves) {
        if (buffers <= limits_.videos)
            break;
        buffers -= sv.first;
        plans[sv.second] = composed[sv.second];
    }
    if (buffers > limits_.videos)
        *err = span ? span
                    : "too many video buffers on the display (at most " + std::to_string(limits_.videos) +
                          "): a target's videos share one past it, but a video turned, scaled or at an odd x or y "
                          "needs its own";
    return plans;
}

bool Compositor::videos_fit(const std::vector<VideoSpec> &specs, uint64_t *bytes, std::string *err)
{
    const std::vector<VideoPlan> plans = plan_videos(specs, err);
    if (!err->empty())
        return false;
    bool composite = false;
    for (const VideoPlan &pl : plans)
        composite = composite || !pl.composited.empty();
    *bytes = composites_cost(plans);
    return !composite || view_effects(err);
}

bool Compositor::videos_fit_both(const std::vector<VideoSpec> &held, const std::vector<VideoSpec> &real,
                                 uint64_t *bytes, std::string *err)
{
    uint64_t later = 0;
    if (!videos_fit(held, bytes, err) || !videos_fit(real, &later, err))
        return false;
    *bytes = std::max(*bytes, later);
    return true;
}

uint64_t Compositor::composites_cost(const std::vector<VideoPlan> &plans) const
{
    uint64_t bytes = 0;
    for (size_t ti = 0; ti < targets_.size(); ti++)
        bytes += std::max(plans[ti].bytes, targets_[ti].composite_bytes);
    return bytes;
}

bool Compositor::view_effects(std::string *err)
{
    if (!atlas_view_fx_)
        atlas_view_fx_ = gpu_->effect(kQuadVertexShader, kAtlasViewFragmentShader, err);
    if (atlas_view_fx_ && !block_view_fx_)
        block_view_fx_ = gpu_->effect(kQuadVertexShader, kBlockViewFragmentShader, err);
    return block_view_fx_ != 0;
}

bool Compositor::check(const Layer *l, const LayerParams &p, std::string *err)
{
    if (std::string(l->kind()) != "video")
        return true;
    std::vector<VideoSpec> specs = video_specs(), real = video_specs(true);
    for (VideoSpec &s : specs)
        if (s.id == l->id()) {
            s.params = p;
            s.composable = composable_now(layers_.at(s.id), p, s.vp, &s.held);
        }
    for (VideoSpec &s : real)
        if (s.id == l->id()) {
            s.params = p;
            s.composable = composable(p, s.vp);
        }
    uint64_t composites = 0, now_composites = 0;
    if (!videos_fit_both(specs, real, &composites, err))
        return false;
    for (const Target &t : targets_)
        now_composites += t.composite_bytes;
    if (memory() - now_composites + composites > limits_.pixel_bytes) {
        *err = "the service's pixel memory is full";
        return false;
    }
    return true;
}

void Compositor::attach_composite(Target &t)
{
    std::string err;
    if (!t.composite->attach(t.graph.get(), now_ms_, &err)) {
        t.composite_error = err;
        log_error("composite on %s: %s", slots_->targets()[t.composite->target()].name.c_str(), err.c_str());
    } else {
        t.composite_error.clear();
    }
    t.dirty = true;
}

void Compositor::reconcile()
{
    std::string err;
    const std::vector<VideoSpec> specs = video_specs();
    std::vector<VideoPlan> plans = plan_videos(specs, &err);
    if (!err.empty()) {
        /* Videos that do not fit (a change let in past what fits): no buffer
         * changes, every video keeps its mode, until a change fits again. */
        if (err != plan_error_)
            log_error("videos: %s; the display's videos stay as they are", err.c_str());
        plan_error_ = err;
        for (Target &t : targets_)
            t.hold_ms = t.remake_ms = -1;
        return;
    }
    plan_error_.clear();
    /* Each target's scene reaches the display with its own flush. A shown
     * target whose plan takes more video buffers than its scene holds keeps
     * its videos as they are until the display can hold its plan with every
     * other target's scene as flushed or as planned, whichever holds more
     * (another's dropped buffers go with that target's flush). */
    const size_t n = targets_.size();
    std::vector<uint32_t> after(n);
    std::vector<bool> grows(n), waits(n);
    for (size_t ti = 0; ti < n; ti++) {
        grows[ti] = targets_[ti].graph && plans[ti].buffers > video_buffers((int)ti);
        after[ti] = plans[ti].buffers;
    }
    for (size_t ti = 0; ti < n; ti++) {
        if (!grows[ti])
            continue;
        uint32_t sum = 0;
        for (size_t j = 0; j < n; j++)
            sum += video_hold((int)j, after[j]);
        if (sum > limits_.videos) {
            waits[ti] = true;
            after[ti] = video_buffers((int)ti);
        }
    }
    /* What goes, on every target first: so the display never holds more
     * video buffers than the plans allow. */
    for (size_t ti = 0; ti < n; ti++)
        if (!waits[ti])
            reconcile_drop((int)ti, plans[ti]);
    for (size_t ti = 0; ti < n; ti++) {
        if (waits[ti]) {
            /* render() reconciles again each tick while it waits. */
            targets_[ti].make_waiting = true;
            targets_[ti].remake_ms = -1;
        } else {
            reconcile_make((int)ti, plans[ti]);
        }
    }
    for (Target &t : targets_)
        t.hold_ms = -1;
    for (const VideoSpec &s : specs) {
        if (waits[s.target])
            continue;
        layers_[s.id].composable = s.composable;
        if (s.held)
            targets_[s.target].hold_ms = targets_[s.target].members_ms + kRemakeMs;
    }
}

static bool planned(const std::vector<uint32_t> &composited, uint32_t id)
{
    return std::find(composited.begin(), composited.end(), id) != composited.end();
}

static bool inside(const Rect &in, const Rect &out)
{
    return in.x >= out.x && in.y >= out.y && (int64_t)in.x + in.w <= (int64_t)out.x + out.w &&
           (int64_t)in.y + in.h <= (int64_t)out.y + out.h;
}

bool Compositor::keep_composite(int ti, VideoPlan *pl)
{
    Target &t = targets_[ti];
    const CompositeVideo *c = t.composite.get();
    t.remake_ms = -1;
    if (!c || pl->composited.empty())
        return false;
    const VideoParams &cv = c->video();
    const bool settings = cv.fps == pl->vp.fps && cv.dirty_threshold == pl->vp.dirty_threshold &&
                          cv.dirty_settle_ms == pl->vp.dirty_settle_ms && cv.pan_blocks == pl->vp.pan_blocks &&
                          cv.stall_ms == pl->vp.stall_ms;
    if (settings && c->x() == pl->x && c->y() == pl->y && c->clip() == pl->clip && cv.w == pl->vp.w &&
        cv.h == pl->vp.h) {
        t.smaller_since_ms = -1;
        return true;
    }
    int64_t due;
    if (settings && inside(pl->clip, c->clip())) {
        /* Its videos fit in a smaller one: made so once that has held. */
        if (t.smaller_since_ms < 0 || t.smaller != pl->clip) {
            t.smaller_since_ms = now_ms_;
            t.smaller = pl->clip;
        }
        due = std::max(t.smaller_since_ms + kShrinkMs, t.composite_made_ms + kRemakeMs);
    } else {
        /* Grown past it, or a joining video's settings: made over the old
         * rect and the new one, when the memory allows. */
        t.smaller_since_ms = -1;
        due = t.composite_made_ms + kRemakeMs;
        if (now_ms_ >= due) {
            std::vector<VideoSpec> specs = video_specs();
            const VideoPlan grown = compose_plan(ti, specs, &c->clip());
            if (!grown.error && memory() - t.composite_bytes + grown.bytes <= limits_.pixel_bytes) {
                const uint32_t buffers = pl->buffers;
                *pl = grown;
                pl->buffers = buffers;
            }
        }
    }
    if (now_ms_ >= due)
        return false;
    t.remake_ms = due;
    return true;
}

void Compositor::reconcile_drop(int ti, VideoPlan &pl)
{
    Target &t = targets_[ti];
    CompositeVideo *c = t.composite.get();
    if (c && !keep_composite(ti, &pl)) {
        if (c->attached()) {
            c->detach(true);
            t.dirty = true;
        }
        for (auto &kv : layers_)
            if (kv.second.layer->target() == ti)
                kv.second.composited = false;
        t.composite.reset();
        t.composite_bytes = 0;
        t.composite_error.clear();
        t.smaller_since_ms = t.remake_ms = -1;
        c = nullptr;
    }
    for (auto &kv : layers_) {
        Entry &e = kv.second;
        if (e.layer->target() != ti || std::string(e.layer->kind()) != "video")
            continue;
        VideoLayer *v = static_cast<VideoLayer *>(e.layer.get());
        if (planned(pl.composited, kv.first)) {
            if (!v->source()) {
                if (v->attached()) {
                    v->detach(true);
                    t.dirty = true;
                }
                v->to_source();
                t.members_ms = now_ms_;
                /* Drawn by the composite from now: its own held look goes
                 * with its objects, and the composite's deferred moves
                 * place its quads from what the client set. */
                if (e.held) {
                    e.held = false;
                    t.held_n--;
                    v->hold_scene(nullptr);
                }
                /* Its own objects' refusal goes with them. */
                e.error.clear();
                e.drawn = false;
                e.order = -1;
            }
        } else if (e.composited) {
            c->remove(kv.first);
            e.composited = false;
            t.dirty = true;
        }
    }
}

void Compositor::reconcile_make(int ti, const VideoPlan &pl)
{
    Target &t = targets_[ti];
    t.make_waiting = false;
    /* A new video, or a source drawing itself again, is attached once the
     * display can hold its buffers; a refusal stands until the scene is
     * published again, as for any layer. */
    std::vector<uint32_t> waiting;
    for (auto &kv : layers_) {
        Entry &e = kv.second;
        if (e.layer->target() != ti || std::string(e.layer->kind()) != "video" || planned(pl.composited, kv.first))
            continue;
        VideoLayer *v = static_cast<VideoLayer *>(e.layer.get());
        const bool moved = v->source() || !e.placed;
        if (v->source()) {
            v->direct();
            t.members_ms = now_ms_;
        }
        if (moved && t.graph && !v->attached()) {
            if (may_make_video(ti)) {
                attach(e, t);
            } else {
                waiting.push_back(kv.first);
                t.make_waiting = true;
            }
        }
    }
    for (auto &kv : layers_)
        if (kv.second.layer->target() == ti && !planned(waiting, kv.first))
            kv.second.placed = true;
    if (pl.composited.empty())
        return;
    CompositeVideo *c = t.composite.get();
    if (!c) {
        std::string err;
        if (!view_effects(&err))
            log_error("composite on %s: %s", slots_->targets()[ti].name.c_str(), err.c_str());
        t.composite.reset(new CompositeVideo(ti, pacer_for(ti), atlas_view_fx_, block_view_fx_, pl.vp, pl.x, pl.y,
                                             pl.clip, now_ms_));
        t.composite_bytes = pl.bytes;
        t.composite_made_ms = now_ms_;
        t.composite->set_fade(t.applied);
        c = t.composite.get();
        log_info("target %s: %zu videos composed into one %ux%u at %d,%d", slots_->targets()[ti].name.c_str(),
                 pl.composited.size(), pl.vp.w, pl.vp.h, pl.x, pl.y);
    }
    for (uint32_t id : pl.composited) {
        if (c->has(id))
            continue;
        Entry &e = layers_[id];
        c->add(id, static_cast<VideoLayer *>(e.layer.get()));
        e.composited = true;
        t.dirty = t.dirty || c->attached();
    }
    if (t.graph && !c->attached()) {
        if (may_make_video(ti))
            attach_composite(t);
        else
            t.make_waiting = true;
    }
}

uint32_t Compositor::video_buffers(int ti) const
{
    const Target &t = targets_[ti];
    uint32_t n = t.composite && t.composite->attached() ? 1 : 0;
    for (const auto &kv : layers_)
        n += kv.second.layer->target() == ti && !kv.second.composited &&
             std::string(kv.second.layer->kind()) == "video" && kv.second.layer->attached();
    return n;
}

uint32_t Compositor::video_hold(int ti, uint32_t pending) const
{
    const bool flushes = targets_[ti].graph && slots_->requested(ti) &&
                         slots_->status(ti).state != TargetState::Parked;
    return flushes ? std::max(targets_[ti].flushed_videos, pending) : pending;
}

bool Compositor::may_make_video(int ti) const
{
    uint32_t n = 0;
    for (size_t i = 0; i < targets_.size(); i++)
        n += video_hold((int)i, video_buffers((int)i) + ((int)i == ti ? 1 : 0));
    return n <= limits_.videos;
}

}  // namespace crs
