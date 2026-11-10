/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "compositor.h"

#include <time.h>

#include <algorithm>
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
        t.graph.reset();
        t.file.reset();
    }
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

uint32_t Compositor::admit(uint64_t owner, int target, size_t pixel_bytes, std::string *err)
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
    if (memory() + pixel_bytes > limits_.pixel_bytes) {
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
        bytes += (uint64_t)kv.second.asset->bytes() * 2;   /* its pixels and a rectangle scratch */
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
    uint32_t videos = 0;
    for (const auto &kv : layers_)
        videos += std::string(kv.second.layer->kind()) == "video";
    if (videos >= limits_.videos) {
        *err = "too many video layers";
        return 0;
    }
    const size_t atlas = (size_t)vp.w * crk::atlas_height(vp.h);
    const size_t bytes = atlas * 4 + (vp.pan_blocks ? crk::block_bytes(vp.w, vp.h) : 0);
    const uint32_t id = admit(owner, target, bytes, err);
    if (!id)
        return 0;
    Entry &e = layers_[id];
    e.layer.reset(new VideoLayer(id, target, ++seq_, pacer_for(target), atlas_fx_, block_fx_, vp, now_ms));
    e.layer->params = lp;
    e.owner = owner;
    e.pixel_bytes = bytes;
    if (targets_[target].graph)
        attach(e, targets_[target]);
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
    const uint64_t bytes = (uint64_t)ap.w * ap.h * (ap.format == TexFormat::R8 ? 1 : 4) * 2;
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
    return true;
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
    return "on " + slots_->targets()[s.target].name + ": " + s.src.file + " and " +
           std::to_string(s.src.res.size()) + " resource files, " + std::to_string(s.src.bytes / 1024) + " KB; " +
           std::to_string(s.edits.kept()) + " edit lines kept";
}

uint32_t Compositor::content(int target) const
{
    for (const auto &kv : scenes_)
        if (kv.second.target == target && kv.second.error.empty())
            return kv.first;
    return 0;
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
    l->place();
    Target &t = targets_[l->target()];
    if (t.graph)
        t.dirty = true;
}

void Compositor::remove(uint64_t owner, uint32_t id)
{
    /* A scene content: its scene is stale now (the slot manager). */
    auto s = scenes_.find(id);
    if (s != scenes_.end()) {
        if (s->second.owner == owner)
            scenes_.erase(s);
        return;
    }
    auto it = layers_.find(id);
    if (it == layers_.end() || it->second.owner != owner)
        return;
    Target &t = targets_[it->second.layer->target()];
    it->second.layer->detach(true);
    if (t.graph)
        t.dirty = true;
    drop_effects(it->second);
    layers_.erase(it);
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
    for (auto it = scenes_.begin(); it != scenes_.end();)
        it = it->second.owner == owner ? scenes_.erase(it) : std::next(it);
}

void Compositor::arrange(int64_t now_ms)
{
    now_ms_ = now_ms;
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
            it.visible = l.params.visible && e.error.empty() && l.refusal().empty();
            it.ready = l.ready(now_ms) && assets_have(e);
            it.exclusive = l.params.exclusive;
            it.prewarm = l.params.prewarm;
            it.contend = l.params.contend;
            it.guide_hold = l.params.guide_hold;
            it.yield = l.params.yield;
            items.push_back(it);
        }
        t.arr = crs::arrange(items);
        if (const uint32_t sc = content((int)ti)) {
            /* The target's scene is the content's file: it wants the
             * target, and every layer waits (state()). */
            t.arr.drawn.clear();
            t.arr.wanted = true;
            t.arr.contend = t.arr.contend || scenes_[sc].contend;
        }
        fade((int)ti, t, now_ms);
        slots_->want((int)ti, t.arr.wanted, t.arr.contend, t.arr.guide_hold, t.arr.yield);
        if (!t.graph)
            continue;
        for (size_t pos = 0; pos < t.arr.order.size(); pos++) {
            Entry &e = layers_[t.arr.order[pos]];
            if (!e.layer->attached())
                continue;
            const bool drawn =
                std::find(t.arr.drawn.begin(), t.arr.drawn.end(), t.arr.order[pos]) != t.arr.drawn.end() &&
                assets_clean(e);
            const int32_t order = (int32_t)pos * 4;
            if (drawn != e.drawn || order != e.order) {
                e.layer->arrange(drawn, order);
                e.drawn = drawn;
                e.order = order;
                t.dirty = true;
            }
        }
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
            if (it == layers_.end() || !it->second.layer->attached() || it->second.layer->target() != ti)
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
    for (auto &kv : layers_) {
        Layer &l = *kv.second.layer;
        if (l.target() != ti || l.fade() == t.applied)
            continue;
        l.set_fade(t.applied);
        if (l.attached() && l.fades())
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
    for (size_t ti = 0; ti < targets_.size(); ti++) {
        Target &t = targets_[ti];
        /* Released: nothing of it is asked for. Parked: nothing of it is
         * on screen, and the link carries the HMI's content too. It stages
         * again once shown. */
        if ((!t.graph && !t.file) || !slots_->requested((int)ti) ||
            slots_->status((int)ti).state == TargetState::Parked)
            continue;
        if (t.file) {
            render_file((int)ti, t, now_ms);
            continue;
        }
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
            if (hold || a.target() != (int)ti || !a.attached())
                continue;
            const Stage s = a.stage(now_ms);
            if (s == Stage::Idle)
                continue;
            staged_assets.push_back(&a);
            wrote = wrote || s == Stage::Wrote;
            failed = failed || s == Stage::Failed;
        }
        for (size_t pos = t.arr.order.size(); pos-- > 0;) {
            auto it = layers_.find(t.arr.order[pos]);
            if (it == layers_.end() || !it->second.layer->attached())
                continue;
            if (hold && !it->second.layer->holds_flush())
                continue;
            const Stage s = it->second.layer->stage(now_ms);
            if (s == Stage::Idle)
                continue;
            staged.push_back(it->second.layer.get());
            wrote = wrote || s == Stage::Wrote;
            failed = failed || s == Stage::Failed;
        }
        if (!wrote && !failed && !t.dirty)
            continue;
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
        if (!failed && !holds(t))
            ok = flush((int)ti, t, now_ms);
        for (Asset *a : staged_assets)
            a->committed(ok);
        for (Layer *l : staged)
            l->committed(ok, now_ms);
        /* What the layers undid for a flush that did not go goes with the
         * next one. */
        t.dirty = !ok;
    }
    if (governor_.tick(now_ms))
        pacer_.set_rate(governor_.cap());
}

bool Compositor::flush(int ti, Target &t, int64_t now_ms)
{
    const TargetSpec &spec = slots_->targets()[ti];
    const int64_t t0 = clock_us_();
    const bool ok = t.file ? t.file->flush(++t.tag) : t.graph->flush(++t.tag);
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
    return ok;
}

void Compositor::render_file(int ti, Target &t, int64_t now_ms)
{
    /* The content gone, its scene stays as last flushed until released. */
    auto s = scenes_.find(t.file_of);
    const bool mine = s != scenes_.end() && s->second.serial == t.file_serial;
    /* Once a tick: lines coming faster stay due (wake_in_ms). */
    if (mine && !t.dirty && file_tick_ms_ > 0 && t.file_flush_ms >= 0 && now_ms - t.file_flush_ms < file_tick_ms_)
        return;
    const bool staged = mine && s->second.edits.stage(t.file.get());
    if (staged || t.dirty) {
        t.dirty = !flush(ti, t, now_ms);
        t.file_flush_ms = now_ms;
    }
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
    if (!bytes)
        return;
    charge(ti, bytes, now_ms);
    log_info("target %s: its scene goes whole to the renderer again: %llu KB charged to the upload budget",
             spec.name.c_str(), (unsigned long long)(bytes / 1024));
}

bool Compositor::holds(const Target &t) const
{
    for (uint32_t id : t.arr.order) {
        auto it = layers_.find(id);
        if (it != layers_.end() && it->second.layer->attached() && it->second.layer->holds_flush())
            return true;
    }
    return false;
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
    if (!l.params.visible)
        return LayerState::Hidden;
    if (!l.ready(now_ms_) || !assets_have(e))
        return LayerState::Stalled;
    if (content(l.target())) {
        *reason = "scene";
        return LayerState::Waiting;
    }
    const Target &t = targets_[l.target()];
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
            /* The client's file: it wants nothing more; the layers come back. */
            s.error = *err;
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
        return true;
    }
    t.graph = gpu_->scene(spec, err);
    if (!t.graph)
        return false;
    t.tag = 0;
    t.flush_failing = false;
    for (auto &kv : assets_)
        if (kv.second.asset->target() == ti)
            kv.second.asset->attach(t.graph.get());
    for (auto &kv : layers_)
        if (kv.second.layer->target() == ti)
            attach(kv.second, t);
    for (auto &kv : layers_)
        if (kv.second.layer->target() == ti)
            apply_binds(kv.second);
    /* Drawn in the scene's first flush, before the display fetches it;
     * layers sampling assets once those are written. */
    for (size_t pos = 0; pos < t.arr.order.size(); pos++) {
        Entry &e = layers_[t.arr.order[pos]];
        if (!e.layer->attached())
            continue;
        const bool drawn =
            std::find(t.arr.drawn.begin(), t.arr.drawn.end(), t.arr.order[pos]) != t.arr.drawn.end() &&
            assets_clean(e);
        e.layer->arrange(drawn, (int32_t)pos * 4);
        e.drawn = drawn;
        e.order = (int32_t)pos * 4;
    }
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
    t.graph.reset();
    t.file.reset();
    t.file_of = 0;
    t.dirty = false;
}

}  // namespace crs
