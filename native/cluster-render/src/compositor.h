/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The compositor: every client's layers in their targets' scenes.
 *
 * Each tick, arrange() orders each target's layers (stack.h), shows the
 * drawn ones in its scene and tells the slot manager whether the target is
 * wanted; render() then has every layer of each requested target stage its
 * changes, top layer first (so overlays get the byte budget before a video
 * under them), and flushes the scene once: a tick's changes of all layers
 * reach the display together or not at all. A setData a layer could not
 * finish holds the flush back for the tick, and every layer that staged is
 * told the flush did not go (layer.h). A video layer or an exclusive image
 * whose buffer that left half written holds the target from then on: only
 * its whole rewrite stages, when the budget allows, and nothing is flushed
 * until it goes, so the display keeps the last frame.
 *
 * Assets (asset.h) are staged before the layers, in the same flush. A layer
 * that samples an asset (bind) is ready once every asset it samples has
 * pixels, and drawn only while their buffers hold them whole.
 *
 * Fades: a target's video and image layers are drawn at their opacity times
 * the target's fade. The fade is 0 while the target is not on screen
 * (acquiring, parked), rises to 1 over fade_ms once it is shown, and, when
 * nothing wants the target any more, falls back to 0 over fade_ms with the
 * layers last drawn kept drawn and the target kept wanted, so it is released
 * after the fade. A fade step goes at most every kFadeStepMs, in the
 * target's ordinary flush. A parked target stages nothing: nothing of it is
 * on screen.
 *
 * It is the slot manager's Scenes: publishing a target builds its scene and
 * attaches the target's assets and then its layers, which start from what
 * they last showed, binds and animations included; unpublishing takes the
 * scene down and keeps the assets and the layers. Only a target the slot
 * manager has requested stages and flushes. A released target's scene stays
 * published as last flushed, its layers attached and holding what they have
 * not written: a video's newest frame, an image's changed rectangles. Asked
 * for again, its layers continue from there in the tick that asks: what
 * their buffers hold is what the scene last flushed, and a renderer that
 * subscribes gets the scene whole, as last flushed, from the RAMSES client
 * (VERIFIED on a test unit: sent at the subscription), so a video writes
 * the tiles that differ from it, not the whole frame. That resend is
 * charged to the byte budget when the display takes the scene (resent()).
 *
 * The layers and assets of a target with the HUD's share (TargetSpec::share)
 * write from a budget of their own (upload_pacer.h share_of): while one of
 * them is asked for and has a layer, up to kSharePct of each refill of the
 * budget, only as much as they spend, what they leave going back to the
 * other targets at once, and the whole refill while the other targets'
 * budget sits full. The two together never hold more than one burst. The
 * other targets never get less than the rest, and with no such layer they
 * go exactly as they would without it. Evidence from such a target is
 * logged, never counted towards the cap, and its bytes are not the link's
 * load for the governor, so it never moves the cap.
 *
 * Scene contents (PROTOCOL.md "Scene files"): a client's scene file on a
 * target, at most one a target. While one is there and has not failed to
 * load, the target is wanted, its scene is the file's (Gpu::load), and every
 * layer on it waits with reason "scene", as below an exclusive layer. The
 * content's edits (scene_edit.h) stage in the target's one flush a tick; a
 * scene loaded again gets them all again. When the content comes or goes,
 * the published scene is stale (Scenes::stale) and the slot manager
 * publishes the target again: at once while something wants it, else after
 * the linger, when it is released. A content's file replaced, its scene is
 * outdated (Scenes::outdated) and published again in place, the target
 * kept in its slot. The files' sizes are charged to the
 * budget when the scene is published and when the display takes it again:
 * the renderer fetches its resources then. */
#ifndef CRS_COMPOSITOR_H
#define CRS_COMPOSITOR_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "asset.h"
#include "governor.h"
#include "graph.h"
#include "layer.h"
#include "mesh_layer.h"
#include "scene_edit.h"
#include "scene_file.h"
#include "slots.h"
#include "stack.h"
#include "video_layer.h"

namespace crs {

enum class LayerState { Hidden, Stalled, Waiting, Shown, Suspended, Error };
const char *layer_state_name(LayerState s);

struct CompositorLimits {
    /* An app drawing a cluster layout or a page of readings makes one small
     * effect layer per piece (a short shader over its own rectangle), so it
     * needs dozens; one large shader drawing many pieces instead has been
     * reported to restart the KOMBI. */
    uint32_t layers = 96;               /* across all clients */
    uint32_t per_client = 64;
    uint32_t videos = 2;                /* video layers: four atlases each, plus packing */
    uint64_t pixel_bytes = 16u << 20;   /* CPU copies of all images, videos, assets and meshes */
    uint32_t assets = 32;               /* across all clients */
    uint32_t assets_per_client = 16;
};

class Compositor : public Scenes {
public:
    /* The byte budget all layers share starts at budget.max_bps (0: none)
     * and follows the governor (governor.h). */
    Compositor(Gpu *gpu, SlotManager *slots, const GovernorConfig &budget, int upload_burst_ms,
               const CompositorLimits &limits = CompositorLimits());
    ~Compositor() override;

    /* Compiles the built-in effects (video atlas, packed blocks, image). */
    bool init(std::string *err);

    /* New layers, owned by a client; 0 with *err when refused. */
    uint32_t add_video(uint64_t owner, int target, const LayerParams &lp, const VideoParams &vp, int64_t now_ms,
                       std::string *err);
    uint32_t add_image(uint64_t owner, int target, const LayerParams &lp, uint32_t w, uint32_t h, uint32_t fps,
                       Sampling s, int64_t now_ms, std::string *err);
    uint32_t add_effect(uint64_t owner, int target, const LayerParams &lp, const std::string &vs,
                        const std::string &fs, uint32_t time_fps, int64_t now_ms, std::string *err);
    uint32_t add_mesh(uint64_t owner, int target, const LayerParams &lp, const MeshParams &mp, int64_t now_ms,
                      std::string *err);
    /* A mesh layer's part, its effect compiled here; false with *err. */
    bool add_part(uint64_t owner, uint32_t layer, uint32_t part, const std::string &vs, const std::string &fs,
                  const PartLook &look, std::string *err);
    /* A part's geometry, within the service's memory; false with *err. */
    bool geometry(uint64_t owner, uint32_t layer, uint32_t part, MeshData &&data, int64_t now_ms,
                  std::string *err);

    /* A new asset, owned by a client; 0 with *err when refused. */
    uint32_t add_asset(uint64_t owner, int target, const AssetParams &ap, std::string *err);
    /* The asset if it exists and belongs to owner. */
    Asset *asset(uint64_t owner, uint32_t id);
    /* False with *err while a layer samples it. */
    bool remove_asset(uint64_t owner, uint32_t id, std::string *err);
    /* The layer's sampler (of part, for a mesh layer) samples the asset,
     * which must be on its target; replaces what it sampled. */
    bool bind(uint64_t owner, uint32_t layer, uint32_t part, const std::string &sampler, uint32_t asset,
              std::string *err);
    /* After a change of a layer's scene objects outside stage(), so the next
     * flush carries it. */
    void touch(Layer *l);

    /* A scene content, owned by a client (above); 0 with *err when the
     * target has one already. */
    uint32_t add_scene(uint64_t owner, int target, const SceneSource &src, bool contend, std::string *err);
    /* Its file replaced, its edits gone: published again in place
     * (Scenes::outdated), the target not released. False when id is not
     * owner's scene content. */
    bool replace_scene(uint64_t owner, uint32_t id, const SceneSource &src, bool contend);
    /* The target's scene content (not failed to load); 0: none. */
    uint32_t scene_on(int target) const { return content(target); }
    /* A scene content's target; -1: id is none. */
    int scene_target(uint32_t id) const
    {
        auto it = scenes_.find(id);
        return it == scenes_.end() ? -1 : it->second.target;
    }
    /* The scene content's edits if it exists and belongs to owner. */
    SceneEdits *edits(uint64_t owner, uint32_t id);
    /* For the status: "on <target>: " its files and edits; empty when id is
     * none. */
    std::string scene_text(uint32_t id) const;

    /* The layer if it exists and belongs to owner. */
    Layer *layer(uint64_t owner, uint32_t id);
    /* After a change of layer->params: placed and arranged again. */
    void changed(Layer *l);
    void remove(uint64_t owner, uint32_t id);
    void remove_owner(uint64_t owner);

    void arrange(int64_t now_ms);
    void render(int64_t now_ms);
    int64_t wake_in_ms(int64_t now_ms) const;

    LayerState state(uint32_t id, std::string *reason) const;
    Governor &governor() { return governor_; }
    const Governor &governor() const { return governor_; }
    /* A flush taking this long is evidence of a link falling behind
     * (INFERRED: the RAMSES client sends the flush's scene actions through
     * vsomeip, which blocks once its queue to the display backs up). */
    void set_slow_flush_ms(int ms) { slow_flush_us_ = (int64_t)ms * 1000; }
    /* A scene content's edits flush at most once in this long (the
     * service's tick, 1000 / fps; 0: as they come); lines coming faster
     * wait for the next. */
    void set_file_tick_ms(int ms) { file_tick_ms_ = ms; }
    /* How long a fade in or out takes; 0: none. */
    void set_fade_ms(int ms) { fade_ms_ = ms; }
    static const int64_t kFadeStepMs = 50;
    /* The most of each refill the HUD's share takes (upload_pacer.h). */
    static const unsigned kSharePct = 50;
    /* The clock flushes are timed with, in microseconds (tests set it). */
    void set_clock(int64_t (*clock_us)()) { clock_us_ = clock_us; }
    /* Layers by id, for status reports. */
    std::vector<const Layer *> layers() const;
    /* The sum of the target's layers' write_stats(). */
    WriteStats write_stats(int target) const;
    std::vector<const Asset *> assets() const;
    uint64_t owner_of(uint32_t id) const;

    bool publish(const TargetSpec &t, std::string *err) override;
    void unpublish(const TargetSpec &t) override;
    /* Charges the budget the scene's buffer bytes, layers' and assets', or
     * a file scene's files. */
    void resent(const TargetSpec &t, int64_t now_ms) override;
    /* A scene content came or went since the target's scene was published. */
    bool stale(const TargetSpec &t) const override;
    /* The published file scene's content has another file since. */
    bool outdated(const TargetSpec &t) const override;

private:
    struct Entry {
        std::unique_ptr<Layer> layer;
        uint64_t owner = 0;
        int effect = 0;        /* a client effect this layer holds */
        size_t pixel_bytes = 0;
        std::string error;     /* the renderer refused the layer's objects */
        bool drawn = false;
        int32_t order = -1;
        std::map<std::pair<uint32_t, std::string>, uint32_t> binds;   /* (part, sampler) -> asset */
    };
    struct AssetEntry {
        std::unique_ptr<Asset> asset;
        uint64_t owner = 0;
    };
    struct SceneEntry {
        uint64_t owner = 0;
        int target = 0;
        SceneSource src;
        bool contend = false;
        std::string error;   /* the file did not load: the content wants nothing */
        SceneEdits edits;
        uint32_t serial = 1;   /* its file's: one more each replacement */
    };
    struct Target {
        std::unique_ptr<Graph> graph;
        /* Or the scene of a scene content's file: which content's, its
         * serial, and its files' bytes. */
        std::unique_ptr<FileScene> file;
        uint32_t file_of = 0, file_serial = 0;
        uint64_t file_bytes = 0;
        int64_t file_flush_ms = -1;   /* its edits' last flush */
        Arrangement arr;
        bool dirty = false;    /* changed outside a layer's stage(): flush */
        uint64_t tag = 0;
        bool flush_failing = false;
        /* The fade (above): where it is, what the layers were last
         * given and when, and while fading out, the layers drawn when the
         * target was last wanted. */
        float fade = 1.0f, applied = 1.0f;
        int64_t fade_ms = 0, applied_ms = 0;
        bool fading_out = false;
        std::vector<uint32_t> last_drawn;
        bool last_contend = false, last_guide_hold = true, last_yield = true;
    };

    uint32_t admit(uint64_t owner, int target, size_t pixel_bytes, std::string *err);
    /* The target's scene content that has not failed to load; 0: none. */
    uint32_t content(int target) const;
    /* Flushes the target's scene, the evidence of a refusal or a slow one
     * for the governor. */
    bool flush(int ti, Target &t, int64_t now_ms);
    /* Stages the scene content's due edits into the target's file scene and
     * flushes it. */
    void render_file(int ti, Target &t, int64_t now_ms);
    /* bytes the renderer fetches now, charged to the target's budget. */
    void charge(int ti, uint64_t bytes, int64_t now_ms);
    /* The budget the target's writes spend: the HUD's share for a share
     * target. */
    crk::UploadPacer &pacer_for(int target);
    /* The governor's evidence, but a share target's only logged. */
    void evidence(const TargetSpec &t, Evidence e, int64_t now_ms, const std::string &detail);
    /* Bytes written now_ms, flushed or not, for the governor's load. */
    void load(uint64_t bytes, int64_t now_ms);
    /* Moves the target's fade on to now_ms and, fading out, keeps the
     * layers last drawn in t.arr; gives the layers the fade by steps. */
    void fade(int ti, Target &t, int64_t now_ms);
    uint64_t memory() const;
    void attach(Entry &e, Target &t);
    /* A drawn layer on the target holds its flushes (Layer::holds_flush). */
    bool holds(const Target &t) const;
    void apply_binds(Entry &e);
    void drop_effects(Entry &e);
    /* Every asset the layer samples has pixels; and its buffer holds them. */
    bool assets_have(const Entry &e) const;
    bool assets_clean(const Entry &e) const;

    Gpu *gpu_;
    SlotManager *slots_;
    CompositorLimits limits_;
    Governor governor_;
    crk::UploadPacer pacer_;
    crk::UploadPacer share_;
    int64_t slow_flush_us_ = 150000;
    int64_t file_tick_ms_ = 0;
    int fade_ms_ = 0;
    /* Bytes written this second, flushed or not: the link counts as
     * carrying load (for the governor's evidence and healthy time) from
     * kLoadBytes a second. */
    static const uint64_t kLoadBytes = 64 * 1024;
    static const size_t kMaxBinds = 32;
    uint64_t second_bytes_ = 0;
    int64_t second_ms_ = 0;
    int64_t (*clock_us_)() = nullptr;
    int atlas_fx_ = 0, block_fx_ = 0, image_fx_ = 0;
    std::map<uint32_t, Entry> layers_;
    std::map<uint32_t, AssetEntry> assets_;
    std::map<uint32_t, SceneEntry> scenes_;
    std::vector<Target> targets_;
    uint32_t next_id_ = 1;
    uint64_t seq_ = 0;
    int64_t now_ms_ = 0;
};

}  // namespace crs

#endif
