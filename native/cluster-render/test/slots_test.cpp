/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for slot arbitration (slots.h): a slot is requested once while
 * any layer wants it and released only after none has for linger_ms, so
 * clients coming and going cause no display traffic; content of the stock
 * HMI is yielded to unless a layer contends, which retries on the 2/5/10/20/30
 * s ladder, reset after 30 s shown; content of anything else is always
 * yielded to; a slot the table names as held is not asked for; refusals and
 * no answer retry on the ladder; requests are spaced; the display service
 * going away takes every target down and its return maps the wanted ones
 * again after the settle time; two targets sharing a scene id are never up
 * together. With tables of one row per mapping, as the car reports them:
 * the holder is the row shown; a parked target stays mapped with no
 * timeout until shown; a contending target parked behind the stock HMI's
 * content unmaps it as the HMI, 1.5 s apart four times, then 10 s, then
 * every 30 s, the count starting over after 10 s shown or a release and the
 * spacing never under 1.5 s, brief shows not ending the back-off, never foreign content, never without a
 * contending layer, never in another slot; a released target is idle;
 * nothing is asked for while the table reports the slot disabled (for at
 * most 30 s after that report), a request left unanswered by a lock is
 * taken down when the slot is free again, and a locked slot coming back is
 * asked for at once. Warm scenes: a target's scene is
 * published once and kept across releases, losses and the display service
 * going; a target with the same scene id takes it down only when it needs
 * the id. After a release the next request waits for the display's
 * confirmation and the renderer's unsubscription, at most 1 s; a fetch failing right after the publication is
 * asked again after 250 ms, three times, then on the ladder; a scene the
 * display removed is asked for again 2 s after, and a fetch failing within
 * 300 ms of that request is asked again after 1.5 s, four times, then on
 * the ladder. Rendering: a
 * shown scene its renderer subscribed to is verified; one not subscribed
 * 1.5 s after it was shown, or after the renderer unsubscribed, is released
 * and asked for again at once, then on the ladder, its scene kept; with no
 * subscription seen since start, nothing is verified or done. Holds: asked
 * for only while the map is shown and released within three passes of it
 * not being shown; a take-over unmaps the shown stock content and the stock
 * contents mapped before ours or while ours was asked for, never those
 * mapped after it; the take-overs are counted; the HMI mapping its content
 * again behind ours is logged; rendering verification never releases a
 * hold; HMI content off the take-over list (the seat-belt panel) shown in
 * the slot makes it yield at once, and it is not asked for while that is
 * shown; parked there, it is never taken over or yielded to; the HMI
 * mapping the box again behind ours takes no second take-over; more than 3
 * losses within 60 s, not to a lock, give it up; a slot reported disabled
 * (a lock, or a slot the unit does not use) is not asked for, the hold
 * waiting as "slot locked", until it is enabled; after a door lock the hold
 * is asked for again and takes over again once the slot is free and the
 * map shown, at once again after a second lock; its take-overs' count
 * starts over after 10 s shown, not at a release, so the map coming and
 * going every 2 s keeps them on their steps; the map flapping spaces
 * requests 1 s and take-overs 1.5 s apart. A hold with its slot from the
 * table (guide_slots = auto) takes 0x103 or 0x105, whichever the table
 * reports the box in (each with the other disabled, as on two cars), and
 * before the box, 0x105 when it is enabled, else 0x103, else none, keeps
 * its slot when the box is unmapped or mapped in any other slot or a
 * disabled one, moves with it (released first, never two slots; each move
 * a loss, never a fault; a ladder wait carried over),
 * is one mapping to the guard in both slots (two drops in any of them
 * disable it, across boots), and yields to the seat-belt panel there; a
 * listed slot stays where it is. A loss
 * with the slot locked does not climb the ladder, and after a lock a
 * take-over target is asked for once the slot is not locked, with stock
 * content shown there or not. */
#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

#include "../src/guard.h"
#include "../src/log.h"
#include "../src/slots.h"

using namespace crs;

static int failures;
static std::vector<std::string> calls;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static std::string joined()
{
    std::string s;
    for (auto &c : calls) s += (s.empty() ? "" : "; ") + c;
    calls.clear();
    return s;
}

static bool calls_are(const std::string &want)
{
    const std::string got = joined();
    if (got == want)
        return true;
    std::printf("     got  \"%s\"\n     want \"%s\"\n", got.c_str(), want.c_str());
    return false;
}

struct FakePort : Port {
    bool up = true;
    bool confirm = true;
    bool available() const override { return up; }
    bool confirmed(const TargetSpec &) const override { return confirm; }
    void request(const TargetSpec &t) override { calls.push_back("request " + t.name); }
    void release(const TargetSpec &t) override { calls.push_back("release " + t.name); }
    void take_over(const TargetSpec &t, uint64_t content) override
    {
        char b[24];
        std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)content);
        calls.push_back("take over " + t.name + " " + b);
    }
    void refresh_slots() override { calls.push_back("slots?"); }
};

struct FakeScenes : Scenes {
    bool fail = false;
    bool publish(const TargetSpec &t, std::string *err) override
    {
        calls.push_back("publish " + t.name);
        if (fail) *err = "refused";
        return !fail;
    }
    void unpublish(const TargetSpec &t) override { calls.push_back("unpublish " + t.name); }
    int resends = 0;
    void resent(const TargetSpec &, int64_t) override { resends++; }
};

static const uint64_t kMapScene = 0x010004f0, kPanelScene = 0x01000400;
static const uint64_t kMapSlot = 0x100, kPanelSlot = 0x203, kHudSlot = 0x323;
static const uint64_t kStockMap = 0x01000101;   /* an HmiMain3D scene */
static const uint64_t kHmiMap = 0x01000300;     /* the HMI's map in Map view (VERIFIED) */

struct Rig {
    FakePort port;
    FakeScenes scenes;
    SlotManager m{&port, &scenes};
    int64_t now = 100000;
    Rig()
    {
        TargetSpec map;
        map.name = "map"; map.slot = kMapSlot; map.scene_id = kMapScene; map.width = 1088; map.height = 480;
        map.linger_ms = 2000;
        map.take_over_stock = true;
        TargetSpec panel;
        panel.name = "panel"; panel.slot = kPanelSlot; panel.scene_id = kPanelScene; panel.width = 1920;
        panel.height = 720; panel.linger_ms = 3000;
        TargetSpec hud;
        hud.name = "hud"; hud.slot = kHudSlot; hud.scene_id = kPanelScene; hud.width = 800; hud.height = 480;
        m.add(map);
        m.add(panel);
        m.add(hud);
    }
    void run(int64_t ms, int64_t step = 10)
    {
        for (int64_t t = 0; t < ms; t += step) {
            now += step;
            m.tick(now);
        }
    }
    /* A report of one row: occupied, its content is shown. */
    void slot(uint64_t id, uint32_t status, uint64_t content)
    {
        SlotInfo s;
        s.slot = id; s.status = status; s.content = content; s.map_status = status == 2 ? 1 : 0;
        m.on_slots({s}, now);
    }
    /* A report of one slot as the car gives it: one row per mapping
     * (content, map status), the slot's status in each. */
    void rows(uint64_t id, uint32_t status, std::vector<std::pair<uint64_t, uint32_t>> mappings)
    {
        std::vector<SlotInfo> t;
        for (const auto &mp : mappings) {
            SlotInfo s;
            s.slot = id; s.status = status; s.content = mp.first; s.map_status = mp.second;
            t.push_back(s);
        }
        m.on_slots(t, now);
    }
    TargetState state(int i) const { return m.status(i).state; }
};

static void refcount_and_linger()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    ok("a wanted target is published and requested", calls_are("publish map; request map") &&
                                                         r.state(0) == TargetState::Acquiring);
    r.m.on_shown(kMapScene, r.now);
    ok("and shown once the display says so", r.state(0) == TargetState::Shown && r.m.published(0));
    /* A second client, then the first leaving: still wanted (the compositor
     * ORs its layers), so nothing reaches the display. */
    r.m.want(0, true, false);
    r.run(5000);
    ok("layers coming and going while one wants it: no display traffic", calls_are(""));
    r.m.want(0, false, false);
    r.run(1000);
    ok("the last layer gone: lingering, still mapped", r.state(0) == TargetState::Lingering && calls_are(""));
    r.m.want(0, true, false);
    r.m.tick(r.now);
    ok("wanted again within the linger: shown at once, no request",
       r.state(0) == TargetState::Shown && calls_are(""));
    r.m.want(0, false, false);
    r.run(1990);
    ok("not released before linger_ms", r.state(0) == TargetState::Lingering && calls_are(""));
    r.run(20);
    ok("released after linger_ms: released, its scene kept published", calls_are("release map") &&
           r.state(0) == TargetState::Idle && !r.m.requested(0) && r.m.published(0));
}

static void timeout_and_ladder()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    joined();
    r.run(4990);
    ok("no answer: still acquiring before 5 s", r.state(0) == TargetState::Acquiring);
    r.run(20);
    ok("no answer for 5 s: taken down, waiting 2 s",
       calls_are("release map") && r.state(0) == TargetState::Waiting &&
           r.m.status(0).retry_ms == r.now + 2000 - 10);
    r.run(1980);
    ok("nothing before the 2 s retry", calls_are(""));
    r.run(20);
    ok("requested again after 2 s", calls_are("request map"));
    r.m.on_refused(kMapScene, Refusal::NoSlot, "E_DCSM_SLOTID_UNKNOWN", r.now);
    ok("a configuration refusal retries on the ladder (5 s)",
       calls_are("release map") && r.m.status(0).retry_ms == r.now + 5000 &&
           r.m.status(0).holder == Holder::None);
    const int64_t at = r.now;
    r.run(4000);
    r.m.on_shown(kMapScene, r.now);
    ok("a late answer to a request taken down is ignored", r.state(0) == TargetState::Waiting);
    r.run(1000);
    ok("next request at the 5 s step", calls_are("request map") && r.now - at == 5000);
}

static void stock_yield()
{
    Rig r;
    r.m.want(1, true, false);   /* an overlay: yields */
    r.m.tick(r.now);
    r.m.on_shown(kPanelScene, r.now);
    joined();
    /* The stock HMI maps its speech dialog over ours. */
    r.slot(kPanelSlot, 2, 0x01000400 + 1);
    r.m.on_lost(kPanelScene, "fetch removed", r.now);
    ok("taken by the stock HMI: down, waiting for the slot to be free",
       calls_are("release panel") && r.state(1) == TargetState::Waiting &&
           r.m.status(1).holder == Holder::Stock && r.m.status(1).retry_ms == -1);
    r.run(60000);
    const std::string polled = joined();
    ok("no request while the stock HMI holds it, the table polled every 5 s",
       polled.find("request") == std::string::npos && polled.find("slots?") != std::string::npos &&
           polled.size() <= std::string("slots?; ").size() * 12);
    r.slot(kPanelSlot, 1, 0);
    r.m.tick(r.now);
    ok("requested again once the slot is free", calls_are("request panel"));
}

static void stock_contend()
{
    Rig r;
    r.m.want(0, true, true);   /* the Waze map contends */
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    joined();
    const int64_t steps[] = {2000, 5000, 10000, 20000, 30000, 30000};
    bool ladder = true;
    for (int64_t step : steps) {
        r.slot(kMapSlot, 2, kStockMap);
        r.m.on_lost(kMapScene, "fetch removed", r.now);
        joined();
        const int64_t at = r.now;
        r.run(step - 10);
        ladder = ladder && calls_are("");
        r.run(10);
        ladder = ladder && calls_are("request map") && r.now - at == step;
        r.m.on_shown(kMapScene, r.now);
        r.run(1000);
    }
    ok("contending with the stock map: retries after 2, 5, 10, 20, 30, 30 s", ladder);
    r.run(30000);
    r.slot(kMapSlot, 2, kStockMap);
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    joined();
    ok("30 s shown resets the ladder to 2 s", r.m.status(0).retry_ms == r.now + 2000);
}

static void foreign_always_yields()
{
    Rig r;
    r.slot(kMapSlot, 2, 0x00007777);   /* content of an app outside the service */
    r.m.want(0, true, true);
    r.m.tick(r.now);
    ok("a slot held by a foreign app is not asked for, even contending",
       calls_are("slots?") && r.state(0) == TargetState::Waiting && r.m.status(0).holder == Holder::Foreign);
    r.run(20000);
    ok("still not asked for while it holds it", joined().find("request") == std::string::npos);
    r.slot(kMapSlot, 1, 0);
    r.m.tick(r.now);
    ok("asked for once it is free", calls_are("publish map; request map"));
}

static void idle_precheck()
{
    Rig r;
    r.slot(kPanelSlot, 2, 0x01000400);   /* our panel scene id, but not ours: the speech dialog */
    r.m.want(1, true, false);
    r.m.tick(r.now);
    ok("our scene id in the slot while we publish nothing is the stock HMI's: yield",
       r.state(1) == TargetState::Waiting && r.m.status(1).holder == Holder::Stock &&
           joined().find("request") == std::string::npos);
    r.m.want(1, true, true);
    r.m.tick(r.now);
    ok("a contending layer arriving while yielding retries on the ladder",
       r.m.status(1).retry_ms == r.now + 2000);
    r.m.want(1, false, false);
    r.run(2000);
    ok("nobody wants it when the timer passes: idle", r.state(1) == TargetState::Idle && calls_are(""));
}

static void busy_refusal_unknown_holder()
{
    Rig r;
    r.m.want(1, true, false);
    r.m.tick(r.now);
    joined();
    r.m.on_refused(kPanelScene, Refusal::Busy, "E_DCSM_PRIOTOOLOW", r.now);
    ok("a busy refusal with no table: held by the stock HMI, yield",
       calls_are("release panel") && r.m.status(1).holder == Holder::Stock &&
           r.m.status(1).retry_ms == -1);
    r.run(100);
    ok("the slot table is asked for", calls_are("slots?"));
}

static void availability()
{
    Rig r;
    r.m.want(0, true, true);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    joined();
    r.port.up = false;
    r.m.on_availability(false, r.now);
    r.m.tick(r.now);
    ok("display service gone: nothing sent, the scene kept, unavailable",
       calls_are("") && r.state(0) == TargetState::Unavailable && r.m.published(0) && !r.m.requested(0));
    r.run(10000);
    ok("nothing while it is gone", calls_are(""));
    r.port.up = true;
    r.m.on_availability(true, r.now);
    const int64_t at = r.now;
    r.run(990);
    ok("back: nothing during the settle time", calls_are("") && r.state(0) == TargetState::Idle);
    r.run(20);
    ok("requested again after it", calls_are("request map") && r.now - at >= 1000);
    r.m.want(1, false, false);
    ok("an unwanted target stays idle throughout", r.state(1) == TargetState::Idle);
}

static void shared_scene_id()
{
    Rig r;
    r.m.want(1, true, false);
    r.m.tick(r.now);
    r.m.on_shown(kPanelScene, r.now);
    joined();
    r.m.want(2, true, false);   /* the HUD has the panel's scene id */
    r.run(5000);
    ok("a second target with a published scene id waits",
       r.state(2) == TargetState::Waiting && joined().find("hud") == std::string::npos);
    r.m.want(1, false, false);
    r.run(3100);
    const std::string c = joined();
    ok("and is requested once the first is idle",
       c.find("release panel") != std::string::npos &&
           c.find("publish hud; request hud") != std::string::npos);
}

static void request_gap()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    r.m.on_refused(kMapScene, Refusal::Transient, "E_FETCH_STATUS_FAILED", r.now);
    joined();
    r.m.want(0, false, false);
    r.run(2000);
    r.m.want(0, true, false);
    r.m.tick(r.now);
    ok("after a refusal the ladder is not bypassed by the demand flapping",
       calls_are("request map") && r.now >= 100000 + 2000);
    r.m.on_refused(kMapScene, Refusal::Transient, "E_FETCH_STATUS_FAILED", r.now);
    joined();
    r.m.want(0, false, false);
    r.run(4000);
    r.m.want(0, true, false);
    r.run(500);
    ok("the next step is still 5 s", calls_are(""));
}

static void lost_while_lingering()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    r.m.want(0, false, false);
    r.m.tick(r.now);
    joined();
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    ok("lost while lingering: down and idle, no retry", calls_are("release map") &&
                                                           r.state(0) == TargetState::Idle);
}

static void scene_failure()
{
    Rig r;
    r.scenes.fail = true;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    ok("a scene that cannot be published is not requested, and retried on the ladder",
       calls_are("publish map") && r.state(0) == TargetState::Waiting && r.m.status(0).retry_ms == r.now + 2000);
}

static void ladder_rechecks_the_table()
{
    Rig r;
    r.m.want(0, true, true);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    r.slot(kMapSlot, 2, kStockMap);
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    joined();
    r.m.want(0, true, false);   /* the contending layer goes; a yielding one stays */
    r.run(3000);
    ok("on the ladder, the contending layer gone: the stock map's slot is not asked for",
       joined().find("request") == std::string::npos && r.m.status(0).holder == Holder::Stock &&
           r.m.status(0).retry_ms == -1);
}

static void lost_to_our_own_id()
{
    std::vector<std::string> evidence;
    auto shown_panel = [&](Rig &r) {
        r.m.on_evidence = [&](Evidence e, const std::string &, int64_t) { evidence.push_back(evidence_name(e)); };
        r.m.want(1, true, false);
        r.m.tick(r.now);
        r.m.on_shown(kPanelScene, r.now);
        r.slot(kPanelSlot, 2, kPanelScene);   /* the table names our mapping */
        joined();
    };
    Rig a;
    shown_panel(a);
    a.m.on_lost(kPanelScene, "fetch removed", a.now);
    ok("lost while the table names our id: undecided, the table asked for, no evidence yet",
       a.m.status(1).state == TargetState::Waiting && evidence.empty() &&
           joined() == "release panel; slots?");
    a.slot(kPanelSlot, 2, kPanelScene);   /* after our unfetch it still names that id */
    a.m.tick(a.now);
    ok("the next report still names it: the stock HMI's (the speech dialog), yielded to, no evidence",
       a.m.status(1).holder == Holder::Stock && a.m.status(1).retry_ms == -1 && evidence.empty());

    Rig b;
    shown_panel(b);
    b.m.on_lost(kPanelScene, "fetch removed", b.now);
    b.slot(kPanelSlot, 1, 0);
    b.m.tick(b.now);
    ok("the next report shows it free: the loss was ours, evidence, and a retry after the removal wait",
       evidence.size() == 1 && evidence[0] == std::string("scene removed") && b.m.status(1).retry_ms == b.now + SlotManager::kRemovalWaitMs);

    Rig c;
    evidence.clear();
    shown_panel(c);
    c.m.on_lost(kPanelScene, "fetch removed", c.now);
    c.run(990);
    const bool waited = evidence.empty();
    c.run(20);
    ok("no report within 1 s: the loss was ours", waited && evidence.size() == 1);
    ok("and the wait counts from the loss", c.m.status(1).retry_ms == c.now - 1010 + SlotManager::kRemovalWaitMs);

    Rig d;
    std::vector<std::string> ev2;
    d.m.on_evidence = [&](Evidence e, const std::string &, int64_t) { ev2.push_back(evidence_name(e)); };
    d.m.want(0, true, false);
    d.m.tick(d.now);
    d.m.on_shown(kMapScene, d.now);
    d.slot(kMapSlot, 1, kMapScene);   /* a removal: the slot available, our content still named */
    d.m.on_lost(kMapScene, "fetch removed", d.now);
    ok("removed with the slot shown available: evidence at once, and the removal wait",
       ev2.size() == 1 && d.m.status(0).retry_ms == d.now + SlotManager::kRemovalWaitMs);
}

static void lock_is_not_evidence()
{
    Rig r;
    std::vector<std::string> ev;
    r.m.on_evidence = [&](Evidence e, const std::string &, int64_t) { ev.push_back(evidence_name(e)); };
    r.m.want(0, true, true);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    joined();
    r.slot(kMapSlot, 0, kMapScene);   /* a door-open warning locks the slot */
    r.m.on_lost(kMapScene, "unmapped", r.now);
    r.run(1500);
    ok("a scene the display takes with a lock is not upload evidence", ev.empty());
    r.slot(kMapSlot, 1, 0);
    r.run(1100);
    r.m.on_shown(kMapScene, r.now);
    joined();
    r.slot(kMapSlot, 1, 0);           /* removed with the slot free: the link */
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    ok("one removed with the slot free still is", ev.size() == 1 && ev[0] == std::string("scene removed"));

    Rig l;   /* lingering: the last layer stopped wanting it */
    std::vector<std::string> evl;
    l.m.on_evidence = [&](Evidence e, const std::string &, int64_t) { evl.push_back(evidence_name(e)); };
    l.m.want(1, true, false);
    l.m.tick(l.now);
    l.m.on_shown(kPanelScene, l.now);
    l.m.want(1, false, false);
    l.run(100);
    l.slot(kPanelSlot, 0, kPanelScene);
    l.m.on_lost(kPanelScene, "unmapped", l.now);
    ok("a lock while lingering is not evidence either", evl.empty());
}

static void slot_comes_back()
{
    Rig r;
    r.m.want(0, true, true);   /* contending, like the Waze map */
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    joined();
    /* Three locks in a row. */
    for (int k = 0; k < 3; k++) {
        r.slot(kMapSlot, 0, kMapScene);   /* a door-open warning disables the slot */
        r.m.on_lost(kMapScene, "unmapped", r.now);
        r.slot(kMapSlot, 1, 0);
        r.run(1100);
        r.m.on_shown(kMapScene, r.now);
        joined();
    }
    r.slot(kMapSlot, 0, kMapScene);
    r.m.on_lost(kMapScene, "unmapped", r.now);
    const int64_t lost = r.now;
    ok("a fourth lock in a row: the ladder not climbed, 2 s", r.m.status(0).retry_ms == lost + 2000);
    joined();
    r.run(3000);   /* one report, before the loss: the display reports a slot only when it changes */
    ok("while the slot stays disabled, nothing is asked for", joined().find("request") == std::string::npos);
    r.slot(kMapSlot, 1, 0);           /* the door closed: the slot is free again */
    r.run(1100);
    ok("the slot free again after it was away: asked for at once",
       joined().find("request map") != std::string::npos && r.now - lost < 5000);

    Rig v;
    v.m.want(0, true, true);
    v.m.tick(v.now);
    v.m.on_shown(kMapScene, v.now);
    v.slot(kMapSlot, 2, kMapScene);   /* the table names our mapping */
    joined();
    v.m.on_lost(kMapScene, "unmapped", v.now);   /* undecided: the table is asked for */
    v.slot(kMapSlot, 0, kMapScene);   /* its answer: disabled */
    v.run(20);
    v.slot(kMapSlot, 1, 0);
    v.run(1100);
    ok("a loss decided by the next report, the slot disabled then free: asked for at once",
       joined().find("request map") != std::string::npos);

    /* The table as the car reports slot 0x100: our entry and the stock
     * map's, with one slot status. */
    auto table = [](uint32_t status, uint32_t ours_map, uint32_t stock_map) {
        SlotInfo o, h;
        o.slot = h.slot = kMapSlot;
        o.status = h.status = status;
        o.content = kMapScene;
        h.content = 0x01000300;
        o.map_status = ours_map;
        h.map_status = stock_map;
        return std::vector<SlotInfo>{o, h};
    };
    Rig e;
    e.m.want(0, true, true);
    e.m.tick(e.now);
    e.m.on_shown(kMapScene, e.now);
    e.m.on_slots(table(2, 1, 0), e.now);   /* ours mapped, the stock entry listed but not mapped */
    joined();
    e.m.on_lost(kMapScene, "unmapped", e.now);
    e.m.on_slots(table(1, 0, 0), e.now);
    e.run(1500);
    ok("a stock entry listed beside ours but not mapped is not the slot away: the removal wait",
       joined().find("request") == std::string::npos);
    e.run(3600);
    e.m.on_shown(kMapScene, e.now);
    e.m.on_slots(table(2, 0, 1), e.now);   /* the stock map mapped over ours */
    joined();
    e.m.on_lost(kMapScene, "unmapped", e.now);
    e.m.on_slots(table(1, 0, 0), e.now);   /* and gone again */
    e.run(1100);
    ok("the stock map mapped there and gone again: asked for at once",
       joined().find("request map") != std::string::npos);

    /* A lock of 0x100 with everything unmapped, then the slot back straight
     * to occupied with the HMI's map shown (the sequence the car reported for
     * 0x105 after a door lock). */
    Rig s;
    s.m.want(0, true, true);
    s.m.tick(s.now);
    s.m.on_shown(kMapScene, s.now);
    s.run(15000);
    for (int k = 0; k < 2; k++) {
        joined();
        s.rows(kMapSlot, 0, {{kMapScene, 0}, {kHmiMap, 0}});
        s.m.on_lost(kMapScene, "unmapped", s.now);
        s.run(500);
        s.rows(kMapSlot, 2, {{kHmiMap, 1}});
        s.run(10);
        ok(k ? "a second lock 15 s later: asked for at once again"
             : "the slot back 0 -> 2 with the stock map shown: a take-over target asked for at once",
           calls_are("release map; request map"));
        s.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
        s.m.on_parked(kMapScene, s.now);
        s.run(10);
        s.rows(kMapSlot, 2, {{kMapScene, 1}, {kHmiMap, 3}});
        s.m.on_shown(kMapScene, s.now);
        s.run(15000);
    }

    Rig q;
    q.m.want(0, true, true);
    q.m.tick(q.now);
    q.m.on_shown(kMapScene, q.now);
    joined();
    q.slot(kMapSlot, 1, 0);           /* the display drops our scene and reports the slot free */
    q.m.on_lost(kMapScene, "unmapped", q.now);
    q.slot(kMapSlot, 1, 0);
    q.run(1500);
    ok("a slot reported free without having been away waits for the ladder",
       joined().find("request") == std::string::npos && q.m.status(0).retry_ms > q.now);
}

static void multi_row_holder()
{
    Rig a;
    /* The stock map shown, ours still listed (unmapped) after it: the last
     * row is not the holder. */
    a.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 0}});
    a.m.want(0, true, false);
    a.m.tick(a.now);
    ok("rows like the car's: the shown row holds the slot, whatever comes last",
       a.state(0) == TargetState::Waiting && a.m.status(0).holder == Holder::Stock &&
           joined().find("request") == std::string::npos);
    a.rows(kMapSlot, 1, {{kHmiMap, 0}, {kMapScene, 0}});
    a.m.tick(a.now);
    ok("every row listed unmapped: free, asked for", calls_are("publish map; request map"));

    Rig b;
    b.rows(kMapSlot, 2, {{0x00007777, 0}, {kHmiMap, 1}});
    b.m.want(0, true, true);
    b.m.tick(b.now);
    ok("a foreign row listed but not shown does not hold it: the stock map does, and is contended",
       calls_are("publish map; request map"));

    Rig c;
    c.rows(kMapSlot, 2, {{kHmiMap, 3}, {0x00007777, 1}});
    c.m.want(0, true, true);
    c.m.tick(c.now);
    ok("a foreign row shown, the stock map parked: foreign holds it, yielded to even contending",
       c.m.status(0).holder == Holder::Foreign && joined().find("request") == std::string::npos);
}

/* The map requested and parked behind the stock map: (ours, 3), (stock, 1). */
static void park(Rig &r, bool contend, uint64_t holder = kHmiMap)
{
    r.m.want(0, true, contend);
    r.m.tick(r.now);
    r.rows(kMapSlot, 2, {{holder, 1}, {kMapScene, 3}});
    r.m.on_parked(kMapScene, r.now);
}

static void parked_then_shown()
{
    Rig r;
    park(r, false);
    ok("map status 3: parked behind the stock HMI, the table asked for",
       r.state(0) == TargetState::Parked && r.m.status(0).holder == Holder::Stock &&
           r.m.status(0).reason == "parked behind stock" && r.m.published(0) &&
           calls_are("publish map; request map; slots?"));
    r.run(30000);
    const std::string c = joined();
    ok("parked for 30 s: no timeout, no release, no request, no take-over without a contending layer",
       r.state(0) == TargetState::Parked && c.find("release") == std::string::npos &&
           c.find("request") == std::string::npos && c.find("take over") == std::string::npos);
    r.rows(kMapSlot, 2, {{kHmiMap, 0}, {kMapScene, 1}});
    r.m.on_shown(kMapScene, r.now);
    ok("the stock map unmapped: ours shown (3 -> 1)", r.state(0) == TargetState::Shown && calls_are(""));
    r.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
    r.m.on_parked(kMapScene, r.now);
    ok("shown, then 1 -> 3: parked again, still mapped", r.state(0) == TargetState::Parked &&
                                                            calls_are("slots?") && r.m.published(0));
    r.m.want(0, false, false);
    r.m.tick(r.now);
    ok("parked and nobody wants it: released at once, idle", calls_are("release map") &&
                                                                r.state(0) == TargetState::Idle);

    Rig u;
    u.m.want(0, true, false);
    u.m.tick(u.now);
    joined();
    u.m.on_parked(kMapScene, u.now);
    ok("parked before the table names a holder: parked, holder unknown",
       u.state(0) == TargetState::Parked && u.m.status(0).holder == Holder::None &&
           u.m.status(0).reason == "parked");
    u.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
    ok("the table names it: parked behind stock", u.m.status(0).holder == Holder::Stock &&
                                                     u.m.status(0).reason == "parked behind stock");
}

static void take_over()
{
    Rig r;
    park(r, true);
    joined();
    r.m.tick(r.now);
    ok("contending, parked behind the stock map: it is unmapped as the HMI at once",
       calls_are("take over map 0x1000300; slots?"));
    ok("the next take-over wakes the loop", r.m.wake_in_ms(r.now) == 1500);
    /* The stock map stays on top: the take-overs back off. */
    const int64_t gaps[] = {1500, 1500, 1500, 1500, 10000, 30000, 30000};
    bool spaced = true;
    for (int64_t gap : gaps) {
        const int64_t at = r.now;
        r.run(gap - 10);
        spaced = spaced && joined().find("take over") == std::string::npos;
        r.run(10);
        spaced = spaced && joined().find("take over map 0x1000300") != std::string::npos && r.now - at == gap;
    }
    ok("still behind it: again after 1.5 s four times, then 10, 30, 30 s", spaced);
    r.rows(kMapSlot, 2, {{kHmiMap, 3}, {kMapScene, 1}});
    r.m.on_shown(kMapScene, r.now);
    ok("promoted: shown, the HMI's map parked behind ours", r.state(0) == TargetState::Shown);
    r.run(20000);
    r.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
    r.m.on_parked(kMapScene, r.now);
    joined();
    r.m.tick(r.now);
    ok("on top again after 20 s shown: the count started over after 10 s shown, at once",
       joined().find("take over map 0x1000300") != std::string::npos);
    r.run(1490);
    const bool quiet = joined().find("take over") == std::string::npos;
    r.run(10);
    ok("and 1.5 s to the next", quiet && joined().find("take over map 0x1000300") != std::string::npos);

    Rig s;
    park(s, true);
    s.m.tick(s.now);
    s.run(1500);
    const int64_t second = s.now;
    ok("two take-overs 1.5 s apart", joined().find("take over map 0x1000300; slots?; take over") !=
                                         std::string::npos);
    s.m.want(0, false, false);
    s.m.tick(s.now);
    s.run(500);
    park(s, true);   /* the stream back, the stock map shown again because of our release */
    joined();
    s.run(990);
    const bool floor = joined().find("take over") == std::string::npos;
    s.run(20);
    ok("released and asked for again: the count starts over, still 1.5 s after the last take-over",
       floor && joined().find("take over map 0x1000300") != std::string::npos && s.now - second <= 1510);

    Rig h;
    park(h, true);
    h.m.tick(h.now);
    const int64_t first = h.now;
    h.run(100);
    h.rows(kMapSlot, 2, {{kHmiMap, 3}, {kMapScene, 1}});
    h.m.on_shown(kMapScene, h.now);
    h.run(100);
    h.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
    h.m.on_parked(kMapScene, h.now);
    joined();
    h.run(1290);
    const bool held = joined().find("take over") == std::string::npos;
    h.run(20);
    ok("shown and parked again at once: the next take-over still 1.5 s after the last",
       held && joined().find("take over map 0x1000300") != std::string::npos && h.now - first <= 1510);

    Rig f;
    park(f, true, 0x00007777);
    f.run(60000);
    ok("parked behind foreign content: never unmapped", joined().find("take over") == std::string::npos &&
                                                           f.state(0) == TargetState::Parked);

    Rig n;
    park(n, false);
    n.run(10000);
    n.m.want(0, true, true);
    n.m.tick(n.now);
    ok("no take-over until a layer contends; then at once",
       joined().find("take over map 0x1000300") != std::string::npos);

    Rig o;
    o.m.want(0, true, true);
    o.m.tick(o.now);
    o.rows(kPanelSlot, 2, {{kHmiMap, 1}});   /* the stock map shown in another slot */
    o.rows(kMapSlot, 2, {{kMapScene, 3}});
    o.m.on_parked(kMapScene, o.now);
    o.run(10000);
    ok("stock content shown only in another slot: never unmapped", joined().find("take over") == std::string::npos);

    Rig p;
    p.m.want(1, true, true);   /* the panel does not take over */
    p.m.tick(p.now);
    p.rows(kPanelSlot, 2, {{0x01000401, 1}, {kPanelScene, 3}});
    p.m.on_parked(kPanelScene, p.now);
    p.run(10000);
    ok("a target without take_over_stock never unmaps", joined().find("take over") == std::string::npos &&
                                                           p.state(1) == TargetState::Parked);
}

/* The map shown, then a lock (a door-open or check-control warning):
 * every mapping in the slot unmapped, the slot disabled. */
static void lock_map(Rig &r)
{
    r.rows(kMapSlot, 0, {{kHmiMap, 3}, {kMapScene, 1}});
    r.m.on_lost(kMapScene, "unmapped", r.now);
}

/* The lock gone: our request goes, the HMI's map (still fetched) is mapped
 * first and ours parks behind it. */
static void lift_and_park(Rig &r)
{
    r.rows(kMapSlot, 1, {{kHmiMap, 0}, {kMapScene, 0}});
    r.run(10);
    r.rows(kMapSlot, 2, {{kHmiMap, 1}});
    r.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
    r.m.on_parked(kMapScene, r.now);
}

/* Car log 2026-11-03 06:52 (A): an 8.2 s lock, then the stock map back on
 * top after four take-overs in a row; the fifth shows ours. */
static void lock_then_take_overs()
{
    Rig r;
    r.m.want(0, true, true);
    r.m.tick(r.now);
    r.rows(kMapSlot, 2, {{kMapScene, 1}, {kHmiMap, 3}});
    r.m.on_shown(kMapScene, r.now);
    joined();
    lock_map(r);
    r.run(8200);
    const std::string locked = joined();
    ok("an 8.2 s lock: taken down, nothing asked for while the slot is disabled",
       locked.find("release map") != std::string::npos && locked.find("request") == std::string::npos &&
           r.m.status(0).reason == "slot locked");
    lift_and_park(r);
    ok("the lock gone: asked for at once", joined().find("request map") != std::string::npos);
    r.m.tick(r.now);
    const int64_t parked = r.now;
    int taken = 0;
    while (taken < 5 && r.now - parked < 60000) {
        if (joined().find("take over map 0x1000300") != std::string::npos && ++taken == 5) {
            r.rows(kMapSlot, 2, {{kHmiMap, 3}, {kMapScene, 1}});
            r.m.on_shown(kMapScene, r.now);
        }
        r.run(10);
    }
    ok("the stock map back on top four times, the fifth take-over shows ours within 6.1 s of the park",
       r.state(0) == TargetState::Shown && r.now - parked <= 6100);
    r.run(40000);
    ok("shown: no more take-overs", joined().find("take over") == std::string::npos);
}

/* The display shows ours and parks it again within moments, over and over:
 * the take-overs keep backing off. */
static void brief_shows_back_off()
{
    Rig r;
    park(r, true);
    joined();
    std::vector<int64_t> at;
    for (int64_t t = 0; t < 45000; t += 10) {
        if (joined().find("take over map 0x1000300") != std::string::npos) {
            at.push_back(r.now);
            r.run(200);
            r.rows(kMapSlot, 2, {{kHmiMap, 3}, {kMapScene, 1}});
            r.m.on_shown(kMapScene, r.now);
            r.run(100);
            r.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
            r.m.on_parked(kMapScene, r.now);
        }
        r.run(10);
    }
    const int64_t want[] = {1500, 1500, 1500, 1500, 10000, 30000};
    bool ladder = at.size() == 7;
    for (size_t k = 0; ladder && k < 6; k++)
        ladder = at[k + 1] - at[k] == want[k];
    ok("shown 200 ms, then parked again, each time: 1.5 s four times, then 10, then 30 s", ladder);
}

/* Car log 2026-11-03 07:00 (B, C): shown 18 s after a take-over, then
 * another lock: its take-overs start from the first step again, whatever
 * step the last run reached. */
static void take_overs_restart_when_shown()
{
    Rig r;
    r.m.want(0, true, true);
    r.m.tick(r.now);
    r.rows(kMapSlot, 2, {{kHmiMap, 1}, {kMapScene, 3}});
    r.m.on_parked(kMapScene, r.now);
    r.m.tick(r.now);
    r.run(6000);   /* four lost the race, the fifth worked */
    r.rows(kMapSlot, 2, {{kHmiMap, 3}, {kMapScene, 1}});
    r.m.on_shown(kMapScene, r.now);
    r.run(18000);
    joined();
    lock_map(r);
    r.run(2000);
    lift_and_park(r);
    joined();
    r.m.tick(r.now);
    ok("parked behind the stock map after the next lock: a take-over at once",
       joined().find("take over map 0x1000300") != std::string::npos);
    r.run(1490);
    const bool quiet = joined().find("take over") == std::string::npos;
    r.run(10);
    ok("and the next 1.5 s later", quiet && joined().find("take over map 0x1000300") != std::string::npos);
}

/* Car log 2026-11-01 13:49: a 6 s lock with the map's 2 s retry due in it.
 * Nothing is asked for while the slot is reported disabled; the map is
 * asked for on the first tick the slot is reported free. */
static void no_request_while_locked()
{
    Rig r;
    r.m.want(0, true, true);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    joined();
    lock_map(r);
    r.run(4000);
    ok("the 2 s retry due while the slot is disabled: not asked for; the table asked for at the loss, polled at the retry",
       calls_are("release map; slots?; slots?") && r.state(0) == TargetState::Waiting);
    r.rows(kMapSlot, 1, {{kHmiMap, 0}});
    r.run(10);
    ok("the slot free again: asked for on the next tick", calls_are("request map"));

    Rig a;   /* a request out when the lock comes */
    a.m.want(0, true, true);
    a.m.tick(a.now);
    joined();
    a.run(100);
    a.rows(kMapSlot, 0, {{kHmiMap, 0}});
    a.run(2000);
    ok("a request out when the slot is locked: still acquiring", a.state(0) == TargetState::Acquiring);
    a.rows(kMapSlot, 1, {{kHmiMap, 0}});
    ok("the slot free again: that request taken down", calls_are("release map"));
    a.run(10);
    ok("and asked for again at once, not after the 5 s timeout", calls_are("request map"));
    a.rows(kMapSlot, 1, {{kHmiMap, 0}});
    a.run(10);
    ok("a request sent with the slot free is left alone", calls_are("") && a.state(0) == TargetState::Acquiring);

    Rig d;   /* a removal, then a lock before its 2 s wait is over */
    d.m.want(0, true, false);
    d.m.tick(d.now);
    d.m.on_shown(kMapScene, d.now);
    d.slot(kMapSlot, 1, 0);
    d.m.on_lost(kMapScene, "fetch removed", d.now);
    d.run(1000);
    d.slot(kMapSlot, 0, 0);
    joined();
    d.run(3000);
    ok("a removal's retry falls due while the slot is disabled: not asked for",
       joined().find("request") == std::string::npos);
    d.slot(kMapSlot, 1, 0);
    d.run(10);
    ok("free again: asked for at once", calls_are("request map"));

    Rig i;   /* wanted while the slot is disabled */
    i.slot(kMapSlot, 0, 0);
    i.m.want(0, true, false);
    i.m.tick(i.now);
    ok("a target wanted while its slot is disabled waits, slot locked",
       joined().find("request") == std::string::npos && i.state(0) == TargetState::Waiting &&
           i.m.status(0).reason == "slot locked");

    Rig k;   /* the display reports the lock, then nothing more */
    k.m.want(0, true, true);
    k.m.tick(k.now);
    k.m.on_shown(kMapScene, k.now);
    lock_map(k);
    const int64_t reported = k.now;
    joined();
    k.run(SlotManager::kLockTrustMs - 10);
    const bool held = joined().find("request") == std::string::npos;
    k.run(20);
    ok("no report since the lock: asked for again once that report is 30 s old",
       held && joined().find("request map") != std::string::npos && k.now - reported <= SlotManager::kLockTrustMs + 10);

    Rig p;   /* a lock reported again by each poll is trusted */
    p.m.want(0, true, true);
    p.m.tick(p.now);
    p.m.on_shown(kMapScene, p.now);
    lock_map(p);
    joined();
    for (int n = 0; n < 12; n++) {
        p.run(5000);
        p.rows(kMapSlot, 0, {{kHmiMap, 0}, {kMapScene, 0}});
    }
    ok("a lock each poll still reports: nothing asked for in 60 s", joined().find("request") == std::string::npos);
}

static void fallback_and_lock()
{
    Rig r;
    park(r, true);
    r.m.tick(r.now);
    r.rows(kMapSlot, 2, {{kHmiMap, 3}, {kMapScene, 1}});
    r.m.on_shown(kMapScene, r.now);
    joined();
    r.m.want(0, false, false);
    r.run(2010);
    ok("the stream ended: released after the linger, idle (the display shows the parked stock map)",
       calls_are("release map") && r.state(0) == TargetState::Idle);
    r.rows(kMapSlot, 2, {{kHmiMap, 1}});
    r.m.want(0, true, false);
    r.m.tick(r.now);
    ok("then a yielding layer: the stock map's slot is not asked for",
       r.state(0) == TargetState::Waiting && r.m.status(0).holder == Holder::Stock);

    Rig l;
    l.m.want(0, true, true);
    l.m.tick(l.now);
    l.rows(kMapSlot, 2, {{kMapScene, 1}, {kHmiMap, 3}});
    l.m.on_shown(kMapScene, l.now);
    joined();
    /* A door opens: the slot locked, every mapping unmapped. */
    l.rows(kMapSlot, 0, {{kMapScene, 0}, {kHmiMap, 0}});
    l.m.on_lost(kMapScene, "unmapped", l.now);
    l.run(500);
    ok("a lock: down, waiting on the ladder", l.state(0) == TargetState::Waiting &&
                                                 joined().find("request") == std::string::npos);
    l.rows(kMapSlot, 1, {{kHmiMap, 0}});
    l.run(600);
    ok("the lock gone: asked for at once", calls_are("request map") && l.m.status(0).retry_ms < 0);

    Rig q;
    park(q, false);
    joined();
    q.rows(kMapSlot, 0, {{kHmiMap, 0}, {kMapScene, 0}});
    q.m.on_lost(kMapScene, "unmapped", q.now);
    ok("a lock while parked: down and waiting", calls_are("release map") &&
                                                   q.state(0) == TargetState::Waiting);
    q.rows(kMapSlot, 1, {{kHmiMap, 0}});
    q.run(1100);
    ok("free again: asked for at once", calls_are("request map"));
}

static void wake()
{
    Rig r;
    ok("no timer when idle", r.m.wake_in_ms(r.now) == -1);
    r.m.want(0, true, false);
    r.m.tick(r.now);
    ok("acquiring wakes at the start timeout", r.m.wake_in_ms(r.now) == 5000);
    r.m.on_shown(kMapScene, r.now);
    r.m.want(0, false, false);
    r.m.tick(r.now);
    ok("lingering wakes at its end", r.m.wake_in_ms(r.now) == 2000);
}

static const std::string kRenderer = "00000000-0000-0000-0000-000000000001";

static void warm_scenes()
{
    Rig r;
    r.m.want(1, true, false);
    r.m.tick(r.now);
    r.m.on_shown(kPanelScene, r.now);
    joined();
    r.m.want(1, false, false);
    r.run(3100);
    ok("released after its linger: the panel's scene stays published",
       calls_are("release panel") && r.m.published(1) && !r.m.requested(1));
    r.m.want(1, true, false);
    r.m.tick(r.now);
    ok("wanted again: requested, nothing published", calls_are("request panel") && r.m.requested(1));
    r.m.on_shown(kPanelScene, r.now);
    r.m.want(1, false, false);
    r.run(3100);
    joined();
    r.m.want(2, true, false);
    r.m.tick(r.now);
    ok("the HUD (the panel's scene id) wanted: the panel's released scene unpublished, then the HUD's published",
       calls_are("unpublish panel; publish hud; request hud") && !r.m.published(1) && r.m.published(2));
    r.m.want(1, true, false);
    r.run(100);
    ok("the panel wanted while the HUD is requested: it waits, the HUD's scene untouched",
       calls_are("") && r.state(1) == TargetState::Waiting && r.m.published(2));
}

/* The panel yields to the stock HMI (waiting for the slot to be free) while
 * the HUD, with the panel's scene id, is shown: the slot coming free must not
 * take the HUD's scene. */
static void shared_scene_in_use()
{
    Rig r;
    r.slot(kPanelSlot, 2, 0x01000101);
    r.m.want(1, true, false);
    r.run(100);
    ok("the panel yields to the stock HMI, waiting for the slot",
       r.state(1) == TargetState::Waiting && r.m.status(1).retry_ms < 0 && calls_are("slots?"));
    r.m.want(2, true, false);
    r.m.tick(r.now);
    r.m.on_shown(kPanelScene, r.now);
    ok("the HUD is shown", calls_are("publish hud; request hud") && r.state(2) == TargetState::Shown);
    r.slot(kPanelSlot, 1, 0);
    r.run(3000);
    ok("the slot free: the panel waits, the HUD's scene and request untouched",
       calls_are("") && r.state(1) == TargetState::Waiting &&
           r.m.status(1).reason == "scene id in use by another target" && r.state(2) == TargetState::Shown &&
           r.m.published(2) && r.m.requested(2) && !r.m.requested(1));
    r.m.want(2, false, false);
    r.run(3100);
    ok("the HUD released: the panel takes the scene id",
       calls_are("release hud; unpublish hud; publish panel; request panel") && r.m.requested(1));
}

/* A fresh retry's early request goes only from its wait. */
static void fresh_retry_cleared()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    const int64_t asked = r.now;
    r.run(10);
    r.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", r.now);
    r.m.want(0, false, false);
    r.run(300);
    ok("unwanted during the 250 ms retry: idle", r.state(0) == TargetState::Idle);
    joined();
    r.m.want(0, true, false);
    while (r.now - asked < 990)
        r.run(10);
    ok("wanted again: no request within 1 s of the last", calls_are(""));
    r.run(10);
    ok("then requested", calls_are("request map") && r.now - asked == 1000);
}

static void release_confirmation()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    joined();
    r.port.confirm = false;
    r.m.want(0, false, false);
    while (r.state(0) != TargetState::Idle)
        r.run(10);
    const int64_t released = r.now;
    r.m.want(0, true, false);
    r.run(500);
    ok("wanted again before the display confirms the release: not asked for", calls_are("release map"));
    ok("woken when the wait for the confirmation ends", r.m.wake_in_ms(r.now) == released + 1000 - r.now);
    r.port.confirm = true;
    r.m.tick(r.now);
    ok("confirmed: asked for at once", calls_are("request map"));

    r.m.on_shown(kMapScene, r.now);
    r.port.confirm = false;
    r.m.want(0, false, false);
    while (r.state(0) != TargetState::Idle)
        r.run(10);
    const int64_t again = r.now;
    joined();
    r.m.want(0, true, false);
    r.run(990);
    ok("never confirmed: nothing for 1 s", calls_are(""));
    r.run(10);
    ok("then asked for anyway", calls_are("request map") && r.now - again == 1000);

    /* Subscriptions observable: the renderer's unsubscription is awaited
     * too (it follows the display's confirmation by about 150 ms). */
    r.m.on_subscription(kMapScene, true, false, kRenderer, r.now);
    r.m.on_shown(kMapScene, r.now);
    r.port.confirm = true;
    r.m.want(0, false, false);
    while (r.state(0) != TargetState::Idle)
        r.run(10);
    const int64_t third = r.now;
    joined();
    r.m.want(0, true, false);
    r.run(150);
    ok("confirmed, the renderer still subscribed: not asked for", calls_are(""));
    r.m.on_subscription(kMapScene, false, false, kRenderer, r.now);
    r.m.tick(r.now);
    ok("the renderer unsubscribed: asked for at once", calls_are("request map") && r.now - third == 150);
    r.m.on_shown(kMapScene, r.now);
    r.m.on_subscription(kMapScene, true, false, kRenderer, r.now);
    r.m.want(0, false, false);
    while (r.state(0) != TargetState::Idle)
        r.run(10);
    const int64_t fourth = r.now;
    joined();
    r.m.want(0, true, false);
    r.run(990);
    ok("a renderer that stays subscribed: nothing for 1 s", calls_are(""));
    r.run(10);
    ok("then asked for anyway", calls_are("request map") && r.now - fourth == 1000);
}

static void fresh_fetch_failed()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    joined();
    bool quick = true;
    for (int k = 0; k < SlotManager::kFreshRetries; k++) {
        r.run(10);
        r.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", r.now);
        const int64_t at = r.now;
        quick = quick && calls_are("release map");
        r.run(240);
        quick = quick && calls_are("");
        r.run(10);
        quick = quick && calls_are("request map") && r.now - at == 250;
    }
    ok("a fetch failing right after the publication: asked again after 250 ms, three times", quick);
    r.run(10);
    r.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", r.now);
    int64_t at = r.now;
    joined();
    r.run(1990);
    ok("the fourth: the ladder", calls_are(""));
    r.run(10);
    ok("its first step, 2 s", calls_are("request map") && r.now - at == 2000);
    r.m.on_shown(kMapScene, r.now);
    r.run(40000);
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    r.run(5000);
    r.run(500);
    joined();
    r.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", r.now);
    at = r.now;
    joined();
    r.run(4990);
    ok("a fetch failing long after the publication: the ladder at once", calls_are(""));
    r.run(10);
    ok("its next step, 5 s", calls_are("request map") && r.now - at == 5000);
}

/* On the car: the KOMBI removed the shown map scene, failed the fetch asked
 * 2 s later within 125 ms, and took the one asked 7 s after the removal:
 * asked at 2 s and again 1.5 s after each quick failure, the fourth retry
 * comes at about 8.5 s. */
static void removal_retry()
{
    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    r.m.on_shown(kMapScene, r.now);
    r.run(40000);
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    const int64_t removed = r.now;
    joined();
    r.run(1990);
    ok("a scene the display removed: not asked for again within 2 s", calls_are(""));
    r.run(10);
    ok("then asked for", calls_are("request map") && r.now - removed == 2000);
    bool quick = true;
    for (int k = 0; k < SlotManager::kNotReadyRetries; k++) {
        r.run(130);
        r.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", r.now);
        const int64_t at = r.now;
        quick = quick && calls_are("release map") && r.m.status(0).reason == "refused: E_FETCH_STATUS_FAILED";
        r.run(1490);
        quick = quick && calls_are("");
        r.run(10);
        quick = quick && calls_are("request map") && r.now - at == 1500;
    }
    ok("a fetch failing within 300 ms of it: not ready, asked again after 1.5 s, four times", quick);
    ok("no resend charged while the display fails the fetches", r.scenes.resends == 0);
    r.run(130);
    r.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", r.now);
    const int64_t at = r.now;
    joined();
    r.run(4990);
    ok("the fifth: the ladder's next step", calls_are(""));
    r.run(10);
    ok("5 s", calls_are("request map") && r.now - at == 5000);
    r.m.on_shown(kMapScene, r.now);
    ok("charged once when the display takes it", r.scenes.resends == 1);

    Rig s;   /* failing later than 300 ms after the request: a refusal, the ladder */
    s.m.want(0, true, false);
    s.m.tick(s.now);
    s.m.on_shown(kMapScene, s.now);
    s.run(40000);
    s.m.on_lost(kMapScene, "fetch removed", s.now);
    s.run(5000);
    s.run(400);
    joined();
    s.m.on_refused(kMapScene, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", s.now);
    ok("a slow failure after a removal is not 'not ready': the ladder",
       s.m.status(0).retry_ms == s.now + 5000);

    Rig h;   /* the stock map taking the slot is not a removal by the display */
    h.m.want(0, true, true);
    h.m.tick(h.now);
    h.m.on_shown(kMapScene, h.now);
    h.slot(kMapSlot, 2, kStockMap);
    h.m.on_lost(kMapScene, "fetch removed", h.now);
    ok("a slot the stock HMI takes: the ladder's 2 s", h.m.status(0).retry_ms == h.now + 2000);
}

static void rendering()
{
    Rig n;
    n.m.want(0, true, false);
    n.m.tick(n.now);
    n.m.on_shown(kMapScene, n.now);
    joined();
    n.run(10000);
    ok("no subscription line since start: shown, not observable, nothing done",
       n.m.status(0).rendering == Rendering::Unobservable && n.state(0) == TargetState::Shown && calls_are("") &&
           n.m.renderer_text(n.now).find("not observable") != std::string::npos);

    Rig r;
    r.m.want(0, true, false);
    r.m.tick(r.now);
    r.run(100);
    r.m.on_subscription(kMapScene, true, false, kRenderer, r.now);
    r.run(300);
    ok("subscribed at the fetch, before the map: not counted yet", r.m.status(0).rendering == Rendering::None);
    r.m.on_shown(kMapScene, r.now);
    joined();
    ok("shown: verified at once", r.m.status(0).rendering == Rendering::Verified);
    r.run(10000);
    ok("and kept, nothing done", calls_are("") && r.state(0) == TargetState::Shown &&
                                    r.m.status(0).rendering == Rendering::Verified);
    r.m.on_subscription(kMapScene, false, false, kRenderer, r.now);
    r.m.tick(r.now);
    ok("the renderer unsubscribing while shown: checking", r.m.status(0).rendering == Rendering::Checking &&
                                                               r.m.wake_in_ms(r.now) == SlotManager::kVerifyMs);
    r.run(1000);
    r.m.on_subscription(kMapScene, true, false, kRenderer, r.now);
    r.run(1000);
    ok("subscribed again within 1.5 s: verified, nothing done",
       r.m.status(0).rendering == Rendering::Verified && calls_are(""));
    r.m.on_renderer_gone(kRenderer, r.now);
    r.m.tick(r.now);
    r.run(1490);
    ok("the renderer gone: not subscribed for 1.49 s, still shown",
       calls_are("") && r.state(0) == TargetState::Shown && r.m.status(0).rendering == Rendering::Checking);
    r.run(20);
    ok("at 1.5 s: released and asked for again at once, the scene kept",
       calls_are("release map; request map") && r.m.published(0) && r.state(0) == TargetState::Acquiring);
    r.run(200);
    r.m.on_shown(kMapScene, r.now);
    const int64_t shown = r.now;
    r.run(1510);
    ok("shown again, still not subscribed: released, on the ladder (2 s) this time",
       calls_are("release map") && r.state(0) == TargetState::Waiting &&
           r.m.status(0).retry_ms == shown + SlotManager::kVerifyMs + 2000);
    r.run(2000);
    ok("asked for again at the step", calls_are("request map"));
    r.m.on_subscription(kMapScene, true, false, kRenderer, r.now);
    r.run(100);
    r.m.on_shown(kMapScene, r.now);
    ok("subscribed: verified", r.m.status(0).rendering == Rendering::Verified && r.m.published(0));

    Rig u;
    u.m.on_subscription(0x01000300, true, false, kRenderer, u.now);   /* no target of ours publishes it */
    u.m.want(0, true, false);
    u.m.tick(u.now);
    u.m.on_shown(kMapScene, u.now);
    u.m.on_subscription(kMapScene, true, true, kRenderer, u.now);     /* a scene the client does not know */
    joined();
    u.run(1600);
    ok("a subscription line of any scene makes subscriptions observable; one the client did not know is not ours",
       calls_are("release map; request map"));
}

/* The lines logged since the last call. */
static std::string logged()
{
    static uint64_t seq;
    std::vector<std::string> lines;
    cld::log_lines_since(&seq, &lines);
    std::string s;
    for (const std::string &l : lines)
        s += l + "\n";
    return s;
}

static bool requested_none(const std::string &calls) { return calls.find("request") == std::string::npos; }

static const uint64_t kGuideSlot = 0x105, kGuideScene = 0x010004f1;
/* The HMI's maneuver panel; a second content a take-over may unmap (not the
 * HMI's: for the queue's order); the seat-belt-warning panel, never taken
 * over. */
static const uint64_t kBox = 0x01000310, kBoxAlt = 0x01000315, kBelt = 0x01000314;

/* The rig with a hold of kGuideSlot following the map (target 3); from_table:
 * its slot from the slot table instead (guide_slots = auto). */
struct GuideRig : Rig {
    static const int kGuide = 3;
    explicit GuideRig(bool from_table = false)
    {
        TargetSpec g;
        g.name = "guide"; g.slot = from_table ? 0 : kGuideSlot; g.scene_id = kGuideScene; g.width = g.height = 16;
        g.linger_ms = 0; g.take_over_stock = true; g.follows = "map"; g.take_over_contents = {kBox, kBoxAlt};
        g.strike_limit = 2;   /* as load_holds makes it */
        if (from_table)
            g.table_slots = {0x105, 0x103};
        m.add(g);
    }
    uint64_t guide_slot() const { return m.targets()[kGuide].slot; }
    /* The hold asked for and shown, the box parked behind it. */
    void hold()
    {
        pass();
        rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}});
        m.on_shown(kGuideScene, now);
    }
    /* Passes of the render loop, as Service::after_poll runs them. */
    void pass(int64_t ms = 10)
    {
        for (int64_t t = 0; t < ms; t += 10) {
            now += 10;
            m.follow(now);
            m.tick(now);
        }
    }
    void show_map()
    {
        m.want(0, true, true);
        pass();
        rows(kMapSlot, 2, {{kMapScene, 1}});
        m.on_shown(kMapScene, now);
    }
};

static void guide_follows_the_map()
{
    GuideRig r;
    logged();
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBoxAlt, 3}});   /* a route: the box shown */
    r.pass(3000);
    ok("hold: the map not shown, the guidance slot not asked for", joined().find("guide") == std::string::npos);
    r.show_map();
    joined();
    r.pass();
    ok("the map shown: the hold's empty scene published and asked for", calls_are("publish guide; request guide") &&
                                                                          r.state(GuideRig::kGuide) ==
                                                                              TargetState::Acquiring);
    ok("logged with ours and the slot's rows",
       logged().find("target guide: hold acquiring (was idle); ours 0x10004f1; slot 0x105 status 2: 0x1000310 class "
                     "0 map 1, 0x1000315 class 0 map 3") != std::string::npos);
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBoxAlt, 3}, {kGuideScene, 3}});
    r.m.on_parked(kGuideScene, r.now);
    joined();
    r.pass();
    ok("parked behind the box: the HMI's contents mapped before ours unmapped as the HMI, shown and parked",
       calls_are("take over guide 0x1000310; take over guide 0x1000315; slots?"));
    const std::string over = logged();
    ok("the take-over logged with what it unmapped, ours and the rows",
       over.find("target guide: take-over 1 (1 since start): unmapping the stock HMI's 0x1000310 (class 0, map "
                 "status 1), 0x1000315 (class 0, map status 3) in slot 0x105 as the HMI; the next") !=
               std::string::npos &&
           over.find("ours 0x10004f1; slot status 2:") != std::string::npos);
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}, {kBoxAlt, 3}});
    r.m.on_shown(kGuideScene, r.now);
    const std::string again = logged();
    ok("the HMI maps the box again behind ours: a line for each content, and the hold shown",
       again.find("target guide: the stock HMI's 0x1000310 is mapped again, parked behind ours (map status 3, "
                  "was 1)") != std::string::npos &&
           again.find("the stock HMI's 0x1000315 is mapped again, parked behind ours (map status 3, was 3)") ==
               std::string::npos &&
           again.find("target guide: hold shown (was parked)") != std::string::npos &&
           r.state(GuideRig::kGuide) == TargetState::Shown);
    r.pass(60000);
    ok("held behind the map for a minute: no more take-overs, no traffic", calls_are("") &&
                                                                            r.state(GuideRig::kGuide) ==
                                                                                TargetState::Shown);
    r.m.want(0, false, false);
    r.pass(30);
    ok("the map no longer shown (lingering): the hold released within three passes",
       calls_are("release guide") && r.state(0) == TargetState::Lingering &&
           r.state(GuideRig::kGuide) == TargetState::Idle &&
           logged().find("target guide: hold idle (was lingering)") != std::string::npos);
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBoxAlt, 3}});
    r.m.want(0, true, true);
    r.pass(20);
    ok("the map shown again from its linger: the hold asked for again at once, in the same scene",
       calls_are("request guide") && r.state(0) == TargetState::Shown);

    GuideRig p;
    p.rows(kGuideSlot, 2, {{kBox, 1}});
    p.show_map();
    p.pass();
    p.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}});
    p.m.on_parked(kGuideScene, p.now);
    p.m.want(0, false, false);
    joined();
    p.pass(30);
    ok("parked when the map goes: released", joined().find("release guide") != std::string::npos &&
                                                 p.state(GuideRig::kGuide) == TargetState::Idle);
}

/* The status lists every slot the reports name: what the KOMBI shows beside ours (a dark box under a layout). */
static void table_in_status()
{
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBoxAlt, 3}});
    r.rows(kMapSlot, 2, {{kMapScene, 1}});
    const std::string t = r.m.table_text();
    ok("the table text: a line for each slot reported, its rows in order",
       t.find("slot 0x100 status 2: 0x10004f0 class 0 map 1\n") != std::string::npos &&
           t.find("slot 0x105 status 2: 0x1000310 class 0 map 1, 0x1000315 class 0 map 3\n") != std::string::npos);
}

static void guide_queue_order()
{
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}});
    r.show_map();
    r.pass();
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}});
    r.m.on_parked(kGuideScene, r.now);
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}, {kBoxAlt, 3}});   /* mapped after ours */
    joined();
    logged();
    r.pass();
    ok("take-over: the shown box unmapped; content mapped after ours left parked behind it",
       calls_are("take over guide 0x1000310; slots?") &&
           logged().find("unmapping the stock HMI's 0x1000310 (class 0, map status 1) in slot 0x105 as the HMI; "
                         "left parked, mapped after ours: 0x1000315 (class 0, map status 3)") != std::string::npos);
    /* One report for the unmap and the HMI's map again: 1 -> 3, never 0. */
    r.rows(kGuideSlot, 2, {{kBoxAlt, 1}, {kGuideScene, 3}, {kBox, 3}});
    r.pass(3000);
    ok("a stock content shown in its place: unmapped at the next take-over, the re-mapped box left behind ours",
       joined().find("take over guide 0x1000315") != std::string::npos &&
           logged().find("take-over 2 (2 since start): unmapping the stock HMI's 0x1000315 (class 0, map status 1) "
                         "in slot 0x105 as the HMI; left parked, mapped after ours: 0x1000310") != std::string::npos);

    GuideRig a;
    a.rows(kGuideSlot, 2, {{kBox, 1}});
    a.show_map();
    a.pass();
    a.rows(kGuideSlot, 2, {{kBox, 1}, {kBoxAlt, 3}});   /* mapped while ours is asked for */
    a.rows(kGuideSlot, 2, {{kBox, 1}, {kBoxAlt, 3}, {kGuideScene, 3}});
    a.m.on_parked(kGuideScene, a.now);
    joined();
    a.pass();
    ok("content mapped while ours was asked for can be ahead of it: unmapped too",
       calls_are("take over guide 0x1000310; take over guide 0x1000315; slots?"));
}

static void guide_never_verified_out()
{
    GuideRig r;
    r.slot(kGuideSlot, 1, 0);
    r.show_map();
    r.m.on_subscription(kMapScene, true, false, "renderer-1", r.now);   /* subscriptions observable */
    r.pass();
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}});
    r.m.on_shown(kGuideScene, r.now);
    joined();
    r.pass(60000);
    ok("subscriptions observable, the KOMBI never subscribing to the hold's empty scene: kept, never released",
       calls_are("") && r.state(GuideRig::kGuide) == TargetState::Shown &&
           r.m.status(GuideRig::kGuide).rendering == Rendering::Checking && r.state(0) == TargetState::Shown);
}

/* Slot 0x105 as the car reported it with a route (2026-11-03): the box
 * shown, the seat-belt variant 0x01000314 parked behind it at all times. */
static void guide_live_route()
{
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    r.show_map();
    joined();
    r.pass();
    ok("the box shown, the seat-belt panel parked: the hold asked for", calls_are("publish guide; request guide"));
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}, {kGuideScene, 3}});
    r.m.on_parked(kGuideScene, r.now);
    joined();
    logged();
    r.pass();
    ok("parked behind both: the box taken over, the seat-belt panel left alone",
       calls_are("take over guide 0x1000310; slots?") &&
           logged().find("unmapping the stock HMI's 0x1000310 (class 0, map status 1) in slot 0x105 as the HMI; the "
                         "next") != std::string::npos);
    /* The KOMBI shows ours, not the parked seat-belt panel; the HMI maps the
     * box again behind ours about 100 ms later, before our map status. */
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}});
    r.pass(100);
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    r.pass(5000);
    ok("ours shown, the box mapped again behind it: no second take-over", joined().find("take over") ==
                                                                               std::string::npos);
    r.m.on_shown(kGuideScene, r.now);
    r.pass(60000);
    ok("held a minute with both parked behind it: no traffic, no yield",
       calls_are("") && r.state(GuideRig::kGuide) == TargetState::Shown &&
           logged().find("yields") == std::string::npos);
    r.m.want(0, false, false);
    r.pass(30);
    ok("the map hidden: released", calls_are("release guide") && r.state(GuideRig::kGuide) == TargetState::Idle);
}

static void guide_yields()
{
    /* The take-over as on the car, but the KOMBI promotes the seat-belt
     * panel instead of ours. */
    GuideRig k;
    k.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    k.show_map();
    k.pass();
    k.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}, {kGuideScene, 3}});
    k.m.on_parked(kGuideScene, k.now);
    k.pass();
    joined();
    logged();
    k.rows(kGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}});
    k.rows(kGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}, {kBox, 3}});
    k.pass();
    ok("the seat-belt panel promoted instead of ours: released in the same pass, never taken over",
       calls_are("release guide") && k.state(GuideRig::kGuide) == TargetState::Idle &&
           logged().find("target guide: hold yields to 0x1000314 (class 0, map status 1)") != std::string::npos);
    k.rows(kGuideSlot, 2, {{kBelt, 1}, {kBox, 3}});
    k.pass(10000);
    ok("not asked for while it is shown, the map shown", calls_are("") && k.state(0) == TargetState::Shown);
    k.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    k.pass();
    ok("parked again: asked for again", calls_are("request guide") &&
                                            logged().find("hold no longer yields: 0x1000314 is not shown") !=
                                                std::string::npos);

    /* A seat-belt warning while held: the KOMBI shows the panel over ours. */
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    r.show_map();
    r.pass();
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    r.m.on_shown(kGuideScene, r.now);
    joined();
    r.rows(kGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}, {kBox, 3}});
    r.m.on_parked(kGuideScene, r.now);
    r.pass();
    ok("the seat-belt panel shown over the hold: released, never taken over",
       calls_are("slots?; release guide") && r.state(GuideRig::kGuide) == TargetState::Idle);
    r.rows(kGuideSlot, 2, {{kBelt, 1}, {kBox, 3}});
    r.pass(10000);
    ok("not asked for while it is shown", calls_are(""));
    r.rows(kGuideSlot, 2, {{kBelt, 0}, {kBox, 1}});
    r.pass();
    ok("unmapped: asked for again", calls_are("request guide"));

    GuideRig b;
    b.rows(kGuideSlot, 2, {{kBelt, 1}});
    b.show_map();
    joined();
    b.pass(5000);
    ok("the seat-belt panel shown when the map is: never asked for, never taken over", calls_are(""));

    GuideRig o;
    o.rows(kGuideSlot, 2, {{kBox, 1}});
    o.show_map();
    o.pass();
    o.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}});
    o.m.on_parked(kGuideScene, o.now);
    o.rows(kGuideSlot, 2, {{0x01000330, 1}, {kGuideScene, 3}});   /* other HMI content in its place */
    joined();
    o.pass(5000);
    ok("parked behind HMI content off the list: yields, no take-over", calls_are("release guide") &&
                                                                        o.state(GuideRig::kGuide) == TargetState::Idle);
}

/* A door opened: the KOMBI locks 0x105 and unmaps everything in it, then
 * the HMI maps the box and the seat-belt panel again, as seen on the car
 * (2026-11-03) with `panel` there. The map staying shown is this test's. */
static void guide_door_lock()
{
    GuideRig h;
    h.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    h.show_map();
    h.pass();
    h.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    h.m.on_shown(kGuideScene, h.now);
    joined();
    logged();
    const int64_t door = h.now;
    h.rows(kGuideSlot, 0, {{kGuideScene, 0}, {kBelt, 0}, {kBox, 0}});
    h.m.on_lost(kGuideScene, "unmapped", h.now);
    h.pass(9000);
    const std::string locked = joined();
    ok("the lock: the hold down, not asked for while it lasts, waiting as \"slot locked\"",
       locked.find("release guide") == 0 && requested_none(locked) &&
           h.state(GuideRig::kGuide) == TargetState::Waiting &&
           h.m.status(GuideRig::kGuide).reason == "slot locked");
    h.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    h.pass();
    ok("the slot free, the box shown again: asked for at once", calls_are("request guide"));
    h.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}, {kGuideScene, 3}});
    h.m.on_parked(kGuideScene, h.now);
    joined();
    h.pass();
    const std::string over = joined();
    ok("parked: one take-over, of the box", over.find("take over guide 0x1000310") == 0 &&
                                                over.rfind("take over") == 0);
    h.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    h.m.on_shown(kGuideScene, h.now);
    h.pass(door + 15000 - h.now);
    ok("held again, not given up", calls_are("") && h.state(GuideRig::kGuide) == TargetState::Shown &&
                                       logged().find("given up") == std::string::npos);
    /* A second lock 15 s after the first, the slot back 1 s later. */
    h.rows(kGuideSlot, 0, {{kGuideScene, 0}, {kBelt, 0}, {kBox, 0}});
    h.m.on_lost(kGuideScene, "unmapped", h.now);
    h.pass(1000);
    joined();
    h.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    h.pass();
    ok("a second lock 15 s later: asked for at once when the slot is free, the ladder not climbed",
       calls_are("request guide"));

    /* The map lost with it: its slot locked too, the map refused "fetch
     * removed" once when the slots come back and shown at the next try. */
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    r.show_map();
    r.pass();
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    r.m.on_shown(kGuideScene, r.now);
    joined();
    logged();
    const int64_t lock = r.now;
    r.rows(kMapSlot, 0, {{kMapScene, 0}});
    r.rows(kGuideSlot, 0, {{kGuideScene, 0}, {kBelt, 0}, {kBox, 0}});
    r.m.on_lost(kMapScene, "unmapped", r.now);
    r.m.on_lost(kGuideScene, "unmapped", r.now);
    r.pass();
    ok("the lock: the map and the hold down", calls_are("release map; release guide"));
    int map_requests = 0;
    int64_t map_shown = -1, guide_request = -1;
    std::string all;
    for (int64_t w = 0; w < 30000 && guide_request < 0; w += 10) {
        if (w == 2000) {
            r.slot(kMapSlot, 1, 0);
            r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});   /* the HMI maps its contents again */
        }
        r.pass();
        const std::string c = joined();
        all += c + "; ";
        if (c.find("request map") != std::string::npos && ++map_requests == 1) {
            r.m.on_refused(kMapScene, Refusal::Transient, "fetch removed", r.now);
        } else if (c.find("request map") != std::string::npos) {
            r.rows(kMapSlot, 2, {{kMapScene, 1}});
            r.m.on_shown(kMapScene, r.now);
            map_shown = r.now;
        }
        if (c.find("request guide") != std::string::npos)
            guide_request = r.now;
    }
    ok("the slots free: the map refused once, then shown; the hold asked for only once the map is shown",
       map_requests == 2 && map_shown > lock && guide_request > map_shown &&
           all.find("request guide") == all.rfind("request guide"));
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}, {kGuideScene, 3}});
    r.m.on_parked(kGuideScene, r.now);
    joined();
    r.pass();
    const std::string again = joined();
    ok("one take-over, of the box", again.find("take over guide 0x1000310") == 0 && again.rfind("take over") == 0);
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    r.m.on_shown(kGuideScene, r.now);
    r.pass(10000);
    ok("the map lost too: held again once it is shown, not given up",
       calls_are("") && r.state(GuideRig::kGuide) == TargetState::Shown &&
           logged().find("given up") == std::string::npos);
}

/* The take-overs' count starts over once the hold has been shown 10 s. */
static void guide_take_over_settle()
{
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    r.show_map();
    r.pass();
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}, {kGuideScene, 3}});
    r.m.on_parked(kGuideScene, r.now);
    r.pass();
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    r.m.on_shown(kGuideScene, r.now);
    r.pass(5000);
    logged();
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}, {kBelt, 3}});   /* the box over ours again */
    r.m.on_parked(kGuideScene, r.now);
    r.pass();
    ok("parked again 5 s after shown: the count goes on",
       logged().find("take-over 2 (2 since start): unmapping the stock HMI's 0x1000310") != std::string::npos);
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}, {kBelt, 3}});
    r.m.on_shown(kGuideScene, r.now);
    r.pass(10000);
    r.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}, {kBelt, 3}});
    r.m.on_parked(kGuideScene, r.now);
    r.pass();
    ok("parked again 10 s after shown: the count started over, the take-over at once",
       logged().find("take-over 1 (3 since start): unmapping the stock HMI's 0x1000310") != std::string::npos);
}

static void guide_gives_up()
{
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}});
    r.show_map();
    logged();
    int requests = 0;
    for (int k = 0; k < 6; k++) {
        bool asked = false;
        for (int64_t w = 0; w < 30000 && !asked; w += 100) {
            r.pass(100);
            asked = joined().find("request guide") != std::string::npos;
        }
        if (!asked)
            break;
        requests++;
        r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}});
        r.m.on_shown(kGuideScene, r.now);
        r.rows(kGuideSlot, 2, {{kBox, 1}});   /* the HMI's box over ours again */
        r.m.on_lost(kGuideScene, "fetch removed", r.now);
    }
    r.pass(300000);
    ok("our scene removed 4 times within 60 s: given up, not asked for again",
       requests == 4 && joined().find("request guide") == std::string::npos &&
           logged().find("target guide: hold given up: its scene fetch removed, 4 losses within 60 s") !=
               std::string::npos);

    GuideRig l;
    l.slot(kGuideSlot, 1, 0);
    l.show_map();
    int asked = 0;
    for (int k = 0; k < 6; k++) {
        l.pass(1100);
        if (joined().find("request guide") == std::string::npos)
            break;
        asked++;
        l.rows(kGuideSlot, 2, {{kGuideScene, 1}});
        l.m.on_shown(kGuideScene, l.now);
        l.rows(kGuideSlot, 0, {{kGuideScene, 0}});   /* a lock */
        l.m.on_lost(kGuideScene, "unmapped", l.now);
        l.slot(kGuideSlot, 1, 0);
    }
    ok("losses to a lock do not count", asked == 6);

    /* The seat-belt panel shown over the hold: parked again and yielded to,
     * one loss each time. */
    GuideRig y;
    y.rows(kGuideSlot, 2, {{kBox, 1}});
    y.show_map();
    logged();
    int held = 0;
    for (int k = 0; k < 4; k++) {
        bool asked = false;
        for (int64_t w = 0; w < 30000 && !asked; w += 100) {
            y.pass(100);
            asked = joined().find("request guide") != std::string::npos;
        }
        if (!asked)
            break;
        y.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}});
        y.m.on_shown(kGuideScene, y.now);
        held++;
        y.rows(kGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}, {kBox, 3}});
        y.m.on_parked(kGuideScene, y.now);
        y.pass();
        y.rows(kGuideSlot, 2, {{kBox, 1}});   /* the seat belt fastened */
        if (k == 2)
            ok("three seat-belt events within 60 s: three losses, not given up",
               logged().find("given up") == std::string::npos);
    }
    ok("a fourth: given up", held == 4 && logged().find("hold given up") != std::string::npos);
}

/* Losses until the hold gives up: yields, the seat-belt panel shown over
 * the hold (BMW's own content), or else faults, our scene unmapped with
 * only the box (on the take-over list) in the slot, Dcsm2 staying up (the
 * KOMBI's renderer restarting). Returns the requests. */
static int lose_until_given_up(GuideRig &r, bool yields)
{
    r.rows(kGuideSlot, 2, {{kBox, 1}});
    r.show_map();
    int requests = 0;
    for (int k = 0; k < 6; k++) {
        bool asked = false;
        for (int64_t w = 0; w < 30000 && !asked; w += 100) {
            r.pass(100);
            asked = joined().find("request guide") != std::string::npos;
        }
        if (!asked)
            break;
        requests++;
        r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}});
        r.m.on_shown(kGuideScene, r.now);
        if (yields) {
            r.rows(kGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}, {kBox, 3}});
            r.m.on_parked(kGuideScene, r.now);
            r.pass();
            r.rows(kGuideSlot, 2, {{kBox, 1}});   /* the seat belt fastened */
        } else {
            r.rows(kGuideSlot, 2, {{kBox, 1}});
            r.m.on_lost(kGuideScene, "unmapped", r.now);
        }
    }
    r.pass(300000);
    return requests;
}

/* Give-ups in two boots: of faults alone, a strike each and disabled; of
 * yields to the seat-belt panel, no strike, held again the boot after. */
static void guide_gives_up_twice()
{
    for (bool yields : {false, true}) {
        const std::string path = "/tmp/crs-slots-giveup-" + std::to_string(getpid()) + ".txt";
        const std::string what = yields ? "yields only" : "unmaps only";
        unlink(path.c_str());
        {
            Guard g;
            g.load(path, "t", "boot-1");
            GuideRig r;
            r.m.set_guard(&g);
            const int asked = lose_until_given_up(r, yields);
            ok((what + ", one boot: given up, " + (yields ? "no strike" : "a strike")).c_str(),
               asked == 4 && logged().find("hold given up") != std::string::npos &&
                   g.status_text() == (yields ? "" : "guard: guide 0x105 0x10004f1 0: 1 strike\n") &&
                   !r.m.hold_disabled(0));
        }
        {
            Guard g;
            g.load(path, "t", "boot-2");
            GuideRig r;
            r.m.set_guard(&g);
            const int asked = lose_until_given_up(r, yields);
            ok((what + ", the next boot: given up again, " + (yields ? "still no strike" : "disabled")).c_str(),
               asked == 4 && r.m.hold_disabled(0) == !yields &&
                   g.status_text() ==
                       (yields ? "" : "guard: guide 0x105 0x10004f1 0: disabled, 2 strikes\n"));
        }
        {
            Guard g;
            g.load(path, "t", "boot-3");
            GuideRig r;
            r.m.set_guard(&g);
            const int asked = lose_until_given_up(r, yields);
            ok((what + ", the boot after: " + (yields ? "held again" : "never asked for")).c_str(),
               (asked > 0) == yields && r.m.hold_disabled(0) == !yields);
        }
        unlink(path.c_str());
    }
}

static void guide_slot_disabled()
{
    GuideRig r;
    r.slot(kGuideSlot, 1, 0);
    r.show_map();
    r.pass();
    r.rows(kGuideSlot, 2, {{kGuideScene, 1}});
    r.m.on_shown(kGuideScene, r.now);
    joined();
    r.rows(kGuideSlot, 0, {{kGuideScene, 1}});   /* reported disabled before the KOMBI unmaps ours */
    r.pass(20);
    ok("held, then the slot disabled: kept until the KOMBI unmaps it", calls_are("") &&
                                                                        r.state(GuideRig::kGuide) ==
                                                                            TargetState::Shown);
    r.rows(kGuideSlot, 0, {{kGuideScene, 0}});
    r.m.on_lost(kGuideScene, "unmapped", r.now);
    r.pass(10000);
    ok("unmapped: not asked for while the slot is disabled, the map shown, waiting as \"slot locked\"",
       requested_none(joined()) && r.state(GuideRig::kGuide) == TargetState::Waiting &&
           r.m.status(GuideRig::kGuide).reason == "slot locked");
    r.slot(kGuideSlot, 1, 0);
    r.pass();
    ok("enabled again: asked for at once", calls_are("request guide"));

    GuideRig d;
    d.slot(kGuideSlot, 0, 0);   /* 0x103 on the owner's car */
    d.show_map();
    joined();
    d.pass(10000);
    ok("a slot reported disabled when the map is shown: never asked for, waiting as \"slot locked\"",
       requested_none(joined()) && d.m.status(GuideRig::kGuide).reason == "slot locked");
    d.pass(40000);
    ok("50 s with no report since: still not asked for (no lock trust for a hold)",
       requested_none(joined()) && d.m.status(GuideRig::kGuide).reason == "slot locked");

    GuideRig u;
    u.show_map();
    joined();
    u.pass(40000);
    ok("a slot the table never names: never asked for, waiting as \"slot not reported\"",
       requested_none(joined()) && u.m.status(GuideRig::kGuide).reason == "slot not reported");
    u.rows(kGuideSlot, 2, {{kBox, 1}});
    u.pass();
    ok("named: asked for at once", joined().find("request guide") != std::string::npos);
}

static bool has_line(const std::string &text, const std::string &line)
{
    return text.find(line + "\n") != std::string::npos;
}

/* The HUD under the mapping guard: a display drop within 10 s of its
 * mapping is a strike against it, not the map, whichever was mapped last;
 * two (one a boot) disable it, and only it; wanting it again does not undo
 * that. panel and hud (one scene id) are never asked for at once. */
struct HudRig {
    FakePort port;
    FakeScenes scenes;
    SlotManager m{&port, &scenes};
    int64_t now = 100000;
    int map = 0, panel = 1, hud = 2;
    HudRig()
    {
        TargetSpec mp, pn, h;
        mp.name = "map"; mp.slot = kMapSlot; mp.scene_id = kMapScene; mp.width = 1088; mp.height = 480; mp.linger_ms = 0;
        pn.name = "panel"; pn.slot = kPanelSlot; pn.scene_id = kPanelScene; pn.width = 1920; pn.height = 720;
        h.name = "hud"; h.slot = kHudSlot; h.scene_id = kPanelScene; h.width = 800; h.height = 480;
        h.share = true; h.strike_limit = 2;   /* as cluster-render's main sets the iDrive 7 HUD */
        m.add(mp);
        m.add(pn);
        m.add(h);
    }
    void pass(int64_t ms) { for (int64_t t = 0; t < ms; t += 10) { now += 10; m.tick(now); } }
    void show(int i, uint64_t scene)
    {
        m.want(i, true, true);
        pass(1200);
        m.on_shown(scene, now);
        pass(20);
    }
    void drop()
    {
        port.up = false;
        m.on_availability(false, now);
        port.up = true;
        m.on_availability(true, now);
    }
};

static void hud_guard()
{
    const std::string path = "/tmp/crs-slots-hud-guard-" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());
    {
        Guard g;
        g.load(path, "t", "boot-1");
        HudRig r;
        r.m.set_guard(&g);
        r.show(r.map, kMapScene);
        r.show(r.hud, kPanelScene);
        r.pass(3000);
        r.drop();
        ok("hud guard: a drop 3 s after the HUD's mapping strikes the HUD, not the map",
           g.status_text() == "guard: hud 0x323 0x1000400 0: 1 strike\n");
        r.pass(1500);
        r.m.on_shown(kPanelScene, r.now);
        r.pass(100);
        r.m.on_shown(kMapScene, r.now);   /* the map mapped again after the HUD */
        r.pass(2000);
        r.drop();
        ok("hud guard: the HUD blamed again over a map mapped after it, and the HUD, struck twice, disabled",
           g.status_text() == "guard: hud 0x323 0x1000400 0: disabled, 2 strikes\n" && r.m.disabled(r.hud) &&
               !r.m.disabled(r.map));
    }
    {
        Guard g;
        g.load(path, "t", "boot-2");
        HudRig r;
        r.m.set_guard(&g);
        calls.clear();
        r.show(r.map, kMapScene);
        r.m.want(r.hud, true, true);
        r.pass(5000);
        const std::string c = joined();
        ok("hud guard, next boot: the HUD never asked for, the map shown",
           c.find("request hud") == std::string::npos && r.m.status(r.map).state == TargetState::Shown &&
               r.m.disabled(r.hud));
        r.m.want(r.hud, false, false);
        r.pass(100);
        r.m.want(r.hud, true, true);
        r.pass(5000);
        ok("hud guard: turned off and on, still disabled and never asked for",
           joined().find("request hud") == std::string::npos && r.m.disabled(r.hud));
    }
    unlink(path.c_str());

    HudRig r;
    calls.clear();
    r.m.want(r.panel, true, false);
    r.m.want(r.hud, true, true);
    bool both = false;
    for (int k = 0; k < 600; k++) {
        r.pass(10);
        if (k == 150)
            r.m.on_shown(kPanelScene, r.now);
        const bool p = r.m.requested(r.panel), h = r.m.requested(r.hud);
        both = both || (p && h);
    }
    ok("panel and hud wanted at once: never both asked for, the second waits for the scene id",
       !both && r.m.status(r.hud).reason == "scene id in use by another target");
}

/* The mapping guard (guard.h) in the slot manager: one strike per mapping
 * event. */
static void guide_guard()
{
    const std::string path = "/tmp/crs-slots-guard-" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());
    Guard g;
    g.load(path, "t", "boot-1");

    GuideRig r;
    r.m.set_guard(&g);
    r.rows(kGuideSlot, 2, {{kBox, 1}});
    r.show_map();
    r.hold();
    r.pass(2000);
    r.rows(kMapSlot, 2, {{kStockMap, 1}});
    r.m.on_lost(kMapScene, "fetch removed", r.now);
    r.pass(5000);
    joined();
    r.pass(100);
    ok("the map lost: the hold released, the map asked for again", r.state(GuideRig::kGuide) == TargetState::Idle &&
                                                                     r.state(0) == TargetState::Acquiring);
    r.port.up = false;
    r.m.on_availability(false, r.now);
    ok("the display service gone right after the map's request, 7 s after the released hold's: the hold blamed",
       g.status_text() == "guard: guide 0x105 0x10004f1 0: 1 strike\n");
    r.port.up = true;
    r.m.on_availability(true, r.now);
    r.pass(500);
    r.port.up = false;
    r.m.on_availability(false, r.now);   /* again, before the settle: nothing asked for since */
    r.m.on_availability(false, r.now);   /* and a duplicate notice */
    ok("two more drops within 10 s of the same events: no second strike against the hold",
       has_line(g.status_text(), "guard: guide 0x105 0x10004f1 0: 1 strike"));

    GuideRig s;
    Guard g2;
    unlink(path.c_str());
    g2.load(path, "t", "boot-1");
    s.m.set_guard(&g2);
    s.rows(kGuideSlot, 2, {{kBox, 1}});
    s.show_map();
    s.hold();
    s.pass(11000);
    s.rows(kGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}});
    s.m.on_parked(kGuideScene, s.now);
    s.rows(kGuideSlot, 2, {{kGuideScene, 1}});
    s.m.on_shown(kGuideScene, s.now);   /* parked and shown again by the display */
    s.pass(100);
    s.port.up = false;
    s.m.on_availability(false, s.now);
    ok("a re-park and re-show by the display is no mapping: no strike 11 s after the hold's", g2.status_text().empty());

    GuideRig n;
    Guard g3;
    unlink(path.c_str());
    g3.load(path, "t", "boot-1");
    n.m.set_guard(&g3);
    n.rows(kGuideSlot, 2, {{kBox, 1}});
    n.show_map();
    n.pass();
    joined();
    n.m.on_refused(kGuideScene, Refusal::NoSlot, "map E_DCSM_SLOTID_UNKNOWN", n.now);
    n.pass(60000);
    ok("the hold's slot refused as unknown: a strike, not asked for again, idle; the map kept",
       g3.status_text() == "guard: guide 0x105 0x10004f1 0: 1 strike\n" && requested_none(joined()) &&
           n.state(GuideRig::kGuide) == TargetState::Idle && n.m.hold_disabled(0) && n.state(0) == TargetState::Shown);
    unlink(path.c_str());
}

static void guide_no_flapping()
{
    GuideRig r;
    r.rows(kGuideSlot, 2, {{kBox, 1}});
    r.show_map();
    joined();
    /* The map shown and not, every 150 ms for 20 s; the KOMBI parks the
     * hold behind the box, a take-over shows it with the box parked, a
     * release shows the box. */
    std::vector<int64_t> requests, overs;
    for (int k = 0; k < 2000; k++) {
        if (k % 15 == 0)
            r.m.want(0, (k / 15) % 2 == 1, true);
        r.pass();
        const std::string c = joined();
        if (c.find("request guide") != std::string::npos) {
            requests.push_back(r.now);
            r.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}});
            r.m.on_parked(kGuideScene, r.now);
        }
        if (c.find("take over guide 0x1000310") != std::string::npos) {
            overs.push_back(r.now);
            r.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}});
            r.m.on_shown(kGuideScene, r.now);
        }
        if (c.find("release guide") != std::string::npos)
            r.rows(kGuideSlot, 2, {{kBox, 1}});
    }
    bool spaced = requests.size() >= 2 && overs.size() >= 2;
    for (size_t i = 1; i < requests.size(); i++)
        spaced = spaced && requests[i] - requests[i - 1] >= SlotManager::kMinRequestGapMs;
    for (size_t i = 1; i < overs.size(); i++)
        spaced = spaced && overs[i] - overs[i - 1] >= SlotManager::kTakeOverMs[0];
    ok("the map flapping every 150 ms for 20 s: requests at least 1 s apart, take-overs at least 1.5 s",
       spaced && requests.size() <= 20 && overs.size() <= 14);
    r.m.want(0, false, false);
    r.pass(30);
    ok("and the map left hidden: the hold released, idle", r.state(GuideRig::kGuide) == TargetState::Idle);

    /* The map shown and hidden every 2 s for 60 s: the hold never shown
     * 10 s, its take-overs on their steps. */
    GuideRig f;
    f.rows(kGuideSlot, 2, {{kBox, 1}});
    f.show_map();
    joined();
    std::vector<int64_t> takes;
    for (int k = 0; k < 6000; k++) {
        if (k % 200 == 0)
            f.m.want(0, (k / 200) % 2 == 0, true);
        f.pass();
        const std::string c = joined();
        if (c.find("request guide") != std::string::npos) {
            f.rows(kGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}});
            f.m.on_parked(kGuideScene, f.now);
        }
        if (c.find("take over guide 0x1000310") != std::string::npos) {
            takes.push_back(f.now);
            f.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}});
            f.m.on_shown(kGuideScene, f.now);
        }
        if (c.find("release guide") != std::string::npos)
            f.rows(kGuideSlot, 2, {{kBox, 1}});
    }
    bool stepped = takes.size() >= 6;
    for (size_t i = 1; i < takes.size(); i++)
        stepped = stepped && takes[i] - takes[i - 1] >= SlotManager::kTakeOverMs[std::min<size_t>(i - 1, 5)];
    ok("the map flapping every 2 s for 60 s: take-overs 1.5 s apart four times, then 10 s, then 30 s",
       stepped && takes.size() <= 7);
}

/* guide_slots = auto: the hold goes to 0x103 or 0x105, whichever the table
 * reports the box in, or before the box shows, the one the HMI chooses, with
 * the one scene id. */
static const uint64_t kOtherGuideSlot = 0x103;

/* The hold, wanted with the map shown and the box shown in slot, asked for
 * there and shown, then the display service gone 2 s later and back. */
static void map_then_drop(GuideRig &r, uint64_t slot)
{
    r.port.up = true;
    r.m.on_availability(true, r.now);
    r.rows(slot, 2, {{kBox, 1}});
    r.show_map();
    r.pass(2000);
    r.rows(kMapSlot, 2, {{kMapScene, 1}});
    r.m.on_shown(kMapScene, r.now);
    r.pass();
    r.rows(slot, 2, {{kGuideScene, 1}, {kBox, 3}});
    r.m.on_shown(kGuideScene, r.now);
    r.pass(2000);
    r.port.up = false;
    r.m.on_availability(false, r.now);
}

static void guide_auto_slot()
{
    /* A 2021 X5 G05 (US, 2026-11-04): 0x105 reported disabled and empty,
     * the box shown in 0x103 with the seat-belt panel parked behind it. */
    GuideRig r(true);
    logged();
    r.slot(kGuideSlot, 0, 0);
    r.rows(kOtherGuideSlot, 2, {{kBelt, 3}, {kBox, 1}});
    ok("auto, the box in 0x103 and 0x105 disabled: the hold's slot is 0x103, logged with the rows",
       r.guide_slot() == kOtherGuideSlot &&
           logged().find("target guide: hold moves to slot 0x103, where the slot table reports 0x1000310 (class 0, "
                         "map status 1) (no slot before); ours 0x10004f1; slot 0x103 status 2: 0x1000314 class 0 "
                         "map 3, 0x1000310 class 0 map 1") != std::string::npos);
    r.pass(3000);
    ok("auto: the map not shown, nothing asked for", requested_none(joined()));
    r.show_map();
    joined();
    r.pass();
    ok("the map shown: the hold asked for, in 0x103", calls_are("publish guide; request guide") &&
                                                          r.state(GuideRig::kGuide) == TargetState::Acquiring);
    r.rows(kOtherGuideSlot, 2, {{kBelt, 3}, {kBox, 1}, {kGuideScene, 3}});
    r.m.on_parked(kGuideScene, r.now);
    joined();
    logged();
    r.pass();
    ok("parked in 0x103: the box taken over, the seat-belt panel left alone",
       calls_are("take over guide 0x1000310; slots?") &&
           logged().find("unmapping the stock HMI's 0x1000310 (class 0, map status 1) in slot 0x103 as the HMI; the "
                         "next") != std::string::npos);
    r.rows(kOtherGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    r.m.on_shown(kGuideScene, r.now);
    r.slot(kGuideSlot, 0, 0);
    r.pass(60000);
    ok("held in 0x103 a minute, the box parked behind it, 0x105 still disabled: no traffic, not moved",
       calls_are("") && r.state(GuideRig::kGuide) == TargetState::Shown && r.guide_slot() == kOtherGuideSlot);
    r.m.want(0, false, false);
    r.pass(30);
    ok("the map hidden: released", calls_are("release guide") && r.state(GuideRig::kGuide) == TargetState::Idle);
    r.rows(kOtherGuideSlot, 1, {{kBelt, 3}, {kBox, 0}});   /* the route over: the box listed, unmapped */
    r.pass(100);
    ok("the box unmapped: the hold keeps 0x103", r.guide_slot() == kOtherGuideSlot);

    /* The owner's car (2026-11-03): the box in 0x105, 0x103 disabled. */
    GuideRig f(true);
    f.slot(kOtherGuideSlot, 0, 0);
    f.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    f.show_map();
    joined();
    f.pass();
    ok("auto, the box in 0x105 and 0x103 disabled: asked for in 0x105",
       f.guide_slot() == kGuideSlot && calls_are("publish guide; request guide"));
    f.rows(kGuideSlot, 2, {{kBox, 1}, {kBelt, 3}, {kGuideScene, 3}});
    f.m.on_parked(kGuideScene, f.now);
    joined();
    f.pass();
    ok("parked: the box taken over, the seat-belt panel left alone, as with guide_slots = 0x105",
       calls_are("take over guide 0x1000310; slots?"));

    /* 0x105 and 0x103 disabled, the box nowhere the hold follows it. */
    GuideRig a(true);
    a.slot(kGuideSlot, 0, 0);
    a.rows(kOtherGuideSlot, 0, {{kBox, 0}});   /* listed, unmapped */
    a.rows(0x104, 2, {{kBox, 1}});
    a.rows(0x203, 2, {{kBoxAlt, 1}});
    a.show_map();
    joined();
    a.pass(40000);
    ok("auto, 0x105 and 0x103 disabled, the box shown only in 0x104 and 0x203 (another target's): never asked for, "
       "waiting as \"no slot: 0x105 0x103 not reported enabled\"",
       requested_none(joined()) && a.guide_slot() == 0 && a.state(GuideRig::kGuide) == TargetState::Waiting &&
           a.m.status(GuideRig::kGuide).reason == "no slot: 0x105 0x103 not reported enabled");
    a.rows(kOtherGuideSlot, 0, {{kBox, 1}});
    a.pass(100);
    ok("the box in 0x103 reported disabled: not followed", a.guide_slot() == 0 && requested_none(joined()));
    a.rows(kOtherGuideSlot, 2, {{kBox, 1}});
    a.pass();
    ok("the box shown in 0x103: asked for there at once", calls_are("publish guide; request guide") &&
                                                              a.guide_slot() == kOtherGuideSlot);
    a.rows(0x104, 2, {{kBox, 1}});
    a.rows(kOtherGuideSlot, 1, {{kBox, 0}});
    a.pass(100);
    ok("then the box in 0x104 only: the hold stays in 0x103", a.guide_slot() == kOtherGuideSlot &&
                                                                 calls_are(""));

    /* The box moving while held. */
    GuideRig v(true);
    v.rows(kGuideSlot, 2, {{kBox, 1}});
    v.show_map();
    v.hold();
    joined();
    logged();
    v.rows(kOtherGuideSlot, 2, {{kBox, 1}});
    ok("held in 0x105, the box shown in 0x103: released from 0x105 in the same report, idle",
       calls_are("release guide") && v.guide_slot() == kOtherGuideSlot &&
           v.state(GuideRig::kGuide) == TargetState::Idle &&
           logged().find("target guide: hold moves to slot 0x103, where the slot table reports 0x1000310 (class 0, "
                         "map status 1) (was slot 0x105, released there)") != std::string::npos);
    v.pass(1000);
    ok("then asked for in 0x103, once, the request gap kept", calls_are("request guide"));
    v.rows(kGuideSlot, 1, {{kBox, 0}});
    v.rows(kOtherGuideSlot, 2, {{kBox, 1}, {kGuideScene, 3}});
    v.m.on_parked(kGuideScene, v.now);
    joined();
    v.pass();
    ok("the box left listed unmapped in 0x105: not followed back; in 0x103 taken over",
       v.guide_slot() == kOtherGuideSlot && calls_are("take over guide 0x1000310; slots?"));

    /* Moved while waiting on the ladder: the wait carries over. */
    GuideRig l(true);
    l.rows(kGuideSlot, 2, {{kBox, 1}});
    l.show_map();
    l.pass();
    joined();
    l.m.on_refused(kGuideScene, Refusal::Transient, "no valid reply", l.now);
    l.pass(2000);
    ok("refused in 0x105: asked again after the ladder's 2 s", calls_are("release guide; request guide"));
    l.m.on_refused(kGuideScene, Refusal::Transient, "no valid reply", l.now);
    joined();
    l.pass(100);
    l.rows(kOtherGuideSlot, 2, {{kBox, 1}});
    l.pass(4700);
    ok("refused again, then the box in 0x103: still waiting, not asked for before the ladder's 5 s",
       l.guide_slot() == kOtherGuideSlot && l.state(GuideRig::kGuide) == TargetState::Waiting && calls_are(""));
    l.pass(300);
    ok("then asked for in 0x103 at the end of that wait", calls_are("request guide"));

    /* The box moving to and fro: each move a loss, never a fault; more than
     * 3 within 60 s give the hold up, with no strike. */
    const std::string path = "/tmp/crs-slots-auto-" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());
    Guard g;
    g.load(path, "t", "boot-1");
    GuideRig p(true);
    p.m.set_guard(&g);
    p.rows(kGuideSlot, 2, {{kBox, 1}});
    p.show_map();
    logged();
    int moves = 0;
    for (int k = 0; k < 6; k++) {
        bool asked = false;
        for (int64_t w = 0; w < 10000 && !asked; w += 100) {
            p.pass(100);
            asked = joined().find("request guide") != std::string::npos;
        }
        if (!asked)
            break;
        const uint64_t at = p.guide_slot(), to = at == kGuideSlot ? kOtherGuideSlot : kGuideSlot;
        p.rows(at, 2, {{kGuideScene, 1}, {kBox, 3}});
        p.m.on_shown(kGuideScene, p.now);
        p.rows(at, 1, {{kGuideScene, 1}});
        p.rows(to, 2, {{kBox, 1}});
        moves++;
    }
    p.pass(120000);
    ok("the box moving between 0x105 and 0x103 while held: given up after 4 moves, no guard strike",
       moves == 4 && requested_none(joined()) && g.status_text().empty() &&
           logged().find("target guide: hold given up: its scene moved with the box to another slot, 4 losses "
                         "within 60 s") != std::string::npos);
    GuideRig w(true);
    w.m.set_guard(&g);
    w.rows(kGuideSlot, 2, {{kBox, 1}});
    w.pass(2000);
    w.slot(kGuideSlot, 0, 0);   /* a move from a locked slot counts too */
    for (int k = 0; k < 4; k++) {
        w.rows(k % 2 ? kGuideSlot : kOtherGuideSlot, 2, {{kBox, 1}});
        w.pass(2000);
    }
    w.show_map();
    joined();
    w.pass(60000);
    ok("the box moving 4 times within 60 s while the map is hidden: given up, never asked for, no strike",
       requested_none(joined()) && g.status_text().empty());

    /* The guard: one mapping whichever slot the hold is in, so a car whose
     * KOMBI drops Dcsm2 at the hold's mapping restarts twice at most. */
    unlink(path.c_str());
    {
        Guard d;
        d.load(path, "t", "boot-1");
        GuideRig e(true);
        e.m.set_guard(&d);
        map_then_drop(e, kGuideSlot);
        ok("the display service gone 2 s after the hold's mapping in 0x105: one strike against guide auto",
           d.status_text() == "guard: guide auto 0x10004f1 0: 1 strike\n" && !e.m.hold_disabled(0));
        map_then_drop(e, kOtherGuideSlot);
        ok("then 2 s after its mapping in 0x103: the second strike, disabled",
           d.status_text() == "guard: guide auto 0x10004f1 0: disabled, 2 strikes\n" && e.m.hold_disabled(0));
        joined();
        e.port.up = true;
        e.m.on_availability(true, e.now);
        e.rows(kGuideSlot, 2, {{kBox, 1}});
        e.show_map();
        e.pass(30000);
        ok("then the box in 0x105 again: never asked for, the map told",
           joined().find("guide") == std::string::npos && e.m.hold_disabled(0));
    }
    {
        Guard d;
        d.load(path, "t", "boot-2");
        GuideRig e(true);
        e.m.set_guard(&d);
        e.rows(kOtherGuideSlot, 2, {{kBox, 1}});
        e.show_map();
        joined();
        e.pass(30000);
        e.rows(kGuideSlot, 2, {{kBox, 1}});
        e.pass(30000);
        ok("the next boot: the box in 0x103, then 0x105, never asked for, the map told",
           requested_none(joined()) && e.m.hold_disabled(0) && e.state(0) == TargetState::Shown);
    }
    unlink(path.c_str());

    /* The seat-belt warning in the followed slot. */
    GuideRig b(true);
    b.rows(kOtherGuideSlot, 2, {{kBox, 1}, {kBelt, 3}});
    b.show_map();
    b.pass();
    b.rows(kOtherGuideSlot, 2, {{kGuideScene, 1}, {kBelt, 3}, {kBox, 3}});
    b.m.on_shown(kGuideScene, b.now);
    joined();
    b.rows(kOtherGuideSlot, 2, {{kBelt, 1}, {kGuideScene, 3}, {kBox, 3}});
    b.m.on_parked(kGuideScene, b.now);
    b.pass();
    ok("auto, the seat-belt panel shown over the hold in 0x103: released at once, never taken over, not moved",
       calls_are("slots?; release guide") && b.state(GuideRig::kGuide) == TargetState::Idle &&
           b.guide_slot() == kOtherGuideSlot);
    b.rows(kOtherGuideSlot, 2, {{kBelt, 1}, {kBox, 3}});
    b.pass(10000);
    ok("not asked for while it is shown", calls_are(""));
    b.rows(kOtherGuideSlot, 2, {{kBelt, 0}, {kBox, 1}});
    b.pass();
    ok("the warning over: asked for again in 0x103", calls_are("request guide"));

    /* The first table, the box in neither slot yet: the HMI's choice,
     * 0x105 when it is enabled, else 0x103. */
    auto first = [](GuideRig &g, uint32_t s105, uint32_t s103) {
        SlotInfo m, a, b;
        m.slot = kMapSlot; m.status = 2; m.content = kStockMap; m.map_status = 1;
        a.slot = kGuideSlot; a.status = s105;
        b.slot = kOtherGuideSlot; b.status = s103;
        g.m.on_slots({m, b, a}, g.now);
    };
    GuideRig o(true);
    logged();
    first(o, 1, 0);
    ok("the first table, 0x105 available and 0x103 disabled, no box: the hold's slot is 0x105, logged",
       o.guide_slot() == kGuideSlot &&
           logged().find("target guide: hold moves to slot 0x105, the first of the box's slots the slot table "
                         "reports enabled (status 1) (no slot before)") != std::string::npos);
    o.show_map();
    joined();
    o.pass();
    o.rows(kGuideSlot, 2, {{kGuideScene, 1}});
    o.m.on_shown(kGuideScene, o.now);
    ok("the map shown: asked for in 0x105 before the box", calls_are("publish guide; request guide") &&
                                                              o.state(GuideRig::kGuide) == TargetState::Shown);
    o.rows(kGuideSlot, 2, {{kGuideScene, 1}, {kBox, 3}, {kBelt, 3}});
    o.pass(20000);
    ok("then the box mapped, parked behind ours: no take-over, not moved, still shown",
       calls_are("") && o.guide_slot() == kGuideSlot && o.state(GuideRig::kGuide) == TargetState::Shown);
    GuideRig c(true);
    first(c, 0, 1);
    c.show_map();
    joined();
    c.pass();
    ok("the first table, 0x105 disabled and 0x103 available, no box: asked for in 0x103",
       c.guide_slot() == kOtherGuideSlot && calls_are("publish guide; request guide"));
    GuideRig both(true);
    first(both, 2, 1);
    ok("both enabled, no box: 0x105", both.guide_slot() == kGuideSlot);
    GuideRig none(true);
    first(none, 0, 0);
    none.show_map();
    joined();
    none.pass(10000);
    ok("both disabled, no box: no slot, never asked for", none.guide_slot() == 0 && requested_none(joined()));
    first(none, 0, 1);
    none.pass();
    ok("then 0x103 reported available: asked for there", none.guide_slot() == kOtherGuideSlot &&
                                                            calls_are("publish guide; request guide"));
    first(none, 1, 1);
    none.pass(100);
    ok("then 0x105 available too, no box: the hold keeps 0x103", none.guide_slot() == kOtherGuideSlot);

    /* An explicit slot keeps its slot whatever the table says. */
    GuideRig x;
    x.slot(kGuideSlot, 0, 0);
    x.rows(kOtherGuideSlot, 2, {{kBox, 1}});
    x.show_map();
    joined();
    x.pass(10000);
    ok("guide_slots = 0x105, the box in 0x103 and 0x105 disabled: never asked for, waiting as \"slot locked\"",
       requested_none(joined()) && x.guide_slot() == kGuideSlot &&
           x.m.status(GuideRig::kGuide).reason == "slot locked");
}

int main()
{
    refcount_and_linger();
    timeout_and_ladder();
    stock_yield();
    stock_contend();
    foreign_always_yields();
    idle_precheck();
    busy_refusal_unknown_holder();
    availability();
    shared_scene_id();
    request_gap();
    lost_while_lingering();
    scene_failure();
    ladder_rechecks_the_table();
    lost_to_our_own_id();
    lock_is_not_evidence();
    slot_comes_back();
    multi_row_holder();
    parked_then_shown();
    take_over();
    lock_then_take_overs();
    brief_shows_back_off();
    take_overs_restart_when_shown();
    no_request_while_locked();
    fallback_and_lock();
    wake();
    warm_scenes();
    shared_scene_in_use();
    fresh_retry_cleared();
    release_confirmation();
    fresh_fetch_failed();
    removal_retry();
    rendering();
    guide_follows_the_map();
    table_in_status();
    guide_queue_order();
    guide_never_verified_out();
    guide_live_route();
    guide_yields();
    guide_door_lock();
    guide_take_over_settle();
    guide_gives_up();
    guide_slot_disabled();
    guide_guard();
    guide_gives_up_twice();
    guide_no_flapping();
    guide_auto_slot();
    hud_guard();
    {
        /* The HUD's media pop-up (0x322): the HUD map gives way while it shows */
        TargetSpec hud2;
        FakePort port2;
        FakeScenes scenes2;
        SlotManager m2{&port2, &scenes2};
        hud2.name = "hud"; hud2.slot = kHudSlot; hud2.scene_id = kPanelScene; hud2.width = 800; hud2.height = 480;
        hud2.linger_ms = 3000;
        hud2.yield_slots = {0x322};
        hud2.yield_ms = 1500;
        m2.add(hud2);
        int64_t now = 100000;
        const auto run = [&](int64_t ms) { for (int64_t x = 0; x < ms; x += 10) { now += 10; m2.tick(now); } };
        const auto row = [&](uint64_t slot, uint32_t status, uint64_t content, uint32_t map) {
            SlotInfo si; si.slot = slot; si.status = status; si.content = content; si.map_status = map;
            m2.on_slots({si}, now);
        };
        calls.clear();
        m2.want(0, true, true);
        run(1200);
        m2.on_shown(kPanelScene, now);
        run(20);
        ok("popup: the HUD map asked for and shown", m2.status(0).state == TargetState::Shown);
        calls.clear();
        row(0x322, 2, 0x01000251, 1);          /* the media list opens in the HUD */
        row(kHudSlot, 0, kPanelScene, 1);      /* the KOMBI takes the map down for it */
        run(50);
        ok("popup: the map releases its slot and waits",
           m2.status(0).state == TargetState::Waiting && joined().find("release hud") != std::string::npos);
        row(kHudSlot, 1, kPanelScene, 0);      /* the HUD slot is free again while the pop-up shows */
        run(3000);
        ok("popup: not asked for while the pop-up shows, though the slot is free",
           m2.status(0).state == TargetState::Waiting && joined().find("request hud") == std::string::npos);
        row(0x322, 1, 0x01000251, 0);          /* the pop-up closes */
        run(1000);
        ok("popup: still waiting within yield_ms after it closed", joined().find("request hud") == std::string::npos);
        run(1500);
        ok("popup: asked for again once yield_ms has passed", joined().find("request hud") != std::string::npos);
    }
    {
        /* BMW's HUD guidance (0x01000210 in 0x330): the HUD gives way while it shows, in any slot, and only to
         * the contents listed (BMW's speed in another slot does not count) */
        TargetSpec hud5;
        FakePort port5;
        FakeScenes scenes5;
        SlotManager m5{&port5, &scenes5};
        hud5.name = "hud"; hud5.slot = kHudSlot; hud5.scene_id = kPanelScene; hud5.width = 800; hud5.height = 480;
        hud5.linger_ms = 3000;
        hud5.yield_slots = {0x322};
        hud5.yield_contents = {0x01000210};
        hud5.yield_ms = 1500;
        m5.add(hud5);
        int64_t now = 400000;
        const auto run = [&](int64_t ms) { for (int64_t x = 0; x < ms; x += 10) { now += 10; m5.tick(now); } };
        const auto row = [&](uint64_t slot, uint32_t status, uint64_t content, uint32_t map) {
            SlotInfo si; si.slot = slot; si.status = status; si.content = content; si.map_status = map;
            m5.on_slots({si}, now);
        };
        calls.clear();
        m5.want(0, true, false);
        run(1200);
        m5.on_shown(kPanelScene, now);
        row(kHudSlot, 2, kPanelScene, 1);
        run(20);
        ok("guidance: the HUD asked for and shown", m5.status(0).state == TargetState::Shown);
        calls.clear();
        row(0x331, 2, 0x01000299, 1);          /* other stock content in a HUD slot: no give-way */
        run(500);
        ok("guidance: other content shown elsewhere does not move the HUD",
           m5.status(0).state == TargetState::Shown && joined().find("release hud") == std::string::npos);
        row(0x330, 2, 0x01000210, 1);          /* a route starts: BMW's guidance shows */
        run(50);
        ok("guidance: the HUD releases its slot and waits",
           m5.status(0).state == TargetState::Waiting && joined().find("release hud") != std::string::npos);
        row(kHudSlot, 1, 0, 0);
        run(3000);
        ok("guidance: not asked for while it shows, though our slot is free",
           joined().find("request hud") == std::string::npos);
        row(0x330, 1, 0x01000210, 0);          /* the route ends */
        run(1000);
        ok("guidance: still waiting within yield_ms after it went", joined().find("request hud") == std::string::npos);
        run(1500);
        ok("guidance: asked for again once yield_ms has passed", joined().find("request hud") != std::string::npos);
    }
    {
        /* Lost while the table names our id, the media pop-up showing before
         * the next report: the pop-up's wait settles that loss, so a later
         * wait (a request unanswered) is not taken for it */
        TargetSpec hud4;
        FakePort port4;
        FakeScenes scenes4;
        SlotManager m4{&port4, &scenes4};
        hud4.name = "hud"; hud4.slot = kHudSlot; hud4.scene_id = kPanelScene; hud4.width = 800; hud4.height = 480;
        hud4.linger_ms = 3000;
        hud4.yield_slots = {0x322};
        hud4.yield_ms = 1500;
        m4.add(hud4);
        std::vector<std::string> ev;
        m4.on_evidence = [&](Evidence e, const std::string &, int64_t) { ev.push_back(evidence_name(e)); };
        int64_t now = 300000;
        const auto run = [&](int64_t ms) { for (int64_t x = 0; x < ms; x += 10) { now += 10; m4.tick(now); } };
        const auto row = [&](uint64_t slot, uint32_t status, uint64_t content, uint32_t map) {
            SlotInfo si; si.slot = slot; si.status = status; si.content = content; si.map_status = map;
            m4.on_slots({si}, now);
        };
        m4.want(0, true, true);
        run(1200);
        m4.on_shown(kPanelScene, now);
        row(kHudSlot, 2, kPanelScene, 1);
        run(20);
        m4.on_lost(kPanelScene, "fetch removed", now);
        row(0x322, 2, 0x01000251, 1);
        row(kHudSlot, 1, 0, 0);
        run(50);
        row(0x322, 1, 0x01000251, 0);
        calls.clear();
        run(2000);
        const bool asked = joined().find("request hud") != std::string::npos;
        run(SlotManager::kStartTimeoutMs + 20);
        ok("popup over an undecided loss: asked for again after it, an unanswered request waits as that, no evidence",
           asked && m4.status(0).reason == "no answer from the display" && ev.empty());
    }
    for (int contending = 0; contending < 2; contending++) {
        /* Stock content parked behind ours in the HUD slot (Google Maps' turn cards while only an overlay holds
         * the HUD): a target that doesn't contend gives way and waits until the slot is free; one that contends
         * (the Waze map) keeps it */
        TargetSpec hud3;
        FakePort port3;
        FakeScenes scenes3;
        SlotManager m3{&port3, &scenes3};
        hud3.name = "hud"; hud3.slot = kHudSlot; hud3.scene_id = kPanelScene; hud3.width = 800; hud3.height = 480;
        hud3.linger_ms = 3000;
        hud3.yield_slots = {0x322};
        hud3.yield_ms = 1500;
        m3.add(hud3);
        int64_t now = 200000;
        const auto run = [&](int64_t ms) { for (int64_t x = 0; x < ms; x += 10) { now += 10; m3.tick(now); } };
        const auto rows = [&](std::vector<SlotInfo> v) { m3.on_slots(v, now); };
        const auto row = [&](uint64_t slot, uint32_t status, uint64_t content, uint32_t map) {
            SlotInfo si; si.slot = slot; si.status = status; si.content = content; si.map_status = map; return si;
        };
        calls.clear();
        m3.want(0, true, contending == 1);
        run(1200);
        m3.on_shown(kPanelScene, now);
        run(20);
        calls.clear();
        rows({row(kHudSlot, 2, kPanelScene, 1), row(kHudSlot, 2, 0x01000361, 3)});   /* cards mapped, parked behind ours */
        run(50);
        if (!contending) {
            ok("parked behind: an overlay-only HUD gives way to the stock content",
               m3.status(0).state == TargetState::Waiting && joined().find("release hud") != std::string::npos);
            rows({row(kHudSlot, 2, 0x01000361, 1)});                                /* the cards show */
            run(3000);
            ok("parked behind: not asked for while the stock content shows", joined().find("request hud") == std::string::npos);
            rows({row(kHudSlot, 1, 0x01000361, 0)});                                /* they go */
            run(3000);
            ok("parked behind: asked for again once the slot is free", joined().find("request hud") != std::string::npos);
        } else {
            ok("parked behind: a contending target (the Waze map) keeps its slot",
               m3.status(0).state == TargetState::Shown && joined().find("release hud") == std::string::npos);
        }
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
