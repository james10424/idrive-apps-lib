/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Robert Nisipeanu */
/* Which parts of a decoded frame differ visibly from what the map buffer
 * holds.
 *
 * The buffer holds an earlier frame. The new frame is compared with it tile
 * by tile; the plan lists the rectangles that differ visibly, and the
 * producer writes only those (the render service's video_layer.h), so a map
 * that stands still sends almost nothing and a frame identical to the shown
 * one sends nothing.
 *
 * Tiles are 64x64 pixels from the top-left; the last row and column are
 * partial. 64 is four H.264 macroblocks, so a changed macroblock dirties one
 * tile, and it divides the 1088-pixel map width into 17 columns. Runs of
 * adjacent written tiles in a tile row become one rectangle, and a run
 * directly below a rectangle of the same columns extends it, so a
 * full-width change is a single contiguous rectangle. Rectangles never take
 * in unchanged tiles: the bytes are what the link limits, and on a
 * test unit writes of 1 to 37 rectangles took the same time within noise. A
 * plan has at most 102 rectangles at 1088x720, each at least 1 KB.
 *
 * Noise filter. A still CarPlay map is not still in the decoded pixels: the
 * phone keeps coding residuals for macroblocks that did not change, and the
 * decoded values wander by a few levels. On a parked-car Waze capture from
 * this car (1920x900, 1338 frames) a third of the tiles differed from the
 * frame before, nearly all by at most 3 levels in any channel; 0.7% of them
 * by more than 8. A tile whose largest channel difference from what the
 * buffer holds is at most the threshold is quiet: it is left as shown. The
 * comparison is always against what the buffer holds, so a slow drift is
 * written once it adds up past the threshold, and after each plan is written
 * no shown pixel is more than the threshold off the frame it was planned
 * from. Frames the caller does not plan (upload_pacer.h holding them back)
 * are not shown at all until the next plan, which carries their changes. A
 * tile that has stayed quiet (but not equal) for settle_ms is written anyway,
 * so a small difference left at a seam with a written neighbour does not
 * stay. Threshold 0 writes every difference.
 *
 * When at least kWholePercent of the tiles are to be written (a pan, a zoom,
 * a keyframe), the plan is the whole frame instead: the tiles left out would
 * save at most a quarter of the bytes.
 *
 * One tile the size of the frame makes every plan nothing or the whole
 * frame. */
#ifndef CRK_DIRTY_TILES_H
#define CRK_DIRTY_TILES_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace crk {

constexpr uint32_t kTileSize = 64;
constexpr uint32_t kWholePercent = 75;
/* Tiles by their largest channel difference d: bin k holds 2^k <= d < 2^(k+1). */
constexpr int kDeltaBins = 8;

struct PixelRect {
    uint32_t x, y, w, h;
};

struct UploadPlan {
    std::vector<PixelRect> rects;   /* disjoint; empty when nothing is to be written */
    bool whole = false;             /* rects is the one whole-frame rectangle */
    uint32_t dirty_tiles = 0, tiles = 0;   /* dirty: to be written, settled ones included */
    uint32_t quiet_tiles = 0;       /* different, but not past the threshold: left as shown */
    uint32_t settled_tiles = 0;     /* quiet for settle_ms, so written */
    uint32_t delta_tiles[kDeltaBins] = {};   /* the differing tiles by largest difference */
    size_t bytes = 0;               /* pixel bytes the rects carry */
};

/* Plans each frame's write over the buffer, and remembers since when each
 * tile has been left quiet. Each plan is assumed written: a tile it writes is
 * then what the buffer holds. */
class UploadPlanner {
public:
    /* threshold: the largest channel difference left unwritten (0: any
     * difference is written). settle_ms: how long a tile may stay quiet before
     * it is written (0: as long as it stays quiet). */
    UploadPlanner(uint32_t w, uint32_t h, uint32_t bpp, uint32_t tile_w, uint32_t tile_h,
                  uint32_t threshold = 0, int64_t settle_ms = 0);

    /* Plans writing cur over a texture holding shown. shown null: the whole
     * frame. */
    UploadPlan plan(const uint8_t *shown, const uint8_t *cur, int64_t now_ms);
    /* The texture was written whole: no tile is quiet. */
    void reset();
    /* When the earliest quiet tile is due to be written, as of the last plan;
     * -1: none is due (no tile is quiet, or settle_ms is 0). */
    int64_t settle_due_ms() const { return settle_due_ms_; }
    /* Whether the last plan left a quiet tile: the texture is not the frame
     * it was planned from. */
    bool quiet() const { return quiet_ > 0; }

private:
    uint32_t w_, h_, bpp_, tile_w_, tile_h_, cols_, rows_, threshold_;
    int64_t settle_ms_;
    std::vector<int64_t> quiet_since_;   /* per tile; -1: equal to what is shown */
    uint32_t quiet_ = 0;                 /* tiles with quiet_since_ set */
    int64_t settle_due_ms_ = -1;
};

/* plan() with no noise filter and no memory: every difference is written. */
UploadPlan plan_upload(const uint8_t *prev, const uint8_t *cur, uint32_t w, uint32_t h, uint32_t bpp,
                       uint32_t tile_w, uint32_t tile_h);

/* The largest absolute difference between a[i] and b[i] over n bytes. */
uint8_t max_abs_diff(const uint8_t *a, const uint8_t *b, size_t n);

/* The rectangle's pixels with rows packed: a pointer into frame when the
 * rectangle spans whole rows, else into *scratch, which stays valid until the
 * next call with the same scratch. */
const uint8_t *pack_rect(const uint8_t *frame, uint32_t w, uint32_t bpp, const PixelRect &r,
                         std::vector<uint8_t> *scratch);

/* Copies the rectangles from src into dst, both w pixels wide. */
void copy_rects(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t bpp, const std::vector<PixelRect> &rects);

/* Hands each rectangle of frame, rows packed (pack_rect), to
 * set_data(pixels, rect) in order until it refuses one. Returns the bytes
 * it accepted; *all: whether it accepted every rectangle. */
template <class SetData>
size_t write_rects(const uint8_t *frame, uint32_t w, uint32_t bpp, const std::vector<PixelRect> &rects,
                   std::vector<uint8_t> *scratch, SetData set_data, bool *all)
{
    size_t accepted = 0;
    for (const PixelRect &r : rects) {
        if (!set_data(pack_rect(frame, w, bpp, r, scratch), r)) {
            *all = false;
            return accepted;
        }
        accepted += (size_t)r.w * r.h * bpp;
    }
    *all = true;
    return accepted;
}

}  // namespace crk

#endif
