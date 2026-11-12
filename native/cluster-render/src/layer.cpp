/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "layer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "occlusion.h"

namespace crs {

Turn LayerParams::turn() const
{
    Turn t;
    t.rotate = rotate;
    t.sx = sx;
    t.sy = sy;
    t.px = pivot_x ? px : (float)rect.w / 2.0f;
    t.py = pivot_y ? py : (float)rect.h / 2.0f;
    return t;
}

/* ---- uniforms ------------------------------------------------------------- */

bool Uniforms::set(const std::string &name, const float *v, int n)
{
    auto it = values_.find(name);
    const size_t old = it == values_.end() ? 0 : it->second.v.size();
    if ((it == values_.end() && values_.size() >= kMaxNames) || floats_ - old + (size_t)n > kMaxFloats)
        return false;
    Value &u = values_[name];
    floats_ = floats_ - old + (size_t)n;
    u.v.assign(v, v + n);
    u.n = n;
    u.dirty = true;
    return true;
}

bool Uniforms::set(const std::string &name, UniformType t, const float *v, uint32_t elements)
{
    const size_t n = (size_t)elements * uniform_width(t);
    auto it = values_.find(name);
    const size_t old = it == values_.end() ? 0 : it->second.v.size();
    if ((it == values_.end() && values_.size() >= kMaxNames) || floats_ - old + n > kMaxFloats)
        return false;
    Value &u = values_[name];
    floats_ = floats_ - old + n;
    u.v.assign(v, v + n);
    u.n = 0;
    u.type = t;
    u.elements = elements;
    u.dirty = true;
    return true;
}

void Uniforms::dirty_all()
{
    for (auto &kv : values_)
        kv.second.dirty = true;
}

void Uniforms::dirty(const std::string &name)
{
    auto it = values_.find(name);
    if (it != values_.end())
        it->second.dirty = true;
}

bool Uniforms::stage(Graph *g, int drawable)
{
    bool any = false;
    for (auto &kv : values_) {
        Value &u = kv.second;
        if (!u.dirty)
            continue;
        const bool went = u.n ? g->uniform(drawable, kv.first.c_str(), u.v.data(), u.n)
                              : g->uniform_array(drawable, kv.first.c_str(), u.type, u.v.data(), u.elements);
        if (!went)
            u.dirty = false;
        else
            any = true;
    }
    return any;
}

void Uniforms::committed()
{
    for (auto &kv : values_)
        kv.second.dirty = false;
}

/* ---- animations ----------------------------------------------------------- */

const char *quad_animation_error(uint32_t part, const Animation &a)
{
    if (part)
        return "image and effect layers have no parts";
    switch (a.property) {
    case Animated::Rotate:
        return a.width == 1 ? nullptr : "a quad's rotate takes 1 value (degrees clockwise)";
    case Animated::Translate:
    case Animated::Scale:
        return a.width == 3 ? nullptr : "translate and scale take 3 values";
    default:
        return a.width == 1 || a.width == 3 || a.width == 4 ? nullptr : "a uniform takes 1, 3 or 4 values";
    }
}

bool Layer::animate(uint32_t anim, uint32_t part, const Animation &a, std::string *err)
{
    if (!played_.count(anim) && played_.size() - played_.count(0) >= kMaxAnimations) {
        *err = "at most " + std::to_string(kMaxAnimations) + " animations a layer";
        return false;
    }
    return play(anim, part, a, err);
}

bool Layer::play(uint32_t anim, uint32_t part, const Animation &a, std::string *err)
{
    if (const char *e = animation_error(part, a)) {
        *err = e;
        return false;
    }
    stop(anim);
    Played p;
    p.part = part;
    p.a = a;
    if (g_ && defer_starts_) {
        p.pending = true;   /* the renderer's refusal is logged when it starts */
    } else if (g_) {
        const int node = animated(part);
        p.handle = node ? g_->animate(node, a) : 0;
        if (!p.handle) {
            *err = "the renderer refused the animation";
            return false;
        }
    }
    played_[anim] = p;
    return true;
}

bool Layer::stop(uint32_t anim)
{
    auto it = played_.find(anim);
    if (it == played_.end())
        return false;
    if (g_ && it->second.handle && !it->second.pending) {
        g_->stop(it->second.handle);
        restore(it->second.part, it->second.a);
    }
    played_.erase(it);
    return true;
}

bool Layer::animations_started() const
{
    for (const auto &kv : played_)
        if (!kv.second.pending)
            return true;
    return false;
}

bool Layer::start_pending()
{
    bool any = false;
    for (auto &kv : played_) {
        if (!kv.second.pending || !g_)
            continue;
        kv.second.pending = false;
        any = true;
        const int node = animated(kv.second.part);
        kv.second.handle = node ? g_->animate(node, kv.second.a) : 0;
        if (!kv.second.handle)
            log_warn("layer %u: animation %u refused by the renderer", id_, kv.first);
    }
    return any;
}

void Layer::start_animations()
{
    for (auto &kv : played_) {
        kv.second.pending = false;
        const int node = animated(kv.second.part);
        kv.second.handle = node ? g_->animate(node, kv.second.a) : 0;
        if (!kv.second.handle)
            log_warn("layer %u: animation %u refused by the renderer", id_, kv.first);
    }
}

void Layer::forget_animations()
{
    for (auto &kv : played_)
        kv.second.handle = 0;
}

/* ---- image ---------------------------------------------------------------- */

ImageLayer::ImageLayer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer, int effect, uint32_t w,
                       uint32_t h, uint32_t fps, Sampling sampling, int64_t now_ms)
    : Layer(id, target, seq, pacer), effect_(effect), w_(w), h_(h), sampling_(sampling),
      px_((size_t)w * h * 4, 0)
{
    clock_.configure(fps);
    clock_.reset(now_ms);
}

const char *ImageLayer::size_error(uint32_t w, uint32_t h)
{
    if (w < 1 || h < 1)
        return "image size must be at least 1x1";
    if (w > 1920 || h > 720)
        return "image larger than 1920x720";
    return nullptr;
}

bool ImageLayer::update(const std::vector<crk::PixelRect> &rects, const uint8_t *data, size_t n, std::string *err)
{
    if (rects.empty()) {
        if (n != px_.size()) {
            *err = "a whole image is " + std::to_string(px_.size()) + " bytes";
            return false;
        }
        std::memcpy(px_.data(), data, n);
        whole_ = true;
        dirty_.clear();
        have_ = true;
        if (opq_on_) {
            std::fill(opq_px_.begin(), opq_px_.end(), 2);
            opq_ver_++;
        }
        return true;
    }
    size_t need = 0;
    for (const crk::PixelRect &r : rects) {
        if (r.w == 0 || r.h == 0 || r.x + r.w > w_ || r.y + r.h > h_ || r.x + r.w < r.x || r.y + r.h < r.y) {
            *err = "rectangle outside the image";
            return false;
        }
        need += (size_t)r.w * r.h * 4;
    }
    if (n != need) {
        *err = "rectangles need " + std::to_string(need) + " bytes";
        return false;
    }
    for (const crk::PixelRect &r : rects) {
        for (uint32_t y = 0; y < r.h; y++)
            std::memcpy(&px_[((size_t)(r.y + y) * w_ + r.x) * 4], data + (size_t)y * r.w * 4, (size_t)r.w * 4);
        data += (size_t)r.w * r.h * 4;
        if (opq_on_) {
            opq_changed(r);
            opq_ver_++;
        }
        if (!whole_) {
            if (dirty_.size() >= kMaxRects) {
                whole_ = true;
                dirty_.clear();
            } else {
                dirty_.push_back(r);
            }
        }
    }
    have_ = true;
    return true;
}

bool ImageLayer::attach(Graph *g, int64_t, std::string *err)
{
    g_ = g;
    written_ = false;
    buf_ = g->buffer(TexFormat::RGBA8, w_, h_, sampling_);
    quad_ = buf_ ? g->quad(effect_, drawn_params().rect) : 0;
    const float op = drawn_params().opacity * fade_;
    if (!quad_ || !g->texture(quad_, "u_image", buf_) || !g->uniform(quad_, "u_opacity", &op, 1) ||
        !g->turn(quad_, drawn_params().turn()) || !g->show(quad_, false) || !g->order(quad_, order_)) {
        *err = "image buffer or quad refused";
        detach(true);
        return false;
    }
    drawn_ = false;
    whole_ = true;   /* a new buffer holds nothing yet */
    opq_buf_known_ = false;
    opq_ver_++;
    start_animations();
    return true;
}

void ImageLayer::detach(bool drop)
{
    forget_animations();
    if (g_ && drop) {
        if (quad_)
            g_->drop_quad(quad_);
        if (buf_)
            g_->drop_buffer(buf_);
    }
    g_ = nullptr;
    buf_ = quad_ = 0;
    drawn_ = parked_ = hold_ = false;
    whole_ = true;
    opq_buf_known_ = false;
    opq_ver_++;
}

void ImageLayer::arrange(bool drawn, int32_t order)
{
    order_ = order;
    if (!g_)
        return;
    if (drawn != drawn_)
        g_->show(quad_, drawn && (!parked_ || hold_));
    g_->order(quad_, order);
    drawn_ = drawn;
}

void ImageLayer::place()
{
    if (!g_)
        return;
    const float op = drawn_params().opacity * fade_;
    g_->place(quad_, drawn_params().rect);
    g_->turn(quad_, drawn_params().turn());
    g_->uniform(quad_, "u_opacity", &op, 1);
}

void ImageLayer::faded()
{
    if (!g_)
        return;
    const float op = drawn_params().opacity * fade_;
    g_->uniform(quad_, "u_opacity", &op, 1);
}

void ImageLayer::restore(uint32_t, const Animation &)
{
    place();
}

Stage ImageLayer::stage(int64_t now_ms)
{
    staged_ = 0;
    if (!g_ || !drawn_ || !have_ || (!whole_ && dirty_.empty()))
        return Stage::Idle;
    if (!clock_.due(now_ms) && !holding())
        return Stage::Idle;
    if (!pacer_.ready(now_ms)) {
        held_budget_++;
        return Stage::Idle;
    }
    const std::vector<crk::PixelRect> all{crk::PixelRect{0, 0, w_, h_}};
    bool written = true;
    staged_ = crk::write_rects(
        px_.data(), w_, 4, whole_ ? all : dirty_, &scratch_,
        [this](const uint8_t *px, const crk::PixelRect &r) { return g_->write(buf_, r.x, r.y, r.w, r.h, px); },
        &written);
    if (written && parked_) {
        parked_ = false;   /* whole_ was set with it: this write is whole */
        g_->show(quad_, true);
    }
    return written ? Stage::Wrote : Stage::Failed;
}

void ImageLayer::committed(bool ok, int64_t now_ms)
{
    pacer_.spend(staged_);
    clock_.sent(now_ms);
    if (ok) {
        writes_++;
        bytes_ += staged_;
        written_ = written_ || staged_ > 0;
        whole_ = false;
        /* The write carried every change waiting: the buffer is px_. */
        if (opq_on_) {
            opq_resolve();
            opq_buf_ = opq_px_;
            opq_buf_known_ = true;
            opq_ver_++;
        }
    } else {
        opq_buf_known_ = false;
        opq_ver_++;
        /* The buffer may hold part of it unflushed: write it all again. An
         * exclusive image holds the target until then (the layers below it
         * are suspended, so hiding it would show nothing); any other is
         * hidden until then, so no other layer's flush shows it half
         * written. */
        failed_++;
        whole_ = true;
        parked_ = true;
        hold_ = params.exclusive;
        if (g_ && !hold_)
            g_->show(quad_, false);
    }
    dirty_.clear();
    staged_ = 0;
}

void ImageLayer::opq_changed(const crk::PixelRect &r)
{
    for (uint32_t by = r.y / kOpaqueBlock; by <= (r.y + r.h - 1) / kOpaqueBlock; by++)
        std::fill(&opq_px_[(size_t)by * opq_cols_ + r.x / kOpaqueBlock],
                  &opq_px_[(size_t)by * opq_cols_ + (r.x + r.w - 1) / kOpaqueBlock] + 1, 2);
}

void ImageLayer::opq_resolve()
{
    for (uint32_t by = 0; by < opq_rows_; by++)
        for (uint32_t bx = 0; bx < opq_cols_; bx++) {
            uint8_t &o = opq_px_[(size_t)by * opq_cols_ + bx];
            if (o != 2)
                continue;
            const uint32_t x0 = bx * kOpaqueBlock, x1 = std::min(w_, x0 + kOpaqueBlock);
            const uint32_t y0 = by * kOpaqueBlock, y1 = std::min(h_, y0 + kOpaqueBlock);
            uint8_t a = 255;
            for (uint32_t y = y0; y < y1 && a == 255; y++) {
                const uint8_t *p = &px_[((size_t)y * w_ + x0) * 4 + 3];
                for (uint32_t x = x0; x < x1; x++, p += 4)
                    a &= *p;
            }
            o = a == 255;
        }
}

void ImageLayer::opq_summary()
{
    if (opq_sum_ver_ == opq_ver_)
        return;
    opq_resolve();
    opq_all_buf_ = opq_all_both_ = true;
    opq_none_buf_ = opq_none_both_ = true;
    for (size_t b = 0; b < opq_px_.size(); b++) {
        const bool buf = opq_buf_[b] != 0, both = buf && opq_px_[b] == 1;
        opq_all_buf_ = opq_all_buf_ && buf;
        opq_none_buf_ = opq_none_buf_ && !buf;
        opq_all_both_ = opq_all_both_ && both;
        opq_none_both_ = opq_none_both_ && !both;
    }
    opq_sum_ver_ = opq_ver_;
}

void ImageLayer::occlude(Coverage *cov, const Shown &s)
{
    const Rect &r = s.rect;
    defer_starts_ = false;
    if (!g_ || !s.drawn || parked_ || s.animating || s.opacity * s.fade < 1.0f || s.turned || !r.w || !r.h)
        return;
    if (!opq_on_) {
        opq_on_ = true;
        opq_cols_ = (w_ + kOpaqueBlock - 1) / kOpaqueBlock;
        opq_rows_ = (h_ + kOpaqueBlock - 1) / kOpaqueBlock;
        opq_px_.assign((size_t)opq_cols_ * opq_rows_, 2);
        opq_buf_.assign(opq_px_.size(), 0);
        /* The buffer holds px_ while nothing waits to be written. */
        opq_buf_known_ = !whole_ && dirty_.empty();
        if (opq_buf_known_) {
            opq_resolve();
            opq_buf_ = opq_px_;
        }
        opq_ver_++;
    }
    if (!opq_buf_known_)
        return;
    opq_summary();
    /* What the buffer holds shows in the next flush, and the pixels waiting
     * too when the budget allows the image's write this tick; while it
     * does not, they cannot reach the scene. */
    const bool both = s.budget_ready;
    if (both ? opq_none_both_ : opq_none_buf_)
        return;
    if (both ? opq_all_both_ : opq_all_buf_) {
        cov->add(box_of(r));
        defer_starts_ = true;
        return;
    }
    /* A target pixel samples about the image coordinate of its centre: the
     * image pixels under it, one more each way for the filter. */
    defer_starts_ = cov->add_if(box_of(r), [&](const Box &c) {
        const int64_t ix0 = std::max<int64_t>(0, (int64_t)(c.x0 - r.x) * w_ / r.w - 1);
        const int64_t ix1 = std::min<int64_t>(w_, ((int64_t)(c.x1 - r.x) * w_ + r.w - 1) / r.w + 1);
        const int64_t iy0 = std::max<int64_t>(0, (int64_t)(c.y0 - r.y) * h_ / r.h - 1);
        const int64_t iy1 = std::min<int64_t>(h_, ((int64_t)(c.y1 - r.y) * h_ + r.h - 1) / r.h + 1);
        for (int64_t by = iy0 / kOpaqueBlock; by <= (iy1 - 1) / kOpaqueBlock; by++)
            for (int64_t bx = ix0 / kOpaqueBlock; bx <= (ix1 - 1) / kOpaqueBlock; bx++) {
                const size_t b = (size_t)by * opq_cols_ + (size_t)bx;
                if (!opq_buf_[b] || (both && opq_px_[b] != 1))
                    return false;
            }
        return true;
    });
}

int64_t ImageLayer::wake_in_ms(int64_t now_ms) const
{
    if (!g_ || !drawn_ || !have_ || (!whole_ && dirty_.empty()))
        return -1;
    const int64_t w = clock_.ms_until_due(now_ms);
    return w > 0 ? w : -1;
}

WriteStats ImageLayer::write_stats() const
{
    WriteStats w;
    w.frames = writes_;
    w.bytes = bytes_;
    w.failed = failed_;
    w.holds = held_budget_;
    return w;
}

std::string ImageLayer::stats() const
{
    char b[96];
    std::snprintf(b, sizeof b, "%llu writes, %llu KB, %llu failed", (unsigned long long)writes_,
                  (unsigned long long)(bytes_ / 1024), (unsigned long long)failed_);
    return b;
}

/* ---- effect --------------------------------------------------------------- */

EffectLayer::EffectLayer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer, int effect,
                         uint32_t time_fps, int64_t now_ms)
    : Layer(id, target, seq, pacer), effect_(effect), time_fps_(time_fps), born_ms_(now_ms)
{
    time_clock_.configure(time_fps);
    time_clock_.reset(now_ms);
    if (time_fps_) {
        const float zero = 0.0f;
        set_uniform("u_time", &zero, 1);
    }
}

bool EffectLayer::set_uniform(const std::string &name, const float *v, int n)
{
    return uniforms_.set(name, v, n);
}

bool EffectLayer::set_uniform(const std::string &name, UniformType t, const float *v, uint32_t elements)
{
    return uniforms_.set(name, t, v, elements);
}

void EffectLayer::cluster_time()
{
    /* An hour of seconds, then again: the value stays small enough for a
     * shader's highp float to keep millisecond steps. */
    Animation a;
    a.property = Animated::Uniform;
    a.uniform = "u_time";
    a.width = 1;
    a.loop = true;
    a.t = {0, 3600000};
    a.v = {0.0f, 3600.0f};
    std::string err;
    if (!play(0, 0, a, &err))
        log_warn("layer %u: u_time: %s", id_, err.c_str());
}

bool EffectLayer::attach(Graph *g, int64_t, std::string *err)
{
    g_ = g;
    quad_ = g->quad(effect_, drawn_params().rect);
    if (!quad_ || !g->turn(quad_, drawn_params().turn()) || !g->show(quad_, false) || !g->order(quad_, order_)) {
        *err = "effect quad refused";
        detach(true);
        return false;
    }
    drawn_ = false;
    uniforms_.dirty_all();
    start_animations();
    return true;
}

void EffectLayer::detach(bool drop)
{
    forget_animations();
    if (g_ && drop && quad_)
        g_->drop_quad(quad_);
    g_ = nullptr;
    quad_ = 0;
    drawn_ = false;
}

void EffectLayer::arrange(bool drawn, int32_t order)
{
    order_ = order;
    if (!g_)
        return;
    if (drawn != drawn_)
        g_->show(quad_, drawn);
    g_->order(quad_, order);
    drawn_ = drawn;
}

void EffectLayer::place()
{
    if (!g_)
        return;
    g_->place(quad_, drawn_params().rect);
    g_->turn(quad_, drawn_params().turn());
}

void EffectLayer::restore(uint32_t, const Animation &a)
{
    if (a.property == Animated::Uniform)
        uniforms_.dirty(a.uniform);
    else
        place();
}

Stage EffectLayer::stage(int64_t now_ms)
{
    staged_ = false;
    if (!g_ || !drawn_)
        return Stage::Idle;
    if (time_fps_ && time_clock_.due(now_ms)) {
        const float t = (float)(now_ms - born_ms_) / 1000.0f;
        set_uniform("u_time", &t, 1);
    }
    staged_ = uniforms_.stage(g_, quad_);
    return staged_ ? Stage::Wrote : Stage::Idle;
}

void EffectLayer::committed(bool ok, int64_t now_ms)
{
    if (!ok)
        return;   /* still dirty: sent again with the next flush */
    uniforms_.committed();
    if (time_fps_ && time_clock_.due(now_ms))
        time_clock_.sent(now_ms);
    staged_ = false;
}

int64_t EffectLayer::wake_in_ms(int64_t now_ms) const
{
    if (!g_ || !drawn_ || !time_fps_)
        return -1;
    const int64_t w = time_clock_.ms_until_due(now_ms);
    return w > 0 ? w : -1;
}

}  // namespace crs
