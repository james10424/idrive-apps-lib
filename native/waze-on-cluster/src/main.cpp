// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* waze-on-cluster: decode the captured CarPlay 2nd-screen H.264 (the Waze/Maps
 * cluster map) and hand it to the shared cluster renderer (cluster-render),
 * which draws it in a cluster slot. The service runs from boot, so it decodes
 * from the stream's first keyframe; the map goes up with the first non-blank
 * picture and comes down when the stream ends, freezes or stays blank
 * (map_feed.h, freeze_watch.h). The control socket
 * reports its status and log to the Waze app.
 *
 *   waze-on-cluster [-c FILE] [-s key=value]...     serve the control socket
 *   waze-on-cluster [-c FILE] run sail|map|panel
 *                                                   map that target from now on,
 *                                                   and follow the status from a shell
 *                                                   (sail = map, the native central-map slot)
 *   waze-on-cluster [-c FILE] bench-pack I420_FILE  time the packed-motion packing
 *
 * The H.264 reaches this tool through the sink the carplay patch points at
 * (patch/carplay_patch.js --sink/--capture), after the patch advertises the 2nd
 * display and makes CarPlay self-start the stream (PATCH.md Edits 1 and 3). It
 * receives on the media bus (avb_stream, the AVB capture mode), a local UDP
 * socket (udp_port) and the FIFO. README.md. */
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "alt_screen.h"
#include "config.h"
#include "control.h"
#include "guide_setting.h"
#include "head_unit.h"
#include "log.h"
#include "block_pack.h"
#include "map_feed.h"
#include "map_feeds.h"
#include "map_geometry.h"
#include "map_state.h"
#include "refused.h"
#include "video_producer.h"
#include "yuv_atlas.h"

using namespace cld;
using namespace woc;
using crk::atlas_height;
using crk::block_bytes;
using crk::pack_blocks;
using crk::texture_size_error;

/* component.json's version, given by the target build. */
#ifndef WOC_VERSION
#define WOC_VERSION ""
#endif

namespace {

/* Accepted and ignored, so a local_conf that sets them still starts the
 * service: the map is drawn by the shared renderer, whose own configuration
 * owns the slots, scene ids and the upload budget. */
const char *const kIgnoredKeys[] = {"tex_format",         "upload",          "dirty_tiles",
                                    "max_frame_textures", "resource_linger_ms", "resource_fetch_log",
                                    "texture_replay",     "texture_replay_frames", "mutable_r8",
                                    "upload_kb_per_s",    "upload_burst_ms"};

volatile sig_atomic_t g_stop;
void on_signal(int) { g_stop = 1; }

std::string status_line(const FeedStatus &s)
{
    return "{\"ev\":\"status\",\"state\":" + json_escape(s.state) + ",\"target\":" + json_escape(s.target) +
           ",\"page\":" + json_escape(s.page) + ",\"reason\":" + json_escape(s.reason) +
           ",\"upload_cap_kb_per_s\":" + std::to_string(s.upload_cap_kb) +
           ",\"upload_reason\":" + json_escape(s.upload_reason) +
           ",\"hide_bmw_guidance\":" + (s.guide_hold ? "1" : "0") +
           ",\"guide_disabled\":" + (s.guide_disabled ? "1" : "0") +
           ",\"cluster\":" + (s.cluster ? "1" : "0") +
           ",\"hud\":" + (s.hud ? "1" : "0") + ",\"hud_state\":" + json_escape(s.hud_state) +
           ",\"hud_width_pct\":" + std::to_string(s.hud_width_pct) +
           ",\"hud_x\":" + std::to_string(s.hud_offset_x) + ",\"hud_y\":" + std::to_string(s.hud_offset_y) +
           ",\"hud_keep_shown\":" + (s.hud_keep_shown ? "1" : "0") +
           /* only while hosted: otherwise the line is as it always was */
           (s.hosted ? ",\"hosted\":1" : "") + "}";
}

class Server : public Control::Handler {
public:
    Server(MapFeeds *feeds, GuideSetting *guide) : feeds_(feeds), guide_(guide) {}
    void set_control(Control *c) { control_ = c; }

    /* start picks the cluster target the map goes to (`run`); cluster and
     * hud turn the map on each off or on ("on"), hud also sizes and places
     * the HUD's; the map itself
     * comes and goes with the stream. guide is the app's switch for BMW's
     * turn box (guide_setting.h). */
    void on_command(const std::map<std::string, std::string> &msg) override
    {
        const std::string &cmd = msg.at("cmd");
        auto it = msg.find("target");
        auto hide = msg.find("hide");
        auto pct = msg.find("hud_width_pct");
        auto hx = msg.find("hud_x"), hy = msg.find("hud_y"), on = msg.find("on");
        auto keep = msg.find("hud_keep_shown");
        if (cmd == "start" && it != msg.end()) {
            log_info("start %s", it->second.c_str());
            if (!feeds_->set_target(it->second))
                log_warn("start: the cluster renderer has no cluster target %s", it->second.c_str());
        } else if (cmd == "guide" && hide != msg.end() && guide_->set(hide->second)) {
            log_info("guide: BMW's turn box %s while the map shows", guide_->hide() ? "hidden" : "shown");
            feeds_->set_guide_hold(guide_->hide());
        } else if (cmd == "cluster" && on != msg.end() && (on->second == "0" || on->second == "1")) {
            feeds_->set_cluster(on->second == "1");
        } else if (cmd == "hud" && (on != msg.end() || pct != msg.end() || hx != msg.end() || hy != msg.end() ||
                                    keep != msg.end())) {
            if (on != msg.end()) {
                if (on->second == "0" || on->second == "1")
                    feeds_->set_hud(on->second == "1");
                else
                    log_warn("hud: on must be 0 or 1, not %s", on->second.c_str());
            }
            if (pct != msg.end()) {
                const long v = std::strtol(pct->second.c_str(), nullptr, 10);
                if (v == 0 || (v >= 20 && v <= 100)) {
                    log_info("hud: map %ld%% wide", v);
                    feeds_->set_hud_width((unsigned)v);
                } else {
                    log_warn("hud: width %s out of range (0, or 20-100)", pct->second.c_str());
                }
            }
            /* and where it is: moved from the centre, in HUD pixels (-800..800 each; held inside the HUD) */
            if (hx != msg.end() || hy != msg.end()) {
                const long x = hx != msg.end() ? std::strtol(hx->second.c_str(), nullptr, 10) : feeds_->hud_offset_x();
                const long y = hy != msg.end() ? std::strtol(hy->second.c_str(), nullptr, 10) : feeds_->hud_offset_y();
                if (x >= -800 && x <= 800 && y >= -800 && y <= 800) {
                    log_info("hud: map moved %+ld,%+ld from the centre", x, y);
                    feeds_->set_hud_offset((int)x, (int)y);
                } else {
                    log_warn("hud: offset %ld,%ld out of range (-800..800)", x, y);
                }
            }
            /* and whether it stays shown over BMW's pop-ups */
            if (keep != msg.end()) {
                if (keep->second == "0" || keep->second == "1")
                    feeds_->set_hud_keep_shown(keep->second == "1");
                else
                    log_warn("hud: hud_keep_shown must be 0 or 1, not %s", keep->second.c_str());
            }
        } else {
            log_warn("control: unknown or malformed command %s", cmd.c_str());
        }
    }
    void on_owner_connected() override
    {
        log_seq_ = 0;
        last_.clear();
    }
    /* The map does not depend on the app: it stays while CarPlay is on the
     * CID and the app's page is hidden. */
    void on_owner_gone() override {}

    void report(const std::string &status)
    {
        if (control_->owner_fd() < 0)
            return;
        if (status != last_) {
            last_ = status;
            control_->send(status);
        }
        std::vector<std::string> lines;
        log_lines_since(&log_seq_, &lines);
        for (const std::string &l : lines)
            control_->send("{\"ev\":\"log\",\"line\":" + json_escape(l) + "}");
    }

private:
    MapFeeds *feeds_;
    GuideSetting *guide_;
    Control *control_ = nullptr;
    uint64_t log_seq_ = 0;
    std::string last_;
};

int serve(Config cfg)
{
    if (cfg.has("log_file"))
        log_to_file(cfg.str("log_file"), log_file_cap(cfg.num("log_file_kb")), "waze-on-cluster", WOC_VERSION);
    load_map_state(&cfg, cfg.str("map_state_file"));
    VideoConfig vc;
    vc.fifo_path = cfg.str("fifo_path");
    vc.carplay_group = cfg.str("carplay_group");
    vc.udp_port = (uint16_t)cfg.num("udp_port");
    vc.avb_stream = cfg.str("avb_stream");
    vc.drm_device = cfg.str("drm_device");
    if (!cfg.size("tex_size", &vc.tex_width, &vc.tex_height)) {
        log_error("tex_size must be WxH");
        return 1;
    }
    if (const char *e = texture_size_error(vc.tex_width, vc.tex_height)) {
        log_error("tex_size %ux%u: %s", vc.tex_width, vc.tex_height, e);
        return 1;
    }
    if (const char *why = parse_source_crop(cfg.str("src_crop"), &vc.src_crop))
        log_warn("src_crop = %s is ignored: %s; the whole picture is the map", cfg.str("src_crop").c_str(), why);
    for (const char *key : kIgnoredKeys)
        if (cfg.has(key))
            log_warn("%s = %s is ignored: the upload budget is the cluster renderer's "
                     "(upload_kb_per_s in /var/opt/cluster-render/cluster-render.local.conf)", key, cfg.str(key).c_str());
    struct Range { const char *key; long lo, hi; } ranges[] = {
        {"dirty_threshold", 0, 64}, {"dirty_settle_ms", 0, 3600000}, {"tex_level_bits", 0, 3},
        {"target_fps", 0, 60},      {"picture_fps", 0, 60},          {"pan_blocks", 0, 1},
        {"stall_ms", 1000, 600000},
        {"cluster", 0, 1},          {"hud", 0, 1},                   {"hud_fps", 1, 30},
        {"hud_width_pct", 0, 100},  {"hud_offset_x", -800, 800},     {"hud_offset_y", -800, 800},
        {"hud_scale_down", 0, 1},   {"hud_round_corners", 0, 1},     {"hud_black_background", 0, 1},
        {"hud_keep_shown", 0, 1},
        {"freeze_ms", 0, 600000},   {"freeze_min_fps", 1, 60},       {"alt_screen_port", 0, 65535},
        {"idle_ms", 1000, 600000},  {"blank_ms", 1000, 600000},
        {"alt_screen_uid", 0, 4294967294L}, {"gpu_copy", 0, 1}};
    for (const Range &r : ranges)
        if (cfg.num(r.key) < r.lo || cfg.num(r.key) > r.hi) {
            log_error("%s must be from %ld to %ld", r.key, r.lo, r.hi);
            return 1;
        }
    vc.tex_level_bits = (uint32_t)cfg.num("tex_level_bits");
    vc.gpu_copy = cfg.num("gpu_copy") == 1;
    vc.stall_ms = (int)cfg.num("stall_ms");
    vc.picture_fps = (unsigned)cfg.num("picture_fps");
    if (vc.picture_fps) {
        /* At least as many pictures as the cluster, the HUD (the app turns it
         * on at run time) and the freeze watch take. */
        const struct { const char *key; unsigned fps; } floors[] = {
            {"target_fps", (unsigned)cfg.num("target_fps")},
            {"hud_fps", (unsigned)cfg.num("hud_fps")},
            {"freeze_min_fps", cfg.num("freeze_ms") ? (unsigned)cfg.num("freeze_min_fps") : 0u}};
        for (const auto &f : floors)
            if (vc.picture_fps < f.fps) {
                log_warn("picture_fps %u is below %s %u: %u instead", vc.picture_fps, f.key, f.fps, f.fps);
                vc.picture_fps = f.fps;
            }
        log_info("waze: pictures at most %u a second (picture_fps); the frames between are decoded without one", vc.picture_fps);
    }
    vc.freeze.ms = (unsigned)cfg.num("freeze_ms");
    vc.freeze.min_fps = (unsigned)cfg.num("freeze_min_fps");
    vc.freeze.blank_ms = (unsigned)cfg.num("blank_ms");

    FeedConfig fc;
    fc.socket = cfg.str("render_socket");
    fc.target = cfg.str("target");
    if (const char *why = parse_map_rect(cfg.str("map_rect"), &fc.rect))
        log_warn("map_rect = %s is ignored: %s; the map area instead", cfg.str("map_rect").c_str(), why);
    fc.hud_width_pct = (unsigned)cfg.num("hud_width_pct");
    if (fc.hud_width_pct && fc.hud_width_pct < 20) {
        log_warn("hud_width_pct %u: 0 or 20-100; the default box instead", fc.hud_width_pct);
        fc.hud_width_pct = 0;
    }
    fc.hud_offset_x = (int)cfg.num("hud_offset_x");
    fc.hud_offset_y = (int)cfg.num("hud_offset_y");
    fc.hud_keep_shown = cfg.num("hud_keep_shown") != 0;
    fc.scale_down = cfg.num("hud_scale_down") != 0;
    fc.hud_round_corners = cfg.num("hud_round_corners") != 0;
    fc.hud_black_background = cfg.num("hud_black_background") != 0;
    fc.scaled_level_bits = vc.tex_level_bits;
    fc.video.w = vc.tex_width;
    fc.video.h = vc.tex_height;
    fc.video.fps = (unsigned)cfg.num("target_fps");
    fc.video.level_bits = 0;   /* rounded in the decoder's packing (video_producer.h) */
    fc.video.dirty_threshold = (unsigned)cfg.num("dirty_threshold");
    fc.video.settle_ms = (unsigned)cfg.num("dirty_settle_ms");
    fc.video.pan_blocks = cfg.num("pan_blocks") == 1 && vc.tex_height % 4 == 0;
    /* The layer's visibility follows the stream (set_stream); the renderer's
     * stall takes it down if the stream's connection outlives its frames. */
    fc.video.stall_ms = (unsigned)cfg.num("idle_ms");

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    int sig_fd = signalfd(-1, &sigs, SFD_CLOEXEC);
    if (sig_fd < 0) { log_error("signalfd: %s", strerror(errno)); return 1; }

    GuideSetting guide(cfg.str("hide_bmw_guidance"), cfg.str("state_file"));
    fc.guide_hold = guide.hide();

    std::string err;
    VideoProducer producer(vc);
    MapFeeds feed(fc, cfg.num("cluster") == 1, cfg.num("hud") == 1, (unsigned)cfg.num("hud_fps"));
    Server server(&feed, &guide);
    Control control(&server);
    server.set_control(&control);
    if (!control.listen(cfg.str("socket_path"), (unsigned)std::strtoul(cfg.str("socket_mode").c_str(), nullptr, 8),
                        cfg.str("socket_group"), &err)) {
        log_error("%s", err.c_str());
        return 1;
    }

    std::vector<uint8_t> frame;
    StatusLog status_log;
    int64_t last_status_write = 0;
    uint32_t errs_logged = 0;
    int64_t errs_logged_ms = -60000;
    const AltScreen alt((uint16_t)cfg.num("alt_screen_port"), (uint32_t)cfg.num("alt_screen_uid"));
    StreamLiveness liveness(vc.stall_ms);
    std::string last_detail;
    AltScreen::State alt_state = AltScreen::Unknown;
    int64_t alt_read_ms = 0;
    for (;;) {
        std::vector<pollfd> p = {{sig_fd, POLLIN, 0}, {control.listen_fd(), POLLIN, 0}, {control.owner_fd(), POLLIN, 0},
                                 {producer.ready_fd(), POLLIN, 0}};
        feed.poll_fds(&p);   /* one per target's renderer connection */
        /* A decoded frame wakes it (the producer's ready fd), and so does the
         * renderer's socket, and the HUD's next frame when one is due; four
         * times a second otherwise, so the client retries its connection, a
         * source that is down is retried and the status files stay current. */
        const int64_t hud_wake = feed.wake_in_ms(now_ms());
        int n = poll(p.data(), p.size(), hud_wake >= 0 && hud_wake < 250 ? (int)hud_wake : 250);
        if (n < 0 && errno != EINTR)
            break;
        if (p[0].revents & POLLIN)
            break;
        if (control.owner_fd() >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR)))
            control.on_owner_readable();
        if (p[1].revents & POLLIN)
            control.on_listen_readable();
        const int64_t now = now_ms();
        uint64_t id = 0;
        bool same = false;
        if (producer.take(&frame, &id, &same)) {
            if (same)
                feed.repeat(id, now);
            else
                feed.offer(&frame, id, now);
        }
        if (now - alt_read_ms >= 1000) {
            const AltScreen::State s = cfg.num("alt_screen_port") ? alt.read() : AltScreen::Unknown;
            if (s != alt_state)
                log_info("CarPlay's alternate-screen connection: %s",
                         s == AltScreen::Connected ? "connected" : s == AltScreen::Absent ? "absent" : "not readable");
            alt_state = s;
            alt_read_ms = now;
        }
        const bool live = liveness.live(alt_state, producer.last_frame_ms(), now);
        producer.set_live(live, now);
        producer.set_pictures(feed.wants_frames());
        feed.set_stream(live, producer.frozen(now), producer.picture(now));
        feed.poll(now);
        producer.retry_sources(now);
        const FeedStatus fs = feed.status(producer.decode_status(), producer.stream_reason());
        const std::string status = status_line(fs);
        /* The frame rate changes the status every second; the log keeps
         * to its state, reason and target, so the app's last lines are not
         * all status. */
        if (status_log.due(fs, now))
            log_info("status %s", status.c_str());
        server.report(status);
        if (now - last_status_write > 1000) {
            FrameTally::Minute m;
            StreamHealth::Minute sm;
            Backlog::Minute bm;
            FrameTiming::Minute tm;
            uint64_t overflows = 0;
            if (producer.minute(now, &m, &sm, &bm, &tm, &overflows)) {
                const unsigned paced = (unsigned)producer.take_paced();
                char paced_text[48] = "";
                if (vc.picture_fps)
                    std::snprintf(paced_text, sizeof paced_text, ", %u without one for picture_fps", paced);
                log_info("waze: %u frames decoded in the last minute, %u without a picture to catch up%s, %u changed, "
                         "%u corrupted; the last picture is %s; %u losses (%u packets), %llu dropped by the receive "
                         "queue; packets waited %.1f ms on average, %lld ms at most",
                         m.frames + bm.skipped + paced, bm.skipped, paced_text, m.changed, sm.corrupted,
                         m.blank ? "blank" : "not blank",
                         sm.losses, sm.lost, (unsigned long long)overflows, bm.wait_us.mean() / 1000.0,
                         (long long)(bm.wait_us.max / 1000));
                log_info("waze: %s", FrameTiming::describe(tm).c_str());
            }
            producer.track_keyframe_wait(now);
            const std::string detail = producer.stream_detail();
            if (producer.stream_lost() && detail != last_detail)
                log_warn("waze: stream lost: %s", detail.c_str());
            last_detail = detail;
            /* New decode errors, at most once a minute. */
            std::string last_err;
            const uint32_t errs = producer.decode_errors(&last_err);
            if (errs != errs_logged && now - errs_logged_ms >= 60000) {
                log_warn("waze: %u decode errors so far, the last: %s", errs, last_err.c_str());
                errs_logged = errs;
                errs_logged_ms = now;
            }
            write_sibling_file(cfg.str("fifo_path"), "status.json", status);
            /* The framing probe and the renderer's own status (targets, the
             * map layer's writes, the upload cap), for the diagnostics. */
            write_sibling_file(cfg.str("fifo_path"), "probe.txt",
                               producer.probe_report() + "stream: " + (detail.empty() ? "whole" : detail) + "\n" +
                                   feed.report());
            feed.request_report();
            last_status_write = now;
        }
    }
    log_info("stopped");
    return 0;
}

int run(const Config &cfg, const char *target)
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", cfg.str("socket_path").c_str());
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        std::fprintf(stderr, "waze-on-cluster: %s: %s\n", addr.sun_path, strerror(errno));
        return 1;
    }
    std::string start = "{\"cmd\":\"start\",\"target\":" + json_escape(target) + "}\n";
    if (write(fd, start.data(), start.size()) != (ssize_t)start.size())
        return 1;
    char buf[1024];
    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    close(fd);
    return 0;
}

bool load_local(Config *cfg, std::string *err)
{
    std::string path = cfg->str("local_conf");
    return path.empty() || access(path.c_str(), F_OK) != 0 || cfg->load(path, err);
}

void usage()
{
    std::fprintf(stderr,
                 "usage: waze-on-cluster [-c FILE] [-s key=value]... [run sail|map|panel | bench-pack I420_FILE]\n");
}

/* Times pan_blocks' packing (4:2:0 to RGB and the block fit) of each
 * tex_size I420 frame in the file on 1 to 4 threads, ten rounds each. */
int bench_pack(const Config &cfg, const char *path)
{
    uint32_t w = 0, h = 0;
    if (!cfg.size("tex_size", &w, &h) || texture_size_error(w, h) || h % 4) {
        std::fprintf(stderr, "bench-pack: tex_size must be a valid map size with a height that is a multiple of 4\n");
        return 2;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "bench-pack: %s: %s\n", path, strerror(errno)); return 2; }
    const size_t frame = (size_t)w * h * 3 / 2;
    std::vector<uint8_t> i420(frame);
    std::vector<std::vector<uint8_t>> atlases;
    while (atlases.size() < 64 && std::fread(i420.data(), 1, frame, f) == frame) {
        std::vector<uint8_t> a((size_t)w * atlas_height(h));
        std::memcpy(a.data(), i420.data(), (size_t)w * h);
        const uint8_t *u = i420.data() + (size_t)w * h, *v = u + (size_t)w / 2 * (h / 2);
        for (uint32_t y = 0; y < h / 2; y++) {
            std::memcpy(&a[(size_t)(h + y) * w], u + (size_t)y * (w / 2), w / 2);
            std::memcpy(&a[(size_t)(h + y) * w + w / 2], v + (size_t)y * (w / 2), w / 2);
        }
        atlases.push_back(std::move(a));
    }
    std::fclose(f);
    if (atlases.empty()) { std::fprintf(stderr, "bench-pack: no whole %ux%u frame in %s\n", w, h, path); return 2; }
    std::vector<uint8_t> out(block_bytes(w, h));
    std::printf("bench-pack: %zu frames of %ux%u, %zu B packed each\n", atlases.size(), w, h, out.size());
    for (unsigned threads = 1; threads <= 4; threads++) {
        double sum = 0, worst = 0;
        int n = 0;
        for (int round = 0; round < 10; round++)
            for (const std::vector<uint8_t> &a : atlases) {
                const auto t0 = std::chrono::steady_clock::now();
                pack_blocks(a.data(), w, h, out.data(), threads);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                sum += ms;
                worst = std::max(worst, ms);
                n++;
            }
        std::printf("bench-pack: %u thread%s: %.2f ms mean, %.2f ms max\n", threads, threads > 1 ? "s" : "",
                    sum / n, worst);
    }
    return 0;
}

}  // namespace

/* The keys beyond the shared Config's own that this tool reads. Registered
 * before any conf is loaded, so Config accepts them. */
void define_keys(Config *cfg)
{
    cfg->define("fifo_path", "/run/waze-on-cluster/alt.h264");
    cfg->define("carplay_group", "video");
    cfg->define("udp_port", "20557");
    cfg->define("avb_stream", "media_transport.avb_streaming.15");
    cfg->define("drm_device", "/dev/dri/renderD128");
    cfg->define("tex_size", "1088x480");
    cfg->define("src_crop", "");
    cfg->define("map_rect", "");
    cfg->define("dirty_threshold", "8");
    cfg->define("dirty_settle_ms", "1000");
    cfg->define("stall_ms", "6000");
    cfg->define("idle_ms", "60000");
    cfg->define("freeze_ms", "10000");
    cfg->define("freeze_min_fps", "3");
    cfg->define("blank_ms", "10000");
    cfg->define("alt_screen_port", "6031");
    cfg->define("alt_screen_uid", "13002");
    cfg->define("target_fps", "30");
    cfg->define("picture_fps", "0");
    cfg->define("pan_blocks", "1");
    cfg->define("tex_level_bits", "2");
    cfg->define("gpu_copy", "1");
    cfg->define("target", "map");
    cfg->define("hide_bmw_guidance", "1");
    cfg->define("state_file", "/var/opt/waze-on-cluster/state/waze-on-cluster.state.conf");
    cfg->define("map_state_file", "/var/opt/waze-on-cluster/map.state.conf");
    cfg->define("cluster", "1");
    cfg->define("hud", "0");
    cfg->define("hud_fps", "10");
    cfg->define("hud_width_pct", "0");
    cfg->define("hud_offset_x", "0");
    cfg->define("hud_offset_y", "0");
    cfg->define("hud_keep_shown", "0");
    cfg->define("hud_scale_down", "1");
    cfg->define("hud_round_corners", "1");
    cfg->define("hud_black_background", "1");
    cfg->define("render_socket", "/dev/shm/cluster-render/render.sock");
    cfg->define("log_file", "/var/opt/waze-on-cluster/log/waze-on-cluster.log");
    cfg->define("log_file_kb", "512");
    for (const char *key : kIgnoredKeys)
        cfg->define(key, "");
}

int main(int argc, char **argv)
{
    cld::log_name = "waze-on-cluster";
    Config cfg;
    define_keys(&cfg);
    std::string err;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        bool has_arg = i + 1 < argc;
        if (!std::strcmp(argv[i], "-c") && has_arg) {
            if (!cfg.load(argv[++i], &err) || !load_local(&cfg, &err)) { std::fprintf(stderr, "waze-on-cluster: %s\n", err.c_str()); return 2; }
        } else if (!std::strcmp(argv[i], "-s") && has_arg) {
            if (!cfg.set_pair(argv[++i], &err)) { std::fprintf(stderr, "waze-on-cluster: %s\n", err.c_str()); return 2; }
        } else {
            usage();
            return 2;
        }
    }
    log_verbose = (int)cfg.num("verbose");
    signal(SIGPIPE, SIG_IGN);

    if (i == argc) {
        /* Before any source, decoder or renderer connection. */
        const std::string why = head_unit_refused();
        return why.empty() ? serve(cfg) : serve_refused(cfg, why);
    }
    if (i + 2 == argc && !std::strcmp(argv[i], "run"))
        return run(cfg, argv[i + 1]);
    if (i + 2 == argc && !std::strcmp(argv[i], "bench-pack"))
        return bench_pack(cfg, argv[i + 1]);
    usage();
    return 2;
}
