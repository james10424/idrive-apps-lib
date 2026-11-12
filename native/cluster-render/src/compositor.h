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
 * Videos: each video layer keeps buffers on the display (video_layer.h),
 * whose memory nobody has measured, so the display holds at most
 * CompositorLimits::videos of them, however many video layers the clients
 * have. While the display can hold every video's buffers, each video draws
 * itself, exactly as with no composites at all. Past that, the videos on
 * one target drawn unturned, at their frame's size and at even x and y are
 * drawn through the target's composite (video_composite.h): one video
 * layer composed on the CPU, one set of buffers, one video in the byte
 * budget and the dirty-tile planner; the targets whose composite saves the
 * most buffers compose, as few as it takes. A video turned, scaled or at an
 * odd x or y keeps buffers of its own. A video joining or leaving a
 * composite, and a composite made
 * again, start their scene objects again: their first frame goes whole. A
 * composite is made again for its videos' rectangle growing past it at most
 * every kRemakeMs (till then what lies outside is cut), and smaller only
 * once the smaller rectangle has held for kShrinkMs. A video starts or stops
 * being composited by a change of its params (an odd x, a turn) at most
 * kRemakeMs after its target's last such change; till then it keeps its
 * mode, a composited one drawn unturned at its frame's size and at even x
 * and y. A change is taken only when the videos fit the display both while
 * it is held and once it is real; and reconcile() never changes a buffer
 * for videos that do not fit, whatever let them in. On a target, old objects
 * go before new ones are made, in the same flush. Each target's scene reaches
 * the display with its own flush, so a target whose videos need more buffers
 * keeps them as they are until the other targets' scenes, as flushed, leave
 * room. The display never holds more video buffers than the limit.
 *
 * Hidden tiles: each tick, before a requested target stages, its layers are
 * told from the top down what the layers drawn above them cover opaquely in
 * the next flush (occlusion.h), so each video, and the composite, leaves
 * the tiles wholly under that unwritten (video_layer.h). Only a layer drawn
 * strictly above another in the draw order covers it: of layers at one
 * place in it the renderer chooses the order. A target with no video, or
 * nothing drawn above its lowest, has nothing hidden and no walk.
 *
 * A video whose unwritten tiles a change uncovers (a layer above moved,
 * hidden, turned, made translucent, faded, animated or gone, or the video
 * itself moved or shown) writes them in the flush that carries the change,
 * whatever its frame clock. Its target stages first in the tick, so that
 * write takes the next positive allowance before other targets' writes; so
 * does a target whose alert or exclusive layer waits for its first write
 * (an exclusive layer suspends the layers below it only from the tick
 * after that write went: stack.h).
 *
 * While the budget does not allow it, only that change waits
 * (defer_uncover): the layer it concerns keeps in the scene what the last
 * flush that went showed of it (rect, turn, opacity, drawn, draw order, its
 * animations not started yet), the fade its last flushed step, the
 * composite's sources their quads' places and opacity. The changes wait in
 * units: an exclusive layer coming or going waits with the layers it
 * suspends or resumes, and a layer moved or hidden off a place where a
 * waiting layer is to be drawn waits with it, so no flush shows a state
 * that neither the client's nor the last flush's is. Everything else on the
 * target stages and flushes as usual, and a layer waiting touches the scene
 * only when it is held or released; a held layer's fade applies to its last
 * flushed opacity. The wait is the time the budget takes to repay what it
 * owes, and at most one write of another target that has waited longer:
 * targets with tiles to uncover, or an alert's first write, stage
 * longest-waiting first. A target's wait
 * starts at the first tick what its client set uncovers tiles and lasts,
 * however often the target flushes its other changes meanwhile, until the
 * flush carrying the uncovering write goes. A held layer that becomes a
 * composite's source is held no more: the composite's own deferred moves
 * place it. An image's
 * pixels waiting to be written count for coverage only while the budget
 * allows the image's write, so pixels turning transparent never uncover
 * before they can show.
 *
 * Changes that cannot wait so go at once: a layer removed (with what an
 * exclusive one resumes), an image hidden after a failed write, video
 * objects made again (a composite made or
 * dropped, a video leaving or joining one), and a composited video hidden
 * or put at another place in the draw order (its quads change at once). A
 * video they uncover writes in that flush whatever the budget allows,
 * hiding nothing: with packed blocks the newest frame packed (at most one
 * packed frame, about a third of an exact one, past the budget's debt; the
 * exact atlas comes back with a write the budget allows), without them
 * every changed tile; when none of the tiles it uncovers differs from its
 * newest frame past the threshold, nothing. It then hides no tile until a
 * write the budget
 * allowed has gone (so the debt was repaid), so such writes never come one
 * after the other.
 *
 * A target with no video, or nothing drawn above its lowest one, and
 * nothing hidden, held or waiting, does no occlusion work in a tick beyond
 * a check of what arrange() found.
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
#include "occlusion.h"
#include "scene_edit.h"
#include "scene_file.h"
#include "slots.h"
#include "stack.h"
#include "video_composite.h"
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
    /* Video buffers on the display, each an atlas and two packed buffers:
     * a video drawing itself, or a target's composite of its videos. */
    uint32_t videos = 2;
    /* Client video layers, across all clients; each also within
     * pixel_bytes (four atlases and the packing each, and a composite as
     * much at its own size). */
    uint32_t client_videos = 8;
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
    /* Whether layer l may take params p (before they are set and
     * changed()): a video's display buffers and memory stay within the
     * limits; false with *err. */
    bool check(const Layer *l, const LayerParams &p, std::string *err);
    /* The target's composite of its videos; null while it has none. */
    const CompositeVideo *composite(int target) const;

    void arrange(int64_t now_ms);
    void render(int64_t now_ms);
    int64_t wake_in_ms(int64_t now_ms) const;

    LayerState state(uint32_t id, std::string *reason) const;
    Governor &governor() { return governor_; }
    /* The target's layers whose change waits to uncover a video's tiles
     * (defer_uncover). */
    uint32_t held_layers(int target) const { return targets_[target].held_n; }
    /* Since when the target's uncover has waited; -1: none waits. */
    int64_t uncover_waiting_since(int target) const { return targets_[target].uncover_since; }
    /* The byte budget the targets without the HUD's share write from. */
    const crk::UploadPacer &pacer() const { return pacer_; }
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
    /* A composite is made again for its videos' rect growing past it, or a
     * joining video's settings, at most this often; it is then made over
     * its old rect and the new one together. One whose videos fit in a
     * smaller rect is made again once that has held for kShrinkMs. */
    static const int64_t kRemakeMs = 1000;
    static const int64_t kShrinkMs = 3000;
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
    /* A layer as the scene shows it, for what it covers: drawn, its place
     * in the draw order, its rect, turn and opacity. */
    struct Look {
        bool drawn = false;
        int32_t order = -1;
        Rect rect;
        float opacity = 1, rotate = 0, sx = 1, sy = 1, px = 0, py = 0;
        bool pivot_x = false, pivot_y = false;
        bool animating = false, exclusive = false;
    };
    struct Entry {
        std::unique_ptr<Layer> layer;
        uint64_t owner = 0;
        int effect = 0;        /* a client effect this layer holds */
        size_t pixel_bytes = 0;
        std::string error;     /* the renderer refused the layer's objects */
        bool drawn = false;
        int32_t order = -1;
        bool composited = false;   /* a video drawn by its target's composite */
        bool placed = false;       /* a video the plans have placed (reconcile) */
        bool composable = false;   /* a video's composability, as the last plan took it */
        /* A layer of its own: what its target's last flush that went shows
         * of it. held: the scene keeps showing that while the change the
         * client made (params, drawn, order: always what it set) waits to
         * uncover a video's tiles (defer_uncover); eval_held: occlusion
         * takes it so, this evaluation. */
        Look flushed;
        bool held = false, eval_held = false;
        /* Its look may differ from flushed: in its target's touched list. */
        bool touched = false;
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
        /* Its videos' composite, its pixel bytes, and why the renderer
         * refused its objects. */
        std::unique_ptr<CompositeVideo> composite;
        uint64_t composite_bytes = 0;
        std::string composite_error;
        /* When the composite was made; since when the plan has fitted a
         * smaller one (-1: it does not), and that one's rect; when a kept
         * composite is due to be made again (-1: none is). */
        int64_t composite_made_ms = 0, smaller_since_ms = -1, remake_ms = -1;
        Rect smaller;
        /* When a video last started or stopped being composited; when a
         * held change of a video's composability is due (-1: none is). */
        int64_t members_ms = INT64_MIN / 4, hold_ms = -1;
        /* Video buffers its scene holds as last flushed; video objects it
         * has to make once the display can hold them (may_make_video). */
        uint32_t flushed_videos = 0;
        bool make_waiting = false;
        /* What the layers drawn above a point of its stack cover; the fade
         * its last flush that went carried, and whether this tick's is held
         * back. */
        Coverage coverage;
        float flushed_fade = 1.0f;
        bool fade_held = false, eval_fade_held = false;
        /* From arrange(): its videos (a composite's sources while it is
         * attached), the lowest one's place in the draw order and the
         * highest drawn layer's. */
        int videos = 0;
        int32_t lowest = INT32_MAX, top = INT32_MIN;
        /* quiet: nothing hidden or held on it, no tile to uncover, nothing
         * waiting; while it stays so and nothing is drawn above a video, a
         * tick does no occlusion work. uncover: a video uncovers this tick;
         * since when one has waited (-1: none). decided: defer_uncover ran
         * this tick. held_n: its layers held. */
        bool quiet = true, uncover = false, decided = false, budget_ready = true;
        /* An exclusive layer drawn by its last flush that went has been
         * removed since: what it resumes cannot wait for the budget. */
        bool exclusive_gone = false;
        int64_t uncover_since = -1;
        /* Since when an alert or exclusive layer drawn on it has waited for
         * its first write (-1: none waits). */
        int64_t alert_since = -1;
        uint32_t held_n = 0;
        /* Layers whose look changed since its last flush that went. */
        std::vector<uint32_t> touched;
        /* The occlusion walk's layers above the lowest video, by draw
         * order: kept to save allocating it each tick. */
        std::vector<std::pair<int32_t, uint32_t>> walk;
    };
    struct VideoSpec {
        uint32_t id = 0;
        int target = 0;
        LayerParams params;
        VideoParams vp;
        /* Composable as the plan takes it; held: its change waits. */
        bool composable = false, held = false;
    };
    /* How a target's videos are drawn: those its composite draws (none, or
     * two or more), its canvas and place, and the video buffers they take
     * on the display. */
    struct VideoPlan {
        std::vector<uint32_t> composited;
        VideoParams vp;
        int32_t x = 0, y = 0;
        Rect clip;
        uint64_t bytes = 0;
        uint32_t buffers = 0;
        const char *error = nullptr;
    };

    uint32_t admit(uint64_t owner, int target, int64_t pixel_bytes, std::string *err);
    /* real: every video's own composability, no hold (as once the holds
     * are over). */
    std::vector<VideoSpec> video_specs(bool real = false) const;
    /* Whether specs fit the display's video buffers both as held and once
     * the holds are over (real); *bytes: the larger cost. */
    bool videos_fit_both(const std::vector<VideoSpec> &held, const std::vector<VideoSpec> &real, uint64_t *bytes,
                         std::string *err);
    /* A placed video's composability changing with its params waits
     * kRemakeMs from its target's last membership change: the plan takes
     * the old one meanwhile (*held). */
    bool composable_now(const Entry &e, const LayerParams &p, const VideoParams &vp, bool *held) const;
    /* The plans' composites' pixel bytes at the most each target's holds
     * while they take effect: its plan's or its composite's, kept. */
    uint64_t composites_cost(const std::vector<VideoPlan> &plans) const;
    /* The target's videos composed: its plan when it composes (none for
     * fewer than two composable videos). */
    VideoPlan compose_plan(int ti, const std::vector<VideoSpec> &specs, const Rect *with = nullptr) const;
    /* Every target's plan: every video separate while the display holds
     * them all (CompositorLimits::videos); past it, the targets whose
     * composite saves the most buffers compose, as few as it takes. *err:
     * why they do not fit even so; empty when they do. */
    std::vector<VideoPlan> plan_videos(const std::vector<VideoSpec> &specs, std::string *err) const;
    /* Whether the videos fit the display's video buffers; *bytes: their
     * composites' pixel bytes (composites_cost). Compiles the composite's
     * effects when one is needed. */
    bool videos_fit(const std::vector<VideoSpec> &specs, uint64_t *bytes, std::string *err);
    bool view_effects(std::string *err);
    /* Brings every target's videos to its plan: which draw themselves and
     * which its composite draws; what goes, on every target, before what
     * is made. */
    void reconcile();
    /* Whether the target's composite stays for plan pl (made again at once,
     * or once allowed: kRemakeMs, kShrinkMs); *pl becomes the plan it is
     * made with when it does not. */
    bool keep_composite(int ti, VideoPlan *pl);
    void reconcile_drop(int ti, VideoPlan &pl);
    void reconcile_make(int ti, const VideoPlan &pl);
    void attach_composite(Target &t);
    /* Video buffers in the target's scene: its videos drawing themselves
     * and its composite. */
    uint32_t video_buffers(int ti) const;
    /* What the target's scene holds for the display's video buffers: for
     * a target render() flushes, its scene as flushed or as pending (plus
     * extra), whichever holds more; else (released, parked, unpublished)
     * as pending. */
    uint32_t video_hold(int ti, uint32_t pending) const;
    /* Each target's scene reaches the display with its own flush, in any
     * order: one more video buffer on target ti is made only while every
     * target's video_hold stays within CompositorLimits::videos together. */
    bool may_make_video(int ti) const;
    /* The layer is drawn in its target's scene: attached, or composited by
     * an attached composite. */
    bool on_scene(const Entry &e) const;
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
    /* Tells the target's layers and composite, top first, what is drawn
     * opaquely above each in the next flush. */
    void occlude(int ti, Target &t);
    /* Whether a tick of the target has occlusion work: something hidden,
     * held or waiting, or a layer drawn above a video. */
    bool occluding(const Target &t) const;
    /* An alert or exclusive layer drawn on the target has no pixels on the
     * display yet. */
    bool alert_waits(int ti) const;
    /* A video on the target is uncovering (Layer::uncovering). */
    bool uncovers(const Target &t) const;
    /* One target's stage and flush (render()). */
    void render_target(int ti, Target &t, int64_t now_ms);
    /* The budget does not allow the uncovering write: chooses the changes
     * that wait (eval_held, eval_fade_held, the composite's hold), as few as
     * keep every held tile covered, in units that never show what the
     * client's state and the last flush's would not. False when holding
     * every change does not keep them covered: the uncovering videos then
     * write now (VideoLayer::force). */
    bool defer_uncover(int ti, Target &t);
    /* Occlusion with the changes chosen; whether a video uncovers. */
    bool evaluate(int ti, Target &t);
    /* Brings the scene to the changes chosen: a layer held shows its last
     * flushed look, one released what the client set (and its waiting
     * animations start); the same for the fade and the composite's quads.
     * Only what differs touches the scene. */
    void commit(int ti, Target &t);
    /* After the target's flush (ok: it went): what it shows, taken. */
    void settle(int ti, Target &t, bool ok);
    /* What the client set for a layer, as the scene would show it. */
    static Look look_of(const Entry &e);
    static bool same(const Look &a, const Look &b);
    Shown shown_of(const Entry &e, const Target &t) const;
    /* The layer's look changed: its flushed look is taken at the next
     * flush that goes. */
    void mark(Entry &e, uint32_t id, Target &t);
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
    int atlas_view_fx_ = 0, block_view_fx_ = 0;   /* the composite's, made with the first one */
    std::string plan_error_;   /* why the last reconcile changed nothing; logged once */
    std::map<uint32_t, Entry> layers_;
    std::vector<int> render_order_, render_later_;   /* kept to save allocating them each tick */
    std::map<uint32_t, AssetEntry> assets_;
    std::map<uint32_t, SceneEntry> scenes_;
    std::vector<Target> targets_;
    uint32_t next_id_ = 1;
    uint64_t seq_ = 0;
    int64_t now_ms_ = 0;
};

}  // namespace crs

#endif
