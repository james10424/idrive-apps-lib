// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mlurker@gmail.com
/* The part of a decoded picture that becomes the map: `src_crop` in the
 * conf, "SWxSH:X,Y,W,H". It applies only to a picture of exactly SW x SH, so
 * one setting can serve a phone that draws its UI in part of its picture
 * (Android Auto's cluster view: a 1280x720 picture with the map in its top
 * 1280x564) without touching a stream of another size (CarPlay's, already
 * the map's own size). Empty: the whole picture. Every number is even
 * (4:2:0), the rect lies inside the picture and is at least 16x16. */
#ifndef WOC_SOURCE_CROP_H
#define WOC_SOURCE_CROP_H

#include <stdint.h>
#include <stdio.h>
#include <string>

namespace woc {

struct SourceCrop {
    uint32_t seq_w = 0, seq_h = 0;   /* 0: no crop */
    uint32_t x = 0, y = 0, w = 0, h = 0;

    bool enabled() const { return seq_w != 0; }
    bool applies(uint32_t sw, uint32_t sh) const { return enabled() && sw == seq_w && sh == seq_h; }
};

/* Parses `text` into *out. Returns null, or why it cannot be used. */
inline const char *parse_source_crop(const std::string &text, SourceCrop *out)
{
    *out = SourceCrop();
    if (text.empty())
        return nullptr;
    unsigned sw, sh, x, y, w, h;
    char tail;
    if (sscanf(text.c_str(), "%ux%u:%u,%u,%u,%u%c", &sw, &sh, &x, &y, &w, &h, &tail) != 6)
        return "must be SWxSH:X,Y,W,H, or empty";
    if (sw == 0 || sh == 0 || sw > 4096 || sh > 4096)
        return "the picture size must be from 1x1 to 4096x4096";
    if ((sw | sh | x | y | w | h) & 1)
        return "every number must be even (4:2:0)";
    if (w < 16 || h < 16)
        return "the rect must be at least 16x16";
    if ((uint64_t)x + w > sw || (uint64_t)y + h > sh)
        return "the rect must lie inside the picture";
    out->seq_w = sw;
    out->seq_h = sh;
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    return nullptr;
}

/* One decoded picture's visible rect, *x,*y,*w,*h as the decoder set it:
 * replaced by the crop's rect inside it when the picture is SW x SH, else
 * left as it is. Decided per picture, so a stream of another size that
 * follows (a compatible sequence the decoder does not restart for) is never
 * cropped. Returns whether it applied. */
template <typename T>
inline bool crop_picture(const SourceCrop &c, T *x, T *y, T *w, T *h)
{
    if (!c.applies(*w, *h))
        return false;
    *x = (T)(*x + c.x);
    *y = (T)(*y + c.y);
    *w = (T)c.w;
    *h = (T)c.h;
    return true;
}

/* Whether a converter set up for an input rect of iw x ih must be set up
 * again for a picture of pw x ph: its rect after crop_picture differs. */
inline bool rect_needs_reset(const SourceCrop &c, uint32_t iw, uint32_t ih, uint32_t pw, uint32_t ph)
{
    uint32_t x = 0, y = 0;
    crop_picture(c, &x, &y, &pw, &ph);
    return pw != iw || ph != ih;
}

/* Whether the pictures of a sequence of sw x sh need the per-picture
 * decision (crop_picture, rect_needs_reset) under a converter set up for an
 * input rect of iw x ih: the crop applies to the sequence, or the sequence
 * needs another rect. An unknown sequence (0 x 0) does. */
inline bool sequence_needs_check(const SourceCrop &c, uint32_t iw, uint32_t ih, uint32_t sw, uint32_t sh)
{
    return c.enabled() && (c.applies(sw, sh) || rect_needs_reset(c, iw, ih, sw, sh));
}

/* Whether a picture restarts the decoder at its sequence for its VPP input
 * rect. After a rect restart, pictures that still need another rect are
 * converted with VPP as it is set up, with one warning, until one fits it:
 * a stream whose pictures never fit their sequence's rect keeps its map
 * instead of restarting at every access unit, and a later size change
 * (Android Auto, CarPlay, Android Auto) still restarts. */
class RectRestarts {
public:
    void started(bool by_rect_restart)
    {
        exempt_ = by_rect_restart;
        warned_ = false;
    }
    /* needs_other: the picture's rect differs from VPP's (rect_needs_reset). */
    bool restart(bool needs_other)
    {
        if (!needs_other)
            exempt_ = false;
        return needs_other && !exempt_;
    }
    /* What restart(needs_other) would return, without counting the picture. */
    bool would_restart(bool needs_other) const { return needs_other && !exempt_; }
    /* True the first time a picture of this sequence is converted as is. */
    bool warn()
    {
        const bool first = !warned_;
        warned_ = true;
        return first;
    }

private:
    bool exempt_ = false;
    bool warned_ = false;
};

}  // namespace woc

#endif
