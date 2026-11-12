/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "video_composite.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "yuv_atlas.h"

namespace crs {

using crk::PixelRect;

/* yuv_atlas.cpp's shader over the part u_view of the canvas: l is the
 * canvas pixel, and the clamps keep luma rows and chroma samples inside the
 * part, as the whole-frame shader keeps them inside the frame. */
const char *const kAtlasViewFragmentShader = R"(#version 100
precision highp float;
uniform sampler2D u_image;
uniform vec2 u_size;
uniform vec4 u_view;
uniform float u_opacity;
varying vec2 v_uv;
void main() {
    vec2 l = u_view.xy + v_uv * u_view.zw;
    float th = u_size.y * 1.5;
    vec2 c = clamp(vec2(l.x * 0.5 + 0.25, l.y * 0.5), u_view.xy * 0.5 + 0.5, (u_view.xy + u_view.zw) * 0.5 - 0.5);
    float y = texture2D(u_image, vec2(l.x / u_size.x, clamp(l.y, u_view.y + 0.5, u_view.y + u_view.w - 0.5) / th)).r;
    float u = texture2D(u_image, vec2(c.x / u_size.x, (u_size.y + c.y) / th)).r - 128.0 / 255.0;
    float v = texture2D(u_image, vec2((u_size.x * 0.5 + c.x) / u_size.x, (u_size.y + c.y) / th)).r - 128.0 / 255.0;
    vec3 rgb = vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), u_opacity);
})";

/* block_pack.cpp's shader over the part u_view of the canvas. */
const char *const kBlockViewFragmentShader = R"(#version 100
precision highp float;
uniform sampler2D u_image;
uniform vec2 u_size;
uniform vec4 u_view;
uniform float u_opacity;
varying vec2 v_uv;
float byte_of(float s) { return floor(s * 255.0 + 0.5); }
vec3 rgb565(float lo, float hi) {
    float v = hi * 256.0 + lo;
    float r = floor(v / 2048.0);
    float g = floor(mod(v, 2048.0) / 32.0);
    float b = mod(v, 32.0);
    return vec3(r / 31.0, g / 63.0, b / 31.0);
}
void main() {
    vec2 p = clamp(floor(u_view.xy + v_uv * u_view.zw), u_view.xy, u_view.xy + u_view.zw - 1.0);
    vec2 blk = floor(p / 4.0);
    vec2 q = p - blk * 4.0;
    vec2 tex = vec2(u_size.x * 0.5, u_size.y * 0.25);
    vec4 e = texture2D(u_image, vec2(blk.x * 2.0 + 0.5, blk.y + 0.5) / tex);
    vec4 s = texture2D(u_image, vec2(blk.x * 2.0 + 1.5, blk.y + 0.5) / tex);
    vec3 c0 = rgb565(byte_of(e.r), byte_of(e.g));
    vec3 c1 = rgb565(byte_of(e.b), byte_of(e.a));
    float row = byte_of(q.y < 1.0 ? s.r : q.y < 2.0 ? s.g : q.y < 3.0 ? s.b : s.a);
    float div = q.x < 1.0 ? 1.0 : q.x < 2.0 ? 4.0 : q.x < 3.0 ? 16.0 : 64.0;
    float sel = mod(floor(row / div), 4.0);
    vec3 c = sel < 0.5 ? c0 : sel < 1.5 ? c1 : sel < 2.5 ? (2.0 * c0 + c1) / 3.0 : (c0 + 2.0 * c1) / 3.0;
    gl_FragColor = vec4(c, u_opacity);
})";

void blend_bytes(uint8_t *dst, const uint8_t *src, size_t n, uint32_t t, bool simd)
{
    size_t i = 0;
#if defined(__SSE2__)
    if (simd) {
        /* d * (256 - t) + s * t + 128 is at most 65408: unsigned 16-bit lanes. */
        const __m128i wt = _mm_set1_epi16((short)t), wd = _mm_set1_epi16((short)(256 - t));
        const __m128i half = _mm_set1_epi16(128), zero = _mm_setzero_si128();
        for (; i + 16 <= n; i += 16) {
            const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
            const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
            __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), wd),
                                       _mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), wt));
            __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), wd),
                                       _mm_mullo_epi16(_mm_unpackhi_epi8(s, zero), wt));
            lo = _mm_srli_epi16(_mm_add_epi16(lo, half), 8);
            hi = _mm_srli_epi16(_mm_add_epi16(hi, half), 8);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_packus_epi16(lo, hi));
        }
    }
#else
    (void)simd;
#endif
    for (; i < n; i++)
        dst[i] = (uint8_t)((dst[i] * (256 - t) + src[i] * t + 128) >> 8);
}

namespace {

/* The source's cell (canvas pixels x0..x1, y0..y1, all even) copied into
 * the canvas, or blended in by t when t < 256: Y, then U and V. */
void put_cell(uint8_t *canvas, uint32_t cw, uint32_t ch, const ComposeSource &s, uint32_t x0, uint32_t y0, uint32_t x1,
              uint32_t y1, uint32_t t, bool simd)
{
    const uint32_t w = x1 - x0, sx = x0 - (uint32_t)s.x, sy = y0 - (uint32_t)s.y;
    auto row = [&](uint8_t *d, const uint8_t *p, uint32_t n) {
        if (t >= 256)
            std::memcpy(d, p, n);
        else
            blend_bytes(d, p, n, t, simd);
    };
    for (uint32_t y = y0; y < y1; y++)
        row(canvas + (size_t)y * cw + x0, s.atlas + (size_t)(sy + y - y0) * s.w + sx, w);
    const uint8_t *su = s.atlas + (size_t)s.w * s.h, *sv = su + s.w / 2;
    uint8_t *cu = canvas + (size_t)cw * ch, *cv = cu + cw / 2;
    for (uint32_t k = 0; k < (y1 - y0) / 2; k++) {
        const size_t c = (size_t)(y0 / 2 + k) * cw + x0 / 2, f = (size_t)(sy / 2 + k) * s.w + sx / 2;
        row(cu + c, su + f, w / 2);
        row(cv + c, sv + f, w / 2);
    }
}

bool covers(const PixelRect &c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    return c.x <= x0 && x1 <= c.x + c.w && c.y <= y0 && y1 <= c.y + c.h;
}

}  // namespace

void compose_rect(uint8_t *canvas, uint32_t cw, uint32_t ch, const PixelRect &r, const ComposeSource *src, size_t n,
                  bool simd)
{
    /* The cells: r cut at every source's edges, each covered by the same
     * sources throughout. */
    n = std::min(n, kComposeMax);
    uint32_t xs[2 + 2 * kComposeMax], ys[2 + 2 * kComposeMax];
    size_t nx = 0, ny = 0;
    xs[nx++] = r.x, xs[nx++] = r.x + r.w;
    ys[ny++] = r.y, ys[ny++] = r.y + r.h;
    for (size_t i = 0; i < n; i++) {
        const PixelRect &c = src[i].clip;
        for (uint32_t e : {c.x, c.x + c.w})
            if (e > r.x && e < r.x + r.w)
                xs[nx++] = e;
        for (uint32_t e : {c.y, c.y + c.h})
            if (e > r.y && e < r.y + r.h)
                ys[ny++] = e;
    }
    std::sort(xs, xs + nx);
    std::sort(ys, ys + ny);
    nx = std::unique(xs, xs + nx) - xs;
    ny = std::unique(ys, ys + ny) - ys;
    for (size_t j = 0; j + 1 < ny; j++)
        for (size_t i = 0; i + 1 < nx; i++) {
            const uint32_t x0 = xs[i], x1 = xs[i + 1], y0 = ys[j], y1 = ys[j + 1];
            /* From the top opaque source up; none: from the bottom one. */
            size_t from = n, top = n;
            for (size_t k = 0; k < n; k++) {
                if (!src[k].alpha || !covers(src[k].clip, x0, y0, x1, y1))
                    continue;
                if (from == n || src[k].alpha >= 256)
                    from = k;
                top = k;
            }
            if (from == n)
                continue;
            put_cell(canvas, cw, ch, src[from], x0, y0, x1, y1, 256, simd);
            /* acc: the coverage so far, 0-256; each source above takes its
             * share of the coverage with it. */
            uint32_t acc = std::min<uint32_t>(src[from].alpha, 256);
            for (size_t k = from + 1; k <= top && k < n; k++) {
                if (!src[k].alpha || !covers(src[k].clip, x0, y0, x1, y1))
                    continue;
                const uint32_t a = src[k].alpha, cover = a * 256 + (256 - a) * acc;
                const uint32_t t = (a * 65536 + cover / 2) / cover;
                put_cell(canvas, cw, ch, src[k], x0, y0, x1, y1, t, simd);
                acc = a + ((256 - a) * acc + 128) / 256;
            }
        }
}

namespace {

uint32_t alpha_of(float opacity)
{
    const float a = opacity * 256.0f + 0.5f;
    return a <= 0 ? 0 : a >= 256 ? 256 : (uint32_t)a;
}

/* Whether the frames a and b (w x h atlases) differ over r (frame pixels,
 * even), and copy_part() copying it. */
bool part_differs(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h, const PixelRect &r)
{
    for (uint32_t y = r.y; y < r.y + r.h; y++)
        if (std::memcmp(a + (size_t)y * w + r.x, b + (size_t)y * w + r.x, r.w))
            return true;
    const size_t base = (size_t)w * h;
    for (uint32_t k = r.y / 2; k < (r.y + r.h) / 2; k++)
        for (size_t off : {base + (size_t)k * w + r.x / 2, base + (size_t)k * w + w / 2 + r.x / 2})
            if (std::memcmp(a + off, b + off, r.w / 2))
                return true;
    return false;
}

void copy_part(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h, const PixelRect &r)
{
    for (uint32_t y = r.y; y < r.y + r.h; y++)
        std::memcpy(dst + (size_t)y * w + r.x, src + (size_t)y * w + r.x, r.w);
    const size_t base = (size_t)w * h;
    for (uint32_t k = r.y / 2; k < (r.y + r.h) / 2; k++)
        for (size_t off : {base + (size_t)k * w + r.x / 2, base + (size_t)k * w + w / 2 + r.x / 2})
            std::memcpy(dst + off, src + off, r.w / 2);
}

bool intersect(const PixelRect &a, const PixelRect &b, PixelRect *out)
{
    const uint32_t x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const uint32_t x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x0 >= x1 || y0 >= y1)
        return false;
    *out = PixelRect{x0, y0, x1 - x0, y1 - y0};
    return true;
}


}  // namespace

CompositeVideo::CompositeVideo(int target, crk::UploadPacer &pacer, int atlas_effect, int block_effect,
                               const VideoParams &p, int32_t x, int32_t y, const Rect &clip, int64_t now_ms)
    : VideoLayer(0, target, 0, pacer, atlas_effect, block_effect, p, now_ms), x_(x), y_(y), clip_(clip),
      cols_((p.w + crk::kTileSize - 1) / crk::kTileSize), rows_((p.h + crk::kTileSize - 1) / crk::kTileSize),
      dirty_(cols_ * rows_, 0)
{
    params.rect.x = x;
    params.rect.y = y;
    params.rect.w = p.w;
    params.rect.h = p.h;
}

CompositeVideo::Source *CompositeVideo::find(uint32_t id)
{
    for (Source &s : src_)
        if (s.id == id)
            return &s;
    return nullptr;
}

const CompositeVideo::Source *CompositeVideo::find(uint32_t id) const
{
    for (const Source &s : src_)
        if (s.id == id)
            return &s;
    return nullptr;
}

bool CompositeVideo::has(uint32_t id) const
{
    for (const Source &s : src_)
        if (s.id == id)
            return true;
    return false;
}

PixelRect CompositeVideo::in_canvas(const Rect &r) const
{
    return PixelRect{(uint32_t)(r.x - x_), (uint32_t)(r.y - y_), r.w, r.h};
}

void CompositeVideo::mark(const Rect &r)
{
    if (!r.w || !r.h)
        return;
    const PixelRect c = in_canvas(r);
    for (uint32_t ty = c.y / crk::kTileSize; ty <= (c.y + c.h - 1) / crk::kTileSize && ty < rows_; ty++)
        for (uint32_t tx = c.x / crk::kTileSize; tx <= (c.x + c.w - 1) / crk::kTileSize && tx < cols_; tx++)
            dirty_[ty * cols_ + tx] = 1;
}

void CompositeVideo::add(uint32_t id, VideoLayer *src)
{
    Source s;
    s.id = id;
    s.layer = src;
    s.opacity = s.quad_opacity = src->params.opacity;
    src_.push_back(s);
    if (g_) {
        Source &n = src_.back();
        if (make_quad(g_, n, false, 0) && (!p_.pan_blocks || make_quad(g_, n, true, block_buf_[block_bank_]))) {
            source_opacity(n);
            source_shows(n);
        }
        scene_changed_ = true;
    }
}

void CompositeVideo::remove(uint32_t id)
{
    for (size_t i = 0; i < src_.size(); i++) {
        Source &s = src_[i];
        if (s.id != id)
            continue;
        if (s.drawn)
            mark(s.rect);
        if (g_) {
            if (s.atlas_quad) g_->drop_quad(s.atlas_quad);
            if (s.block_quad) g_->drop_quad(s.block_quad);
            scene_changed_ = true;
        }
        src_.erase(src_.begin() + i);
        return;
    }
}

void CompositeVideo::arrange_source(uint32_t id, bool drawn, int32_t order)
{
    Source *s = find(id);
    if (!s)
        return;
    const LayerParams &lp = s->layer->params;
    /* Its frame unturned at its size and at even x and y: what a composable
     * video's rect is, and how one kept here a while after it stopped being
     * composable (compositor.h) is drawn. */
    const int32_t fx = lp.rect.x & ~1, fy = lp.rect.y & ~1;
    const int32_t fw = (int32_t)s->layer->video().w, fh = (int32_t)s->layer->video().h;
    Rect r;
    const int32_t x0 = std::max(fx, clip_.x), y0 = std::max(fy, clip_.y);
    const int32_t x1 = std::min(fx + fw, clip_.x + (int32_t)clip_.w);
    const int32_t y1 = std::min(fy + fh, clip_.y + (int32_t)clip_.h);
    if (x0 < x1 && y0 < y1) {
        r.x = x0, r.y = y0;
        r.w = (uint32_t)(x1 - x0), r.h = (uint32_t)(y1 - y0);
    }
    const bool moved = r != s->rect, shown = drawn != s->drawn, faded = lp.opacity != s->opacity;
    if (s->drawn && (moved || shown || faded))
        mark(s->rect);   /* what it covered is composed again, without it or with its new look */
    if (moved || shown || faded)
        s->full = true;
    s->rect = r;
    s->fx = fx - x_;
    s->fy = fy - y_;
    s->opacity = lp.opacity;
    s->drawn = drawn;
    const bool reorder = order != s->order;
    s->order = order;
    if (!g_)
        return;
    if (!drawn) {
        /* Hidden at once; its place follows, nothing of it showing. */
        if (s->showing || s->placed != r) {
            s->placed = r;
            s->showing = false;
            place_quads(*s);
            scene_changed_ = true;
        }
        s->moved = s->composed = false;
    } else if (moved || shown) {
        s->moved = true;
        s->composed = false;
    }
    if (reorder) {
        g_->order(s->atlas_quad, order);
        if (s->block_quad)
            g_->order(s->block_quad, order + 1);
    }
    /* The quads take the new opacity in the target's next flush
     * (show_opacity), unless that is held (hold). */
    scene_changed_ = scene_changed_ || faded || reorder;
}

void CompositeVideo::place_quads(Source &s)
{
    g_->place(s.atlas_quad, s.placed.w ? s.placed : clip_);
    if (s.block_quad)
        g_->place(s.block_quad, s.placed.w ? s.placed : clip_);
    view_uniforms(s);
    source_shows(s);
}

bool CompositeVideo::place_composed()
{
    bool any = false;
    for (Source &s : src_) {
        if (!s.moved || !s.composed)
            continue;
        placing_.push_back(Placed{s.id, s.placed, s.showing});
        s.placed = s.rect;
        s.showing = true;
        s.moved = s.composed = false;
        place_quads(s);
        any = true;
    }
    return any;
}

Stage CompositeVideo::stage(int64_t now_ms)
{
    placing_.clear();
    const Stage st = VideoLayer::stage(now_ms);
    /* A write carries the canvas; with none due, the display's buffer
     * holds it already. */
    placed_now_ = g_ && !hold_ && (st == Stage::Wrote || (st == Stage::Idle && !waiting())) && place_composed();
    if (st != Stage::Wrote)
        placing_.clear();
    return st;
}

void CompositeVideo::committed(bool ok, int64_t now_ms)
{
    VideoLayer::committed(ok, now_ms);
    if (!ok && g_)
        for (const Placed &p : placing_) {
            Source *s = find(p.id);
            if (!s)
                continue;
            /* Back where the display shows them; the move goes with the
             * next write, which carries the canvas whole. */
            s->moved = s->composed = true;
            s->placed = p.placed;
            s->showing = p.showing;
            place_quads(*s);
        }
    placing_.clear();
}

bool CompositeVideo::arranged()
{
    /* The drawn sources' order: where it changed, their overlaps are
     * composed again. */
    std::vector<const Source *> drawn;
    for (const Source &s : src_)
        if (s.drawn)
            drawn.push_back(&s);
    std::sort(drawn.begin(), drawn.end(), [](const Source *a, const Source *b) { return a->order < b->order; });
    std::vector<uint32_t> ranking;
    for (const Source *s : drawn)
        ranking.push_back(s->id);
    /* Those drawn before and now, in a new order (one shown or hidden is
     * composed by itself). */
    auto in = [](const std::vector<uint32_t> &v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); };
    std::vector<uint32_t> was, is;
    for (uint32_t id : ranking_)
        if (in(ranking, id))
            was.push_back(id);
    for (uint32_t id : ranking)
        if (in(ranking_, id))
            is.push_back(id);
    if (was != is)
        for (Source &s : src_)
            if (s.drawn)
                s.full = true;
    ranking_ = ranking;
    const bool any = !drawn.empty();
    if (any != drawn_)
        arrange(any, 0);
    const bool changed = scene_changed_;
    scene_changed_ = false;
    return changed;
}

void CompositeVideo::compose(int64_t now_ms)
{
    if (!due(now_ms))
        return;
    bool frame = false;
    for (Source &s : src_) {
        const uint8_t *f = s.layer->newest();
        const bool waits = s.layer->waiting();
        frame = frame || waits;
        if (s.drawn && s.rect.w && f && (s.full || s.layer->fresh())) {
            const uint32_t w = s.layer->video().w, h = s.layer->video().h;
            uint8_t *taken = s.layer->taken();
            if (s.full) {
                mark(s.rect);
                std::memcpy(taken, f, s.layer->frame_bytes());
                s.full = false;
            } else {
                /* The canvas's tiles over the source where its frame differs
                 * from what was last taken. */
                const PixelRect c = in_canvas(s.rect);
                for (uint32_t ty = c.y / crk::kTileSize; ty <= (c.y + c.h - 1) / crk::kTileSize; ty++)
                    for (uint32_t tx = c.x / crk::kTileSize; tx <= (c.x + c.w - 1) / crk::kTileSize; tx++) {
                        const PixelRect tile{tx * crk::kTileSize, ty * crk::kTileSize, crk::kTileSize,
                                             crk::kTileSize};
                        PixelRect part;
                        if (!intersect(tile, c, &part))
                            continue;
                        const PixelRect in_frame{part.x - (uint32_t)s.fx, part.y - (uint32_t)s.fy, part.w, part.h};
                        if (part_differs(f, taken, w, h, in_frame)) {
                            dirty_[ty * cols_ + tx] = 1;
                            copy_part(taken, f, w, h, in_frame);
                        }
                    }
            }
        }
        if (waits)
            s.layer->took();
    }
    /* The drawn sources bottom first, and each dirty tile composed from them. */
    std::vector<const Source *> drawn;
    for (const Source &s : src_)
        if (s.drawn && s.rect.w && s.layer->newest())
            drawn.push_back(&s);
    std::sort(drawn.begin(), drawn.end(), [](const Source *a, const Source *b) { return a->order < b->order; });
    std::vector<ComposeSource> cs;
    for (const Source *s : drawn) {
        ComposeSource c;
        c.atlas = s->layer->newest();
        c.w = s->layer->video().w;
        c.h = s->layer->video().h;
        c.x = s->fx;
        c.y = s->fy;
        c.clip = in_canvas(s->rect);
        c.alpha = alpha_of(s->opacity);
        cs.push_back(c);
    }
    uint8_t *canvas = nullptr;
    uint64_t tiles = 0;
    for (uint32_t ty = 0; ty < rows_; ty++)
        for (uint32_t tx = 0; tx < cols_; tx++) {
            if (!dirty_[ty * cols_ + tx])
                continue;
            dirty_[ty * cols_ + tx] = 0;
            if (!canvas)
                canvas = this->canvas();
            const PixelRect tile{tx * crk::kTileSize, ty * crk::kTileSize,
                                 std::min(crk::kTileSize, p_.w - tx * crk::kTileSize),
                                 std::min(crk::kTileSize, p_.h - ty * crk::kTileSize)};
            compose_rect(canvas, p_.w, p_.h, tile, cs.data(), cs.size());
            tiles++;
        }
    for (Source &s : src_)
        if (s.moved && (!s.full || !s.rect.w))
            s.composed = true;
    if (tiles) {
        tiles_composed_ += tiles;
        canvas_changed(now_ms);
    } else if (frame) {
        repeat(now_ms);
    }
}

int64_t CompositeVideo::wake_in_ms(int64_t now_ms) const
{
    const int64_t w = VideoLayer::wake_in_ms(now_ms);
    if (w > 0 || !g_ || !drawn_)
        return w;
    /* A source's frame waits to be composed at the frame clock. */
    bool waits = std::find(dirty_.begin(), dirty_.end(), 1) != dirty_.end();
    for (const Source &s : src_)
        waits = waits || (s.drawn && (s.full || s.layer->waiting()));
    const int64_t c = clock_wait_ms(now_ms);
    return waits && c > 0 ? c : -1;
}

void CompositeVideo::begin_hidden()
{
    visible_.assign((size_t)cols_ * ((crk::atlas_height(p_.h) + crk::kTileSize - 1) / crk::kTileSize), 0);
    covered_ = false;
    clear_.clear();
}

void CompositeVideo::see_source(uint32_t id, const Coverage &cov)
{
    const Source *s = find(id);
    if (!s)
        return;
    if (cov.empty()) {
        clear_.push_back(id);   /* all it shows is visible, if anything is covered */
        return;
    }
    covered_ = true;
    see(*s, &cov);
}

void CompositeVideo::see(const Source &s, const Coverage *cov)
{
    Box at[2];
    int n = 0;
    if (drawn_ && s.showing && s.placed.w)
        at[n++] = box_of(s.placed);
    if (s.drawn && s.rect.w && !hold_)
        at[n++] = box_of(s.rect);
    if (!n)
        return;
    Rect canvas;
    canvas.x = x_;
    canvas.y = y_;
    canvas.w = p_.w;
    canvas.h = p_.h;
    const uint32_t rows = (uint32_t)(visible_.size() / cols_);
    Box parts[3];
    for (uint32_t ty = 0; ty < rows; ty++)
        for (uint32_t tx = 0; tx < cols_; tx++) {
            uint8_t &v = visible_[(size_t)ty * cols_ + tx];
            if (v)
                continue;
            const int np = tile_footprint(tx, ty, p_.w, p_.h, canvas, parts);
            for (int i = 0; i < np && !v; i++)
                for (int k = 0; k < n && !v; k++) {
                    const Box b = intersect(parts[i], at[k]);
                    v = !b.empty() && (!cov || !cov->covers(b));
                }
        }
}

void CompositeVideo::occlude_source(uint32_t id, Coverage *cov, float fade) const
{
    const Source *s = find(id);
    if (!s)
        return;
    const float opacity = hold_ ? s->quad_opacity : s->opacity;
    if ( !g_ || !drawn_ || !opaque_ || !s->showing || !s->drawn || !s->placed.w || opacity * fade < 1.0f)
        return;
    /* Where its quads are, and with a move waiting, where they may go too. */
    Box a = box_of(s->placed);
    if (s->moved && !hold_)
        a = intersect(a, box_of(s->rect));
    cov->add(a);
}

bool CompositeVideo::show_opacity()
{
    bool any = false;
    for (Source &s : src_) {
        if (s.quad_opacity == s.opacity)
            continue;
        s.quad_opacity = s.opacity;
        if (g_)
            source_opacity(s);
        any = true;
    }
    return any;
}

bool CompositeVideo::held_changes() const
{
    for (const Source &s : src_)
        if ((s.moved && s.drawn) || s.quad_opacity != s.opacity)
            return true;
    return false;
}

void CompositeVideo::end_hidden()
{
    if (!covered_) {
        hide(nullptr);
        return;
    }
    for (uint32_t id : clear_)
        if (const Source *s = find(id))
            see(*s, nullptr);
    for (uint8_t &v : visible_)
        v = !v;
    hide(visible_.data());
}

bool CompositeVideo::make_quad(Graph *g, Source &s, bool blocks, int bank)
{
    const float size[2] = {(float)p_.w, (float)p_.h}, zero = 0.0f;
    int &q = blocks ? s.block_quad : s.atlas_quad;
    /* New quads start at opacity 0 until the canvas is written: placed
     * where the source is. */
    s.placed = s.rect;
    s.showing = s.drawn;
    s.moved = s.composed = false;
    s.quad_opacity = s.opacity;
    q = g->quad(blocks ? block_effect_ : atlas_effect_, s.rect.w ? s.rect : clip_);
    if (!q || !g->texture(q, "u_image", blocks ? bank : atlas_buf_) ||
        !g->uniform(q, "u_size", size, 2) || !g->uniform(q, "u_opacity", &zero, 1) || !g->show(q, false) ||
        !g->order(q, s.order + (blocks ? 1 : 0)))
        return false;
    view_uniforms(s);
    return true;
}

void CompositeVideo::view_uniforms(Source &s)
{
    const PixelRect c = in_canvas(s.placed.w ? s.placed : clip_);
    const float view[4] = {(float)c.x, (float)c.y, (float)c.w, (float)c.h};
    if (s.atlas_quad)
        g_->uniform(s.atlas_quad, "u_view", view, 4);
    if (s.block_quad)
        g_->uniform(s.block_quad, "u_view", view, 4);
}

bool CompositeVideo::make_atlas_quads(Graph *g)
{
    /* g_ is the graph being attached: view_uniforms() writes through it. */
    for (Source &s : src_)
        if (!make_quad(g, s, false, 0))
            return false;
    return true;
}

bool CompositeVideo::make_block_quads(Graph *g)
{
    for (Source &s : src_)
        if (!make_quad(g, s, true, block_buf_[0]))
            return false;
    return true;
}

void CompositeVideo::drop_quads()
{
    for (Source &s : src_) {
        if (s.atlas_quad) g_->drop_quad(s.atlas_quad);
        if (s.block_quad) g_->drop_quad(s.block_quad);
    }
}

void CompositeVideo::detach(bool drop)
{
    VideoLayer::detach(drop);
    for (Source &s : src_)
        s.atlas_quad = s.block_quad = 0;
}

void CompositeVideo::source_shows(Source &s)
{
    const bool on = drawn_ && s.showing && s.placed.w;
    if (s.atlas_quad)
        g_->show(s.atlas_quad, on && !blocks_shown_);
    if (s.block_quad)
        g_->show(s.block_quad, on && blocks_shown_);
}

void CompositeVideo::show_quads(bool)
{
    for (Source &s : src_)
        source_shows(s);
}

bool CompositeVideo::bank_quads(int buf)
{
    bool ok = true;
    for (Source &s : src_)
        if (s.block_quad)
            ok = g_->texture(s.block_quad, "u_image", buf) && ok;
    return ok;
}

void CompositeVideo::source_opacity(Source &s)
{
    const float a = opaque_ ? s.quad_opacity * fade_ : 0.0f;
    if (s.atlas_quad)
        g_->uniform(s.atlas_quad, "u_opacity", &a, 1);
    if (s.block_quad)
        g_->uniform(s.block_quad, "u_opacity", &a, 1);
}

void CompositeVideo::set_opacity()
{
    for (Source &s : src_)
        source_opacity(s);
}

void CompositeVideo::arrange(bool drawn, int32_t)
{
    if (!g_)
        return;
    drawn_ = drawn;
    show_quads(blocks_shown_);
    scene_changed_ = true;
}

std::string CompositeVideo::stats() const
{
    char b[96];
    std::snprintf(b, sizeof b, "%zu videos, %llu tiles composed; ", src_.size(), (unsigned long long)tiles_composed_);
    return b + VideoLayer::stats();
}

}  // namespace crs
