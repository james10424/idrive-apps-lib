/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Slot arbitration: when each display target's scene is published and shown
 * in its slot, for every client of the service at once (README.md
 * "Arbitration").
 *
 * A target (the cluster panel, the map area, the HUD) is one slot with one
 * scene; all of its clients' layers are drawn in that scene (stack.h). The
 * slot manager is told each tick whether any layer on the target wants it
 * shown, and keeps it so:
 *
 *   Idle --wanted--> Acquiring --shown--> Shown --not wanted--> Lingering
 *                        |                  ^ |                   |    |
 *                        |                  | +--lost--+   wanted |    | linger_ms
 *                        |                  +----------|----------+    v
 *                        +--refused/timeout--> Waiting <+            release --> Idle
 *
 * Acquiring and Shown go to Parked when the display parks the scene, and
 * Lingering is released (Idle); Parked goes to Shown when the display shows
 * it, to Waiting when it is lost, and is released when nothing wants it.
 *
 * - The slot is requested when at least one layer wants it and released only
 *   when none has for linger_ms, so clients coming and going, or one
 *   replacing another, never unmaps it: their layers change in the scene.
 * - The display never takes a slot from the content it shows: a map into a
 *   slot that shows other content is accepted and parked, invisible, and
 *   the oldest parked mapping is shown when the shown one goes (VERIFIED on
 *   the car). Parked keeps the target mapped and fetched, with no timeout,
 *   until the display shows it. A contending target whose slot shows the
 *   stock HMI's content and that takes over from the stock HMI
 *   (TargetSpec::take_over_stock) unmaps that content as the HMI
 *   (Port::take_over), at most once per kTakeOverMs[] step: 1.5 s apart
 *   four times, then 10 s, then every 30 s, the count starting over once
 *   the target has been shown kTakeOverSettleMs, or when it is released
 *   (a hold's only after kTakeOverSettleMs shown). The KOMBI shows ours
 *   when the HMI's map is not mapped again first, a race lost about 30% of
 *   the time (VERIFIED on the car); the HMI maps its content again, parked
 *   behind ours, so it comes back when ours goes.
 * - Waiting is a slot someone else holds, a refusal, no answer, or a loss
 *   whose cause waits for the next table report (kVerdictMs). Who holds
 *   it comes from the display's slot table, one row per mapping: the row
 *   shown (map status foreground or background) is the holder, content of
 *   the stock HMI (scene ids 0x0100xxxx that are not ours), or of anything
 *   else (an app that does not use this service). Content of anything else
 *   is always yielded to: the slot is asked for again only once it is
 *   reported free. The stock HMI's is yielded to the same way unless a
 *   wanting layer contends; a contending target retries after 2, 5, 10, 20,
 *   then every 30 s, and the ladder starts over after 30 s shown. Any other
 *   refusal or no answer retries on the same ladder. A target waiting on
 *   the ladder whose slot the table reports disabled or with another
 *   content mapped, and then free again (a door-open warning come and
 *   gone), is asked for at once; after a lock, a take-over target is
 *   asked for once the slot is reported not locked, even with stock
 *   content shown there, which it takes over. A loss with the slot
 *   reported locked does not climb the ladder. Nothing is asked for while
 *   the table reports the slot disabled, for at most kLockTrustMs after
 *   that report, a hold for as long as the table says so, and a hold not
 *   while the table does not name its slot (lock_holds): the display
 *   ignores a map into a locked slot. A request
 *   sent before or during a lock and still unanswered when the slot is
 *   reported free again is taken down and asked for again at once.
 * - Requests to a slot are spaced by at least kMinRequestGapMs, and one
 *   request is outstanding per slot. After a release, the next request of
 *   the scene waits until the display confirms the release
 *   (Port::confirmed) and, when subscriptions are observable, the renderer
 *   has unsubscribed from the scene (in the emulator about 150 ms after the
 *   display's confirmation), at most kConfirmMs. A fetch the display fails within
 *   kFreshSceneMs of the scene's publication (its renderer not having seen
 *   the publication yet) is asked again after kFreshRetryMs, up to
 *   kFreshRetries times; then the ladder.
 * - A shown scene the display removes (no other content in the slot, the
 *   slot not locked, not our release) is asked for again no sooner than
 *   kRemovalWaitMs after the removal, or the ladder's step if longer. A
 *   fetch that fails within kNotReadyMs of a request made within
 *   kNotReadyWindowMs of the removal is the display not ready yet: asked
 *   again after kNotReadyRetryMs, up to kNotReadyRetries times, which
 *   covers the 7 s the KOMBI once took to accept the scene again; then
 *   the ladder.
 * - Warm scenes: a target's scene is published at its first request and
 *   stays published while the target is released, lost, refused or timed
 *   out; only the display's fetch and map cycle. A released target stages
 *   and flushes nothing. The scene goes only when another target needs its
 *   scene id: two targets with one scene id cannot both be requested (the
 *   second waits until the first is released), and the second's request
 *   takes the first's scene down before it publishes its own.
 * - The display service going away takes every target down; when it comes
 *   back, the targets still wanted are requested again after kSettleMs. The
 *   scenes stay published: RAMSES publication does not go through Dcsm2,
 *   and a renderer that subscribes gets the scene as it was last flushed.
 * - Rendering: the display's renderer subscribes to a scene it is to draw,
 *   which the RAMSES client logs (ramses_console.h). A target the display
 *   shows counts as rendered once its renderer is subscribed to its scene.
 *   Once any subscription has been seen in this process, a shown target
 *   whose scene is not subscribed kVerifyMs after the display showed it
 *   (or after the renderer unsubscribed) is released and asked for again,
 *   at once the first time and on the ladder after that, with its scene
 *   kept. Before any subscription has been seen nothing is verified.
 * - Holds: a target that follows another (TargetSpec::follows) keeps its
 *   slot from the stock HMI exactly while that target is shown (follow()),
 *   with an empty scene, which rendering verification never releases (an
 *   empty scene's subscription proves nothing on screen). Each change of a
 *   hold's state is logged with its slot's rows; so is each take-over, and,
 *   for any take-over target, the stock HMI's content going parked in its
 *   slot (mapped again behind ours). A hold with
 *   TargetSpec::table_slots takes the first of them a table report has
 *   enabled, and moves to the one the latest report has one of its
 *   take_over_contents mapped in (shown or parked), not reported disabled
 *   (move_hold): released there first if requested, so it never holds two
 *   slots; a wait on the ladder carries over.
 *
 * Everything here is host-testable: the display's placement service (Port)
 * and the scenes (Scenes) are interfaces. */
#ifndef CRS_SLOTS_H
#define CRS_SLOTS_H

#include <stdint.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "governor.h"

namespace crs {

/* Where a target's scene goes: its slot, content class and scene. On
 * iDrive 8 the slot is a DCSM category and the scene goes as the content
 * offered in it (dcsm27_port.h). */
struct TargetSpec {
    std::string name;
    uint64_t slot = 0;
    uint32_t content_class = 0;
    uint64_t content = 0;   /* iDrive 8 */
    uint64_t scene_id = 0;
    uint32_t width = 0, height = 0;   /* scene size in pixels */
    int linger_ms = 2000;             /* kept mapped this long after the last layer stops wanting it */
    /* Parked behind the stock HMI's content while a layer contends: unmap
     * that content as the HMI (SlotManager, Port::take_over). */
    bool take_over_stock = false;
    /* A hold (SlotManager::follow): the name of the target it follows. Its
     * scene is never drawn and takes no client's layers. */
    std::string follows;
    /* A hold's: the only contents its take-overs unmap. Any other content
     * shown in its slot makes it yield; parked, it is left alone. */
    std::vector<uint64_t> take_over_contents;
    /* A hold whose slot is whichever of these the slot table reports one of
     * take_over_contents mapped in (SlotManager::on_slots); before any has,
     * the first of them the table reports enabled; slot 0 until one is. The
     * guard counts it as one mapping in any of them. */
    std::vector<uint64_t> table_slots;
    /* The mapping guard's strikes that disable this mapping; 0: never
     * disabled, the strikes only logged (guard.h). Holds: 2. */
    int strike_limit = 0;
    /* The stock HMI's pop-up slots the target gives way to (the HUD's media
     * and list pop-ups, 0x322): while one of them shows content, the target
     * is released and not asked for, so the pop-up keeps its place (asking
     * again pushed it from the HUD to the cluster); yield_ms after it goes,
     * the target is asked for again. */
    std::vector<uint64_t> yield_slots;
    /* Stock contents the target gives way to in any slot while one shows, and
     * yield_contents_ms after the last one goes. The HUD's: BMW's guidance
     * (0x01000210, in 0x300 on an X3) and the cluster's turn box (0x01000310,
     * in 0x105). Our HUD scene in 0x323 disables 0x300, so the guidance can
     * only come while the HUD is released; the turn box, which our HUD does
     * not block, starts the give-way at the route's start and keeps it while
     * the guidance blinks off (an X3, Nov 7 2026: both went for 7 s mid-route,
     * and asking again 1.5 s later kept the guidance out for the rest of it). */
    std::vector<uint64_t> yield_contents;
    int yield_contents_ms = 15000;
    int yield_ms = 1500;
    /* Its layers write from the HUD's share of the upload budget, and its
     * evidence never moves the upload cap (compositor.h). The HUD's. */
    bool share = false;
};

class Guard;

/* Who the display reports in a slot. */
enum class Holder { None, Us, Stock, Foreign };
const char *holder_name(Holder h);

/* One row of the display's slot table. A slot has one row per mapping the
 * display knows for it, each with the slot's status; a row stays listed,
 * unmapped, until its content is unfetched. */
struct SlotInfo {
    uint64_t slot = 0;
    uint32_t status = 0;      /* 0 disabled, 1 available, 2 occupied */
    uint64_t content = 0;     /* the entry's scene id; 0: none */
    uint32_t klass = 0;       /* its content class */
    uint32_t map_status = 0;  /* 0 unmapped, 1 foreground, 2 background, 3 invisible */
};

/* Why a request was not granted. */
enum class Refusal {
    Busy,         /* the slot is held: E_DCSM_PRIOTOOLOW, E_DCSM_CONTENT_ALREADYMAPPED */
    Config,       /* the scene or client is unknown to the display */
    NoSlot,       /* the slot is unknown to the display: E_DCSM_SLOTID_UNKNOWN */
    FetchFailed,  /* E_FETCH_STATUS_FAILED */
    Transient,    /* no valid reply, the fetch removed before it showed */
};

/* The display's placement service (iDrive 7: Dcsm2 fetch and map; iDrive
 * 8: RAMSES 27's DCSM offer). */
class Port {
public:
    virtual ~Port() {}
    virtual bool available() const = 0;
    /* Show the target's published scene in its slot. Answered by
     * SlotManager::on_shown or on_refused. */
    virtual void request(const TargetSpec &t) = 0;
    /* Take it out of its slot. No answer is awaited. */
    virtual void release(const TargetSpec &t) = 0;
    /* The display has confirmed every release of t's scene id since it was
     * last requested. */
    virtual bool confirmed(const TargetSpec &) const { return true; }
    /* Unmap content of the stock HMI shown in t's slot, as the HMI, so the
     * display shows t's parked scene. No answer is awaited. */
    virtual void take_over(const TargetSpec &t, uint64_t content) = 0;
    /* Ask for the slot table (SlotManager::on_slots). */
    virtual void refresh_slots() = 0;
    /* For the log only: the slot manager stopped waiting for an answer to
     * request(t) (kStartTimeoutMs) and releases it. */
    virtual void no_answer(const TargetSpec &, int64_t) {}
    /* Lines for the service's status text; empty: none. */
    virtual std::string status_text(int64_t) const { return std::string(); }
};

/* Builds, publishes and takes down each target's scene (the compositor). A
 * published scene stays published until unpublish(). */
class Scenes {
public:
    virtual ~Scenes() {}
    virtual bool publish(const TargetSpec &t, std::string *err) = 0;
    virtual void unpublish(const TargetSpec &t) = 0;
    /* The display took t's published scene again (parked or shown): the
     * renderer that subscribed to it at the fetch got it whole. */
    virtual void resent(const TargetSpec &, int64_t) {}
};

enum class TargetState { Idle, Acquiring, Parked, Shown, Lingering, Waiting, Unavailable };
const char *target_state_name(TargetState s);

/* Whether the display's renderer draws a shown or lingering target: its
 * scene subscribed (Verified), not yet (Checking), or no subscription seen
 * in this process to tell (Unobservable). None otherwise. */
enum class Rendering { None, Checking, Verified, Unobservable };
const char *rendering_name(Rendering r);

/* A target's state as clients see it. */
struct TargetStatus {
    TargetState state = TargetState::Idle;
    Holder holder = Holder::None;   /* who holds the slot while Waiting or Parked */
    std::string reason;
    int64_t retry_ms = -1;          /* Waiting on a timer: when it asks again (or decides a loss); -1: when
                                       the slot is free */
    Rendering rendering = Rendering::None;
};

class SlotManager {
public:
    static const int64_t kStartTimeoutMs = 5000;
    static const int64_t kMinRequestGapMs = 1000;
    static const int64_t kSettleMs = 1000;
    static const int64_t kSlotPollMs = 5000;
    static const int64_t kLadderResetMs = 30000;
    static const int64_t kVerdictMs = 1000;
    /* Take-overs of the stock HMI's content: the gap after each, the last
     * for every later one. The count starts over once the target has been
     * shown kTakeOverSettleMs, or when it is released (not a hold); the
     * first gap is then the least to the next. */
    static const int64_t kTakeOverMs[6];
    static const int64_t kTakeOverSettleMs = 10000;
    /* A report of a disabled slot holds its targets' requests this long at
     * most, in case the display never reports the slot again. */
    static const int64_t kLockTrustMs = 30000;
    /* A release's confirmation is awaited this long. */
    static const int64_t kConfirmMs = 1000;
    /* A shown scene's renderer subscribes within this long. It normally has
     * by the time the display shows the scene (it subscribes at the fetch);
     * the margin covers the console pipe and the loop's tick. */
    static const int64_t kVerifyMs = 1500;
    static const int64_t kFreshSceneMs = 2000;
    static const int64_t kFreshRetryMs = 250;
    static const int kFreshRetries = 3;
    static const int64_t kRemovalWaitMs = 2000;
    static const int64_t kNotReadyMs = 300;
    static const int64_t kNotReadyWindowMs = 10000;
    static const int64_t kNotReadyRetryMs = 1500;
    static const int kNotReadyRetries = 4;

    SlotManager(Port *port, Scenes *scenes);
    /* The scenes, when they are made after the slot manager (they read its
     * targets). Before the first tick. */
    void set_scenes(Scenes *scenes) { scenes_ = scenes; }
    /* Unpublishes every scene, for a clean exit. */
    void unpublish_all();

    /* Targets are fixed at start (the configuration). */
    void add(const TargetSpec &t);
    const std::vector<TargetSpec> &targets() const { return specs_; }
    int index(const std::string &name) const;   /* -1 if unknown */

    /* What the target's layers want, recomputed by the compositor each tick:
     * shown at all, and whether a wanting layer contends with the stock HMI. */
    void want(int target, bool wanted, bool contend, bool guide_hold = true);
    /* After the compositor's wants: each hold is wanted, contending, exactly
     * while the target it follows is shown and its wanting layers let it
     * hold (none has guide_hold false), no content outside
     * take_over_contents is shown in its slot (map status 1 or 2; parked, it
     * is ignored), and it has not given up; a locked slot holds it as any
     * target (lock_holds). A hold that yields or gives up is released at
     * once. A take-over of a hold unmaps only take_over_contents: the one
     * shown in its slot and those that can be parked ahead of ours
     * (Target::ahead). A hold gives up for the rest of the process when its
     * scene is lost (removed, unmapped, parked again behind other content,
     * or released to yield) with the slot not locked, or moves to another
     * slot (TargetSpec::table_slots), more than kHoldLosses times within
     * kHoldLossMs. */
    void follow(int64_t now_ms);
    static const int kHoldLosses = 3;
    static const int64_t kNoMapping = INT64_MIN / 2;
    static const int64_t kHoldLossMs = 60000;

    /* The display's answers and reports. */
    /* The mapping guard (guard.h). A mapping event is our request of a
     * target and the display's first showing of it after that request; a
     * re-park or re-show the display makes on its own is none. The display
     * service gone within Guard::kWindowMs of a hold's mapping event, held
     * still or released since, is a strike against the hold with the latest
     * one; with none, against the
     * target with the latest event (log only, as no target but a hold can
     * be disabled). A hold that gives up (kHoldLosses) on faults alone,
     * no yield to BMW's own content or move with the box among the losses,
     * is a strike against it too, and a hold the display refuses for its
     * slot (Refusal::NoSlot) is a strike and is not asked for again while
     * the service runs. A target whose mapping is disabled is never wanted. */
    void on_availability(bool up, int64_t now_ms);
    void set_guard(Guard *g);
    std::string guard_text() const;
    /* Every slot the latest reports name, one line each ("slot 0x203 status 2: 0x1000400 class 0 map 1, ..."),
     * for the status: what else the KOMBI shows while ours is up. */
    std::string table_text() const;
    /* A hold following the target is disabled by the guard, or by its
     * slot's refusal. */
    bool hold_disabled(int target) const;
    /* The target's own mapping is disabled (the HUD's, by the guard). */
    bool disabled(int target) const { return t_[target].disabled; }
    void on_shown(uint64_t scene_id, int64_t now_ms);
    /* Mapped, but parked invisible behind the content the slot shows. */
    void on_parked(uint64_t scene_id, int64_t now_ms);
    void on_refused(uint64_t scene_id, Refusal r, const std::string &why, int64_t now_ms);
    void on_lost(uint64_t scene_id, const std::string &why, int64_t now_ms);
    void on_slots(const std::vector<SlotInfo> &slots, int64_t now_ms);

    void tick(int64_t now_ms);

    /* What the display's renderer reports about our scenes, from the RAMSES
     * console (ramses_console.h): a subscription or unsubscription of
     * scene by the renderer guid (unknown: the client did not know the
     * scene), a renderer gone, a renderer's resource request. */
    void on_subscription(uint64_t scene, bool subscribed, bool unknown, const std::string &guid, int64_t now_ms);
    void on_renderer_gone(const std::string &guid, int64_t now_ms);
    void on_resource_request(const std::string &guid, unsigned resources, int64_t now_ms);

    const TargetStatus &status(int target) const { return t_[target].status; }
    /* True when the target is requested (Acquiring, Parked, Shown,
     * Lingering): its layers are staged and flushed. */
    bool requested(int target) const { return t_[target].requested; }
    /* True when the target's scene is published, requested or warm. */
    bool published(int target) const { return t_[target].scene; }
    /* The rendering state and its evidence; empty: none. */
    std::string rendering_text(int target, int64_t now_ms) const;
    /* Whether subscriptions are observable, and what the console reader
     * has read (console_text), for the status text. */
    std::string renderer_text(int64_t now_ms) const;
    /* Milliseconds until the next timer of any target; -1: none. */
    int64_t wake_in_ms(int64_t now_ms) const;

    Holder holder_of(uint64_t content) const;
    const Port &port() const { return *port_; }

    /* Evidence of the link or the display's renderer falling behind, for the
     * upload governor: a shown scene removed with no other content in its
     * slot and the slot not locked. A slot the stock HMI or another app
     * takes, a slot the display locks (reported disabled), a refusal or no
     * answer is arbitration, not evidence. */
    std::function<void(Evidence, const std::string &detail, int64_t now_ms)> on_evidence;
    /* The RAMSES console's line counts (RamsesConsole::counts_text). */
    std::function<std::string(int64_t now_ms)> console_text;

private:
    struct Target {
        TargetStatus status;
        bool wanted = false, contend = false, guide_hold = true;
        bool requested = false;
        bool disabled = false;       /* by the guard */
        bool declined = false;       /* a hold: the target it follows does not let it hold */
        /* The last mapping event (on_availability); kNoMapping: none since
         * the last request, or struck already. */
        int64_t mapped_ms = kNoMapping;
        uint64_t mapped_seq = 0;     /* its order among all targets' mapping events */
        bool first_show = false;     /* requested and not shown since: its showing is a mapping event */
        bool scene = false;          /* published */
        bool resend_due = false;     /* asked for again warm: its resend is charged when the display takes it */
        int64_t scene_ms = 0;        /* when it was published */
        int fresh_retries = 0;       /* fetch failures since, asked again after kFreshRetryMs */
        bool fresh_retry = false;    /* the next request may go kFreshRetryMs after the last */
        int64_t shown_ms = 0;        /* the display showed it */
        int64_t check_ms = 0;        /* Checking since */
        int recoveries = 0;          /* releases for not rendering since it was last rendered */
        int64_t since_ms = 0;        /* entered the current state */
        int64_t last_request_ms = -1000000;
        int ladder = 0;
        /* Lost while the table named our own scene id: who holds the slot
         * is decided at the next table report (or kVerdictMs). */
        bool verdict = false, verdict_removal = false;
        uint64_t verdict_seq = 0;
        int64_t lost_ms = 0;
        /* The display last removed the shown scene then (-1: not since it
         * was last shown or released), and fetch failures since, asked
         * again after kNotReadyRetryMs. */
        int64_t removed_ms = -1;
        int not_ready = 0;
        /* Waiting on the ladder: the table has since reported the slot
         * disabled or held by other content. */
        bool slot_away = false;
        /* Waiting: the table has since reported the slot disabled. */
        bool lock_away = false;
        /* Take-overs since the count last started over, and when the next
         * may go. */
        int takeovers = 0;
        int64_t takeover_ms = 0, takeover_sent_ms = -1000000;
        /* Acquiring: the table has reported the slot disabled since the
         * request, or did when it was sent. */
        bool lock_seen = false;
        /* A hold's stock contents that can be ahead of ours in its slot:
         * mapped when it was asked for, or while it was acquiring. The
         * table's row order is not the slot's queue order (not VERIFIED),
         * so the time of our request decides. Its take-overs unmap these
         * and the shown one, never content mapped after ours. */
        std::set<uint64_t> ahead;
        uint64_t takeovers_total = 0;   /* since start */
        uint64_t yielding = 0;          /* a hold: the content it yields to; 0: none */
        /* A hold's losses within kHoldLossMs: when, and whether a fault
         * (unmapped or removed with nothing off take_over_contents shown
         * in its slot) rather than a yield to BMW's own content. */
        std::vector<std::pair<int64_t, bool>> losses;
        bool given_up = false;
        bool lost_parked = false;       /* a hold: parked again, counted as a loss, since last shown */
        /* A pop-up in one of its yield slots was last seen shown then. */
        int64_t popup_ms = -1;
        /* One of its yield contents was last seen shown then. */
        int64_t content_ms = -1;
    };

    /* The renderers subscribed to one of our scenes since it was published:
     * guid -> since when, and whether it requested resources since. */
    struct Subscriber {
        int64_t since_ms = 0;
        bool asked = false;
    };
    typedef std::map<std::string, Subscriber> Subscribers;

    void set(int i, TargetState s, Holder h, const std::string &reason, int64_t now_ms, int64_t retry_ms = -1);
    void acquire(int i, int64_t now_ms);
    void take_down(int i, int64_t now_ms);
    void drop_scene(int i);
    /* The last release of the target's scene id is confirmed by the display
     * and its renderer has unsubscribed, or neither is awaited any longer. */
    bool settled(int i, int64_t now_ms) const;
    bool subscribed(uint64_t scene_id) const;
    int64_t gap(int i) const { return t_[i].fresh_retry ? kFreshRetryMs : kMinRequestGapMs; }
    /* Moves the rendering state on; a shown target not rendered is
     * released and asked for again. */
    void verify(int i, int64_t now_ms);
    /* ", N ms after the fetch, M ms after the map, target <state>". */
    std::string timing(int i, int64_t now_ms) const;
    /* The target whose published scene is scene_id; -1: none. */
    int by_published(uint64_t scene_id) const;
    void release(int i, int64_t now_ms);
    /* Waiting; on the ladder, the retry counts from from_ms (-1: now_ms),
     * at least min_ms. */
    void wait(int i, Holder h, bool use_ladder, const std::string &reason, int64_t now_ms, int64_t from_ms = -1,
              int64_t min_ms = 0);
    Holder other_holder(int i) const;
    /* The slot's latest report has it disabled: the display's own lock. */
    bool locked(int i) const;
    /* The slot is locked and that report is younger than kLockTrustMs (a
     * hold's: however old), or, for a hold, no table since the display
     * service came up has named the slot, or it has no slot yet
     * (TargetSpec::table_slots): the target waits,
     * reported "slot locked", "slot not reported" or "no slot: <slots> not
     * reported enabled", and is not asked for. */
    bool lock_holds(int i, int64_t now_ms);
    /* The row shown in the target's slot (map status foreground or
     * background) with content not ours; null: none. */
    const SlotInfo *holder_row(int i) const;
    /* The table shows the target's own scene id in its slot. */
    bool named(int i) const;
    /* A pop-up shows in one of the target's yield slots, or went less than
     * its yield_ms ago. */
    bool popup_blocks(int i, int64_t now_ms);
    /* A shown scene the display removed: evidence for the governor, a
     * share target's only logged. */
    void removal_evidence(int i, const std::string &why, int64_t ms);
    void park(int i, int64_t now_ms);
    void charge_resend(int i, int64_t now_ms);
    void take_over(int i, int64_t now_ms);
    void settle_loss(int i, Holder h, bool removal, const std::string &reason, int64_t now_ms, int64_t lost_ms);
    bool yields(int i, int64_t now_ms);
    int by_scene(uint64_t scene_id) const;
    bool scene_busy(int i) const;
    /* The slot's status from its latest rows; -1: not reported. */
    int slot_status(uint64_t id) const;
    /* "status S: <content> class C map M, ..." from the slot's latest rows. */
    std::string rows_text(uint64_t slot) const;
    /* Adds the stock contents mapped in the hold's slot to its ahead set. */
    void note_ahead(int i);
    /* A content shown in the hold's slot that is neither ours nor in its
     * take_over_contents (its row in *row); 0: none. */
    uint64_t yield_to(int i, SlotInfo *row) const;
    bool takes(int i, uint64_t content) const;
    /* A hold's scene lost while its slot is not locked, or (move) its move
     * to another slot, locked or not; fault: see Target::losses. A give-up
     * is a guard strike only when every loss in it is a fault. */
    void note_loss(int i, const char *how, bool fault, int64_t now_ms, bool move = false);
    /* For a hold with TargetSpec::table_slots: the one of them a report's
     * rows have one of its take_over_contents mapped in, the slot not
     * disabled (shown first, then its own slot, then 0x105); with none
     * and no slot yet, the first of them the report has enabled (status 1
     * or 2), as the stock HMI chooses; *why says which; 0: none. */
    uint64_t reported_slot(int i, const std::map<uint64_t, std::vector<SlotInfo>> &rows, std::string *why) const;
    /* The hold to slot: a loss, never a fault, when it had one, released
     * there first if requested; still waiting on the ladder until the same
     * time if it was, else idle. */
    void move_hold(int i, uint64_t slot, const std::string &why, int64_t now_ms);

    Port *port_;
    Scenes *scenes_;
    Guard *guard_ = nullptr;
    uint64_t mapped_seq_ = 0;
    std::vector<TargetSpec> specs_;
    std::vector<Target> t_;
    std::map<uint64_t, std::vector<SlotInfo>> slots_;   /* by slot: its rows in the latest report naming it */
    /* Per slot, from its latest report: disabled, or another content
     * mapped there. */
    std::map<uint64_t, bool> away_;
    /* Per slot: when its latest report had it disabled (locked by the
     * display); -1: that report had it enabled. */
    std::map<uint64_t, int64_t> disabled_;
    uint64_t slots_seq_ = 0;   /* table reports so far */
    int64_t available_since_ms_ = -1;
    int64_t slot_poll_ms_ = 0;
    std::map<uint64_t, int64_t> released_;   /* scene id -> its release not yet followed by a request */
    std::map<uint64_t, Subscribers> subs_;   /* by published scene id */
    uint64_t sub_lines_ = 0;                 /* subscription lines seen, any scene */
    bool unobservable_logged_ = false;
};

}  // namespace crs

#endif
