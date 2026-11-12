/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Robert Nisipeanu */
#include "dirty_tiles.h"

#include <algorithm>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace crk {

namespace {

/* The tile's largest channel difference. Rows that are equal cost a memcmp;
 * only differing rows are scanned for the size of the difference. */
uint8_t tile_delta(const uint8_t *a, const uint8_t *b, size_t stride, size_t x_bytes, uint32_t y0,
                   size_t w_bytes, uint32_t rows)
{
    uint8_t m = 0;
    for (uint32_t y = y0; y < y0 + rows && m < 255; y++) {
        const size_t off = (size_t)y * stride + x_bytes;
        if (std::memcmp(a + off, b + off, w_bytes) != 0)
            m = std::max(m, max_abs_diff(a + off, b + off, w_bytes));
    }
    return m;
}

int delta_bin(uint8_t d)
{
    return 31 - __builtin_clz((unsigned)d);
}

void add_run(UploadPlan *p, const PixelRect &r)
{
    for (PixelRect &q : p->rects)
        if (q.x == r.x && q.w == r.w && q.y + q.h == r.y) {
            q.h += r.h;
            return;
        }
    p->rects.push_back(r);
}

}  // namespace

uint8_t max_abs_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i = 0;
    uint8_t m = 0;
#if defined(__SSE2__)
    __m128i vm = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a + i));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + i));
        vm = _mm_max_epu8(vm, _mm_or_si128(_mm_subs_epu8(x, y), _mm_subs_epu8(y, x)));
    }
    vm = _mm_max_epu8(vm, _mm_srli_si128(vm, 8));
    vm = _mm_max_epu8(vm, _mm_srli_si128(vm, 4));
    vm = _mm_max_epu8(vm, _mm_srli_si128(vm, 2));
    vm = _mm_max_epu8(vm, _mm_srli_si128(vm, 1));
    m = (uint8_t)_mm_cvtsi128_si32(vm);
#endif
    for (; i < n; i++) {
        const uint8_t d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        m = d > m ? d : m;
    }
    return m;
}

UploadPlanner::UploadPlanner(uint32_t w, uint32_t h, uint32_t bpp, uint32_t tile_w, uint32_t tile_h,
                             uint32_t threshold, int64_t settle_ms)
    : w_(w), h_(h), bpp_(bpp), tile_w_(tile_w), tile_h_(tile_h), cols_((w + tile_w - 1) / tile_w),
      rows_((h + tile_h - 1) / tile_h), threshold_(threshold), settle_ms_(settle_ms),
      quiet_since_((size_t)cols_ * rows_, -1)
{
}

void UploadPlanner::reset()
{
    std::fill(quiet_since_.begin(), quiet_since_.end(), -1);
    quiet_ = 0;
    settle_due_ms_ = -1;
}

UploadPlan UploadPlanner::plan(const uint8_t *shown, const uint8_t *cur, int64_t now_ms, const uint8_t *hidden,
                               uint8_t *held)
{
    if (!shown)
        hidden = nullptr;
    UploadPlan p;
    p.tiles = cols_ * rows_;
    const size_t stride = (size_t)w_ * bpp_;
    for (uint32_t ty = 0; ty < rows_; ty++) {
        const uint32_t y0 = ty * tile_h_, th = std::min(tile_h_, h_ - y0);
        bool in_run = false;
        uint32_t run_x = 0;
        for (uint32_t tx = 0; tx <= cols_; tx++) {
            const uint32_t x0 = std::min(tx * tile_w_, w_);
            bool write = false;
            if (tx < cols_ && hidden && hidden[(size_t)ty * cols_ + tx]) {
                /* Not seen: kept as the buffer holds it. A difference within
                 * the threshold leaves it quiet, but not settling. */
                const size_t i = (size_t)ty * cols_ + tx;
                const uint32_t tw = std::min(tile_w_, w_ - x0);
                const uint8_t d = tile_delta(shown, cur, stride, (size_t)x0 * bpp_, y0, (size_t)tw * bpp_, th);
                if (held)
                    held[i] = d > threshold_;
                if (d > 0 && d <= threshold_)
                    p.quiet_tiles++;
                quiet_since_[i] = -1;
                p.hidden_tiles++;
            } else if (tx < cols_) {
                int64_t &since = quiet_since_[(size_t)ty * cols_ + tx];
                const uint32_t tw = std::min(tile_w_, w_ - x0);
                const uint8_t d = shown ? tile_delta(shown, cur, stride, (size_t)x0 * bpp_, y0, (size_t)tw * bpp_, th) : 0;
                if (d > 0)
                    p.delta_tiles[delta_bin(d)]++;
                if (!shown || d > threshold_) {
                    write = true;
                } else if (d > 0) {
                    if (since < 0)
                        since = now_ms;
                    if (settle_ms_ > 0 && now_ms - since >= settle_ms_) {
                        write = true;
                        p.settled_tiles++;
                    } else {
                        p.quiet_tiles++;
                    }
                } else {
                    since = -1;
                }
                if (write) {
                    since = -1;
                    p.dirty_tiles++;
                }
            }
            if (write && !in_run) {
                run_x = x0;
                in_run = true;
            } else if (!write && in_run) {
                add_run(&p, PixelRect{run_x, y0, x0 - run_x, th});
                in_run = false;
            }
        }
    }
    if (p.dirty_tiles > 0 && !p.hidden_tiles && (uint64_t)p.dirty_tiles * 100 >= (uint64_t)p.tiles * kWholePercent) {
        p.rects.assign(1, PixelRect{0, 0, w_, h_});
        p.whole = true;
        p.quiet_tiles = 0;
        reset();
    }
    for (const PixelRect &r : p.rects)
        p.bytes += (size_t)r.w * r.h * bpp_;
    quiet_ = p.quiet_tiles;
    settle_due_ms_ = -1;
    if (settle_ms_ > 0)
        for (int64_t since : quiet_since_)
            if (since >= 0 && (settle_due_ms_ < 0 || since + settle_ms_ < settle_due_ms_))
                settle_due_ms_ = since + settle_ms_;
    return p;
}

UploadPlan plan_upload(const uint8_t *prev, const uint8_t *cur, uint32_t w, uint32_t h, uint32_t bpp,
                       uint32_t tile_w, uint32_t tile_h)
{
    return UploadPlanner(w, h, bpp, tile_w, tile_h).plan(prev, cur, 0);
}

const uint8_t *pack_rect(const uint8_t *frame, uint32_t w, uint32_t bpp, const PixelRect &r,
                         std::vector<uint8_t> *scratch)
{
    const size_t stride = (size_t)w * bpp;
    if (r.x == 0 && r.w == w)
        return frame + (size_t)r.y * stride;
    const size_t row = (size_t)r.w * bpp;
    scratch->resize(row * r.h);
    for (uint32_t y = 0; y < r.h; y++)
        std::memcpy(scratch->data() + (size_t)y * row, frame + (size_t)(r.y + y) * stride + (size_t)r.x * bpp, row);
    return scratch->data();
}

void copy_rects(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t bpp, const std::vector<PixelRect> &rects)
{
    const size_t stride = (size_t)w * bpp;
    for (const PixelRect &r : rects) {
        const size_t off = (size_t)r.y * stride + (size_t)r.x * bpp, row = (size_t)r.w * bpp;
        if (r.x == 0 && r.w == w) {
            std::memcpy(dst + off, src + off, row * r.h);
            continue;
        }
        for (uint32_t y = 0; y < r.h; y++)
            std::memcpy(dst + off + (size_t)y * stride, src + off + (size_t)y * stride, row);
    }
}

}  // namespace crk
