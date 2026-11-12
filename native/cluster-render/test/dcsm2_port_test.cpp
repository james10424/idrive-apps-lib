/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for the iDrive 7 port (dcsm2_port.h) under the slot manager:
 * fetch, then map once FETCHED, shown at MAPPED_FOREGROUND or BACKGROUND;
 * release unmaps only what a map went out for; replies are matched to the
 * oldest unanswered request of their kind across targets; refusals are
 * classified (busy, unknown slot, configuration, transient); a removed fetch of a shown
 * scene is lost, and evidence for the upload governor unless the stock HMI
 * holds the slot; Dcsm2 going away forgets everything and later events for
 * it are ignored. MAPPED_INVISIBLE is parked, with no timeout, and shown
 * when the KOMBI promotes it; a parked scene unmapped is lost; a take-over
 * is an unmap of the stock HMI's content with the HMI's handle, whose reply
 * matches no request of ours: fetch and map replies still go to the oldest
 * fetch and map. A release is confirmed by the REMOVED answering its
 * unfetch when the content was fetched, otherwise by the unfetch's reply,
 * or by a reply refusing the unfetch; the next request of the scene waits
 * for it. FETCH FAILED right after the publication is asked again after
 * 250 ms.
 *
 * The field-log diagnostics: an undecoded notification is logged with its
 * start in hex at most every 10 s per event id, with the number not logged
 * in between; each change of the subscription's answer is logged once; a
 * request the slot manager gives up on logs what arrived since it and the
 * last slot table, or that none came; the status text carries the counts,
 * the availability and the subscription; slot rows are logged for every
 * slot, used by a target or not, when new or changed. */
#include <cstdio>
#include <string>
#include <vector>

#include "../src/dcsm2_port.h"
#include "../src/log.h"
#include "dcsm/dcsm_client.h"

using namespace crs;

static int failures;
static std::vector<std::string> calls;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static bool calls_are(const std::string &want)
{
    std::string got;
    for (auto &c : calls) got += (got.empty() ? "" : "; ") + c;
    calls.clear();
    if (got == want)
        return true;
    std::printf("     got  \"%s\"\n     want \"%s\"\n", got.c_str(), want.c_str());
    return false;
}

static std::string hex(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

struct Wire : Dcsm2Wire {
    void fetch(uint64_t s) override { calls.push_back("fetch " + hex(s)); }
    void map(uint64_t s, uint32_t c, uint64_t slot) override
    {
        calls.push_back("map " + hex(s) + " class " + std::to_string(c) + " slot " + hex(slot));
    }
    void unmap(uint64_t s) override { calls.push_back("unmap " + hex(s)); }
    void unmap_as(uint64_t h, uint64_t s) override { calls.push_back("unmap " + hex(s) + " as " + hex(h)); }
    void unfetch(uint64_t s) override { calls.push_back("unfetch " + hex(s)); }
    void slots() override { calls.push_back("slots?"); }
};

struct Scenes0 : Scenes {
    bool publish(const TargetSpec &, std::string *) override { return true; }
    void unpublish(const TargetSpec &) override {}
};

static const uint64_t kMap = 0x010004f0, kPanel = 0x01000400;

struct Rig {
    Wire wire;
    Scenes0 scenes;
    Dcsm2Port port{&wire};
    SlotManager m{&port, &scenes};
    std::vector<std::string> evidence;
    int64_t now = 10000;
    Rig()
    {
        TargetSpec a;
        a.name = "map"; a.slot = 0x100; a.scene_id = kMap; a.width = 1088; a.height = 480;
        a.take_over_stock = true;
        TargetSpec b;
        b.name = "panel"; b.slot = 0x203; b.scene_id = kPanel; b.width = 1920; b.height = 720; b.content_class = 3;
        m.add(a);
        m.add(b);
        port.set_manager(&m);
        m.on_evidence = [this](Evidence e, const std::string &d, int64_t) {
            evidence.push_back(std::string(evidence_name(e)) + " " + d);
        };
        port.on_availability(true, now);
        now += 1500;
    }
    void tick() { m.tick(now += 10); }
    /* The map requested, fetched and mapped; its map status still to come. */
    void map_asked(bool contend)
    {
        m.want(0, true, contend);
        tick();
        port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", now);
        port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FETCHED, now);
        port.on_reply(dcsm::kReqMapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", now);
        calls.clear();
    }
    /* Slot 0x100 as the car reports it: the stock map and ours. */
    void table(uint32_t stock_map, uint32_t ours)
    {
        SlotInfo h, o;
        h.slot = o.slot = 0x100;
        h.status = o.status = stock_map == 1 || ours == 1 ? 2 : 1;
        h.content = 0x01000300;
        o.content = kMap;
        h.map_status = stock_map;
        o.map_status = ours;
        port.on_slots({h, o}, now);
    }
};

static void parked()
{
    Rig r;
    r.map_asked(false);
    r.table(1, 3);
    r.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_INVISIBLE, r.now);
    ok("MAPPED_INVISIBLE: parked behind the stock map", r.m.status(0).state == TargetState::Parked &&
                                                          r.m.status(0).holder == Holder::Stock);
    calls.clear();
    for (int k = 0; k < 1000; k++)
        r.tick();
    ok("10 s parked: no timeout, nothing released", r.m.status(0).state == TargetState::Parked &&
                                                      calls_are("slots?; slots?"));
    r.table(0, 1);
    r.port.on_map_status(kMap, 3, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
    ok("promoted (3 -> 1): shown", r.m.status(0).state == TargetState::Shown);
    r.port.on_map_status(kMap, 1, dcsm::E_MAP_STATUS_MAPPED_INVISIBLE, r.now);
    ok("1 -> 3: parked, not lost", r.m.status(0).state == TargetState::Parked && calls_are("slots?"));
    r.m.want(0, false, false);
    r.tick();
    ok("released while parked: unmapped, then unfetched", calls_are("unmap 0x10004f0; unfetch 0x10004f0") &&
                                                            r.m.status(0).state == TargetState::Idle);

    Rig l;
    l.map_asked(false);
    l.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_INVISIBLE, l.now);
    calls.clear();
    l.table(0, 0);
    l.port.on_map_status(kMap, 3, dcsm::E_MAP_STATUS_UNMAPPED, l.now);
    ok("parked, then unmapped (a lock): lost, unfetched with no unmap", calls_are("unfetch 0x10004f0") &&
                                                                         l.m.status(0).state == TargetState::Waiting);

    Rig f;
    f.map_asked(false);
    f.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_INVISIBLE, f.now);
    calls.clear();
    f.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_REMOVED, f.now);
    ok("parked, then its fetch removed: lost, no evidence", calls_are("unfetch 0x10004f0") &&
                                                              f.m.status(0).state == TargetState::Waiting &&
                                                              f.evidence.empty());
}

static void take_over()
{
    Rig r;
    r.port.set_hmi_client_handle(1);
    r.map_asked(true);
    r.table(1, 3);
    r.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_INVISIBLE, r.now);
    calls.clear();
    r.tick();
    ok("contending and parked behind the stock map: an unmap of it as client 1",
       calls_are("unmap 0x1000300 as 0x1; slots?"));
    /* The panel's fetch is out when the take-over's reply comes. */
    r.m.want(1, true, false);
    r.tick();
    calls.clear();
    r.port.on_reply(dcsm::kReqUnmapContent, true, dcsm::E_DCSM_HANDLE_UNKNOWN, "E_DCSM_HANDLE_UNKNOWN", r.now);
    ok("the take-over's reply, even a refusal, refuses nothing of ours",
       r.m.status(1).state == TargetState::Acquiring && r.m.status(0).state == TargetState::Parked);
    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_CONTENTID_UNKNOWN, "E_DCSM_CONTENTID_UNKNOWN", r.now);
    ok("the next fetch reply is still the panel's", r.m.status(1).state == TargetState::Waiting &&
                                                     r.m.status(1).reason.find("CONTENTID") != std::string::npos);
    r.table(3, 1);
    r.port.on_map_status(kMap, 3, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
    ok("the KOMBI shows ours: shown", r.m.status(0).state == TargetState::Shown);

    Rig h;
    h.port.set_hmi_client_handle(7);
    h.map_asked(true);
    h.table(1, 3);
    h.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_INVISIBLE, h.now);
    calls.clear();
    h.tick();
    ok("the HMI's handle is configurable", calls_are("unmap 0x1000300 as 0x7; slots?"));
    /* More unmap replies than unmaps sent. */
    h.m.want(0, false, false);
    h.tick();
    calls.clear();
    h.port.on_reply(dcsm::kReqUnmapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", h.now);
    h.port.on_reply(dcsm::kReqUnmapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", h.now);
    h.port.on_reply(dcsm::kReqUnmapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", h.now);
    ok("unmap replies, more than were sent: nothing breaks", h.m.status(0).state == TargetState::Idle);
}

static void basics()
{
    Rig r;
    r.m.want(0, true, true);
    r.tick();
    ok("a request is a fetch", calls_are("fetch 0x10004f0"));
    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FETCHED, r.now);
    ok("FETCHED: the map into its slot with its class", calls_are("map 0x10004f0 class 0 slot 0x100"));
    r.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_BACKGROUND, r.now);
    ok("MAPPED_BACKGROUND is shown", r.m.status(0).state == TargetState::Shown);

    /* Two requests out at once: replies match the oldest of their kind. */
    r.m.want(1, true, false);
    r.tick();
    calls.clear();
    r.port.on_reply(dcsm::kReqMapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_fetch_status(kPanel, dcsm::E_FETCH_STATUS_FETCHED, r.now);
    ok("the panel is mapped with its own class and slot", calls_are("map 0x1000400 class 3 slot 0x203"));
    r.port.on_reply(dcsm::kReqMapContent, true, dcsm::E_DCSM_PRIOTOOLOW, "E_DCSM_PRIOTOOLOW", r.now);
    ok("a busy map refusal: the panel waits for the slot, unfetched without an unmap",
       r.m.status(1).state == TargetState::Waiting && r.m.status(1).holder == Holder::Stock &&
           calls_are("unfetch 0x1000400") && r.evidence.empty());
    r.port.on_map_status(kPanel, 0, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
    ok("a late map answer for the released panel is ignored", r.m.status(1).state == TargetState::Waiting);

    /* The display removes the shown map with nothing else in its slot. */
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_REMOVED, r.now);
    ok("a removed fetch of a shown scene: lost and unfetched (nothing left to unmap)",
       r.m.status(0).state == TargetState::Waiting && calls_are("unfetch 0x10004f0"));
    ok("and it is evidence for the governor", r.evidence.size() == 1 &&
                                                  r.evidence[0].find("scene removed map") == 0);
    calls.clear();

    /* Now the stock map takes it: arbitration, not evidence. The removal
     * is asked for again on the ladder, 5 s after it here. */
    r.now += 5000;
    r.tick();
    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FETCHED, r.now);
    r.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
    SlotInfo s;
    s.slot = 0x100;
    s.status = 2;
    s.content = 0x01000101;
    s.map_status = 1;   /* shown */
    r.port.on_slots({s}, r.now);
    r.port.on_map_status(kMap, 1, dcsm::E_MAP_STATUS_UNMAPPED, r.now);
    ok("unmapped by the stock map: lost, no evidence",
       r.m.status(0).state == TargetState::Waiting && r.m.status(0).holder == Holder::Stock && r.evidence.size() == 1);

    /* Refusal kinds. */
    Rig q;
    q.m.want(0, true, false);
    q.tick();
    q.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_SLOTID_UNKNOWN, "E_DCSM_SLOTID_UNKNOWN", q.now);
    ok("an unknown slot: the ladder, no evidence",
       q.m.status(0).retry_ms > 0 && q.m.status(0).reason.find("SLOTID") != std::string::npos && q.evidence.empty());
    Rig t;
    t.m.want(0, true, false);
    t.tick();
    t.port.on_reply(dcsm::kReqFetchContent, false, 0, "", t.now);
    ok("no valid reply is transient: the ladder, no evidence", t.m.status(0).retry_ms > 0 && t.evidence.empty());

    /* Panel and HUD share a scene id: the HUD asks for it right after the
     * panel let it go, and the REMOVED answering the panel's unfetch
     * arrives while the HUD's fetch is under way. */
    Rig h;
    TargetSpec hud;
    hud.name = "hud"; hud.slot = 0x323; hud.scene_id = kPanel; hud.width = 800; hud.height = 480;
    h.m.add(hud);
    h.m.want(1, true, false);
    h.tick();
    h.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", h.now);
    h.port.on_fetch_status(kPanel, dcsm::E_FETCH_STATUS_FETCHED, h.now);
    h.port.on_map_status(kPanel, 0, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, h.now);
    h.m.want(1, false, false);
    h.m.want(2, true, false);
    for (int k = 0; k < 400; k++)
        h.tick();
    calls.clear();
    h.port.on_fetch_status(kPanel, dcsm::E_FETCH_STATUS_REMOVED, h.now);
    ok("the REMOVED answering our own unfetch does not refuse the HUD's new fetch",
       h.m.status(2).state == TargetState::Acquiring && h.evidence.empty() && calls_are(""));
    h.port.on_fetch_status(kPanel, dcsm::E_FETCH_STATUS_FETCHED, h.now);
    h.port.on_map_status(kPanel, 0, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, h.now);
    h.port.on_fetch_status(kPanel, dcsm::E_FETCH_STATUS_REMOVED, h.now);
    ok("after the HUD's FETCHED a REMOVED is a loss again", h.m.status(2).state == TargetState::Waiting &&
                                                             h.evidence.size() == 1);

    /* Dcsm2 goes away and comes back. */
    Rig u;
    u.m.want(0, true, false);
    u.tick();
    calls.clear();
    u.port.on_availability(false, u.now);
    u.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FETCHED, u.now);
    ok("gone: nothing is sent, later events are ignored", calls_are("") &&
                                                             u.m.status(0).state == TargetState::Unavailable);
}

/* The lines logged since the last call. */
static std::vector<std::string> logged()
{
    static uint64_t seq;
    std::vector<std::string> out;
    cld::log_lines_since(&seq, &out);
    return out;
}

static bool has(const std::string &s, const std::string &part)
{
    if (s.find(part) != std::string::npos)
        return true;
    std::printf("     \"%s\"\n     lacks \"%s\"\n", s.c_str(), part.c_str());
    return false;
}

static void diagnostics()
{
    const uint8_t bytes[70] = {0x00, 0x00, 0x00, 0x20, 0x01, 0x02, 0x03, 0x04, 0xab};
    ok("hex: groups of 4", hex_bytes(bytes, 9, 64) == "00000020 01020304 ab");
    ok("hex: cut at the limit", hex_bytes(bytes, 70, 8) == "00000020 01020304 ...");
    ok("hex: empty", hex_bytes(bytes, 0, 64) == "(empty)");

    Rig r;
    logged();
    r.port.on_undecoded(0x8009, 36, "00000020 ...", r.now);
    std::vector<std::string> l = logged();
    ok("undecoded: logged", l.size() == 1 && l[0] == "dcsm: event 0x8009 (36 bytes) not decoded: 00000020 ...");
    r.port.on_undecoded(0x8009, 36, "x", r.now + 5000);
    r.port.on_undecoded(0x8009, 36, "x", r.now + 9999);
    r.port.on_undecoded(0x8008, 8, "y", r.now + 9999);
    l = logged();
    ok("within 10 s: not logged again; another id is", l.size() == 1 && has(l[0], "event 0x8008 (8 bytes)"));

    /* Slots no target uses (the guidance box's candidates) are logged as
     * any other: each new row, then each change. */
    Rig v;
    logged();
    std::vector<SlotInfo> table;
    for (uint64_t id = 0x101; id <= 0x105; id++) {
        SlotInfo s;
        s.slot = id;
        s.status = id == 0x103 ? 0 : 1;
        table.push_back(s);
    }
    v.port.on_slots(table, v.now);
    l = logged();
    ok("slots 0x101-0x105: a row each",
       l.size() == 5 && has(l[2], "dcsm: slot 0x103 status 0 content 0x0 class 0 map 0") &&
           has(l[4], "dcsm: slot 0x105 status 1 content 0x0"));
    table[4].status = 2;
    table[4].content = 0x01000310;
    table[4].klass = 0x400;
    table[4].map_status = 1;
    v.port.on_slots(table, v.now);
    l = logged();
    ok("the box mapped in one: only its row again",
       l.size() == 1 && has(l[0], "dcsm: slot 0x105 status 2 content 0x1000310 class 1024 map 1"));
    r.port.on_undecoded(0x8009, 40, "z", r.now + 10000);
    l = logged();
    ok("after 10 s: logged with the number not logged",
       l.size() == 1 && l[0] == "dcsm: event 0x8009 (40 bytes) not decoded: z; 2 more not logged since the last");
    r.port.on_undecoded(0x8009, 40, "z", r.now + 20000);
    l = logged();
    ok("and the count starts over", l.size() == 1 && l[0] == "dcsm: event 0x8009 (40 bytes) not decoded: z");

    r.port.on_subscription(0x8008, 0, r.now);
    r.port.on_subscription(0x8008, 0, r.now);
    r.port.on_subscription(0x8009, 7, r.now);
    r.port.on_subscription(0xffff, 0, r.now);
    l = logged();
    ok("subscription: each change once", l.size() == 3 && l[0] == "dcsm: subscription to event 0x8008: accepted" &&
                                             l[1] == "dcsm: subscription to event 0x8009: refused (code 0x07)" &&
                                             l[2] == "dcsm: subscription to the eventgroup: accepted");
    std::string st = r.port.status_text(r.now + 2000);
    ok("status: availability and subscription",
       has(st, "dcsm: Dcsm2 available for 3 s; subscription: event 0x8008 accepted, event 0x8009 refused (code "
               "0x07), the eventgroup accepted\n"));
    ok("status: the counts, no table yet",
       has(st, "dcsm: since start: 0 fetch, 0 map, 0 slot, 6 undecoded events, 0 replies; no slot table since "
               "Dcsm2 came up\n"));
    r.port.on_subscription(0x8009, 0, r.now);
    l = logged();
    ok("refused, then accepted: logged", l.size() == 1 && l[0] == "dcsm: subscription to event 0x8009: accepted");

    /* The map asked for: the fetch accepted and FETCHED, the map accepted,
     * then nothing for 10 s (longer than an accepted map is waited for). */
    r.m.want(0, true, false);
    r.tick();
    r.table(1, 0);
    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FETCHED, r.now);
    r.port.on_reply(dcsm::kReqMapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_undecoded(0x8008, 8, "y", r.now + 20000);
    logged();
    r.now += 9990;
    r.tick();
    l = logged();
    ok("not mapped, then no answer: what arrived since the request and the last table",
       l.size() == 2 && has(l[0], "target map: the display accepted scene 0x10004f0 and has not mapped it for 10000 ms (target acquiring, no map status)") &&
           l[1] == "dcsm: no answer for content 0x10004f0 (target map, slot 0x100, mapping); since its request: 1 "
                   "fetch, 0 map, 1 slot, 1 undecoded events, 2 replies; last slot table 10000 ms ago, 2 rows (slot "
                   "status content class map): 0x100 2 0x1000300 0 1, 0x100 2 0x10004f0 0 0");
    ok("and the target waits, accepted and not mapped", r.m.status(0).state == TargetState::Waiting &&
                                                           r.m.status(0).reason == "accepted, not mapped");
    st = r.port.status_text(r.now);
    ok("status: the totals and the table's age",
       has(st, "dcsm: since start: 1 fetch, 0 map, 1 slot, 7 undecoded events, 2 replies; last slot table 10 s ago\n"));

    r.port.on_availability(false, r.now);
    r.port.on_availability(true, r.now + 1000);
    st = r.port.status_text(r.now + 3500);
    ok("Dcsm2 back: availability since, subscription and table forgotten",
       has(st, "dcsm: Dcsm2 available for 2 s; subscription: no answer yet\n") &&
           has(st, "no slot table since Dcsm2 came up"));
    r.now += 1000;
    for (int k = 0; k < 200; k++)
        r.tick();
    logged();
    for (int k = 0; k < 600; k++)
        r.tick();
    l = logged();
    bool told = false;
    for (const std::string &x : l)
        told = told || x == "dcsm: no answer for content 0x10004f0 (target map, slot 0x100, fetching); since its "
                            "request: 0 fetch, 0 map, 0 slot, 0 undecoded events, 0 replies; no slot table since "
                            "Dcsm2 came up";
    ok("no answer with nothing since: says no table came", told);
    Dcsm2Port fresh(&r.wire);
    ok("status before Dcsm2 was ever seen", has(fresh.status_text(0), "dcsm: Dcsm2 not available yet; subscription: "
                                                                       "no answer yet\n"));
}

static void confirmation()
{
    Rig r;
    const TargetSpec &map = r.m.targets()[0];
    r.map_asked(false);
    r.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
    r.m.want(0, false, false);
    while (r.m.status(0).state != TargetState::Idle)
        r.tick();
    ok("released: unmapped and unfetched, not confirmed yet",
       calls_are("unmap 0x10004f0; unfetch 0x10004f0") && !r.port.confirmed(map));
    r.port.on_reply(dcsm::kReqUnmapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_reply(dcsm::kReqUnfetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_map_status(kMap, 1, 0, r.now);
    r.m.want(0, true, false);
    r.tick();
    ok("the replies alone do not confirm a fetched content's release: wanted again, not asked for",
       !r.port.confirmed(map) && calls_are(""));
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_REMOVED, r.now);
    r.tick();
    ok("the REMOVED answering the unfetch confirms it: asked for at once, no loss",
       r.port.confirmed(map) && calls_are("fetch 0x10004f0") && r.m.status(0).state == TargetState::Acquiring);

    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FAILED, r.now);
    ok("a fetch failed: unfetched", calls_are("unfetch 0x10004f0") && !r.port.confirmed(map));
    r.port.on_reply(dcsm::kReqUnfetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    ok("nothing was fetched: the unfetch's reply confirms it", r.port.confirmed(map));

    Rig e;
    const TargetSpec &emap = e.m.targets()[0];
    e.map_asked(false);
    e.port.on_map_status(kMap, 0, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, e.now);
    e.port.on_map_status(kMap, 1, dcsm::E_MAP_STATUS_UNMAPPED, e.now);
    ok("unmapped by the KOMBI: lost, unfetched", calls_are("unfetch 0x10004f0") && !e.port.confirmed(emap));
    e.port.on_reply(dcsm::kReqUnfetchContent, true, dcsm::E_DCSM_CONTENTID_UNKNOWN, "E_DCSM_CONTENTID_UNKNOWN",
                    e.now);
    ok("an unfetch the KOMBI refuses removes nothing: its reply confirms the release", e.port.confirmed(emap));
}

static void fresh_fetch_failed()
{
    Rig r;
    r.m.want(0, true, false);
    r.tick();
    r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    r.now += 12;
    r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FAILED, r.now);   /* 12 ms after the publication (the car) */
    const int64_t at = r.now;
    r.port.on_reply(dcsm::kReqUnfetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
    calls.clear();
    while (r.now - at < 240)
        r.tick();
    ok("FETCH FAILED right after the publication: nothing for 240 ms", calls_are(""));
    r.tick();
    r.tick();
    ok("then fetched again, 250 ms after the failure", calls_are("fetch 0x10004f0") && r.now - at <= 260);
}

/* A display that is starting accepts the fetch and the map and shows
 * nothing: the map is asked again every 500 ms, with no release in
 * between; 3 s after the acceptance the request is released and asked for
 * again (fetch and map), sooner than one the display never accepted. */
static int count(const std::string &call)
{
    int n = 0;
    for (const std::string &c : calls)
        n += c == call;
    return n;
}

static void map_again()
{
    const std::string map = "map 0x10004f0 class 0 slot 0x100";
    {
        Rig r;
        r.map_asked(false);
        for (int k = 0; k < 40; k++)
            r.tick();
        ok("map accepted, no status: nothing more for 400 ms", calls_are(""));
        for (int k = 0; k < 20; k++)
            r.tick();
        ok("then the map again, and only the map", calls_are(map));
        for (int k = 0; k < 230; k++)
            r.tick();
        ok("every 500 ms, 2.9 s on: still asked for, nothing released",
           count(map) == 4 && (int)calls.size() == 4 && r.m.status(0).state == TargetState::Acquiring);
        r.port.on_map_status(kMap, dcsm::E_MAP_STATUS_UNMAPPED, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
        ok("the display shows it: shown", r.m.status(0).state == TargetState::Shown);
        calls.clear();
        for (int k = 0; k < 200; k++)
            r.tick();
        ok("and no map after that", count(map) == 0);
    }
    {
        Rig r;
        r.map_asked(false);
        for (int k = 0; k < 290; k++)
            r.tick();
        ok("never shown: asked for 2.9 s", r.m.status(0).state == TargetState::Acquiring && count("unfetch 0x10004f0") == 0);
        for (int k = 0; k < 20; k++)
            r.tick();
        ok("3 s after the acceptance: released (unmapped and unfetched), waiting",
           r.m.status(0).state == TargetState::Waiting && r.m.status(0).reason == "accepted, not mapped" &&
               count("unmap 0x10004f0") == 1 && count("unfetch 0x10004f0") == 1 && count(map) == 5);
        r.port.on_reply(dcsm::kReqUnmapContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
        r.port.on_reply(dcsm::kReqUnfetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
        r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_REMOVED, r.now);
        calls.clear();
        for (int k = 0; k < 185; k++)   /* released 100 ms ago */
            r.tick();
        ok("not asked for again before the ladder's 2 s", count("fetch 0x10004f0") == 0);
        for (int k = 0; k < 15; k++)
            r.tick();
        ok("then fetched again, as at a start", calls_are("fetch 0x10004f0"));
    }
    {
        Rig r;
        r.m.want(0, true, false);
        r.tick();
        r.port.on_reply(dcsm::kReqFetchContent, true, dcsm::E_DCSM_OK, "E_DCSM_OK", r.now);
        r.port.on_fetch_status(kMap, dcsm::E_FETCH_STATUS_FETCHED, r.now);
        calls.clear();
        for (int k = 0; k < 495; k++)
            r.tick();
        ok("a map with no reply at all: not repeated", calls_are("") && r.m.status(0).state == TargetState::Acquiring);
        for (int k = 0; k < 10; k++)
            r.tick();
        ok("and given up after 5 s, as before", r.m.status(0).state == TargetState::Waiting);
    }
    {
        Rig r;
        r.map_asked(false);
        for (int k = 0; k < 60; k++)
            r.tick();
        r.port.on_reply(dcsm::kReqMapContent, true, dcsm::E_DCSM_CONTENT_ALREADYMAPPED, "E_DCSM_CONTENT_ALREADYMAPPED", r.now);
        calls.clear();
        for (int k = 0; k < 200; k++)
            r.tick();
        ok("a repeat refused: no further repeat, the first map still awaited",
           calls_are("") && r.m.status(0).state == TargetState::Acquiring);
        r.port.on_map_status(kMap, dcsm::E_MAP_STATUS_UNMAPPED, dcsm::E_MAP_STATUS_MAPPED_FOREGROUND, r.now);
        ok("and shown when its status comes", r.m.status(0).state == TargetState::Shown);
    }
    {
        Rig r;
        r.map_asked(false);
        SlotInfo off;
        off.slot = 0x100;
        off.status = 0;
        r.port.on_slots({off}, r.now);
        calls.clear();
        for (int k = 0; k < 100; k++)
            r.tick();
        ok("the slot reported disabled: no repeat", count(map) == 0);
    }
}

int main()
{
    basics();
    parked();
    take_over();
    diagnostics();
    confirmation();
    fresh_fetch_failed();
    map_again();
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
