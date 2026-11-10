// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* MapFeeds: the decoded map on the cluster, on the HUD, on both or on
 * neither, as the two settings say.
 *
 * The cluster's feed (map_feed.h), on `map` (or `panel` from a shell), is
 * there while the cluster is on; off, nothing of it exists, so the KOMBI
 * shows its own content and no guide hold is asked for. The HUD's is made
 * while the HUD is on, beside the cluster's on either target. Off, nothing
 * of it exists: no connection, no layer, no thread, no copy. With neither,
 * wants_frames() is false: the capture decodes without pictures.
 *
 * Each feed is its own connection to the cluster renderer and its own
 * layer, so each slot is asked for, held and released on its own. A new
 * picture goes to the cluster's feed without a copy; the HUD's feed gets a
 * copy of the newest (MapFeed::latest) when its next frame is due, at most
 * hud_fps a second, and scales it on a worker thread to the size it is
 * shown at. A picture the same as the last goes to each feed as a repeat:
 * the cluster's last frame and the HUD's last scaled one again, with no
 * copy and no scaling, so both layers stay fed. cluster-render writes the
 * HUD's frames from the HUD's share of the upload budget (its README.md
 * "Upload budget"), never packed: a frame goes while the share has any
 * allowance left, into debt, and waits while it is in debt.
 *
 * The HUD alone takes each frame itself, without a copy, with the whole
 * upload budget, at most hud_fps a second, and repeats as with the cluster.
 * A feed made while the other is there starts from its newest picture.
 *
 * The app's status is the cluster's (state off while it is off), with the
 * HUD's own state beside it (FeedStatus::hud_state): off, disabled by
 * cluster-render's mapping guard, or its feed's state. */
#ifndef WOC_MAP_FEEDS_H
#define WOC_MAP_FEEDS_H

#include <poll.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "map_feed.h"

namespace woc {

class MapFeeds {
public:
    /* base: the cluster's feed; cluster, hud: each on from the start, the
     * HUD at hud_fps frames a second at most. */
    MapFeeds(const FeedConfig &base, bool cluster, bool hud, unsigned hud_fps);

    /* Where the cluster's map goes ("map", "sail", "panel"); false (nothing
     * changed) for a target the renderer has not got. */
    bool set_target(const std::string &name);
    /* The cluster and the HUD on or off, each at once. */
    void set_cluster(bool on);
    void set_hud(bool on);
    /* A feed takes frames: the cluster's, or the HUD's with its layer made
     * and not disabled. */
    bool wants_frames() const;
    /* The HUD map's size and place as asked, kept for a HUD feed made
     * later; the status has the place as far as the HUD's feed puts it. */
    void set_hud_width(unsigned pct);
    void set_hud_offset(int dx, int dy);
    /* The HUD map kept shown over the stock HMI's pop-ups or not, kept for a
     * HUD feed made later. */
    void set_hud_keep_shown(bool keep);
    int hud_offset_x() const { return base_.hud_offset_x; }
    int hud_offset_y() const { return base_.hud_offset_y; }
    /* The cluster map layer's guide_hold. */
    void set_guide_hold(bool hold);

    /* For poll(): one entry per feed, appended. */
    size_t poll_fds(std::vector<pollfd> *fds) const;
    void poll(int64_t now_ms);
    /* Until poll() has HUD work to do (-1: none). */
    int64_t wake_in_ms(int64_t now_ms) const;

    void offer(std::vector<uint8_t> *atlas, uint64_t id, int64_t now_ms);
    void repeat(uint64_t id, int64_t now_ms);
    void set_stream(bool live, bool frozen, Picture picture);

    FeedStatus status(const std::string &decode, const std::string &stream = std::string()) const;
    void request_report();
    std::string report() const;

    /* The cluster's feed while there is one, else null. */
    MapFeed *cluster() { return cluster_.get(); }
    /* The HUD's feed while there is one, else null. */
    MapFeed *hud_feed() { return hud_.get(); }

private:
    /* The HUD's feed made or dropped to match the setting and the target. */
    void follow();
    /* The HUD's copy of the newest picture, when it is due. */
    void feed_hud(int64_t now_ms);

    FeedConfig base_;
    unsigned hud_fps_;
    bool cluster_on_, hud_on_;
    std::unique_ptr<MapFeed> cluster_, hud_;
    std::vector<uint8_t> copy_;   /* a picture on its way from one feed to the other */
    uint64_t id_ = 0;   /* the newest frame's */
    bool live_ = false, frozen_ = false;
    Picture picture_ = Picture::None;
};

}  // namespace woc

#endif
