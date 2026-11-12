/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Which tiles of a video nobody can see: those wholly under opaque layers
 * drawn above it on its target. Upload to the display is what is scarce,
 * so such tiles are not written while they stay hidden (video_layer.h).
 *
 * Coverage is the part of a target that the layers drawn above some point
 * of its stack cover opaquely, as the next flush will show them. It is
 * kept in cells of kCell x kCell target pixels: a cell counts as covered
 * only when every pixel of it is under one layer's opaque area, so the
 * coverage never claims a pixel that shows anything below.
 *
 * What counts as opaque (Layer::occlude), drawn as the next flush shows it
 * (Shown), and only for the layers drawn strictly below it:
 * - a video layer drawn unturned, at its params.opacity times its target's
 *   fade at least 1, once its first frame is written: its whole rect (the
 *   video shaders' alpha is u_opacity);
 * - a composited video likewise, over the place its quads have, and where a
 *   move waits for its write, only where the old and the new place overlap;
 * - an image layer drawn unturned, unanimated, at opacity times fade at
 *   least 1, only where both what its buffer holds and the pixels waiting
 *   to be written have alpha 255 (and the sampling filter reaches no other
 *   pixel);
 * - nothing of effect and mesh layers, nor of a layer turned, animated,
 *   translucent or fading.
 *
 * A tile of a video's 4:2:0 atlas is hidden when every target pixel it can
 * change is covered (tile_footprint): its luma rows, and its chroma rows
 * through the bilinear upsampling, each widened by the filters' reach.
 * Pixels off the target are never seen and do not count. A tile of a turned
 * video is never hidden. */
#ifndef CRS_OCCLUSION_H
#define CRS_OCCLUSION_H

#include <stdint.h>
#include <vector>

#include "graph.h"

namespace crs {

/* A rectangle of target pixels, x0 <= x < x1 and y0 <= y < y1. */
struct Box {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
Box box_of(const Rect &r);
Box intersect(const Box &a, const Box &b);

class Coverage {
public:
    static const int32_t kCell = 8;

    /* Nothing covered, on a w x h target. */
    void reset(uint32_t w, uint32_t h);
    bool empty() const { return !any_; }
    /* Every cell wholly inside b. */
    void add(const Box &b);
    /* Every cell wholly inside b for which opaque(cell) holds; whether any
     * was not covered before. */
    template <class F> bool add_if(const Box &b, F opaque)
    {
        Box c;
        if (!cells_inside(b, &c))
            return false;
        bool added = false;
        prepare();
        for (int32_t cy = c.y0; cy < c.y1; cy++)
            for (int32_t cx = c.x0; cx < c.x1; cx++) {
                uint8_t &v = cells_[(size_t)cy * cw_ + cx];
                if (!v && opaque(cell(cx, cy))) {
                    v = 1;
                    added = any_ = true;
                    clean_ = false;
                }
            }
        return added;
    }
    /* Every pixel of b on the target is covered (pixels off it are never
     * seen). */
    bool covers(const Box &b) const;

private:
    /* The cells wholly inside b; false: none. */
    bool cells_inside(const Box &b, Box *c) const;
    Box cell(int32_t cx, int32_t cy) const;
    void prepare();

    uint32_t w_ = 0, h_ = 0, cw_ = 0, ch_ = 0;
    std::vector<uint8_t> cells_;
    bool any_ = false, clean_ = true;
};

/* The target pixels atlas tile (tx, ty) of a vw x vh video (kTileSize tiles
 * over its vw x vh*3/2 atlas) drawn unturned over r can change: up to three
 * boxes (luma, U, V) within r; returns how many. */
int tile_footprint(uint32_t tx, uint32_t ty, uint32_t vw, uint32_t vh, const Rect &r, Box out[3]);

/* Each atlas tile (row by row) of a video drawn unturned over r: 1 where
 * cov covers its footprint. Returns whether any is. */
bool hidden_tiles(const Coverage &cov, uint32_t vw, uint32_t vh, const Rect &r, std::vector<uint8_t> *hidden);


}  // namespace crs

#endif
