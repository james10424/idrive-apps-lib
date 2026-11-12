/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "video_layer.h"

#include <algorithm>

#include <cstdio>

#include "block_pack.h"
#include "yuv_atlas.h"

namespace crs {

using crk::PixelRect;
using crk::UploadPlan;

VideoLayer::VideoLayer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer, int atlas_effect,
                       int block_effect, const VideoParams &p, int64_t now_ms)
    : Layer(id, target, seq, pacer), p_(p), atlas_effect_(atlas_effect), block_effect_(block_effect),
      atlas_bytes_((size_t)p.w * crk::atlas_height(p.h)), atlas_h_(crk::atlas_height(p.h)),
      planner_(p.w, crk::atlas_height(p.h), 1, crk::kTileSize, crk::kTileSize, p.dirty_threshold, p.dirty_settle_ms),
      motion_(p.w, crk::atlas_height(p.h), 1, crk::kTileSize, crk::kTileSize, p.dirty_threshold, 0)
{
    if (p_.h % 4)
        p_.pan_blocks = false;
    if (p_.pan_blocks)
        packed_.resize(crk::block_bytes(p_.w, p_.h));
    clock_.configure(p_.fps);
    clock_.reset(now_ms);
}

const char *VideoLayer::size_error(uint32_t w, uint32_t h)
{
    if (w < 64 || h < 32)
        return "video smaller than 64x32";
    if (w > 1920 || h > 720)
        return "video larger than 1920x720";
    if (w % 4 || h % 2)
        return "video width must be a multiple of 4 and its height even";
    return nullptr;
}

void VideoLayer::offer(const uint8_t *atlas, int64_t now_ms)
{
    if (front_pending_)
        superseded_++;   /* the newest frame replaces the unsent one: it skips ahead, never replays */
    repeated_ = front_stale_ = false;
    front_.resize(atlas_bytes_);
    crk::round_levels(front_.data(), atlas, atlas_bytes_, p_.level_bits);
    front_pending_ = true;
    front_ms_ = last_frame_ms_ = now_ms;
    have_ = true;
}

void VideoLayer::repeat(int64_t now_ms)
{
    if (!have_)
        return;
    if (front_stale_) {
        front_ = shown_;   /* once after each whole write */
        front_stale_ = false;
    }
    if (front_pending_)
        superseded_++;
    else
        repeated_ = true;
    front_pending_ = true;
    front_ms_ = last_frame_ms_ = now_ms;
}

void VideoLayer::to_source()
{
    source_ = true;
    /* The newest frame into front_, as attach() finds it. */
    if (!front_pending_ && !planner_.quiet() && !held_n_ && shown_.size() == atlas_bytes_) {
        front_.swap(shown_);
        front_ms_ = shown_ms_;
    }
    repeated_ = front_stale_ = false;
    front_pending_ = false;
}

void VideoLayer::direct()
{
    source_ = false;
    repeated_ = false;
    front_pending_ = front_.size() == atlas_bytes_ && front_ms_ >= 0;
}

uint8_t *VideoLayer::taken()
{
    if (shown_.size() != atlas_bytes_)
        shown_.assign(atlas_bytes_, 0);
    return shown_.data();
}

uint8_t *VideoLayer::canvas()
{
    if (front_.size() != atlas_bytes_) {
        front_.assign(atlas_bytes_, 128);   /* black: Y 0, U and V neutral */
        std::fill(front_.begin(), front_.begin() + (size_t)p_.w * p_.h, 0);
    } else if (front_stale_) {
        front_ = shown_;   /* once after each whole write */
    }
    front_stale_ = false;
    return front_.data();
}

void VideoLayer::canvas_changed(int64_t now_ms)
{
    if (front_pending_)
        superseded_++;
    repeated_ = front_stale_ = false;
    front_pending_ = true;
    front_ms_ = last_frame_ms_ = now_ms;
    have_ = true;
}

bool VideoLayer::due(int64_t now_ms)
{
    const bool uncover = uncovering();
    return (clock_.due(now_ms) || parked_ || uncover) && (pacer_.ready(now_ms) || (force_ && uncover));
}

void VideoLayer::hide(const uint8_t *tiles)
{
    hiding_ = false;
    shown_drawn_ = g_ && drawn_;
    if (!tiles || no_hide_)
        return;
    hidden_.assign(tiles, tiles + planner_.tiles());
    hiding_ = std::find(hidden_.begin(), hidden_.end(), 1) != hidden_.end();
}

void VideoLayer::occluded(const Coverage &cov, const Shown &s)
{
    shown_drawn_ = g_ && s.drawn;
    if (!shown_drawn_ || cov.empty() || s.turned || no_hide_) {
        hiding_ = false;
        return;
    }
    hiding_ = hidden_tiles(cov, p_.w, p_.h, s.rect, &hidden_);
}

void VideoLayer::occlude(Coverage *cov, const Shown &s)
{
    /* Drawn at u_opacity, 0 until its first frame is written. */
    if (g_ && s.drawn && opaque_ && s.opacity * s.fade >= 1.0f && !s.turned)
        cov->add(box_of(s.rect));
}

bool VideoLayer::uncovering() const
{
    if (!held_n_ || !g_ || !shown_drawn_ || blocks_shown_)
        return false;
    if (!hiding_)
        return true;
    for (size_t i = 0; i < held_.size(); i++)
        if (held_[i] && !hidden_[i])
            return true;
    return false;
}

bool VideoLayer::held_differ()
{
    const uint32_t T = crk::kTileSize, cols = (p_.w + T - 1) / T;
    bool any = false;
    for (size_t i = 0; i < held_.size(); i++) {
        if (!held_[i] || (hiding_ && hidden_[i]))
            continue;
        const uint32_t x0 = (uint32_t)(i % cols) * T, y0 = (uint32_t)(i / cols) * T;
        const uint32_t w = std::min(T, p_.w - x0), h = std::min(T, atlas_h_ - y0);
        bool differs = false;
        for (uint32_t y = y0; y < y0 + h && !differs; y++) {
            const size_t off = (size_t)y * p_.w + x0;
            differs = crk::max_abs_diff(&front_[off], &shown_[off], w) > p_.dirty_threshold;
        }
        if (differs) {
            any = true;
        } else {
            held_[i] = 0;
            held_n_--;
        }
    }
    return any;
}

void VideoLayer::planned_hidden(bool whole)
{
    if (!held_n_ && !hiding_)
        return;
    held_n_ = 0;
    if (whole || !hiding_) {
        std::fill(held_.begin(), held_.end(), 0);
        return;
    }
    for (size_t i = 0; i < held_.size(); i++) {
        held_[i] = hidden_[i] ? held_plan_[i] : 0;
        held_n_ += held_[i];
    }
}

bool VideoLayer::ready(int64_t now_ms) const
{
    return have_ && now_ms - last_frame_ms_ <= p_.stall_ms;
}

bool VideoLayer::attach(Graph *g, int64_t now_ms, std::string *err)
{
    g_ = g;
    /* The scene starts from the newest frame: one not yet written, the
     * newest partly written (quiet or held tiles), or what the last scene's
     * buffer held. The first stage() writes it whole, checked like any other
     * write; a layer whose frames have stopped is not ready, so not drawn. */
    if (!front_pending_ && !planner_.quiet() && !held_n_ && shown_.size() == atlas_bytes_) {
        front_.swap(shown_);
        front_ms_ = shown_ms_;
    }
    repeated_ = front_stale_ = false;
    front_pending_ = front_.size() == atlas_bytes_ && front_ms_ >= 0;
    shown_.assign(atlas_bytes_, 0);
    shown_ms_ = -1;
    planner_.reset();
    held_.assign(planner_.tiles(), 0);
    held_n_ = 0;
    hiding_ = shown_drawn_ = false;
    buffer_stale_ = true;
    opaque_ = false;
    blocks_shown_ = parked_ = false;
    first_ = true;
    refine_due_ms_ = -1;
    drawn_ = false;

    atlas_buf_ = g->buffer(TexFormat::R8, p_.w, atlas_h_, Sampling::Bilinear);
    bool good = atlas_buf_ && make_atlas_quads(g);
    if (good && p_.pan_blocks) {
        const uint32_t bw = crk::block_texels_w(p_.w), bh = crk::block_texels_h(p_.h);
        for (int k = 0; k < 2 && good; k++)
            good = (block_buf_[k] = g->buffer(TexFormat::RGBA8, bw, bh, Sampling::Nearest)) != 0;
        good = good && make_block_quads(g);
        block_bank_ = 0;
    }
    if (!good) {
        *err = p_.pan_blocks ? "video buffers, samplers or quads refused" : "video buffer, sampler or quad refused";
        detach(true);
        return false;
    }
    clock_.reset(now_ms);
    return true;
}

bool VideoLayer::make_atlas_quads(Graph *g)
{
    const float size[2] = {(float)p_.w, (float)p_.h}, zero = 0.0f;
    atlas_quad_ = g->quad(atlas_effect_, drawn_params().rect);
    return atlas_quad_ && g->turn(atlas_quad_, drawn_params().turn()) && g->texture(atlas_quad_, "u_image", atlas_buf_) &&
           g->uniform(atlas_quad_, "u_size", size, 2) && g->uniform(atlas_quad_, "u_opacity", &zero, 1) &&
           g->show(atlas_quad_, false) && g->order(atlas_quad_, order_);
}

bool VideoLayer::make_block_quads(Graph *g)
{
    const float size[2] = {(float)p_.w, (float)p_.h}, zero = 0.0f;
    block_quad_ = g->quad(block_effect_, drawn_params().rect);
    return block_quad_ && g->turn(block_quad_, drawn_params().turn()) && g->texture(block_quad_, "u_image", block_buf_[0]) &&
           g->uniform(block_quad_, "u_size", size, 2) && g->uniform(block_quad_, "u_opacity", &zero, 1) &&
           g->show(block_quad_, false) && g->order(block_quad_, order_ + 1);
}

void VideoLayer::drop_quads()
{
    if (atlas_quad_) g_->drop_quad(atlas_quad_);
    if (block_quad_) g_->drop_quad(block_quad_);
}

void VideoLayer::drop_objects()
{
    if (!g_)
        return;
    drop_quads();
    if (atlas_buf_) g_->drop_buffer(atlas_buf_);
    for (int k = 0; k < 2; k++)
        if (block_buf_[k]) g_->drop_buffer(block_buf_[k]);
}

void VideoLayer::detach(bool drop)
{
    if (drop)
        drop_objects();
    /* A frame the packed quad showed never reached the atlas: the next scene
     * starts from it. */
    if (blocks_shown_)
        front_pending_ = true;
    g_ = nullptr;
    hiding_ = shown_drawn_ = forced_write_ = false;
    atlas_buf_ = atlas_quad_ = block_quad_ = 0;
    block_buf_[0] = block_buf_[1] = 0;
    blocks_shown_ = parked_ = false;
    drawn_ = false;
    refine_due_ms_ = -1;
    staged_ = Staged();
}

void VideoLayer::show_blocks(bool on)
{
    blocks_shown_ = on;
    if (!g_)
        return;
    show_quads(on);
}

void VideoLayer::show_quads(bool blocks)
{
    g_->show(atlas_quad_, drawn_ && !blocks);
    if (block_quad_)
        g_->show(block_quad_, drawn_ && blocks);
}

bool VideoLayer::bank_quads(int buf)
{
    return g_->texture(block_quad_, "u_image", buf);
}

void VideoLayer::set_opacity()
{
    const float a = opaque_ ? drawn_params().opacity * fade_ : 0.0f;
    g_->uniform(atlas_quad_, "u_opacity", &a, 1);
    if (block_quad_)
        g_->uniform(block_quad_, "u_opacity", &a, 1);
}

void VideoLayer::arrange(bool drawn, int32_t order)
{
    order_ = order;
    if (!g_)
        return;
    drawn_ = drawn;
    show_blocks(blocks_shown_);
    g_->order(atlas_quad_, order);
    if (block_quad_)
        g_->order(block_quad_, order + 1);
}

void VideoLayer::place()
{
    if (!g_)
        return;
    g_->place(atlas_quad_, drawn_params().rect);
    g_->turn(atlas_quad_, drawn_params().turn());
    if (block_quad_) {
        g_->place(block_quad_, drawn_params().rect);
        g_->turn(block_quad_, drawn_params().turn());
    }
    set_opacity();
}

Stage VideoLayer::stage(int64_t now_ms)
{
    staged_ = Staged();
    if (!g_ || !drawn_)
        return Stage::Idle;
    /* Quiet tiles due to settle are written from front_, still the newest
     * frame, when no newer one waits; the packed quad gives way to the exact
     * atlas when no newer frame needed it for kRefineMs. */
    const int64_t settle_ms = planner_.settle_due_ms();
    const bool settle = !front_pending_ && settle_ms >= 0 && now_ms >= settle_ms;
    const bool refine = blocks_shown_ && !front_pending_ && now_ms >= refine_due_ms_;
    /* Held tiles no longer hidden go now, whatever the frame clock. */
    const bool uncover = uncovering();
    if (!(front_pending_ || settle || refine || uncover) || (!clock_.due(now_ms) && !parked_ && !uncover))
        return Stage::Idle;
    const bool paced = pacer_.ready(now_ms);
    if (!paced && !(force_ && uncover)) {
        held_budget_++;
        return Stage::Idle;
    }
    /* A forced write (compositor.h) hides nothing. With packed blocks it is
     * the newest frame packed, the atlas hidden until a write the budget
     * allows brings it back exact; without, every changed tile, the held
     * ones with it. */
    forced_write_ = !paced;
    if (forced_write_) {
        /* Held tiles the newest frame no longer differs from past the
         * threshold are as the planner leaves quiet ones: no write. */
        if (!held_differ()) {
            forced_write_ = false;
            return Stage::Idle;
        }
        hiding_ = false;
        if (p_.pan_blocks && !first_)
            return stage_blocks(now_ms, front_pending_);
    }
    return stage_exact(now_ms, front_pending_);
}

/* The plan's rectangles go into the one buffer in place; the compositor's
 * one flush then shows them, so the cluster never shows a frame half
 * written. setData's bytes travel raw in the flush: what the budget
 * charges. A new frame whose exact write costs more than the packed one and
 * more than the budget allows at that moment goes packed instead, except
 * the layer's first frame in a scene: that goes whole and exact, its bytes
 * charged as debt. */
Stage VideoLayer::stage_exact(int64_t now_ms, bool fresh)
{
    /* A repeat of the picture last staged: its plan would find what that
     * picture's found, nothing to write, unless tiles are due to settle. */
    const int64_t settle_ms = planner_.settle_due_ms();
    const bool uncover = uncovering();
    if (fresh && repeated_ && !uncover &&
        (blocks_shown_ || (opaque_ && !buffer_stale_ && (settle_ms < 0 || now_ms < settle_ms)))) {
        repeated_ = false;
        ((blocks_shown_ ? motion_.quiet() : planner_.quiet()) ? quiet_ : same_)++;
        if (!blocks_shown_)
            shown_ms_ = front_ms_;
        front_pending_ = false;
        return Stage::Idle;
    }
    repeated_ = false;
    if (blocks_shown_ && fresh) {
        const UploadPlan m = motion_.plan(block_src_.data(), front_.data(), now_ms);
        if (m.rects.empty()) {
            (m.quiet_tiles ? quiet_ : same_)++;
            front_pending_ = false;
            return Stage::Idle;
        }
    }
    const bool refine = blocks_shown_;
    const uint8_t *hidden = hiding_ ? hidden_.data() : nullptr;
    if (hidden)
        held_plan_.assign(planner_.tiles(), 0);
    const UploadPlan p = planner_.plan(shown_.data(), front_.data(), now_ms, hidden, hidden ? held_plan_.data() : nullptr);
    hidden_tiles_ += p.hidden_tiles;
    if (p.rects.empty() && opaque_ && !refine && !buffer_stale_) {
        planned_hidden(false);
        if (uncover)
            uncovers_++;
        if (fresh)
            (p.quiet_tiles ? quiet_ : same_)++;
        shown_ms_ = front_ms_;   /* the buffer is front_ to within the threshold */
        front_pending_ = false;
        return Stage::Idle;
    }
    /* Whole: a plan of most tiles, or a buffer a failed write (or a new
     * scene) left unlike shown_. */
    const bool whole = p.whole || buffer_stale_;
    const size_t raw = whole ? front_.size() : p.bytes;
    if (fresh && !first_ && p_.pan_blocks && raw > packed_.size() && pacer_.allowance() < (int64_t)raw) {
        planner_.reset();   /* the atlas is not written: no tile of it is left quiet */
        /* Nor are the held ones written: they stay held, with the hidden
         * tiles this plan found changed. */
        for (size_t i = 0; hidden && i < held_.size(); i++)
            if (hidden_[i] && held_plan_[i] && !held_[i]) {
                held_[i] = 1;
                held_n_++;
            }
        return stage_blocks(now_ms, fresh);
    }
    settled_ += p.settled_tiles;
    const std::vector<PixelRect> all{PixelRect{0, 0, p_.w, atlas_h_}};
    staged_.kind = Kind::Exact;
    staged_.fresh = fresh;
    staged_.offered_ms = front_ms_;
    staged_.whole = whole;
    staged_.refine = refine;
    staged_.raw = raw;
    staged_.rects = whole ? all : p.rects;
    bool written = true;
    staged_.accepted = crk::write_rects(
        front_.data(), p_.w, 1, staged_.rects, &rect_scratch_,
        [this](const uint8_t *px, const PixelRect &r) { return g_->write(atlas_buf_, r.x, r.y, r.w, r.h, px); },
        &written);
    if (!written) {
        refused_++;
        return Stage::Failed;
    }
    /* A failed flush makes the next write whole, and until it goes the
     * target holds or the packed quad shows: the held tiles can be taken as
     * written now. */
    planned_hidden(whole);
    if (uncover)
        uncovers_++;
    if (!opaque_) {
        opaque_ = true;   /* undone in committed() if the flush fails */
        set_opacity();
    }
    parked_ = false;
    show_blocks(refine ? false : blocks_shown_);
    return Stage::Wrote;
}

/* The whole of front_ packed into the bank not shown, which then shows: a
 * failed write or flush leaves the shown bank and quad as they were. */
Stage VideoLayer::stage_blocks(int64_t, bool fresh)
{
    crk::pack_blocks(front_.data(), p_.w, p_.h, packed_.data(), kPackThreads);
    staged_.kind = Kind::Blocks;
    staged_.fresh = fresh;
    staged_.offered_ms = front_ms_;
    staged_.next_bank = 1 - block_bank_;
    staged_.was_shown = blocks_shown_;
    staged_.was_parked = parked_;
    staged_.raw = packed_.size();
    bool written = g_->write(block_buf_[staged_.next_bank], 0, 0, crk::block_texels_w(p_.w),
                             crk::block_texels_h(p_.h), packed_.data());
    staged_.accepted = written ? packed_.size() : 0;
    if (!written)
        refused_++;
    if (written)
        written = bank_quads(block_buf_[staged_.next_bank]);
    if (!written)
        return Stage::Failed;
    if (!opaque_) {
        opaque_ = true;
        set_opacity();
    }
    parked_ = false;   /* the stale atlas is hidden in this flush */
    show_blocks(true);
    return Stage::Wrote;
}

void VideoLayer::committed(bool ok, int64_t now_ms)
{
    const Staged s = staged_;
    staged_ = Staged();
    const bool forced = forced_write_;
    forced_write_ = false;
    if (s.kind != Kind::None && ok) {
        /* After a forced write nothing is hidden until a write the budget
         * allowed has gone: it started from a positive allowance, so the
         * forced write's debt was repaid. */
        if (forced) {
            no_hide_ = true;
            forced_++;
        } else {
            no_hide_ = false;
        }
    }
    if (s.kind == Kind::None)
        return;
    /* A failed update is retried with the newest frame: packed blocks at the
     * next deadline, an exact write as soon as the budget allows. */
    clock_.sent(now_ms);
    if (!ok) {
        failed_++;
        pacer_.spend(s.accepted);
        if (s.kind == Kind::Blocks) {
            bank_quads(block_buf_[block_bank_]);
            parked_ = s.was_parked;
            show_blocks(s.was_shown);
        } else {
            /* Part of front_ may be in the buffer, unflushed: the next write
             * is whole, and until it goes the target holds its flushes
             * (holds_flush), so the cluster keeps showing the last frame
             * and never a half-written one. The packed quad keeps showing
             * if it was. */
            buffer_stale_ = true;
            parked_ = !s.refine;
            show_blocks(s.refine ? true : blocks_shown_);
            /* A settle write that failed still has a frame to show: the next
             * deadline writes it whole. */
            if (!s.refine)
                front_pending_ = true;
        }
        if (shown_ms_ < 0 && !blocks_shown_) {
            opaque_ = false;
            set_opacity();
        }
        return;
    }
    if (s.fresh) {
        sent_++;
        age_ms_ += (uint64_t)std::max<int64_t>(0, now_ms - s.offered_ms);
    }
    bytes_ += s.raw;
    pacer_.spend(s.raw);
    if (s.kind == Kind::Blocks) {
        block_bank_ = s.next_bank;
        block_src_.assign(front_.begin(), front_.end());
        motion_.reset();   /* the packed frame is front_: nothing of it is left quiet */
        refine_due_ms_ = now_ms + kRefineMs;
        packed_writes_++;
        front_pending_ = false;   /* buffer_stale_ stays: the atlas still needs a whole write */
        return;
    }
    buffer_stale_ = false;
    first_ = false;
    /* shown_ is what the buffer holds: all of front_, or front_ in the
     * written rectangles; front_ stays the newest frame while quiet tiles
     * are left (planner_.quiet()). */
    if (s.whole) {
        shown_.swap(front_);
        front_stale_ = true;
        planner_.reset();
        whole_writes_++;
    } else {
        crk::copy_rects(shown_.data(), front_.data(), p_.w, 1, s.rects);
        partial_writes_++;
    }
    if (s.refine) {
        refine_due_ms_ = -1;
        refines_++;
    }
    rects_ += s.rects.size();
    shown_ms_ = front_ms_;
    front_pending_ = false;
}

int64_t VideoLayer::wake_in_ms(int64_t now_ms) const
{
    if (!g_ || !drawn_ || !front_pending_)
        return -1;
    const int64_t w = clock_.ms_until_due(now_ms);
    return w > 0 ? w : -1;
}

WriteStats VideoLayer::write_stats() const
{
    WriteStats w;
    w.frames = sent_;
    w.age_ms = age_ms_;
    w.bytes = bytes_;
    w.failed = failed_;
    w.holds = held_budget_;
    return w;
}

std::string VideoLayer::stats() const
{
    char b[400];
    std::snprintf(b, sizeof b,
                  "%s%llu sent, %llu unchanged, %llu within threshold, %llu superseded, %llu settled, %llu failed "
                  "(%llu refused), %llu KB, %llu whole, %llu partial, %llu rects, %llu packed, %llu back to exact, "
                  "%llu budget holds",
                  source_ ? "drawn by the target's composite; " : "", (unsigned long long)sent_,
                  (unsigned long long)same_, (unsigned long long)quiet_,
                  (unsigned long long)superseded_, (unsigned long long)settled_, (unsigned long long)failed_,
                  (unsigned long long)refused_, (unsigned long long)(bytes_ / 1024),
                  (unsigned long long)whole_writes_, (unsigned long long)partial_writes_, (unsigned long long)rects_,
                  (unsigned long long)packed_writes_, (unsigned long long)refines_,
                  (unsigned long long)held_budget_);
    if (!hidden_tiles_)
        return b;
    char h[96];
    std::snprintf(h, sizeof h, ", %llu tiles left hidden, %llu uncovered (%llu forced)",
                  (unsigned long long)hidden_tiles_, (unsigned long long)uncovers_, (unsigned long long)forced_);
    return std::string(b) + h;
}

}  // namespace crs
