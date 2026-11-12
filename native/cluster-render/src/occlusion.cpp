/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "occlusion.h"

#include <algorithm>
#include <cstring>

#include "dirty_tiles.h"
#include "yuv_atlas.h"

namespace crs {

Box box_of(const Rect &r)
{
    Box b;
    b.x0 = r.x;
    b.y0 = r.y;
    b.x1 = r.x + (int32_t)r.w;
    b.y1 = r.y + (int32_t)r.h;
    return b;
}

Box intersect(const Box &a, const Box &b)
{
    Box c;
    c.x0 = std::max(a.x0, b.x0);
    c.y0 = std::max(a.y0, b.y0);
    c.x1 = std::min(a.x1, b.x1);
    c.y1 = std::min(a.y1, b.y1);
    return c;
}

void Coverage::reset(uint32_t w, uint32_t h)
{
    if (w != w_ || h != h_) {
        w_ = w;
        h_ = h;
        cw_ = (w + kCell - 1) / kCell;
        ch_ = (h + kCell - 1) / kCell;
        cells_.assign((size_t)cw_ * ch_, 0);
        clean_ = true;
    }
    any_ = false;
}

/* The cells are cleared when the first layer after a reset adds to them,
 * so a target with nothing opaque costs nothing. */
void Coverage::prepare()
{
    if (!any_ && !clean_) {
        std::memset(cells_.data(), 0, cells_.size());
        clean_ = true;
    }
}

Box Coverage::cell(int32_t cx, int32_t cy) const
{
    Box b;
    b.x0 = cx * kCell;
    b.y0 = cy * kCell;
    b.x1 = std::min<int32_t>(b.x0 + kCell, (int32_t)w_);
    b.y1 = std::min<int32_t>(b.y0 + kCell, (int32_t)h_);
    return b;
}

bool Coverage::cells_inside(const Box &b, Box *c) const
{
    const Box t = intersect(b, Box{0, 0, (int32_t)w_, (int32_t)h_});
    if (t.empty())
        return false;
    /* Cells whose first pixel is in b and whose last is too (the last cell
     * of a row or column may be short). */
    c->x0 = (t.x0 + kCell - 1) / kCell;
    c->y0 = (t.y0 + kCell - 1) / kCell;
    c->x1 = t.x1 == (int32_t)w_ ? (int32_t)cw_ : t.x1 / kCell;
    c->y1 = t.y1 == (int32_t)h_ ? (int32_t)ch_ : t.y1 / kCell;
    return c->x0 < c->x1 && c->y0 < c->y1;
}

void Coverage::add(const Box &b)
{
    Box c;
    if (!cells_inside(b, &c))
        return;
    prepare();
    any_ = true;
    clean_ = false;
    for (int32_t cy = c.y0; cy < c.y1; cy++)
        std::memset(&cells_[(size_t)cy * cw_ + c.x0], 1, (size_t)(c.x1 - c.x0));
}

bool Coverage::covers(const Box &in) const
{
    const Box b = intersect(in, Box{0, 0, (int32_t)w_, (int32_t)h_});
    if (b.empty())
        return true;
    if (!any_)
        return false;
    const int32_t cx0 = b.x0 / kCell, cy0 = b.y0 / kCell;
    const int32_t cx1 = (b.x1 - 1) / kCell, cy1 = (b.y1 - 1) / kCell;
    for (int32_t cy = cy0; cy <= cy1; cy++) {
        const uint8_t *row = &cells_[(size_t)cy * cw_];
        for (int32_t cx = cx0; cx <= cx1; cx++)
            if (!row[cx])
                return false;
    }
    return true;
}

namespace {

/* Image pixels [i0, i1) of a video n pixels across drawn over [at, at + len)
 * of the target: the target pixels that sample them. Drawn at its size,
 * each target pixel samples its own image pixel's centre (the shaders'
 * coordinates are highp); scaled, a target pixel blends the image pixels
 * about its centre's coordinate, so a pixel more each way, rounded out. */
void to_target(int32_t i0, int32_t i1, uint32_t n, int32_t at, uint32_t len, int32_t *t0, int32_t *t1)
{
    i0 = std::max<int32_t>(0, i0);
    i1 = std::min<int32_t>((int32_t)n, i1);
    if (len == n) {
        *t0 = at + i0;
        *t1 = at + i1;
        return;
    }
    i0 = std::max<int32_t>(0, i0 - 1);
    i1 = std::min<int32_t>((int32_t)n, i1 + 1);
    *t0 = std::max(at, at + (int32_t)(((int64_t)i0 * len) / n) - 1);
    *t1 = std::min(at + (int32_t)len, at + (int32_t)(((int64_t)i1 * len + n - 1) / n) + 1);
}

Box image_box(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t vw, uint32_t vh, const Rect &r)
{
    Box b;
    to_target(x0, x1, vw, r.x, r.w, &b.x0, &b.x1);
    to_target(y0, y1, vh, r.y, r.h, &b.y0, &b.y1);
    return b;
}

}  // namespace

int tile_footprint(uint32_t tx, uint32_t ty, uint32_t vw, uint32_t vh, const Rect &r, Box out[3])
{
    const int32_t T = (int32_t)crk::kTileSize;
    const int32_t w = (int32_t)vw, h = (int32_t)vh, ah = (int32_t)crk::atlas_height(vh);
    const int32_t ax0 = (int32_t)tx * T, ax1 = std::min(ax0 + T, w);
    const int32_t ay0 = (int32_t)ty * T, ay1 = std::min(ay0 + T, ah);
    int n = 0;
    /* Luma: the tile's own pixels. */
    if (ay0 < h)
        out[n++] = image_box(ax0, ay0, ax1, std::min(ay1, h), vw, vh, r);
    if (ay1 > h) {
        /* Chroma samples [c0, c1) x [r0, r1): sample k is read for luma
         * 2k-1 to 2k+1 across (2k-2 too, in case the filter's weight there
         * is not exactly 0) and 2k-1 to 2k+2 down (yuv_atlas.cpp's
         * coordinates; the composite's view shader clamps inwards only). */
        const int32_t r0 = std::max(ay0, h) - h, r1 = ay1 - h;
        const int32_t y0 = 2 * r0 - 1, y1 = 2 * r1 + 1;
        const int32_t half = w / 2;
        if (ax0 < half)
            out[n++] = image_box(2 * ax0 - 2, y0, 2 * std::min(ax1, half), y1, vw, vh, r);
        if (ax1 > half)
            out[n++] = image_box(2 * (std::max(ax0, half) - half) - 2, y0, 2 * (ax1 - half), y1, vw, vh, r);
    }
    return n;
}

bool hidden_tiles(const Coverage &cov, uint32_t vw, uint32_t vh, const Rect &r, std::vector<uint8_t> *hidden)
{
    const uint32_t cols = (vw + crk::kTileSize - 1) / crk::kTileSize;
    const uint32_t rows = (crk::atlas_height(vh) + crk::kTileSize - 1) / crk::kTileSize;
    hidden->assign((size_t)cols * rows, 0);
    if (cov.empty() || !r.w || !r.h)
        return false;
    bool any = false;
    Box parts[3];
    for (uint32_t ty = 0; ty < rows; ty++)
        for (uint32_t tx = 0; tx < cols; tx++) {
            const int n = tile_footprint(tx, ty, vw, vh, r, parts);
            bool all = n > 0;
            for (int i = 0; i < n && all; i++)
                all = cov.covers(parts[i]);
            if (all) {
                (*hidden)[(size_t)ty * cols + tx] = 1;
                any = true;
            }
        }
    return any;
}

}  // namespace crs
