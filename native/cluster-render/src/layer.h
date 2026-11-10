/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* A client's layer on a target, and the two simple kinds: image (RGBA
 * pixels a client draws, written in changed rectangles into one persistent
 * buffer) and effect (a client's fragment shader over a quad, driven by
 * uniforms). The video kind is video_layer.h, the mesh kind mesh_layer.h.
 *
 * A layer outlives its target's scene: it keeps what it shows on the CPU,
 * so when the scene is published again (attach) it is drawn as it was, its
 * animations started again. Each render tick the compositor asks every
 * layer of a requested target to stage its changes into the scene, flushes
 * the scene once if any did, and tells each layer whether that flush went
 * (committed). A layer that staged nothing is not asked. A layer whose
 * failed write left its buffer half written either holds the target until
 * its whole rewrite goes (holds_flush) or is hidden until then. No
 * layer makes a texture, sampler or effect per update. */
#ifndef CRS_LAYER_H
#define CRS_LAYER_H

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

#include "dirty_tiles.h"
#include "frame_clock.h"
#include "graph.h"
#include "stack.h"
#include "upload_pacer.h"

namespace crs {

/* What a client sets on a layer, at any time. */
struct LayerParams {
    Klass klass = Klass::Content;
    int z = 0;
    bool visible = true, exclusive = false, prewarm = false, contend = false;
    bool guide_hold = true;   /* false: no guide hold for its target while it wants it (slots.h) */
    bool yield = true;        /* false: its target gives way to no stock pop-up while it wants it (slots.h) */
    Rect rect;
    float opacity = 1.0f;
    /* A quad turned (degrees clockwise) and scaled about its pivot: px, py
     * from the rect's top-left, its centre unless set. */
    float rotate = 0, sx = 1, sy = 1, px = 0, py = 0;
    bool pivot_x = false, pivot_y = false;

    Turn turn() const;
};

enum class Stage { Idle, Wrote, Failed };

/* What a layer's stage() wrote since the layer was made: frames (a video's)
 * or images written, their bytes, writes or flushes that failed, and stages
 * held back by the byte budget. */
struct WriteStats {
    uint64_t frames = 0, bytes = 0, failed = 0, holds = 0;
    uint64_t age_ms = 0;   /* a video's frames' ages at their flush (offer to flush), summed */
};

/* The uniforms a client sets on one quad or part, kept so a new scene gets
 * them, each sent in the next flush after it changes. */
class Uniforms {
public:
    static const size_t kMaxNames = 32;
    static const size_t kMaxFloats = 4096;

    /* n 1-4 floats: a float, vec2, vec3 or vec4. False for a new name past
     * kMaxNames. */
    bool set(const std::string &name, const float *v, int n);
    /* elements values of type t. False past kMaxNames or kMaxFloats. */
    bool set(const std::string &name, UniformType t, const float *v, uint32_t elements);
    void dirty_all();
    void dirty(const std::string &name);
    /* Sends the changed ones to drawable; whether any went. One the shader
     * does not have is the client's mistake: dropped, not retried every
     * tick. */
    bool stage(Graph *g, int drawable);
    /* The flush carrying them went. */
    void committed();

private:
    struct Value {
        std::vector<float> v;
        int n = 0;   /* 1-4: the plain form, else type and elements */
        UniformType type = UniformType::Float;
        uint32_t elements = 1;
        bool dirty = true;
    };
    std::map<std::string, Value> values_;
    size_t floats_ = 0;
};

class Layer {
public:
    static const size_t kMaxAnimations = 16;

    Layer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer)
        : id_(id), target_(target), seq_(seq), pacer_(pacer)
    {
    }
    virtual ~Layer() {}

    uint32_t id() const { return id_; }
    int target() const { return target_; }
    uint64_t seq() const { return seq_; }
    virtual const char *kind() const = 0;

    LayerParams params;

    /* Plays a on part (0: the layer's quad, or a mesh layer's camera) by
     * the renderer, replacing anim of the same number; kept and started
     * again in a new scene. False with *err when the layer or part cannot
     * play it, past kMaxAnimations, or when the renderer refused it. */
    bool animate(uint32_t anim, uint32_t part, const Animation &a, std::string *err);
    /* Stops it; its property goes back to what the client set. */
    bool stop(uint32_t anim);
    /* The quad or part a sampler of part is bound on; 0: none. */
    virtual int drawable(uint32_t) const { return 0; }
    /* What the renderer refused of the layer's content after it was
     * attached; empty: nothing. */
    virtual std::string refusal() const { return std::string(); }

    /* Has content to draw. */
    virtual bool ready(int64_t now_ms) const = 0;
    /* Builds the layer's objects in a published scene, hidden. False: the
     * renderer refused one; *err says which. */
    virtual bool attach(Graph *g, int64_t now_ms, std::string *err) = 0;
    /* The scene goes, or the layer: forgets its objects, dropping them from
     * the scene first when drop (the scene stays). */
    virtual void detach(bool drop) = 0;
    bool attached() const { return g_ != nullptr; }
    /* Drawn or not (stack.h), and its place in the draw order. */
    virtual void arrange(bool drawn, int32_t order) = 0;
    /* params.rect or params.opacity changed. */
    virtual void place() = 0;
    /* The layer draws its own opacity (video and image): params.opacity
     * times its target's fade. */
    virtual bool fades() const { return false; }
    /* Its target's fade, 0-1 (the compositor's); the next flush carries it. */
    void set_fade(float f)
    {
        if (f != fade_) {
            fade_ = f;
            faded();
        }
    }
    float fade() const { return fade_; }
    /* Writes what changed into the scene; the caller flushes. */
    virtual Stage stage(int64_t now_ms) = 0;
    /* After stage() returned Wrote or Failed: whether the flush carrying it
     * went. */
    virtual void committed(bool ok, int64_t now_ms) = 0;
    /* Pixel bytes the last stage() wrote. */
    virtual size_t staged_bytes() const { return 0; }
    /* A failed write left the drawn layer's buffer half written: the
     * target stages and flushes nothing but this layer's whole rewrite
     * until it goes, which is due whenever the budget allows, whatever the
     * layer's frame clock (a video layer, and an exclusive image; any other
     * image is hidden instead). */
    virtual bool holds_flush() const { return false; }
    /* Milliseconds until stage() has something due; -1: nothing sooner
     * than the regular tick. */
    virtual int64_t wake_in_ms(int64_t) const { return -1; }
    /* A line for the status: what it wrote. */
    virtual std::string stats() const { return ""; }
    virtual WriteStats write_stats() const { return WriteStats(); }
    /* Pixel bytes its buffers hold in the scene; 0 when not attached. */
    virtual size_t buffer_bytes() const { return 0; }

protected:
    /* After set_fade() changed the fade. */
    virtual void faded() {}
    /* What an animation of part acts on in the scene; 0: none. */
    virtual int animated(uint32_t) const { return 0; }
    /* Why this layer cannot play a on part; nullptr: it can. */
    virtual const char *animation_error(uint32_t, const Animation &) const
    {
        return "animations are for image, effect and mesh layers";
    }
    /* After an animation of part stops: what the client set goes back. */
    virtual void restore(uint32_t, const Animation &) {}
    /* Starts every animation in the scene just attached. */
    void start_animations();
    /* The scene goes, or the layer: the animations' handles are forgotten
     * (dropping a node stops its animations). */
    void forget_animations();

    uint32_t id_;
    int target_;
    uint64_t seq_;
    crk::UploadPacer &pacer_;
    Graph *g_ = nullptr;
    bool drawn_ = false;
    int32_t order_ = 0;
    float fade_ = 1.0f;

    /* Keeps a and starts it when attached; no limit (the service's own). */
    bool play(uint32_t anim, uint32_t part, const Animation &a, std::string *err);

private:
    struct Played {
        uint32_t part = 0;
        Animation a;
        int handle = 0;   /* in the current scene */
    };
    std::map<uint32_t, Played> played_;
};

/* Why a quad (image or effect layer, part 0) cannot play a; nullptr: it
 * can. */
const char *quad_animation_error(uint32_t part, const Animation &a);

/* RGBA8 pixels (straight alpha) the client draws, w x h, shown over
 * params.rect. A client sends the rectangles that changed; they are written
 * in place at most fps times a second, within the shared byte budget. */
class ImageLayer : public Layer {
public:
    static const uint32_t kMaxRects = 16;   /* more pending: the whole image is written */

    ImageLayer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer, int effect, uint32_t w, uint32_t h,
               uint32_t fps, Sampling sampling, int64_t now_ms);
    const char *kind() const override { return "image"; }
    static const char *size_error(uint32_t w, uint32_t h);

    /* Rectangles of new pixels, each's rows packed one after another in
     * data (4 bytes a pixel); none: data is the whole image. False with
     * *err when a rectangle is outside the image or data is short. */
    bool update(const std::vector<crk::PixelRect> &rects, const uint8_t *data, size_t n, std::string *err);

    bool ready(int64_t) const override { return have_; }
    bool attach(Graph *g, int64_t now_ms, std::string *err) override;
    void detach(bool drop) override;
    void arrange(bool drawn, int32_t order) override;
    void place() override;
    Stage stage(int64_t now_ms) override;
    void committed(bool ok, int64_t now_ms) override;
    size_t staged_bytes() const override { return staged_; }
    bool holds_flush() const override { return holding() && drawn_; }
    int64_t wake_in_ms(int64_t now_ms) const override;
    std::string stats() const override;
    WriteStats write_stats() const override;
    size_t buffer_bytes() const override { return g_ ? (size_t)w_ * h_ * 4 : 0; }
    bool fades() const override { return true; }

protected:
    void faded() override;
    int animated(uint32_t part) const override { return part ? 0 : quad_; }
    const char *animation_error(uint32_t part, const Animation &a) const override
    {
        return quad_animation_error(part, a);
    }
    void restore(uint32_t part, const Animation &a) override;

private:
    int effect_;
    uint32_t w_, h_;
    Sampling sampling_;
    crk::FrameClock clock_;
    std::vector<uint8_t> px_;
    std::vector<crk::PixelRect> dirty_;
    /* parked_: a failed write left the buffer unlike any image; until the
     * next write goes the quad stays hidden, or the target holds while the
     * image is exclusive or was when the write failed (hold_). */
    bool holding() const { return parked_ && (hold_ || params.exclusive); }
    bool whole_ = true, have_ = false, parked_ = false, hold_ = false;
    int buf_ = 0, quad_ = 0;
    std::vector<uint8_t> scratch_;
    size_t staged_ = 0;   /* bytes setData took this tick */
    uint64_t writes_ = 0, bytes_ = 0, failed_ = 0, held_budget_ = 0;
};

/* A fragment shader over params.rect (GLSL ES 1.00, varying v_uv from the
 * quad's top-left), drawn as compiled. Uniforms the client sets are sent in
 * the next flush. u_time (seconds) comes either from the service, updated
 * time_fps times a second (seconds since the layer was made), or from the
 * renderer's own clock (cluster_time: 0 to 3600, again every hour), with
 * nothing sent per frame. */
class EffectLayer : public Layer {
public:
    EffectLayer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer, int effect, uint32_t time_fps,
                int64_t now_ms);
    const char *kind() const override { return "effect"; }

    static const size_t kMaxUniforms = Uniforms::kMaxNames;
    /* n 1-4 floats. False for a new name past kMaxUniforms. */
    bool set_uniform(const std::string &name, const float *v, int n);
    /* elements values of type t. False past the limits (Uniforms). */
    bool set_uniform(const std::string &name, UniformType t, const float *v, uint32_t elements);
    /* u_time from the renderer's clock. */
    void cluster_time();

    bool ready(int64_t) const override { return true; }
    bool attach(Graph *g, int64_t now_ms, std::string *err) override;
    void detach(bool drop) override;
    void arrange(bool drawn, int32_t order) override;
    void place() override;
    Stage stage(int64_t now_ms) override;
    void committed(bool ok, int64_t now_ms) override;
    int64_t wake_in_ms(int64_t now_ms) const override;
    int drawable(uint32_t part) const override { return part ? 0 : quad_; }

protected:
    int animated(uint32_t part) const override { return part ? 0 : quad_; }
    const char *animation_error(uint32_t part, const Animation &a) const override
    {
        return quad_animation_error(part, a);
    }
    void restore(uint32_t part, const Animation &a) override;

private:
    int effect_;
    int quad_ = 0;
    crk::FrameClock time_clock_;
    uint32_t time_fps_;
    int64_t born_ms_;
    Uniforms uniforms_;
    bool staged_ = false;
};

}  // namespace crs

#endif
