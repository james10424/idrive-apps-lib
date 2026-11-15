// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of the map feed (map_feed.h) against the shared cluster renderer
 * (cluster-render, the CR tree) on fake display backends, over its socket:
 * - the map is a contending content layer on the map target, covering the
 *   1088x480 slot; before a frame the app sees idle, with the first frames
 *   the slot is asked for (starting) and the map shown with the frames as
 *   they were decoded;
 * - the stock map taking the slot shows as starting with that reason; the
 *   contending map asks again, parks behind it, takes it over and is shown,
 *   the stock map parked behind it;
 * - a control start moves it to the whole panel, centred at 416,120, and an
 *   unknown target is refused;
 * - frames stopping: idle after stall_ms, the slot released;
 * - the renderer restarting: starting while it is gone, an error once it
 *   stays gone 15 s, then the map again with the next frames;
 * - a lowered shared upload cap shows on the status line, with its reason in
 *   the status, and leaves the line once it is back at the ceiling;
 * - frames offered faster than they can be written are newest-wins;
 * - the stream not live: the layer hidden, idle, the slot released and the
 *   stock map back; live again: shown with the frames; frozen: hidden, the
 *   status says so; no longer frozen: shown again;
 * - before the stream is live the layer is made hidden;
 * - the stream's pictures through the change sampler and freeze watch, as
 *   the service runs them: a stream blank from its start (the car's burst,
 *   then a pause) never asks for the slot, the stock map stays and the app
 *   is told it waits for the phone's map; the first non-blank frame, with no frame
 *   after it, takes the slot over and shows that frame; blank spells
 *   shorter than blank_ms between pictures keep the map up without asking
 *   for the slot again; a picture blank for blank_ms gives the slot back to
 *   the stock map, and the next picture takes it again; a stream blank at
 *   full rate from its start is frozen, the stock map showing; live
 *   through a recent frame (no connection to read), a static map's frame
 *   every 10 s takes the slot over each time it makes the stream live;
 *   a frame of the ended stream decoded after it ended, seconds before the
 *   next stream is live, does not take the slot for it, while one decoded
 *   just before it is live does; the stream ending is logged once, whatever
 *   frames still come;
 * - the status is logged when its state, reason or target changes, not for
 *   the frame rate, the page or the upload cap;
 * - BMW's turn box (the layer's guide_hold): hidden by default, the hold of
 *   its slot following the map; the switch off releases it and the box
 *   shows, the map still showing; on again, held; off from the start,
 *   never held; the status never says the hold is disabled;
 * - only what the renderer needs goes: a picture repeated (as the producer
 *   hands over a picture the same as the last) keeps a static map showing
 *   past stall_ms; nothing reaches the renderer while the layer is hidden,
 *   and shown again it draws the newest picture first; after the renderer
 *   restarts, and on another target, repeats alone bring the picture;
 * - MapFeeds: the HUD off, on beside the cluster (scaled, at most hud_fps),
 *   beside the cluster's map on panel, its size and place, kept shown over
 *   BMW's pop-ups or giving way to them, alone, neither; a still map's repeats keep
 *   both layers, or the HUD alone, shown past stall_ms with no frame whole
 *   and none scaled, and moving again the HUD scales at most hud_fps while
 *   the cluster keeps its rate; a feed made beside the other starts from its
 *   picture;
 * - Off (neither), from the cluster alone and from both: the map's and the
 *   HUD's slots and the turn box's hold released, the stock map and box
 *   shown again, nothing asked of the renderer while off; the cluster, or
 *   both, again: the next frame shown, the hold back;
 * - hosted by another app's scene (hosting): the map in its scene at the
 *   host's rect with the host's part of the picture, its own slot released;
 *   hidden by the host, no layer and nothing sent; shown again; the host
 *   gone, back on its own target. */
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

/* cluster-render's service and fakes, from its tree (-I$(CR)) */
#include "src/service.h"
#include "test/fake_graph.h"
#include "test/fake_port.h"
#include "../src/map_feed.h"
#include "src/log.h"
#include "../src/map_feeds.h"
#include "../src/atlas_scale.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static int rigs;
static const uint64_t kBox = 0x01000310, kGuide = 0x010004f1;

struct Rig {
    std::string path = "/tmp/woc-feed-test-" + std::to_string(getpid()) + "-" + std::to_string(++rigs) + ".sock";
    crs::FakeGpu gpu;
    crs::FakePort port;
    crs::ServiceConfig sc;
    std::unique_ptr<crs::Service> svc;
    std::unique_ptr<MapFeed> feed;
    int64_t now = 200000;
    std::vector<uint8_t> spare;   /* the buffer handed to the feed, as the producer's */
    /* watched: the frames' pictures drive the stream state, as in the
     * service's loop; otherwise the stream is live with a picture */
    bool watched;
    /* watched and > 0: live while a frame came within this long, as the
     * service decides without the stream's connection; 0: live while live
     * is set, as the stream's connection says */
    int64_t live_stall_ms = 0;
    bool live = true;
    /* frames the same as the last offered go as repeats, as the producer
     * hands them over */
    bool repeats = false;
    std::vector<uint8_t> offered;
    int64_t last_frame_ms = -1;
    FreezeWatch watch{FreezeConfig()};
    ChangeSampler sampler{1088, 480};
    cr::Client *other = nullptr;   /* another app of the renderer, polled with the feed */

    /* w: watched (above); hold: with the guide hold of the turn box's slot
     * following the map, as cluster-render.conf ships it; guide_hold: the
     * feed's setting; hud: the HUD target too. */
    explicit Rig(bool w = false, unsigned stall_ms = 6000, bool hold = false, bool guide_hold = true, bool hud = false)
        : watched(w)
    {
        crs::TargetSpec m, p, g, u;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;   /* as cluster-render.conf ships it */
        p.name = "panel"; p.slot = 0x203; p.scene_id = 0x010004f8; p.width = 1920; p.height = 720; p.linger_ms = 3000;
        g.name = "guide_0x105"; g.slot = 0x105; g.scene_id = kGuide; g.width = g.height = 16; g.linger_ms = 0;
        g.take_over_stock = true; g.follows = "map"; g.take_over_contents = {kBox};
        u.name = "hud"; u.slot = 0x323; u.scene_id = 0x01000400; u.width = 800; u.height = 480; u.linger_ms = 3000;
        u.yield_slots = {0x322};   /* as cluster-render.conf ships it */
        u.yield_ms = 1500;
        sc.targets = {m, p};
        if (hud)
            sc.targets.push_back(u);
        if (hold)
            sc.targets.push_back(g);
        sc.socket_path = path;
        sc.backend = "fake";
        start();
        FeedConfig fc;
        fc.socket = path;
        fc.video.level_bits = 0;
        fc.video.stall_ms = stall_ms;
        fc.guide_hold = guide_hold;
        feed.reset(new MapFeed(fc));
        watch.live(true, now);
        feed->set_stream(true, false, watched ? watch.picture(now) : Picture::Shown);
    }
    void start()
    {
        svc.reset(new crs::Service(&gpu, &port, sc));
        port.m = &svc->slots();
        port.pending.clear();
        std::string err;
        if (!svc->start(&err))
            std::printf("start: %s\n", err.c_str());
    }
    /* Simulated time; each step runs the socket exchange until idle. */
    void pump(int64_t ms, const std::vector<uint8_t> *frame = nullptr, int64_t frame_every = 65)
    {
        for (int64_t t = 0; t < ms; t += 5) {
            now += 5;
            if (frame && t % frame_every == 0) {
                if (repeats && offered == *frame) {
                    feed->repeat((uint64_t)now, now);
                } else {
                    spare.assign(frame->begin(), frame->end());
                    feed->offer(&spare, (uint64_t)now, now);
                    offered = *frame;
                }
                if (watched) {
                    const bool changed = sampler.changed(frame->data());
                    watch.frame(now, changed, sampler.blank());
                    last_frame_ms = now;
                }
            }
            if (watched) {
                const bool on = live_stall_ms ? last_frame_ms >= 0 && now - last_frame_ms <= live_stall_ms : live;
                watch.live(on, now);
                feed->set_stream(on, watch.frozen(now), watch.picture(now));
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                size_t first = 0;
                if (svc)
                    first = svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                if (svc) {
                    port.tick(now);
                    svc->after_poll(fds, first, now);
                }
                const bool pending = feed->wants_write() || (other && other->wants_write());
                feed->poll(now);
                if (other)
                    other->poll(now);
                if (round > 0 && n <= 0 && !pending)
                    break;
            }
        }
    }
    FeedStatus st() { return feed->status("decode"); }
    /* The map status of content in the turn box's slot; -1: not mapped. */
    int in_box_slot(uint64_t content) const
    {
        const crs::SlotInfo *s = port.row(0x105, content);
        return s ? (int)s->map_status : -1;
    }
    const crs::Layer *map_layer()
    {
        for (const crs::Layer *l : svc->compositor().layers())
            return l;
        return nullptr;
    }
    /* The map layer's frames the renderer has counted: written, unchanged,
     * within the threshold, superseded. */
    unsigned long long received()
    {
        unsigned long long n[4] = {};
        if (const crs::Layer *l = map_layer())
            std::sscanf(l->stats().c_str(), "%llu sent, %llu unchanged, %llu within threshold, %llu superseded", &n[0],
                        &n[1], &n[2], &n[3]);
        return n[0] + n[1] + n[2] + n[3];
    }
    /* Whether the target's scene draws the map, and with this atlas. */
    bool drawing(const char *target, const std::vector<uint8_t> &f, bool *with)
    {
        crs::FakeGraph *g = gpu.graph(target);
        *with = false;
        if (!g || g->drawn().empty())
            return false;
        for (const auto &kv : g->shown.buffers)
            *with = *with || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f);
        return true;
    }
};

static std::vector<uint8_t> atlas(int seed)
{
    std::vector<uint8_t> f(1088 * 720);
    for (size_t i = 0; i < f.size(); i++)
        f[i] = (uint8_t)(i < 1088 * 480 ? (i / 5 + seed * 17) & 0xff : 128);
    return f;
}

/* The app's switch for BMW's turn box: on by default, the hold following
 * the map and the box taken over; off, the hold released and the box
 * shown, the map still showing; on again, held. */
static void guide_hold()
{
    const std::vector<uint8_t> f = atlas(7);
    Rig h(false, 6000, true);
    h.port.stock_map(0x105, kBox);
    h.pump(1500, &f);
    ok("turn box: hidden by default, the hold shown in its slot with the box parked behind it",
       h.st().state == "showing" && h.st().guide_hold && h.in_box_slot(kGuide) == 1 && h.in_box_slot(kBox) == 3);
    h.feed->set_guide_hold(false);
    h.pump(500, &f);
    const size_t taken = h.port.taken_over.size();
    ok("turn box: the switch off releases the hold, the box shown, the map still showing, the status says so",
       h.in_box_slot(kGuide) == -1 && h.in_box_slot(kBox) == 1 && h.st().state == "showing" && !h.st().guide_hold);
    h.pump(5000, &f);
    ok("turn box: off, no take-over and no hold", h.port.taken_over.size() == taken && h.in_box_slot(kGuide) == -1);
    h.feed->set_guide_hold(true);
    h.pump(500, &f);
    ok("turn box: on again, held, the box taken over",
       h.in_box_slot(kGuide) == 1 && h.in_box_slot(kBox) == 3 && h.port.taken_over.back() == kBox);

    Rig off(false, 6000, true, false);
    off.port.stock_map(0x105, kBox);
    off.pump(3000, &f);
    ok("turn box: set off before the map is made, never held",
       off.st().state == "showing" && off.in_box_slot(kGuide) == -1 && off.in_box_slot(kBox) == 1);
    ok("turn box: the hold not disabled, the status says so", !h.st().guide_disabled);

}

/* The HUD's worker is still scaling a frame: the feed then asks to be woken
 * in kScaleWaitMs however much later it is asked, while a frame due in
 * kScaleWaitMs asks for a millisecond less a millisecond later. */
static bool hud_scaling(const MapFeeds &feeds, int64_t now)
{
    return feeds.wake_in_ms(now) == MapFeed::kScaleWaitMs && feeds.wake_in_ms(now + 1) == MapFeed::kScaleWaitMs;
}

/* The app's Off (MapFeeds on neither), as the app sends it: from the cluster
 * alone the cluster off; from both each off. Off releases the map's slot and
 * the turn box's hold, so the stock map and box show again, and the HUD's
 * slot; nothing is asked of the renderer while off. A display chosen again
 * shows the next frame, the hold back with it. */
static void show_off()
{
    const std::vector<uint8_t> f1 = atlas(11), f2 = atlas(12), f3 = atlas(13);
    const uint64_t kStock = 0x01000101, kMap = 0x010004f0, kHud = 0x01000400;
    Rig b(false, 6000, true, true, true);
    b.feed.reset();
    b.start();
    b.port.stock_map(0x100, kStock);
    b.port.stock_map(0x105, kBox);
    FeedConfig fc;
    fc.socket = b.path;
    fc.video.level_bits = 0;
    fc.guide_hold = true;
    MapFeeds feeds(fc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> spare;
    bool asked = false;
    const auto pump = [&](int64_t ms, const std::vector<uint8_t> *frame) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0 && feeds.wants_frames()) {   /* the service offers only then */
                spare.assign(frame->begin(), frame->end());
                spare[(size_t)(t / 65) % 1088] ^= 0x80;
                feeds.offer(&spare, (uint64_t)b.now, b.now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                for (int i = 0; i < 4; i++)
                    asked = asked || b.svc->slots().requested(i);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    const auto status_of = [&](uint64_t slot, uint64_t content) -> int {
        const crs::SlotInfo *r = b.port.row(slot, content);
        return r ? (int)r->map_status : -1;
    };
    const auto layers = [&]() { return b.svc->compositor().layers().size(); };
    const auto shown = [&](const char *target, const std::vector<uint8_t> &like) {
        crs::FakeGraph *g = b.gpu.graph(target);
        bool on = false;
        if (g)
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() == like.size() &&
                            std::equal(kv.second.px.begin() + 1088, kv.second.px.end(), like.begin() + 1088));
        return on;
    };
    const auto map_up = [&]() {
        return status_of(0x100, kMap) == 1 && status_of(0x100, kStock) == 3 && status_of(0x105, kGuide) == 1 &&
               status_of(0x105, kBox) == 3;
    };
    const auto stock_back = [&]() {
        return status_of(0x100, kMap) == -1 && status_of(0x100, kStock) == 1 && status_of(0x105, kGuide) == -1 &&
               status_of(0x105, kBox) == 1 && status_of(0x323, kHud) == -1;
    };

    pump(2000, &f1);
    ok("Off: the cluster's map up first, the stock map and box parked behind it and the hold",
       map_up() && shown("map", f1) && feeds.status("decode").state == "showing");

    /* Off from the cluster alone: the app sends the cluster off */
    feeds.set_cluster(false);
    pump(5000, &f2);
    FeedStatus st = feeds.status("decode");
    ok("Off from the cluster: no feed or layer, nothing wanted, the map's slot and the hold released, the stock map and box back",
       !feeds.cluster() && !feeds.hud_feed() && layers() == 0 && !feeds.wants_frames() && stock_back() && st.state == "off" &&
           st.hud_state == "off" && !st.cluster && !st.hud);
    const int requests = b.port.requests;
    asked = false;
    pump(10000, &f2);
    ok("Off: nothing asked of the renderer for 10 s, no layer, the stock map and box still shown",
       b.port.requests == requests && !asked && layers() == 0 && stock_back());

    /* Cluster again: the next frame, the hold back */
    feeds.set_cluster(true);
    pump(2000, &f3);
    ok("Off then Cluster: the map with the next frame, the stock map and box parked again, the hold back",
       map_up() && shown("map", f3) && feeds.status("decode").state == "showing");

    /* Off from both: the app sends the cluster off, then the HUD off */
    feeds.set_hud(true);
    pump(2000, &f1);
    ok("Off: both up first, the HUD's scene in its slot", map_up() && status_of(0x323, kHud) == 1 &&
                                                         feeds.status("decode").hud_state == "showing");
    feeds.set_cluster(false);
    feeds.set_hud(false);
    pump(5000, &f2);
    st = feeds.status("decode");
    ok("Off from both: no feed or layer, the map's and the HUD's slots and the hold released, the stock map and box back",
       !feeds.cluster() && !feeds.hud_feed() && layers() == 0 && !feeds.wants_frames() && stock_back() && st.state == "off" &&
           st.hud_state == "off");
    feeds.set_hud(true);
    feeds.set_cluster(true);
    pump(2000, &f3);
    ok("Off then both: both back with the next frame", map_up() && status_of(0x323, kHud) == 1 && shown("map", f3) &&
                                                         feeds.status("decode").hud_state == "showing");
}

/* Only what the renderer needs goes over its socket. */
static void only_what_is_needed()
{
    const std::vector<uint8_t> f1 = atlas(1), f2 = atlas(2);
    Rig r(false, 6000);
    r.repeats = true;
    r.pump(1500, &f1);
    bool drawn = false;
    r.drawing("map", f1, &drawn);
    ok("repeats: the first frame whole, shown", r.st().state == "showing" && drawn);
    const unsigned long long first = r.received();
    bool steady = true;
    for (int s = 0; s < 10; s++) {
        r.pump(1000, &f1);
        steady = steady && r.st().state == "showing";
    }
    ok("repeats: a static map's repeated picture keeps it showing past stall_ms, each repeat counted",
       steady && r.map_layer()->params.visible && r.received() >= first + 150);
    int fps = 0;
    std::sscanf(r.st().page.c_str(), "Waze map \xe2\x80\x94 %d frames/s", &fps);
    ok("repeats: the frame rate counts them, a frame each 65 ms", fps == 15 || fps == 16);

    r.feed->set_stream(false, false, Picture::Shown);
    r.pump(100, &f1);
    const unsigned long long hidden = r.received();
    r.pump(3000, &f2);
    ok("hidden: nothing reaches the renderer, frames or repeats", r.received() == hidden && r.st().state == "idle");
    r.feed->set_stream(true, false, Picture::Shown);
    bool first_drawn = false, newest_first = false;
    for (int step = 0; step < 300 && !first_drawn; step++) {
        r.pump(5);
        first_drawn = r.drawing("map", f2, &newest_first);
    }
    ok("shown again: the newest picture is what it draws first", first_drawn && newest_first);
    r.pump(1500, &f2);
    ok("shown again: showing", r.st().state == "showing");

    r.svc.reset();
    r.pump(100, &f2);
    r.start();
    r.pump(3000, &f2);
    r.drawing("map", f2, &drawn);
    ok("the renderer restarted: repeats alone bring the picture back", r.st().state == "showing" && drawn);

    ok("another target", r.feed->set_target("panel"));
    r.pump(1500, &f2);
    r.drawing("panel", f2, &drawn);
    ok("another target: repeats alone bring the picture", r.st().state == "showing" && r.st().target == "panel" && drawn);
}

/* The w x h part at x, y of a 1088x480 atlas, as an atlas of its own. */
static std::vector<uint8_t> crop_of(const std::vector<uint8_t> &f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> c((size_t)w * (h + h / 2));
    for (uint32_t r = 0; r < h; r++)
        std::copy_n(&f[(size_t)(y + r) * 1088 + x], w, &c[(size_t)r * w]);
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t p = 0; p < 2; p++)
            std::copy_n(&f[(size_t)(480 + y / 2 + r) * 1088 + p * 544 + x / 2], w / 2, &c[(size_t)(h + r) * w + p * w / 2]);
    return c;
}

/* A scene with layers on the panel hosting the map (cluster-render
 * PROTOCOL.md "host"): the map's layer moves into it, at the host's rect,
 * its frames the host's part of the picture, the map's own slot released;
 * hidden by the host, no layer at all and nothing sent, idle with the
 * reason; shown again, the newest picture's part; the host gone, the map on
 * its own target again. */
static void hosting()
{
    char tmpl[] = "/tmp/woc-hosting-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    const std::vector<uint8_t> f = atlas(3), f2 = atlas(4);
    Rig r;
    r.sc.targets[1].scene_files = true;
    r.sc.scene_files.root = root;
    r.sc.scene_files.owner = getuid();
    r.start();
    r.pump(2000, &f);
    ok("hosting: the map shown on its own target", r.st().state == "showing" && r.st().target == "map" && !r.st().hosted);

    cr::Client design("design", r.path);
    r.other = &design;
    cr::SceneOptions so;
    so.target = "panel";
    so.file = file;
    so.sha256 = {crs::sha256_file(file)};
    so.layers = true;
    const uint32_t sc = design.scene(so);
    r.pump(500, &f);
    cr::Host h;
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    design.host(sc, h);
    r.pump(2500, &f);
    bool with = false;
    const int map_i = r.svc->slots().index("map");
    ok("hosted: the map in the panel's scene over the file, its frames the host's 560x400 part, the map's slot released",
       r.st().state == "showing" && r.st().target == "panel" && r.st().hosted && r.drawing("panel", crop_of(f, 264, 40, 560, 400), &with) &&
           with && r.gpu.graph("panel")->over_file && r.svc->compositor().layers().size() == 1 &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);

    h.shown = false;
    design.host(sc, h);
    r.pump(1000, &f2);
    ok("hidden by the host: no layer, nothing sent, idle with the reason",
       r.svc->compositor().layers().empty() && r.st().state == "idle" && r.st().reason == MapFeed::kHostHidden &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);
    h.shown = true;
    design.host(sc, h);
    r.pump(1500);
    ok("shown again: the newest picture's part first", r.st().state == "showing" &&
                                                         r.drawing("panel", crop_of(f2, 264, 40, 560, 400), &with) && with);

    design.drop(sc);
    r.pump(2000, &f2);
    ok("the host gone: the map on its own target again",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f2, &with) && with);

    /* A map smaller than the host's src (map_rect: half size, centred):
     * the host stretches src over its rect, so the map lands as that much
     * of the rect, at its place in src, not over the whole rect. */
    FeedConfig fc;
    fc.socket = r.path;
    fc.video.level_bits = 0;
    fc.rect = MapRect{272, 120, 544, 240};
    r.feed.reset(new MapFeed(fc));
    r.feed->set_stream(true, false, Picture::Shown);
    const uint32_t sc2 = design.scene(so);
    r.pump(500, &f);
    cr::Host whole;
    whole.target = "map";
    whole.x = 700; whole.y = 160; whole.w = 544; whole.h = 240;   /* the map target, half size, no src: all of it */
    design.host(sc2, whole);
    r.pump(2500, &f);
    const crs::Layer *placed = r.map_layer();
    ok("a map smaller than the host's src: the half-size map at its place in the rect (836,220 272x120), "
       "its frame scaled to that",
       r.st().hosted && placed && placed->params.rect.x == 836 && placed->params.rect.y == 220 &&
           placed->params.rect.w == 272 && placed->params.rect.h == 120 &&
           r.drawing("panel", f, &with));
    design.drop(sc2);
    r.pump(500, &f);
    r.other = nullptr;
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IONBF, 0);
    hosting();
    Rig r;
    r.pump(100);
    const crs::Layer *l = r.map_layer();
    ok("connected: one contending content layer on the map target, covering the 1088x480 slot",
       l && l->target() == 0 && l->params.contend && l->params.klass == crs::Klass::Content && l->params.rect.x == 0 &&
           l->params.rect.y == 0 && l->params.rect.w == 1088 && l->params.rect.h == 480);
    ok("no frame yet: idle, and the slot is not asked for", r.st().state == "idle" && r.port.requests == 0);

    const std::vector<uint8_t> f1 = atlas(1);
    r.pump(20, &f1);
    ok("the first frame asks for the slot: starting", r.st().state == "starting" && r.port.requests == 1);
    r.pump(1100, &f1);
    crs::FakeGraph *g = r.gpu.graph("map");
    bool drawn = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            drawn = drawn || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f1);
    ok("granted: showing, with the decoded frame in the slot's scene",
       r.st().state == "showing" && drawn && r.st().page.find("Waze map — ") == 0);
    ok("the status line counts the frames handed over", r.st().page.find("15 frames/s") != std::string::npos);
    const std::string lost = "Stream lost";   /* until the layer stalls */
    ok("showing carries the stream's reason", r.feed->status("decode", lost).state == "showing" &&
                                                  r.feed->status("decode", lost).reason == lost &&
                                                  r.feed->status("decode", lost).stream_reason);

    r.port.steal(r.sc.targets[0]);
    r.pump(20, &f1);
    ok("the stock map takes the slot: starting, with that reason",
       r.st().state == "starting" && r.st().reason == "the stock map holds the slot");
    r.pump(2200, &f1);
    const crs::SlotInfo *ours = r.port.row(0x100, 0x010004f0), *stock = r.port.row(0x100, 0x01000101);
    ok("the map contends: it takes the stock map over and is shown, the stock map parked behind it",
       r.st().state == "showing" && r.port.requests == 2 && r.port.taken_over.size() == 1 &&
           r.port.taken_over[0] == 0x01000101 && ours && ours->map_status == 1 && stock && stock->map_status == 3);

    /* Many frames between two writes: only the newest is drawn. */
    const std::vector<uint8_t> f2 = atlas(2), f3 = atlas(3);
    std::vector<uint8_t> b2 = f2, b3 = f3;
    r.feed->offer(&b2, 2, r.now);
    r.feed->offer(&b3, 3, r.now);
    ok("a frame is taken without a copy: an earlier frame's buffer comes back for each, the last sent kept",
       b2.size() == f2.size() && b3.size() == f3.size() && b2 == f1 && b3 == f1);
    r.pump(200);
    g = r.gpu.graph("map");
    bool newest = false, stale = false;
    for (const auto &kv : g->shown.buffers)
        if (kv.second.f == crs::TexFormat::R8) {
            newest = newest || kv.second.px == f3;
            stale = stale || kv.second.px == f2;
        }
    ok("frames faster than the writes: the newest is drawn", newest && !stale);

    ok("an unknown target is refused", !r.feed->set_target("cockpit"));
    ok("a control start moves the map to the whole panel", r.feed->set_target("panel"));
    r.pump(1200, &f3);
    l = r.map_layer();
    ok("centred in the 1920x720 panel at 416,120, showing",
       l && l->target() == 1 && l->params.rect.x == 416 && l->params.rect.y == 120 && l->params.rect.w == 1088 &&
           r.st().state == "showing" && r.st().target == "panel");
    r.pump(1600);
    ok("the map target, no longer wanted, is released after its linger",
       r.port.releases >= 1 && !r.port.row(0x100, 0x010004f0));
    r.feed->set_target("sail");
    r.pump(1200, &f3);
    ok("sail is the map slot", r.map_layer() && r.map_layer()->target() == 0 && r.st().state == "showing");

    const int releases = r.port.releases;
    r.pump(6100);
    ok("frames stop: idle after stall_ms", r.st().state == "idle");
    const std::string waiting = "Stream lost";
    ok("idle while the stream is live carries the stream's reason", r.feed->status("decode", waiting).reason == waiting);
    r.pump(2000);   /* the fade out, then the linger */
    ok("and the slot is released after the linger",
       r.port.releases > releases && !r.port.row(0x100, 0x010004f0));

    r.svc.reset();
    r.pump(100);
    ok("the renderer gone: starting while it may be starting", r.st().state == "starting" &&
                                                               r.st().reason == "waiting for the cluster renderer");
    r.pump(15000);
    ok("still gone 15 s on: an error the app shows", r.st().state == "error" &&
                                                     r.st().reason == "cluster renderer not reachable");
    r.start();
    r.pump(3000, &f1);
    ok("it comes back: the map layer made again, showing with the next frames",
       r.map_layer() && r.st().state == "showing");

    /* the governor counts evidence seen while writes go */
    r.svc->compositor().governor().activity(r.now);
    r.svc->compositor().governor().evidence(crs::Evidence::LinkStall, r.now);
    r.svc->compositor().governor().evidence(crs::Evidence::LinkStall, r.now);
    r.pump(100, &f1);
    ok("a lowered shared upload cap shows on the status line",
       r.st().page.find("upload cap 5000 KB/s") != std::string::npos);
    ok("the status carries the cap and why",
       r.st().upload_cap_kb == 5000 && r.st().upload_reason.find("lowered from 5500 KB/s") == 0 &&
           r.st().upload_reason.find("heartbeat replies silent") != std::string::npos);
    /* A minute of healthy upload raises it back to the ceiling, which the
     * status line leaves out. */
    for (int s = 0; s < 62; s++) {
        r.svc->compositor().governor().activity(r.now);
        r.pump(1000, &f1);
    }
    ok("back at the ceiling after healthy upload: the cap leaves the status line, its reason stays",
       r.st().upload_cap_kb == 5500 && r.st().page.find("upload cap") == std::string::npos &&
           r.st().upload_reason.find("raised from 5000 KB/s") == 0);
    /* the stream's liveness drives the layer, whatever the frames do */
    const int before = r.port.releases;
    r.feed->set_stream(false, false, Picture::Shown);
    r.pump(2000, &f1);   /* the fade out, then the linger */
    stock = r.port.row(0x100, 0x01000101);
    ok("the stream not live: the layer hidden, idle, and the slot released, the parked stock map shown",
       r.map_layer() && !r.map_layer()->params.visible && r.st().state == "idle" && r.port.releases > before &&
           !r.port.row(0x100, 0x010004f0) && stock && stock->map_status == 1);
    ok("and no stream reason", r.feed->status("decode", waiting).reason.empty());
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(1200, &f1);
    ok("live again: visible and showing with the frames", r.map_layer()->params.visible && r.st().state == "showing");
    r.feed->set_stream(true, true, Picture::Blank);
    r.pump(100, &f1);
    ok("frozen: hidden, and the status says why",
       !r.map_layer()->params.visible && r.st().state == "frozen" && r.st().reason == "the phone's cluster stream froze");
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(1200, &f1);
    ok("no longer frozen: shown again", r.map_layer()->params.visible && r.st().state == "showing");

    Rig h;
    h.feed->set_stream(false, false, Picture::None);
    h.svc.reset();
    h.start();
    h.pump(3000, &f1);
    ok("a layer made while the stream is not live is hidden: idle, the slot not asked for",
       h.map_layer() && !h.map_layer()->params.visible && h.st().state == "idle" && h.port.requests == 0);

    /* The stream's own pictures, from a stream start with the stock map up. */
    std::vector<uint8_t> black(1088 * 720, 128);
    std::fill(black.begin(), black.begin() + 1088 * 480, 16);
    Rig b(true, 60000);
    b.port.stock_map(0x100, 0x01000101);
    b.pump(100);
    ok("blank start: live, no frame yet: idle, no reason, the slot not asked for",
       b.st().state == "idle" && b.st().reason.empty() && b.port.requests == 0);
    b.pump(2000, &black, 20);   /* the car's start: about 100 blank frames in 2 s */
    b.pump(60000);              /* then a pause */
    const crs::SlotInfo *stock_b = b.port.row(0x100, 0x01000101);
    ok("blank start: the burst and a minute's pause never ask for the slot; the stock map stays",
       b.port.requests == 0 && !b.port.row(0x100, 0x010004f0) && stock_b && stock_b->map_status == 1 &&
           b.map_layer() && !b.map_layer()->params.visible);
    ok("blank start: idle, and the app told it waits for the phone's map",
       b.st().state == "idle" && b.st().reason == MapFeed::kWaiting && !b.st().stream_reason &&
           b.st().reason == "Waiting for the phone's map");
    ok("blank start: a lost stream's reason comes first",
       b.feed->status("decode", lost).reason == lost && b.feed->status("decode", lost).stream_reason);
    b.pump(5, &f1);   /* one non-blank frame, then nothing */
    b.pump(1500);
    g = b.gpu.graph("map");
    drawn = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            drawn = drawn || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f1);
    stock_b = b.port.row(0x100, 0x01000101);
    ok("first picture: the slot asked for once, the stock map taken over, that frame shown",
       b.st().state == "showing" && drawn && b.port.requests == 1 && b.port.taken_over.size() == 1 && stock_b &&
           stock_b->map_status == 3);
    for (int spell = 0; spell < 6; spell++) {
        b.pump(9000, &black, 1000);   /* a blank frame a second for 9 s */
        b.pump(5, &f1);
    }
    ok("blank spells of 9 s between pictures: showing throughout, the slot not asked for again",
       b.st().state == "showing" && b.port.requests == 1 && b.map_layer()->params.visible);
    const int rel_b = b.port.releases;
    b.pump(5, &black);   /* one blank frame, then nothing */
    b.pump(9900);
    ok("one blank frame, then nothing: the map stays until blank_ms",
       b.st().state == "showing" && b.port.releases == rel_b);
    b.pump(2500);   /* blank_ms, then the fade out and the linger */
    stock_b = b.port.row(0x100, 0x01000101);
    ok("blank for blank_ms: hidden, the slot given back, the stock map shown, the app told it waits for the phone's map",
       !b.map_layer()->params.visible && b.port.releases > rel_b && !b.port.row(0x100, 0x010004f0) && stock_b &&
           stock_b->map_status == 1 && b.st().state == "idle" && b.st().reason == MapFeed::kWaiting);
    b.pump(5, &f1);
    b.pump(1500);
    ok("the next picture takes the slot again", b.st().state == "showing" && b.port.requests == 2);

    Rig fz(true, 60000);
    fz.port.stock_map(0x100, 0x01000101);
    fz.pump(31000, &black, 65);   /* blank at 15 fps from the start */
    ok("blank at full rate from the start: frozen after 30 s, the slot never asked for",
       fz.st().state == "frozen" && fz.port.requests == 0 && !fz.map_layer()->params.visible);

    Rig lf(true, 60000);
    lf.live_stall_ms = 6000;
    lf.port.stock_map(0x100, 0x01000101);
    lf.pump(100);
    bool every = true;
    for (int k = 0; k < 3; k++) {
        lf.pump(5, &f1);   /* one frame, then nothing for 10 s */
        lf.pump(1500);
        every = every && lf.st().state == "showing" && lf.map_layer()->params.visible;
        lf.pump(8500);
        every = every && lf.st().state == "idle";
    }
    ok("live through a recent frame: a static map's frame every 10 s is shown each time it makes the stream live",
       every && lf.port.requests == 3);

    Rig nl(true, 60000);
    nl.port.stock_map(0x100, 0x01000101);
    nl.pump(1500, &f1);
    ok("reconnect: the first stream shows", nl.st().state == "showing" && nl.port.requests == 1);
    uint64_t seq = 0;
    std::vector<std::string> lines;
    cld::log_lines_since(&seq, &lines);
    lines.clear();
    nl.live = false;   /* the connection ends */
    nl.pump(2000);
    nl.pump(5, &f1);   /* the ended stream's last frame, still on its way */
    nl.pump(200, &black, 20);
    nl.pump(3500);
    nl.live = true;   /* the next connection */
    nl.pump(1500);
    const crs::SlotInfo *stock_nl = nl.port.row(0x100, 0x01000101);
    ok("reconnect: a frame decoded after the end, the next stream live seconds later: idle, hidden, the stock map shown",
       nl.st().state == "idle" && nl.st().reason.empty() && !nl.map_layer()->params.visible &&
           nl.port.requests == 1 && stock_nl && stock_nl->map_status == 1);
    cld::log_lines_since(&seq, &lines);
    int not_live = 0;
    for (const std::string &line : lines)
        not_live += line.find("cluster stream is not live") != std::string::npos;
    ok("reconnect: the stream ending is logged once, whatever frames still come", not_live == 1);
    nl.live = false;
    nl.pump(2000);
    nl.pump(5, &f1);
    nl.pump(500);
    nl.live = true;   /* a new stream's first frame, decoded before its connection is read */
    nl.pump(1500);
    ok("reconnect: a frame decoded just before the stream is live takes the slot",
       nl.st().state == "showing" && nl.port.requests == 2);

    StatusLog sl;
    FeedStatus a;
    a.state = "showing";
    a.target = "map";
    a.page = "Waze map — 15 frames/s";
    int64_t t = 1000000;
    ok("status log: the first status", sl.due(a, t));
    a.page = "Waze map — 14 frames/s";
    a.upload_cap_kb = 5500;
    a.upload_reason = "slow flushes";
    ok("status log: not for the page or the upload cap", !sl.due(a, t));
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: the stream's reason", sl.due(a, t));
    a.reason.clear();
    a.stream_reason = false;
    ok("status log: the stream whole again within 10 s waits", !sl.due(a, t + 2000));
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: so does its next loss; the logged reason is the same, nothing to log",
       !sl.due(a, t + 4000) && !sl.due(a, t + 10000));
    a.reason.clear();
    a.stream_reason = false;
    ok("status log: a change still standing 10 s after the last such line is logged",
       !sl.due(a, t + 9000) && sl.due(a, t + 10500));
    a.state = "idle";
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: a state change is logged at once, with its reason", sl.due(a, t + 11000));
    a.state = "starting";
    a.reason = "the stock map holds the slot";
    a.stream_reason = false;
    ok("status log: a state and reason", sl.due(a, t + 11100));
    ok("status log: the same again is not", !sl.due(a, t + 11200));
    a.reason = "another app holds the slot";
    ok("status log: a reason of the feed's own alone, at once", sl.due(a, t + 11300));
    a.target = "panel";
    ok("status log: a target alone", sl.due(a, t + 11400));

    guide_hold();
    show_off();
    only_what_is_needed();
    {   /* (its own scope: the names of the rest of main) */
    /* ---- MapFeeds: the cluster, and the HUD's copy when it is on ---- */
    Rig b(false, 6000, false, true, true);
    b.feed.reset();   /* the rig's single feed is not used here */
    b.sc.targets[2].share = true;
    b.start();
    FeedConfig bfc;
    bfc.socket = b.path;
    bfc.video.level_bits = 0;
    MapFeeds feeds(bfc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> bspare, boffered;
    bool hud_asked = false;
    /* vary: each frame differs from the one before (a moving map); a frame
     * the same as the last offered goes as a repeat, as the producer hands
     * it over */
    const auto bpump = [&](int64_t ms, const std::vector<uint8_t> *frame, bool vary = false) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0) {
                bspare.assign(frame->begin(), frame->end());
                if (vary)
                    bspare[(size_t)(t / 65) % 1088] ^= 0x80;
                if (bspare == boffered) {
                    feeds.repeat((uint64_t)b.now, b.now);
                } else {
                    boffered = bspare;
                    feeds.offer(&bspare, (uint64_t)b.now, b.now);
                }
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                hud_asked = hud_asked || b.svc->slots().requested(2);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    const auto layer_on = [&](int target) -> const crs::Layer * {
        for (const crs::Layer *x : b.svc->compositor().layers())
            if (x->target() == target)
                return x;
        return nullptr;
    };
    const auto shown_r8 = [&](const char *target, const std::vector<uint8_t> &px) {
        crs::FakeGraph *g = b.gpu.graph(target);
        bool on = false;
        if (g)
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px == px);
        return on;
    };
    bpump(1500, &f1);
    ok("HUD off (the default): the map on the cluster, no HUD feed, layer or request",
       layer_on(0) && shown_r8("map", f1) && !feeds.hud_feed() && !layer_on(2) && !hud_asked &&
           feeds.status("decode").state == "showing" && feeds.status("decode").hud_state == "off" && !feeds.status("decode").hud);
    ok("HUD off: no hud scene ever published", !b.gpu.graph("hud"));

    feeds.set_hud(true);
    bpump(1500, &f1);
    /* the HUD shows the map smaller: its frame is scaled on the head unit */
    AtlasScaler ref;
    std::vector<uint8_t> f1_hud;
    ref.configure(1088, 480, 452, 200, 2);   /* FeedConfig::scaled_level_bits */
    ref.set_corner_radius(200 / 12);         /* FeedConfig::hud_round_corners */
    ref.set_black_background(true);          /* FeedConfig::hud_black_background */
    ref.scale(f1.data(), &f1_hud);
    const crs::Layer *bh = layer_on(2);
    ok("HUD on: a layer on each target, the frame on the cluster whole and on the HUD scaled to 452x200, showing",
       layer_on(0) && bh && shown_r8("map", f1) && shown_r8("hud", f1_hud) && feeds.status("decode").state == "showing" &&
           feeds.status("decode").hud_state == "showing" && feeds.status("decode").target == "map");
    ok("HUD on: its map fitted inside its 800x480 scene",
       bh && bh->params.rect.w <= 800 && bh->params.rect.h <= 480 && bh->params.rect.x + bh->params.rect.w <= 800);
    {
        const uint64_t before = feeds.hud_feed()->scaled_frames();
        bpump(2000, &f2, true);
        const uint64_t scaled = feeds.hud_feed()->scaled_frames() - before;
        char name[160];
        std::snprintf(name, sizeof name, "HUD on: at most 10 frames a second while the cluster takes 15 (%llu in 2 s)",
                      (unsigned long long)scaled);
        ok(name, scaled >= 15 && scaled <= 21);
        /* one more frame right after a HUD frame went, then none: it is not
         * due yet, and still reaches the HUD */
        const auto hud_px = [&]() {
            crs::FakeGraph *g = b.gpu.graph("hud");
            std::vector<uint8_t> px;
            if (g)
                for (const auto &kv : g->shown.buffers)
                    if (kv.second.f == crs::TexFormat::R8)
                        px = kv.second.px;
            return px;
        };
        const std::vector<uint8_t> before_last = hud_px(), f3 = atlas(3);
        bspare.assign(f3.begin(), f3.end());
        boffered = f3;
        feeds.offer(&bspare, (uint64_t)b.now, b.now);
        bpump(600, nullptr);
        ok("HUD on: the newest frame, offered between two HUD frames, reaches the HUD once the stream stops",
           shown_r8("map", f3) && !before_last.empty() && hud_px() != before_last);
        const uint64_t still_from = feeds.hud_feed()->scaled_frames();
        bpump(8000, &f3);   /* past the layers' stall_ms (6 s) */
        const crs::Layer *hs = layer_on(2);
        std::string why;
        ok("HUD on, a still map: the same frame not scaled again, the HUD layer kept fed past its stall time",
           feeds.hud_feed()->scaled_frames() == still_from && shown_r8("map", f3) && hs &&
               b.svc->compositor().state(hs->id(), &why) == crs::LayerState::Shown &&
               feeds.status("decode").hud_state == "showing");
    }

    ok("start: the HUD alone is no cluster target", !feeds.set_target("hud") && feeds.cluster()->target() == "map");
    ok("start: panel keeps the HUD's feed", feeds.set_target("panel") && feeds.hud_feed());
    bpump(2500, &f2);
    ok("on panel: the panel and the HUD both shown, each with the map",
       layer_on(1) && layer_on(2) && shown_r8("panel", f2) && feeds.status("decode").hud_state == "showing" &&
           b.svc->slots().status(1).state == crs::TargetState::Shown &&
           b.svc->slots().status(2).state == crs::TargetState::Shown);
    ok("start: back on map, the HUD's feed kept", feeds.set_target("map") && feeds.hud_feed());
    bpump(2500, &f2);

    /* ---- the HUD map's size ---- */
    const crs::Layer *hl = layer_on(2);
    const unsigned default_w = hl ? hl->params.rect.w : 0;
    ok("the default HUD map is the kit's fitted box, narrower than the HUD", hl && default_w > 0 && default_w < 800);
    feeds.set_hud_width(100);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("100 %: held to the widest, 85 % (680x300), the picture's shape kept, centred",
       hl && hl->params.rect.w == 680 && hl->params.rect.h == 300 && hl->params.rect.x == 60 && hl->params.rect.y == 90 &&
           feeds.status("decode").hud_width_pct == 85);
    feeds.set_hud_width(40);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("40 %: resized live, centred", hl && hl->params.rect.w == 320 && hl->params.rect.h == 140 &&
                                      hl->params.rect.x == 240 && hl->params.rect.y == 169);
    /* the frame goes at the size it is shown: 320x140, 67,200 B instead of 783,360 */
    crs::FakeGraph *g40 = b.gpu.graph("hud");
    bool small = false;
    if (g40)
        for (const auto &kv : g40->shown.buffers)
            small = small || (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() == 320u * 210);
    ok("40 %: the HUD's texture is the scaled frame, 320x140 (67,200 B)", small);
    ok("the size leaves the cluster's map alone", layer_on(0) && layer_on(0)->params.rect.w == 1088);
    feeds.set_hud(false);
    bpump(2500, &f2);
    ok("HUD off again: its layer gone, the cluster's map as it was", !layer_on(2) && layer_on(0) && shown_r8("map", f2));
    feeds.set_hud(true);
    bpump(2500, &f2);
    ok("a HUD feed made later takes the size", layer_on(2) && layer_on(2)->params.rect.w == 320);
    feeds.set_hud_width(0);
    bpump(300, &f2);
    ok("0: back to the default box", layer_on(2) && layer_on(2)->params.rect.w == default_w);

    /* ---- where the HUD map is ---- */
    feeds.set_hud_width(40);
    bpump(300, &f2);
    const crs::Layer *at40 = layer_on(2);
    const uint32_t ref40 = at40 ? at40->id() : 0;
    feeds.set_hud_offset(100, -50);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved: 100 right, 50 up from the centre (240,169 -> 340,119)", hl && hl->params.rect.x == 340 && hl->params.rect.y == 119 &&
                                                                          hl->params.rect.w == 320 && hl->params.rect.h == 140);
    ok("moved: the same layer, only its place changed (no new texture)", hl && hl->id() == ref40);
    ok("moved: the status carries the offset", feeds.status("decode").hud_offset_x == 100 && feeds.status("decode").hud_offset_y == -50);
    feeds.set_hud_offset(800, 800);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved too far: held inside the HUD (its bottom right corner)",
       hl && hl->params.rect.x + hl->params.rect.w == 800 && hl->params.rect.y + hl->params.rect.h >= 479 && hl->params.rect.y + hl->params.rect.h <= 480);
    ok("moved too far: the offset reported as far as it goes (240,170 for 320x140)",
       feeds.status("decode").hud_offset_x == 240 && feeds.status("decode").hud_offset_y == 170);
    feeds.set_hud_offset(-800, -800);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved too far the other way: the top left corner", hl && hl->params.rect.x == 0 && hl->params.rect.y == 0);
    feeds.set_hud_offset(-30, 20);
    feeds.set_hud_width(60);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("resized while moved: the new size, still moved (60 %: 480x211 centred at 160,134, so 130,154)",
       hl && hl->params.rect.w == 480 && hl->params.rect.x == 130 && hl->params.rect.y == 154);
    feeds.set_hud_offset(200, 0);
    feeds.set_hud_width(85);
    bpump(300, &f2);
    feeds.set_hud_width(40);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved, made larger (held to 60 px) and smaller again: back where it was asked (240+200 = 440)",
       hl && hl->params.rect.x == 440 && feeds.status("decode").hud_offset_x == 200);
    feeds.set_hud_offset(-30, 20);
    feeds.set_hud_width(60);
    bpump(300, &f2);
    ok("the offset leaves the cluster's map alone", layer_on(0) && layer_on(0)->params.rect.x == 0 && layer_on(0)->params.rect.y == 0);
    feeds.set_hud_offset(0, 0);
    feeds.set_hud_width(0);

    /* ---- the HUD map over BMW's pop-ups ---- */
    bpump(300, &f2);
    ok("by default the HUD map gives way (its layer's yield 1)",
       layer_on(2) && layer_on(2)->params.yield && !feeds.status("decode").hud_keep_shown);
    b.port.stock_map(0x322, 0x01000251);   /* the media source list on the HUD */
    bpump(300, &f2);
    ok("by default: a list on the HUD releases the HUD map",
       b.svc->slots().status(2).state == crs::TargetState::Waiting);
    const uint32_t yield_ref = layer_on(2) ? layer_on(2)->id() : 0;
    feeds.set_hud_keep_shown(true);
    bpump(2500, &f2);
    ok("kept shown: the same layer with yield 0, shown over the list, the status says so",
       layer_on(2) && layer_on(2)->id() == yield_ref && !layer_on(2)->params.yield &&
           b.svc->slots().status(2).state == crs::TargetState::Shown && feeds.status("decode").hud_keep_shown);
    ok("kept shown: the cluster's map layer untouched", layer_on(0) && layer_on(0)->params.yield);
    feeds.set_hud(false);
    bpump(2500, &f2);
    feeds.set_hud(true);
    bpump(2500, &f2);
    ok("a HUD feed made later keeps it shown", layer_on(2) && !layer_on(2)->params.yield &&
                                                 b.svc->slots().status(2).state == crs::TargetState::Shown);
    feeds.set_hud_keep_shown(false);
    bpump(300, &f2);
    ok("giving way again: released for the list", layer_on(2) && layer_on(2)->params.yield &&
                                                     b.svc->slots().status(2).state == crs::TargetState::Waiting);
    b.port.stock_unmap(0x322, 0x01000251);
    bpump(2500, &f2);
    ok("the list gone: the HUD map shown again", b.svc->slots().status(2).state == crs::TargetState::Shown);

    /* ---- where the map shows: the cluster, the HUD, both, neither ---- */
    {
        const std::vector<uint8_t> f4 = atlas(4), f5 = atlas(5);
        feeds.set_cluster(false);
        bool map_asked = false;
        for (int k = 0; k < 100; k++) {   /* the map's linger (1.5 s) and fade first */
            bpump(50, &f4, true);
            map_asked = map_asked || (k > 50 && b.svc->slots().requested(0));
        }
        FeedStatus st = feeds.status("decode");
        ok("the HUD alone: no cluster feed or map layer, the map target let go, the HUD showing, status off/on",
           !feeds.cluster() && !layer_on(0) && !map_asked && layer_on(2) && feeds.wants_frames() && st.state == "off" &&
               !st.cluster && st.hud && st.hud_state == "showing");
        const uint64_t alone_from = feeds.hud_feed()->scaled_frames();
        bpump(2000, &f4, true);
        const uint64_t alone = feeds.hud_feed()->scaled_frames() - alone_from;
        char name[160];
        std::snprintf(name, sizeof name, "the HUD alone: its frames taken without a copy, at most 10 a second (%llu in 2 s)",
                      (unsigned long long)alone);
        ok(name, alone >= 15 && alone <= 21);
        feeds.set_hud(false);
        bpump(2500, &f4, true);
        st = feeds.status("decode");
        ok("neither: no feed, no layer, no frames wanted, status off/off",
           !feeds.cluster() && !feeds.hud_feed() && !layer_on(0) && !layer_on(2) && !feeds.wants_frames() &&
               st.state == "off" && st.hud_state == "off" && !st.cluster && !st.hud);
        bspare.assign(f4.begin(), f4.end());
        feeds.offer(&bspare, 1, b.now);
        ok("neither: an offered frame goes nowhere", !layer_on(0) && !layer_on(2));
        feeds.set_cluster(true);
        bpump(1500, &f5);
        ok("the cluster on again: its layer back, the next frame shown, the HUD still off",
           layer_on(0) && shown_r8("map", f5) && !layer_on(2) && feeds.status("decode").state == "showing" &&
               feeds.wants_frames());
        feeds.set_hud(true);
        bpump(1500, &f5);
        ok("both again: the HUD's layer back beside the cluster's", layer_on(0) && layer_on(2) &&
                                                                     feeds.status("decode").hud_state == "showing");
        feeds.set_cluster(false);
        feeds.set_cluster(true);
        bpump(1500, &f5);
        ok("the cluster off and on with the HUD on: both shown, the HUD kept", layer_on(0) && layer_on(2) && shown_r8("map", f5));
    }

    /* ---- repeats: a still map costs nothing on either target ---- */
    {
        /* frames whole, and every frame the renderer counted (whole, repeats
         * and the frames they superseded) */
        const auto counted = [&](int target, unsigned long long *whole) {
            unsigned long long n[4] = {};
            if (const crs::Layer *l = layer_on(target))
                std::sscanf(l->stats().c_str(), "%llu sent, %llu unchanged, %llu within threshold, %llu superseded",
                            &n[0], &n[1], &n[2], &n[3]);
            *whole = n[0];
            return n[0] + n[1] + n[2] + n[3];
        };
        const auto shown = [&](int target) {
            const crs::Layer *l = layer_on(target);
            std::string why;
            return l && b.svc->compositor().state(l->id(), &why) == crs::LayerState::Shown;
        };
        const auto hud_r8 = [&]() {
            std::vector<uint8_t> px;
            if (crs::FakeGraph *g = b.gpu.graph("hud"))
                for (const auto &kv : g->shown.buffers)
                    if (kv.second.f == crs::TexFormat::R8)
                        px = kv.second.px;
            return px;
        };
        const std::vector<uint8_t> f6 = atlas(6), f7 = atlas(7);
        bpump(1500, &f6);
        unsigned long long cw0, hw0;
        const unsigned long long c0 = counted(0, &cw0), h0 = counted(2, &hw0);
        const uint64_t s0 = feeds.hud_feed()->scaled_frames();
        const std::vector<uint8_t> still_hud = hud_r8();
        bpump(8000, &f6);   /* repeats only, past the layers' stall_ms (6 s) */
        unsigned long long cw1, hw1;
        const unsigned long long c1 = counted(0, &cw1), h1 = counted(2, &hw1);
        const uint64_t s1 = feeds.hud_feed()->scaled_frames();
        char name[200];
        std::snprintf(name, sizeof name,
                      "both, a still map: repeats keep both layers shown past stall_ms, no frame whole and none scaled "
                      "(cluster %llu, HUD %llu counted in 8 s)",
                      c1 - c0, h1 - h0);
        ok(name, cw1 == cw0 && hw1 == hw0 && s1 == s0 && c1 - c0 >= 100 && h1 - h0 >= 100 && shown(0) && shown(2) &&
                     shown_r8("map", f6) && hud_r8() == still_hud && feeds.status("decode").state == "showing" &&
                     feeds.status("decode").hud_state == "showing");
        bpump(2000, &f7, true);   /* moving */
        unsigned long long cw2, hw2;
        const unsigned long long c2 = counted(0, &cw2);
        counted(2, &hw2);
        const uint64_t s2 = feeds.hud_feed()->scaled_frames();
        std::snprintf(name, sizeof name,
                      "still, then moving: the HUD scales at most 10 a second (%llu in 2 s) and is written (%llu), "
                      "the cluster keeps its rate (%llu frames in 2 s, %llu in 2 s still)",
                      (unsigned long long)(s2 - s1), hw2 - hw1, c2 - c1, (c1 - c0) / 4);
        ok(name, s2 - s1 >= 15 && s2 - s1 <= 21 && hw2 - hw1 >= 5 && hud_r8() != still_hud && cw2 - cw1 >= 25 &&
                     c2 - c1 + 3 >= (c1 - c0) / 4 && shown(0) && shown(2));

        /* the HUD alone, from both with a still map: its picture kept, and
         * repeats of its last scaled frame */
        bpump(1500, &f6);
        feeds.set_cluster(false);
        bpump(2500, &f6);
        unsigned long long hw3;
        const unsigned long long h3 = counted(2, &hw3);
        const uint64_t s3 = feeds.hud_feed()->scaled_frames();
        bpump(8000, &f6);
        unsigned long long hw4;
        const unsigned long long h4 = counted(2, &hw4);
        ok("the HUD alone, a still map: repeats keep it shown past stall_ms, no frame whole and none scaled",
           !layer_on(0) && hw4 == hw3 && h4 - h3 >= 100 && feeds.hud_feed()->scaled_frames() == s3 && shown(2) &&
               feeds.status("decode").hud_state == "showing");
        bpump(2000, &f7, true);
        const uint64_t s5 = feeds.hud_feed()->scaled_frames();
        ok("the HUD alone, then moving: scaled at most 10 a second", s5 - s3 >= 15 && s5 - s3 <= 21 && shown(2));

        /* a feed made beside the other starts from its picture: no new one
         * comes, the map is still */
        bpump(1500, &f6);
        feeds.set_cluster(true);
        bpump(1500, &f6);
        ok("the cluster on beside the HUD, a still map: the picture shown from the HUD's at once",
           shown(0) && shown_r8("map", f6) && shown(2));
        feeds.set_hud(false);
        bpump(1500, &f6);
        feeds.set_hud(true);
        feeds.set_cluster(false);
        bpump(2500, &f6);
        ok("the cluster's map moved to the HUD, a still map: the HUD shows it, scaled once",
           !layer_on(0) && shown(2) && feeds.hud_feed()->scaled_frames() == 1 && !hud_r8().empty());
        feeds.set_cluster(true);
        bpump(1500, &f6);
    }

    feeds.set_stream(false, false, Picture::None);
    bpump(200, &f2);
    ok("the stream not live reaches both feeds: idle", feeds.status("decode").state == "idle" &&
                                                      feeds.status("decode").hud_state == "idle");
    }

    {   /* ---- before the renderer: the HUD's place as saved ---- */
    FeedConfig nfc;
    nfc.socket = "/tmp/woc-feed-no-renderer-" + std::to_string(getpid()) + ".sock";
    nfc.hud_offset_x = 30;
    nfc.hud_offset_y = -20;
    MapFeeds none(nfc, true, true, 10);
    none.poll(1000);
    ok("HUD on, no renderer yet: the status has the saved place, not the centre",
       none.status("decode").hud_offset_x == 30 && none.status("decode").hud_offset_y == -20);
    none.set_hud_offset(40, -20);
    ok("and a move then is taken as asked", none.status("decode").hud_offset_x == 40);
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
