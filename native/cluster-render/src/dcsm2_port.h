/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The iDrive 7 display port: the KOMBI's Dcsm2 service
 * (../dcsm/DCSM.md), for any number of targets at once.
 *
 * A request is a fetch of the target's scene and, once FETCHED, a map into
 * its slot with its content class; MAPPED_FOREGROUND or MAPPED_BACKGROUND
 * (the under-the-gauges layer) is shown, MAPPED_INVISIBLE is parked behind
 * the content the slot shows. A release is an unmap, when a map was asked
 * for and not refused, then an unfetch. A take-over is an unmap of the
 * stock HMI's content sent with the HMI's client handle. The KOMBI names
 * only the scene id in its events, and its replies name nothing: each reply
 * is the answer to the oldest request of its kind still unanswered (SOME/IP
 * over TCP answers in order), so the scenes asked for are queued per kind
 * while Dcsm2 is available (vsomeip drops requests while it is not). Only
 * fetch and map replies decide anything; unmap replies, ours and the
 * take-overs', are matched in their own queue and logged.
 *
 * Seen from the slot manager (slots.h):
 * - a refused fetch or map, or FETCH FAILED, is a refusal: PRIOTOOLOW and
 *   ALREADYMAPPED say the slot is busy, an unknown slot is its own, an
 *   unknown scene or client is the configuration, FETCH FAILED is its own, the rest (no valid reply,
 *   the fetch removed before it showed) is transient;
 * - a mapped scene the KOMBI maps invisible is parked; parked, then shown,
 *   is shown;
 * - a shown or parked scene the KOMBI unmaps or whose fetch it removes is
 *   lost (the stock map re-mapping over ours removes our fetch with no map
 *   transition, seen on the car; a locked slot unmaps every mapping in it);
 * - Dcsm2 going away loses every scene at once;
 * - the REMOVED that answers our own unfetch is not a loss, even when the
 *   same scene id was asked for again since (panel and HUD share one);
 * - a release is confirmed once the replies to its unfetch (matched in a
 *   queue of their own) have come and, when the content was fetched, the
 *   REMOVED that answers it, or a reply saying the unfetch failed, after
 *   which none comes. Seen on the car: the unmap's reply, the unfetch's,
 *   then REMOVED, within about 35 ms.
 *
 * For the field logs it counts what the KOMBI sends (fetch, map and slot
 * events, notifications that did not decode, replies), logs undecoded
 * notifications (at most every kUndecodedLogMs per event id) and each change
 * of the subscription's answer, and, when the slot manager gives up on a
 * request, what arrived since it and the last slot table. The counts, the
 * availability and the subscription go in the service's status text.
 *
 * All of it runs on the render thread: Dcsm2Link queues what vsomeip's
 * thread receives and hands it over in order (dcsm2_link.h). */
#ifndef CRS_DCSM2_PORT_H
#define CRS_DCSM2_PORT_H

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "slots.h"

namespace crs {

/* Up to max of the n bytes at p in hex, a space after every 4, and " ..."
 * when there were more; "(empty)" for none. */
std::string hex_bytes(const uint8_t *p, size_t n, size_t max);

/* The requests, as Dcsm2Link sends them. */
class Dcsm2Wire {
public:
    virtual ~Dcsm2Wire() {}
    virtual void fetch(uint64_t scene) = 0;
    virtual void map(uint64_t scene, uint32_t content_class, uint64_t slot) = 0;
    virtual void unmap(uint64_t scene) = 0;
    /* An unmap sent with another client's handle. */
    virtual void unmap_as(uint64_t client_handle, uint64_t scene) = 0;
    virtual void unfetch(uint64_t scene) = 0;
    virtual void slots() = 0;
};

class Dcsm2Port : public Port {
public:
    static const int64_t kUndecodedLogMs = 10000;

    explicit Dcsm2Port(Dcsm2Wire *wire) : wire_(wire) {}
    void set_manager(SlotManager *m) { manager_ = m; }
    /* The stock HMI's Dcsm2 client handle, for take-overs (HmiMain3D: 1). */
    void set_hmi_client_handle(uint64_t h) { hmi_handle_ = h; }

    bool available() const override { return up_; }
    void request(const TargetSpec &t) override;
    void release(const TargetSpec &t) override;
    bool confirmed(const TargetSpec &t) const override { return !pending_.count(t.scene_id); }
    void take_over(const TargetSpec &t, uint64_t content) override;
    void refresh_slots() override { wire_->slots(); }
    bool remind(const TargetSpec &t, int64_t now_ms) override;
    int64_t accepted_ms(const TargetSpec &t) const override
    {
        auto it = contents_.find(t.scene_id);
        return it != contents_.end() && it->second.phase == Phase::Mapping ? it->second.accepted_ms : -1;
    }
    void no_answer(const TargetSpec &t, int64_t now_ms) override;
    std::string status_text(int64_t now_ms) const override;

    /* What the KOMBI says, in the order it said it (render thread). */
    void on_availability(bool up, int64_t now_ms);
    /* method: 1 fetch, 2 map, 3 unfetch, 4 unmap; ok false: no valid reply;
     * status_text: the status's catalogue name. */
    void on_reply(uint16_t method, bool ok, uint32_t status, const std::string &status_text, int64_t now_ms);
    void on_fetch_status(uint64_t scene, uint32_t status, int64_t now_ms);
    void on_map_status(uint64_t scene, uint32_t initial, uint32_t final_, int64_t now_ms);
    void on_slots(const std::vector<SlotInfo> &slots, int64_t now_ms);
    /* Heartbeat replies stopped (silent) or came back. */
    void on_heartbeat(bool silent, int64_t now_ms)
    {
        if (on_link)
            on_link(silent, now_ms);
    }
    std::function<void(bool silent, int64_t now_ms)> on_link;
    /* A notification that did not decode (dcsm::Client::onUndecoded): its
     * event id, payload length and hex_bytes of its start. */
    void on_undecoded(uint16_t event, size_t length, const std::string &hex, int64_t now_ms);
    /* The subscription's answer for an event (dcsm::Client::onSubscription). */
    void on_subscription(uint16_t event, uint16_t code, int64_t now_ms);

private:
    enum class Phase { Fetching, Mapping, Parked, Mapped, Refused, Lost };
    static const int64_t kMapAgainMs = 500;
    struct Content {
        TargetSpec spec;
        Phase phase = Phase::Fetching;
        bool map_asked = false;   /* a map went out and was not refused */
        bool fetched = false;     /* FETCHED, and not removed since */
        /* The map's first E_DCSM_OK reply came then (-1: none) and no map
         * status has come: the map is asked again every kMapAgainMs
         * (remind), until a map status comes or a repeat is refused. */
        int64_t accepted_ms = -1;
        bool map_again = true;
        int64_t map_ms = 0;       /* the last map went out */
        int map_repeats = 0;
    };

    /* What the KOMBI sent since the service started. */
    struct Counts {
        uint32_t fetch = 0, map = 0, slots = 0, undecoded = 0, replies = 0;
    };
    static std::string counts_text(const Counts &c);

    uint64_t take(std::deque<uint64_t> *q);
    void refuse(uint64_t scene, Content &c, Refusal r, const std::string &why, int64_t now_ms);

    Dcsm2Wire *wire_;
    SlotManager *manager_ = nullptr;
    uint64_t hmi_handle_ = 1;
    bool up_ = false;
    std::map<uint64_t, Content> contents_;   /* by scene id */
    /* Unfetches sent per scene id whose REMOVED may still come. The KOMBI
     * answers in order, so they come before the FETCHED of a later fetch
     * of the same id, which clears the count. */
    std::map<uint64_t, int> unfetched_;
    std::deque<uint64_t> fetches_, maps_;    /* awaiting a reply, oldest first */
    struct Unmap {
        uint64_t scene = 0, handle = 0;   /* handle 0: ours */
    };
    std::deque<Unmap> unmaps_;
    struct Unfetch {
        uint64_t scene = 0;
        bool removal = false;   /* the content was fetched: a REMOVED answers it */
    };
    std::deque<Unfetch> unfetches_;
    /* Releases not confirmed yet, by scene id: unfetch replies and REMOVEDs
     * still to come. */
    struct Pending {
        int replies = 0, removals = 0;
    };
    std::map<uint64_t, Pending> pending_;
    void settle(uint64_t scene, Pending &p);
    std::map<std::pair<uint64_t, uint64_t>, SlotInfo> slots_;   /* (slot, content): as last logged */

    Counts total_;
    std::map<uint64_t, Counts> asked_;          /* by scene id: total_ at its last request */
    std::vector<SlotInfo> table_;               /* the last slot table since Dcsm2 came up */
    int64_t table_ms_ = -1;                     /* when it came; -1: none */
    int64_t changed_ms_ = -1;                   /* the last availability change; -1: none yet */
    std::map<uint16_t, uint16_t> subscribed_;   /* by event id: the last answer's code, since Dcsm2 last went */
    struct Undecoded {
        bool logged = false;
        int64_t logged_ms = 0;
        uint32_t dropped = 0;   /* not logged since */
    };
    std::map<uint16_t, Undecoded> undecoded_;   /* by event id */
};

}  // namespace crs

#endif
