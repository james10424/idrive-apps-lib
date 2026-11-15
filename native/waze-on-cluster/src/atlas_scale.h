// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* AtlasScaler: a decoded 4:2:0 atlas (crk/yuv_atlas.h: Y w x h, then U and V
 * side by side at w/2 x h/2) made smaller, for a target that shows the map
 * smaller than the decoded frame (the HUD: 453x200 of a 1088x480 frame).
 * Sending the frame at the size it is shown cuts the upload to the KOMBI by
 * the ratio of the areas (5.7x at the HUD's standard size), so frames go
 * exact instead of packed and the shared budget is free for the rest.
 *
 * Area averaging (a box filter over the exact source span of each output
 * pixel), separable, in integer arithmetic: each plane is scaled on its
 * own, so U, V and Y never mix. Weights are computed once per size. Values
 * are rounded to the given level bits afterwards (yuv_atlas.h
 * round_levels), as the decoder thread rounds the full frame. Only
 * downscaling: an output larger than the source in either direction is
 * refused. */
#ifndef WOC_ATLAS_SCALE_H
#define WOC_ATLAS_SCALE_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace woc {

class AtlasScaler {
public:
    /* Source w x h and destination dw x dh frames (all even, dw <= w,
     * dh <= h); false if not. With crop_w: only the crop_w x crop_h part at
     * crop_x, crop_y of the source (all even, inside it) is scaled, dw <=
     * crop_w and dh <= crop_h (a part shown elsewhere: MapFeed's host). */
    bool configure(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh, unsigned level_bits, uint32_t crop_x = 0,
                   uint32_t crop_y = 0, uint32_t crop_w = 0, uint32_t crop_h = 0);
    bool configured() const { return dw_ != 0; }
    size_t dst_bytes() const { return (size_t)dw_ * (dh_ + dh_ / 2); }

    /* src: a w x atlas_height(h) atlas; *dst is resized to dst_bytes(). */
    void scale(const uint8_t *src, std::vector<uint8_t> *dst);

    /* Rounded corners of radius r output pixels (0: square), anti-aliased:
     * the corners fade to black (Y 16, U and V 128), which a HUD shows as
     * nothing, so the picture's corners look round. Applied by scale(). */
    void set_corner_radius(uint32_t r);

    /* Black background: the map's background (its most common Y, followed
     * smoothly frame to frame) and the shades just above it fade to black,
     * so a HUD, which shows black as nothing, shows the roads, route and
     * labels without the map's grey-blue field (Waze's night map). Only while the background is
     * dark (a night map): a light one is left as it is. Applied by scale(). */
    void set_black_background(bool on) { black_bg_ = on; bg_ = -1; }
    /* the background Y followed, -1 before the first frame */
    int background() const { return bg_; }

private:
    /* One axis: for each output index, its first source index and weights
     * (sum kOne) over the source pixels its span covers. */
    struct Axis {
        std::vector<uint32_t> first;
        std::vector<uint32_t> count;
        std::vector<uint16_t> weight;   /* concatenated, count[i] per output */
        std::vector<uint32_t> offset;   /* into weight */
        void build(uint32_t src, uint32_t dst);
    };
    void plane(const uint8_t *src, uint32_t sp, uint32_t sh, uint8_t *dst, uint32_t dp, const Axis &x, const Axis &y);
    /* The corner mask on one plane (pw x ph, pitch dp) of radius r with
     * coverage table cov (r x r, 0-256, the top-left corner). */
    static void corners(uint8_t *p, uint32_t dp, uint32_t pw, uint32_t ph, uint32_t r, const std::vector<uint16_t> &cov,
                        uint8_t black);
    static void coverage(uint32_t r, std::vector<uint16_t> *cov);

    uint32_t w_ = 0, h_ = 0, dw_ = 0, dh_ = 0;
    uint32_t crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;   /* the part scaled; all of it without a crop */
    unsigned bits_ = 0;
    Axis yx_, yy_, cx_, cy_;
    std::vector<uint16_t> rows_;   /* the horizontal pass, scaled 256 */
    std::vector<uint32_t> acc_;    /* one output row's vertical sums */
    uint32_t radius_ = 0;
    bool black_bg_ = false;
    int bg_ = -1;   /* the background's Y, x 16 (smoothed) */
    void key_background(uint8_t *d);
    std::vector<uint16_t> ycov_, ccov_;   /* corner coverage: Y at radius_, U and V at radius_ / 2 */
};

/* What a ScaleWorker makes of a frame: a w x h atlas (its crop_w x crop_h
 * part at crop_x, crop_y with crop_w) scaled to dw x dh, rounded to
 * level_bits, with AtlasScaler's corners and background. */
struct ScaleShape {
    uint32_t w = 0, h = 0, dw = 0, dh = 0;
    uint32_t crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    unsigned level_bits = 0;
    uint32_t radius = 0;
    bool black = false;
    bool operator==(const ScaleShape &o) const
    {
        return w == o.w && h == o.h && dw == o.dw && dh == o.dh && crop_x == o.crop_x && crop_y == o.crop_y &&
               crop_w == o.crop_w && crop_h == o.crop_h && level_bits == o.level_bits && radius == o.radius &&
               black == o.black;
    }
    bool operator!=(const ScaleShape &o) const { return !(*this == o); }
};

/* Scales one frame at a time on a thread of its own, at a low priority, so
 * the thread offering frames does a swap and nothing more: the cluster's
 * map, offered on the same thread, never waits for the HUD's. The thread
 * starts with the first frame and is left to finish its frame when the
 * worker goes. The lock is held only to hand a frame over, never while
 * scaling, and the offering thread never waits for the worker. */
class ScaleWorker {
public:
    ScaleWorker() : s_(std::make_shared<State>()) {}
    ~ScaleWorker();
    ScaleWorker(const ScaleWorker &) = delete;
    ScaleWorker &operator=(const ScaleWorker &) = delete;

    /* No frame is being scaled or waits to be taken. */
    bool idle() const { return s_->stage.load(std::memory_order_acquire) == kIdle; }
    /* While idle(): the atlas, taken by a swap (*atlas comes back with the
     * worker's last input), to be scaled to shape; false, *atlas untouched,
     * if not idle or the atlas is not shape's source size. */
    bool submit(std::vector<uint8_t> *atlas, uint64_t id, const ScaleShape &shape);
    /* A frame done: its scaled frame swapped into *out (its frame id in *id),
     * or *out emptied when it was made for another shape than the one
     * asked for now; with in, the frame it scaled swapped into *in. The
     * worker is idle again. False while none is done. */
    bool take(std::vector<uint8_t> *out, uint64_t *id, const ScaleShape &shape, std::vector<uint8_t> *in = nullptr);
    /* Frames scaled since start. */
    uint64_t frames() const { return s_->frames.load(); }

private:
    enum Stage { kIdle, kQueued, kDone };
    /* Shared with the thread, which may outlive the worker by one frame. */
    struct State {
        std::atomic<int> stage{kIdle};
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> frames{0};
        std::mutex mu;
        std::condition_variable cv;
        /* The offering thread's while idle; the worker's while queued. */
        std::vector<uint8_t> in, out;
        uint64_t id = 0;
        ScaleShape shape, made;
        AtlasScaler scaler;   /* the worker's */
    };
    static void run(std::shared_ptr<State> s);

    std::shared_ptr<State> s_;
    bool started_ = false;
};

}  // namespace woc

#endif
