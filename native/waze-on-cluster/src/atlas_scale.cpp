// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "atlas_scale.h"

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "yuv_atlas.h"

namespace woc {

namespace {
const uint32_t kOne = 1u << 12;   /* the weights of one output pixel sum to this */
}

/* Output pixel i covers source [i*src/dst, (i+1)*src/dst); each source pixel
 * it touches weighs the length of the overlap, in units of 1/dst of a pixel,
 * normalised so the weights sum to exactly kOne. */
void AtlasScaler::Axis::build(uint32_t src, uint32_t dst)
{
    first.assign(dst, 0);
    count.assign(dst, 0);
    offset.assign(dst, 0);
    weight.clear();
    for (uint32_t i = 0; i < dst; i++) {
        const uint64_t a = (uint64_t)i * src, b = (uint64_t)(i + 1) * src;   /* in 1/dst units */
        const uint32_t s0 = (uint32_t)(a / dst), s1 = (uint32_t)((b + dst - 1) / dst);
        first[i] = s0;
        count[i] = s1 - s0;
        offset[i] = (uint32_t)weight.size();
        uint32_t sum = 0;
        for (uint32_t s = s0; s < s1; s++) {
            const uint64_t lo = (uint64_t)s * dst > a ? (uint64_t)s * dst : a;
            const uint64_t hi = (uint64_t)(s + 1) * dst < b ? (uint64_t)(s + 1) * dst : b;
            const uint32_t wgt = (uint32_t)(((hi - lo) * kOne + src / 2) / src);
            weight.push_back((uint16_t)wgt);
            sum += wgt;
        }
        /* the rounding error goes to the largest weight, so the sum is exact */
        uint32_t big = offset[i];
        for (uint32_t k = offset[i]; k < offset[i] + count[i]; k++)
            if (weight[k] > weight[big])
                big = k;
        weight[big] = (uint16_t)(weight[big] + (int32_t)kOne - (int32_t)sum);
    }
}

bool AtlasScaler::configure(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh, unsigned level_bits, uint32_t crop_x,
                            uint32_t crop_y, uint32_t crop_w, uint32_t crop_h)
{
    if (!crop_w) {
        crop_x = crop_y = 0;
        crop_w = w;
        crop_h = h;
    }
    if (!w || !h || !dw || !dh || w % 2 || h % 2 || dw % 2 || dh % 2 || !crop_h || (crop_x | crop_y | crop_w | crop_h) % 2 ||
        (uint64_t)crop_x + crop_w > w || (uint64_t)crop_y + crop_h > h || dw > crop_w || dh > crop_h || level_bits > 3) {
        dw_ = dh_ = 0;
        return false;
    }
    if (w == w_ && h == h_ && dw == dw_ && dh == dh_ && level_bits == bits_ && crop_x == crop_x_ && crop_y == crop_y_ &&
        crop_w == crop_w_ && crop_h == crop_h_)
        return true;
    w_ = w;
    h_ = h;
    dw_ = dw;
    dh_ = dh;
    crop_x_ = crop_x;
    crop_y_ = crop_y;
    crop_w_ = crop_w;
    crop_h_ = crop_h;
    bits_ = level_bits;
    yx_.build(crop_w, dw);
    yy_.build(crop_h, dh);
    cx_.build(crop_w / 2, dw / 2);
    cy_.build(crop_h / 2, dh / 2);
    return true;
}

/* Horizontal into 16-bit rows (scaled 256: the kOne-weighted sum over 16),
 * then vertical row by row into 32-bit sums (scaled 2^20), so both passes
 * run along memory and the compiler can vectorise the vertical one. */
void AtlasScaler::plane(const uint8_t *src, uint32_t sp, uint32_t sh, uint8_t *dst, uint32_t dp, const Axis &x,
                        const Axis &y)
{
    const uint32_t dw = (uint32_t)x.first.size(), dh = (uint32_t)y.first.size();
    rows_.resize((size_t)sh * dw);
    acc_.resize(dw);
    for (uint32_t r = 0; r < sh; r++) {
        const uint8_t *s = src + (size_t)r * sp;
        uint16_t *o = &rows_[(size_t)r * dw];
        for (uint32_t i = 0; i < dw; i++) {
            const uint8_t *p = s + x.first[i];
            const uint16_t *wt = &x.weight[x.offset[i]];
            uint32_t a = 0;
            for (uint32_t k = 0; k < x.count[i]; k++)
                a += (uint32_t)p[k] * wt[k];
            o[i] = (uint16_t)((a + 8) >> 4);
        }
    }
    for (uint32_t j = 0; j < dh; j++) {
        uint32_t *a = acc_.data();
        for (uint32_t i = 0; i < dw; i++)
            a[i] = 1u << 19;   /* rounds the shift below */
        const uint16_t *wt = &y.weight[y.offset[j]];
        for (uint32_t k = 0; k < y.count[j]; k++) {
            const uint16_t *row = &rows_[(size_t)(y.first[j] + k) * dw];
            const uint32_t w = wt[k];
            for (uint32_t i = 0; i < dw; i++)
                a[i] += (uint32_t)row[i] * w;
        }
        uint8_t *o = dst + (size_t)j * dp;
        for (uint32_t i = 0; i < dw; i++) {
            const uint32_t v = a[i] >> 20;
            o[i] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

void AtlasScaler::scale(const uint8_t *src, std::vector<uint8_t> *dst)
{
    dst->resize(dst_bytes());
    uint8_t *d = dst->data();
    /* Y, from the crop's top-left */
    plane(src + (size_t)crop_y_ * w_ + crop_x_, w_, crop_h_, d, dw_, yx_, yy_);
    /* U and V, side by side under Y */
    const uint8_t *su = src + (size_t)w_ * h_ + (size_t)(crop_y_ / 2) * w_ + crop_x_ / 2, *sv = su + w_ / 2;
    uint8_t *du = d + (size_t)dw_ * dh_, *dv = du + dw_ / 2;
    plane(su, w_, crop_h_ / 2, du, dw_, cx_, cy_);
    plane(sv, w_, crop_h_ / 2, dv, dw_, cx_, cy_);
    if (black_bg_)
        key_background(d);
    if (radius_) {
        const uint32_t cr = radius_ / 2;
        corners(d, dw_, dw_, dh_, radius_, ycov_, 16);
        if (cr) {
            corners(du, dw_, dw_ / 2, dh_ / 2, cr, ccov_, 128);
            corners(dv, dw_, dw_ / 2, dh_ / 2, cr, ccov_, 128);
        }
    }
    if (bits_)
        crk::round_levels(d, d, dst->size(), bits_);
}

/* The background is the most common Y (a night map's field covers most of
 * the frame), followed at a quarter of each frame's change so a frame with
 * a big label does not flicker it. Y up to kKeyLo above it goes black, the
 * next kKeyRamp fade in, the rest stays; U and V of each 2 x 2 block fade
 * by the most visible of its four Y. A background lighter than kKeyMaxBg
 * (a day map) is left alone.
 *
 * Green is never keyed: parks (Waze's and Google Maps' night greens are about
 * as dark as the grey-blue field) stay, and green pixels don't count towards
 * the background, so a park filling the frame can't become it. Green in
 * YUV: V well below neutral and U not above it - the field's blue has U
 * above. */
static inline bool is_green(int u, int v)
{
    return v - 128 <= -6 && u - 128 <= 2;
}
void AtlasScaler::key_background(uint8_t *d)
{
    static const int kKeyLo = 8, kKeyRamp = 28, kKeyMaxBg = 150;   /* Waze's night field is grey-blue, a day map's near white */
    const size_t n = (size_t)dw_ * dh_;
    const uint8_t *cu = d + n;
    uint32_t hist[256] = {0};
    for (size_t i = 0; i < n; i += 3) {
        const size_t c = (i / dw_ / 2) * dw_ + (i % dw_) / 2;
        if (!is_green(cu[c], cu[c + dw_ / 2]))
            hist[d[i]]++;
    }
    int mode = 0;
    for (int v = 1; v < 256; v++)
        if (hist[v] > hist[mode])
            mode = v;
    bg_ = bg_ < 0 ? mode * 16 : bg_ + (mode * 16 - bg_) / 4;
    const int bg = bg_ / 16;
    if (bg > kKeyMaxBg)
        return;
    uint16_t k[256];
    for (int v = 0; v < 256; v++) {
        const int over = v - bg - kKeyLo;
        k[v] = (uint16_t)(over <= 0 ? 0 : over >= kKeyRamp ? 256 : over * 256 / kKeyRamp);
    }
    uint8_t *u = d + n, *v = u + dw_ / 2;
    for (uint32_t y = 0; y < dh_; y += 2)
        for (uint32_t x = 0; x < dw_; x += 2) {
            uint8_t *p = d + (size_t)y * dw_ + x;
            const size_t c = (size_t)(y / 2) * dw_ + x / 2;
            if (is_green(u[c], v[c]))
                continue;
            const uint32_t k0 = k[p[0]], k1 = k[p[1]], k2 = k[p[dw_]], k3 = k[p[dw_ + 1]];
            p[0] = (uint8_t)(16 + ((int)p[0] - 16) * (int)k0 / 256);
            p[1] = (uint8_t)(16 + ((int)p[1] - 16) * (int)k1 / 256);
            p[dw_] = (uint8_t)(16 + ((int)p[dw_] - 16) * (int)k2 / 256);
            p[dw_ + 1] = (uint8_t)(16 + ((int)p[dw_ + 1] - 16) * (int)k3 / 256);
            uint32_t kc = k0 > k1 ? k0 : k1;
            kc = kc > k2 ? kc : k2;
            kc = kc > k3 ? kc : k3;
            if (kc >= 256)
                continue;
            u[c] = (uint8_t)(128 + ((int)u[c] - 128) * (int)kc / 256);
            v[c] = (uint8_t)(128 + ((int)v[c] - 128) * (int)kc / 256);
        }
}

/* Each pixel of the r x r top-left corner square: how much of it (0-256) lies
 * inside the circle of radius r centred at (r, r), from 4 x 4 samples. */
void AtlasScaler::coverage(uint32_t r, std::vector<uint16_t> *cov)
{
    cov->assign((size_t)r * r, 256);
    const double rr = (double)r * r;
    for (uint32_t y = 0; y < r; y++)
        for (uint32_t x = 0; x < r; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const double dx = r - (x + (sx + 0.5) / 4), dy = r - (y + (sy + 0.5) / 4);
                    in += dx * dx + dy * dy <= rr;
                }
            (*cov)[(size_t)y * r + x] = (uint16_t)(in * 16);
        }
}

void AtlasScaler::set_corner_radius(uint32_t r)
{
    if (dw_ && (r * 2 > dw_ || r * 2 > dh_))
        r = (dw_ < dh_ ? dw_ : dh_) / 2;
    if (r == radius_)
        return;
    radius_ = r;
    ycov_.clear();
    ccov_.clear();
    if (r) {
        coverage(r, &ycov_);
        if (r / 2)
            coverage(r / 2, &ccov_);
    }
}

void AtlasScaler::corners(uint8_t *p, uint32_t dp, uint32_t pw, uint32_t ph, uint32_t r, const std::vector<uint16_t> &cov,
                          uint8_t black)
{
    if (r * 2 > pw || r * 2 > ph)
        return;
    for (uint32_t y = 0; y < r; y++)
        for (uint32_t x = 0; x < r; x++) {
            const uint32_t c = cov[(size_t)y * r + x];
            if (c >= 256)
                continue;
            /* the same coverage, mirrored into each corner */
            const uint32_t xs[2] = {x, pw - 1 - x}, ys[2] = {y, ph - 1 - y};
            for (uint32_t yi = 0; yi < 2; yi++)
                for (uint32_t xi = 0; xi < 2; xi++) {
                    uint8_t &v = p[(size_t)ys[yi] * dp + xs[xi]];
                    v = (uint8_t)(black + (((int)v - black) * (int)c) / 256);
                }
        }
}

ScaleWorker::~ScaleWorker()
{
    if (!started_)
        return;
    {
        std::lock_guard<std::mutex> l(s_->mu);
        s_->stop = true;
    }
    s_->cv.notify_one();
}

bool ScaleWorker::submit(std::vector<uint8_t> *atlas, uint64_t id, const ScaleShape &shape)
{
    if (!idle() || shape.dw > (shape.crop_w ? shape.crop_w : shape.w) || shape.dh > (shape.crop_w ? shape.crop_h : shape.h) ||
        atlas->size() != (size_t)shape.w * (shape.h + shape.h / 2))
        return false;
    s_->in.swap(*atlas);
    s_->id = id;
    s_->shape = shape;
    if (!started_) {
        std::thread(run, s_).detach();
        started_ = true;
    }
    {
        std::lock_guard<std::mutex> l(s_->mu);
        s_->stage.store(kQueued, std::memory_order_release);
    }
    s_->cv.notify_one();
    return true;
}

bool ScaleWorker::take(std::vector<uint8_t> *out, uint64_t *id, const ScaleShape &shape, std::vector<uint8_t> *in)
{
    if (s_->stage.load(std::memory_order_acquire) != kDone)
        return false;
    if (s_->made == shape && !s_->out.empty()) {
        out->swap(s_->out);
        *id = s_->id;
    } else {
        out->clear();
    }
    if (in)
        in->swap(s_->in);
    s_->stage.store(kIdle, std::memory_order_release);
    return true;
}

void ScaleWorker::run(std::shared_ptr<State> s)
{
#ifdef __linux__
    /* below the map's threads: the HUD takes only spare CPU */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    for (;;) {
        {
            std::unique_lock<std::mutex> l(s->mu);
            s->cv.wait(l, [&s] { return s->stop || s->stage.load(std::memory_order_acquire) == kQueued; });
            if (s->stop)
                return;
        }
        if (s->made != s->shape || !s->scaler.configured()) {
            s->made = ScaleShape();
            if (s->scaler.configure(s->shape.w, s->shape.h, s->shape.dw, s->shape.dh, s->shape.level_bits,
                                    s->shape.crop_x, s->shape.crop_y, s->shape.crop_w, s->shape.crop_h)) {
                s->scaler.set_corner_radius(s->shape.radius);
                s->scaler.set_black_background(s->shape.black);
                s->made = s->shape;
            }
        }
        if (s->made == s->shape)
            s->scaler.scale(s->in.data(), &s->out);
        else
            s->out.clear();
        s->frames++;
        s->stage.store(kDone, std::memory_order_release);
    }
}

}  // namespace woc
