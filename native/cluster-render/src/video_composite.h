/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* A target's videos composed on the CPU into one video layer, so the
 * display holds one video's buffers for all of them (compositor.h).
 *
 * The composite is a video layer (video_layer.h) whose frame, the canvas,
 * covers its sources' rectangles: it is written with every rule of a video
 * layer, from the target's one byte budget, its dirty tiles planned against
 * what its buffer holds, packed blocks for motion the budget cannot cover.
 * Each source is drawn by its own pair of quads (atlas and packed blocks)
 * over its rectangle, sampling the canvas there, at the source's place in
 * the stack and at its own opacity times the target's fade. So the stack's
 * order holds between a video and the other layers, and the canvas
 * outside every source is never drawn.
 *
 * The canvas at a pixel is what the sources over it would draw there over
 * nothing, without the alpha: the bottom source's frame, then each source
 * above blended in by its share of the coverage. A source's quad drawn at
 * its opacity a over the quads below it then gives each pixel exactly the
 * colour the sources' own quads would give. Where the top source is opaque
 * or covers alone, the canvas is a plain copy of its frame. The canvas is
 * composed from the layers' own opacity (params.opacity) and the quads
 * apply the target's fade, so during a target's fade in or out overlapping
 * videos, opaque or not, are drawn as one picture faded together rather
 * than each faded on its own. Outside fades they look as drawn separately,
 * unless another layer is drawn between two overlapping translucent
 * videos.
 *
 * A source moved or shown again keeps its quads where they were (hidden,
 * when shown again) until the canvas holding it at its new place is
 * written: the quads move in the flush that carries that write, and go
 * back if that flush does not go. So a quad never shows a place of the
 * canvas that does not hold its source yet. Hiding goes at once.
 *
 * Each source's quads sample only its own part of the canvas: luma at pixel
 * centres and chroma clamped inside the part, as the video shader clamps a
 * whole frame, so an opaque source draws as it would alone.
 *
 * Work per frame: the composite composes when it may write (its frame
 * clock is due and the budget allows), from each source's newest frame, so
 * a frame that would never be sent is never composed. A source's new frame
 * is compared with what the composite last took from it, tile by tile (the
 * canvas's 64x64 tiles); only the
 * tiles a source changed, and those a source moving, hiding, showing or
 * changing opacity or order touched, are composed again. A cell of a tile
 * covered by one source, or under an opaque top source, is copied; only
 * cells where a translucent source overlaps another are blended, with SSE2
 * where there is one. A repeat from every source is a repeat of the
 * canvas.
 *
 * Hidden tiles (video_layer.h, occlusion.h): a canvas tile is hidden when,
 * for every source whose quads may show it after the next flush (where
 * they are, and where a move or show waiting for its write puts them), the
 * part of it they show is wholly under opaque layers drawn above that
 * source. A source draws opaquely, for the layers below it, where its
 * quads are at its opacity times the fade, and where a move waits, only
 * where the old place and the new overlap. A source's new opacity reaches
 * its quads in the target's next flush (show_opacity), which the
 * compositor may hold back like a move (hold) while it would uncover tiles
 * the budget cannot write yet. With nothing opaque above any
 * source, no tile is hidden.
 *
 * Sources are client video layers drawn unturned, at their frame's size
 * (rect w x h = vw x vh) and at even x and y, so the canvas's 4:2:0 planes
 * line up with theirs. */
#ifndef CRS_VIDEO_COMPOSITE_H
#define CRS_VIDEO_COMPOSITE_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "dirty_tiles.h"
#include "video_layer.h"

namespace crs {

/* The video shaders for a part of the canvas: u_view is the part (x, y, w,
 * h in canvas pixels) the quad covers; u_size the canvas. */
extern const char *const kAtlasViewFragmentShader;
extern const char *const kBlockViewFragmentShader;

/* The most sources compose_rect() takes. */
const size_t kComposeMax = 16;

/* A source's part of a composed rectangle. */
struct ComposeSource {
    const uint8_t *atlas = nullptr;   /* its frame's 4:2:0 atlas */
    uint32_t w = 0, h = 0;            /* its frame's size */
    int32_t x = 0, y = 0;             /* its frame's top-left in the canvas, even */
    crk::PixelRect clip{0, 0, 0, 0};  /* what it covers in the canvas, within its frame; even */
    uint32_t alpha = 256;             /* its opacity, 0-256 */
};

/* Composes r (even) of the canvas, a cw x ch 4:2:0 atlas, from the
 * sources, bottom first (above), at most kComposeMax; cells no source
 * covers stay as they are.
 * simd: the SSE2 blend where there is one; both give the same bytes. */
void compose_rect(uint8_t *canvas, uint32_t cw, uint32_t ch, const crk::PixelRect &r, const ComposeSource *src,
                  size_t n, bool simd = true);
/* dst = (dst * (256 - t) + src * t + 128) / 256 over n bytes, t 0-256. */
void blend_bytes(uint8_t *dst, const uint8_t *src, size_t n, uint32_t t, bool simd = true);

class CompositeVideo : public VideoLayer {
public:
    /* The canvas p.w x p.h has its top-left at (x, y) on the target; clip
     * is the target's rectangle the sources are drawn within. */
    CompositeVideo(int target, crk::UploadPacer &pacer, int atlas_effect, int block_effect, const VideoParams &p,
                   int32_t x, int32_t y, const Rect &clip, int64_t now_ms);
    const char *kind() const override { return "composite"; }
    int32_t x() const { return x_; }
    int32_t y() const { return y_; }
    const Rect &clip() const { return clip_; }

    /* A client video layer drawn through the composite from now (already a
     * source, video_layer.h), or no more. */
    void add(uint32_t id, VideoLayer *src);
    void remove(uint32_t id);
    bool has(uint32_t id) const;
    size_t sources() const { return src_.size(); }
    /* The source's place in the stack (drawn, draw order) and its params
     * (rect, opacity), each arrange; then arranged(). */
    void arrange_source(uint32_t id, bool drawn, int32_t order);
    /* Whether anything the scene shows changed since the last call. */
    bool arranged();
    /* Takes the sources' new frames and composes what they changed, when
     * the composite may write (due()); before stage(). Frames that come
     * faster than it writes wait in their sources, newest only. */
    void compose(int64_t now_ms);

    void detach(bool drop) override;
    int64_t wake_in_ms(int64_t now_ms) const override;
    /* stage() also moves and shows the quads of the sources the write
     * carries; committed() puts them back when the flush does not go. */
    Stage stage(int64_t now_ms) override;
    void committed(bool ok, int64_t now_ms) override;
    /* The last stage() changed quads without a write: the target flushes. */
    bool placed_now() const { return placed_now_; }
    void arrange(bool drawn, int32_t order) override;
    std::string stats() const override;

    uint64_t composed_tiles() const { return tiles_composed_; }

    /* Occlusion, each tick before compose(): begin_hidden(); at each
     * source's place in the stack, top first, see_source() with what the
     * layers drawn above it cover, then occlude_source() adds what it
     * covers; end_hidden() then hides the canvas tiles no source shows. */
    void begin_hidden();
    void see_source(uint32_t id, const Coverage &cov);
    void occlude_source(uint32_t id, Coverage *cov, float fade) const;
    void end_hidden();
    /* For this tick, sources' moves, shows and opacity changes wait (no
     * quad goes to a new place or opacity), and occlusion takes the quads
     * as they are. */
    void hold(bool on) { hold_ = on; }
    bool held() const { return hold_; }
    /* A move, show or opacity change waits to reach the quads. */
    bool held_changes() const;
    /* The quads take the sources' opacity; whether any changed. */
    bool show_opacity();

protected:
    bool make_atlas_quads(Graph *g) override;
    bool make_block_quads(Graph *g) override;
    void drop_quads() override;
    void show_quads(bool blocks) override;
    bool bank_quads(int buf) override;
    void set_opacity() override;

private:
    struct Source {
        uint32_t id = 0;
        VideoLayer *layer = nullptr;
        Rect rect;                 /* drawn on the target: its rect within clip_; w 0: nowhere */
        int32_t fx = 0, fy = 0;    /* its frame's top-left in the canvas */
        float opacity = 1.0f;
        float quad_opacity = 1.0f;   /* what its quads have */
        bool drawn = false;
        int32_t order = 0;
        bool full = true;          /* compose its whole rect and take its whole frame next */
        int atlas_quad = 0, block_quad = 0;
        /* Where its quads are and whether they show: rect and drawn once the
         * canvas written holds it there (moved: a move or a show waits for
         * that; composed: the canvas holds it, the write is to go). */
        Rect placed;
        bool showing = false, moved = false, composed = false;
    };
    /* A source's quads as they were before place_composed(), for a flush
     * that does not go. */
    struct Placed {
        uint32_t id;
        Rect placed;
        bool showing;
    };

    Source *find(uint32_t id);
    const Source *find(uint32_t id) const;
    /* rect on the target, in the canvas. */
    crk::PixelRect in_canvas(const Rect &r) const;
    void mark(const Rect &r);
    bool make_quad(Graph *g, Source &s, bool blocks, int bank);
    void place_quads(Source &s);
    /* Moves and shows whose canvas this tick's write carries (or the
     * display's buffer holds already) go to the quads; whether any did. */
    bool place_composed();
    void view_uniforms(Source &s);
    /* The canvas tiles s may show after the next flush that cov (null:
     * nothing) does not cover become visible. */
    void see(const Source &s, const Coverage *cov);
    void source_opacity(Source &s);
    void source_shows(Source &s);

    int32_t x_, y_;
    Rect clip_;
    uint32_t cols_, rows_;
    std::vector<Source> src_;
    std::vector<uint8_t> dirty_;       /* per canvas tile */
    std::vector<uint32_t> ranking_;    /* drawn sources' ids by draw order, as last composed */
    std::vector<Placed> placing_;      /* the quads place_composed() moved this tick */
    bool placed_now_ = false;
    /* Occlusion this tick: the canvas tiles a source shows, whether any
     * source had anything opaque above it, and the sources that had not. */
    std::vector<uint8_t> visible_;
    bool covered_ = false, hold_ = false;
    std::vector<uint32_t> clear_;
    bool scene_changed_ = false;
    uint64_t tiles_composed_ = 0;
};

}  // namespace crs

#endif
