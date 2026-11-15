// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mlurker@gmail.com
/* Where the map is drawn in its target: `map_rect` in the conf, "X,Y,W,H" in
 * the target's pixels from its top-left (cluster-render PROTOCOL.md, a
 * layer's rect), whichever target is current. The map is stretched to it.
 * Empty: the map area (map_geometry.h), all of the map slot. The target's
 * size is known only once the renderer answers, so the rect is checked
 * against it there (MapFeed). */
#ifndef WOC_MAP_RECT_H
#define WOC_MAP_RECT_H

#include <stdint.h>
#include <stdio.h>
#include <cmath>
#include <string>

namespace woc {

struct MapRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;   /* w 0: the map area */

    bool set() const { return w != 0; }
    bool fits(uint32_t tw, uint32_t th) const { return (uint64_t)x + w <= tw && (uint64_t)y + h <= th; }
};

/* Parses `text` into *out. Returns null, or why it cannot be used. */
inline const char *parse_map_rect(const std::string &text, MapRect *out)
{
    *out = MapRect();
    if (text.empty())
        return nullptr;
    unsigned x, y, w, h;
    char tail;
    if (sscanf(text.c_str(), "%u,%u,%u,%u%c", &x, &y, &w, &h, &tail) != 4)
        return "must be X,Y,W,H, or empty";
    if (w < 16 || h < 16 || w > 4096 || h > 4096)
        return "the rect must be from 16x16 to 4096x4096";
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    return nullptr;
}

/* The part of a decoded frame of fw x fh, drawn stretched over rect q of its
 * target, that the rect src of that target covers (a scene hosting the
 * target shows src stretched over its rect: cluster-render PROTOCOL.md
 * "host"), in the frame's pixels, grown to even edges (4:2:0) and held
 * inside the frame; and *place, where that part lands in rect: src's
 * stretch applied to it, so a map smaller than src keeps its place and
 * size there, held inside rect. False when src misses q. */
inline bool host_crop(const MapRect &q, uint32_t fw, uint32_t fh, const MapRect &src, const MapRect &rect,
                      MapRect *out, MapRect *place)
{
    const uint64_t x0 = src.x > q.x ? src.x : q.x, y0 = src.y > q.y ? src.y : q.y;
    const uint64_t x1 = (uint64_t)src.x + src.w < (uint64_t)q.x + q.w ? (uint64_t)src.x + src.w : (uint64_t)q.x + q.w;
    const uint64_t y1 = (uint64_t)src.y + src.h < (uint64_t)q.y + q.h ? (uint64_t)src.y + src.h : (uint64_t)q.y + q.h;
    if (!q.w || !q.h || x1 <= x0 || y1 <= y0)
        return false;
    uint64_t fx0 = (x0 - q.x) * fw / q.w, fy0 = (y0 - q.y) * fh / q.h;
    uint64_t fx1 = ((x1 - q.x) * fw + q.w - 1) / q.w, fy1 = ((y1 - q.y) * fh + q.h - 1) / q.h;
    fx0 &= ~(uint64_t)1;
    fy0 &= ~(uint64_t)1;
    fx1 = (fx1 + 1) & ~(uint64_t)1;
    fy1 = (fy1 + 1) & ~(uint64_t)1;
    out->x = (uint32_t)fx0;
    out->y = (uint32_t)fy0;
    out->w = (uint32_t)((fx1 < fw ? fx1 : fw) - fx0);
    out->h = (uint32_t)((fy1 < fh ? fy1 : fh) - fy0);
    /* the part's edges: frame -> target (over q) -> rect (src stretched) */
    auto edge = [](double f, uint32_t fs, uint32_t q0, uint32_t qs, uint32_t s0, uint32_t ss, uint32_t r0, uint32_t rs) {
        const double v = r0 + (q0 + f * qs / fs - s0) * rs / ss;
        return (int64_t)std::lround(v < r0 ? r0 : v > (double)r0 + rs ? (double)r0 + rs : v);
    };
    if (!src.w || !src.h)
        return false;
    const int64_t px0 = edge(out->x, fw, q.x, q.w, src.x, src.w, rect.x, rect.w),
                  px1 = edge(out->x + out->w, fw, q.x, q.w, src.x, src.w, rect.x, rect.w),
                  py0 = edge(out->y, fh, q.y, q.h, src.y, src.h, rect.y, rect.h),
                  py1 = edge(out->y + out->h, fh, q.y, q.h, src.y, src.h, rect.y, rect.h);
    if (px1 <= px0 || py1 <= py0)
        return false;
    *place = MapRect{(uint32_t)px0, (uint32_t)py0, (uint32_t)(px1 - px0), (uint32_t)(py1 - py0)};
    return true;
}

}  // namespace woc

#endif
