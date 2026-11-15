/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "slots.h"

#include <algorithm>
#include <cstdio>

#include "log.h"

namespace crs {

namespace {

/* Retries of a contending target, and after a refusal or no answer. */
const int64_t kLadder[] = {2000, 5000, 10000, 20000, 30000};
const int kLadderSteps = sizeof kLadder / sizeof kLadder[0];

/* HmiMain3D publishes its cluster and HUD scenes with ids 0x0100xxxx. */
bool stock_scene(uint64_t id) { return (id >> 16) == 0x0100; }

std::string parked_reason(Holder h)
{
    return h == Holder::None ? std::string("parked") : std::string("parked behind ") + holder_name(h);
}

}  // namespace

const int64_t SlotManager::kTakeOverMs[6] = {1500, 1500, 1500, 1500, 10000, 30000};
static const int kTakeOverSteps = sizeof SlotManager::kTakeOverMs / sizeof SlotManager::kTakeOverMs[0];

const char *holder_name(Holder h)
{
    switch (h) {
    case Holder::Us: return "us";
    case Holder::Stock: return "stock";
    case Holder::Foreign: return "foreign";
    default: return "none";
    }
}

const char *rendering_name(Rendering r)
{
    switch (r) {
    case Rendering::Checking: return "checking";
    case Rendering::Verified: return "verified";
    case Rendering::Unobservable: return "unobservable";
    default: return "none";
    }
}

const char *target_state_name(TargetState s)
{
    switch (s) {
    case TargetState::Acquiring: return "acquiring";
    case TargetState::Parked: return "parked";
    case TargetState::Shown: return "shown";
    case TargetState::Lingering: return "lingering";
    case TargetState::Waiting: return "waiting";
    case TargetState::Unavailable: return "unavailable";
    default: return "idle";
    }
}

SlotManager::SlotManager(Port *port, Scenes *scenes) : port_(port), scenes_(scenes)
{
}

void SlotManager::add(const TargetSpec &t)
{
    specs_.push_back(t);
    t_.push_back(Target());
}

int SlotManager::index(const std::string &name) const
{
    for (size_t i = 0; i < specs_.size(); i++)
        if (specs_[i].name == name)
            return (int)i;
    return -1;
}

int SlotManager::by_scene(uint64_t scene_id) const
{
    /* A scene id belongs to at most one requested target (scene_busy). */
    for (size_t i = 0; i < specs_.size(); i++)
        if (specs_[i].scene_id == scene_id && t_[i].requested)
            return (int)i;
    return -1;
}

int SlotManager::by_published(uint64_t scene_id) const
{
    /* and to at most one published target (acquire) */
    for (size_t i = 0; i < specs_.size(); i++)
        if (specs_[i].scene_id == scene_id && t_[i].scene)
            return (int)i;
    return -1;
}

bool SlotManager::scene_busy(int i) const
{
    for (size_t j = 0; j < specs_.size(); j++)
        if ((int)j != i && specs_[j].scene_id == specs_[i].scene_id && t_[j].requested)
            return true;
    return false;
}

int SlotManager::slot_status(uint64_t id) const
{
    auto it = slots_.find(id);
    return it == slots_.end() || it->second.empty() ? -1 : (int)it->second.front().status;
}

const SlotInfo *SlotManager::holder_row(int i) const
{
    auto it = slots_.find(specs_[i].slot);
    if (it == slots_.end())
        return nullptr;
    for (const SlotInfo &s : it->second)
        if ((s.map_status == 1 || s.map_status == 2) && s.content && holder_of(s.content, s.slot) != Holder::Us)
            return &s;
    return nullptr;
}

bool SlotManager::named(int i) const
{
    auto it = slots_.find(specs_[i].slot);
    if (it == slots_.end())
        return false;
    for (const SlotInfo &s : it->second)
        if ((s.map_status == 1 || s.map_status == 2) && s.content == specs_[i].scene_id)
            return true;
    return false;
}

Holder SlotManager::holder_of(uint64_t content, uint64_t slot) const
{
    if (content == 0)
        return Holder::None;
    const int i = by_scene(content);
    if (i >= 0 && specs_[i].slot == slot)
        return Holder::Us;
    return stock_scene(content) ? Holder::Stock : Holder::Foreign;
}

Holder SlotManager::other_holder(int i) const
{
    const SlotInfo *s = holder_row(i);
    return s ? holder_of(s->content, s->slot) : Holder::None;
}

static const char kPopupReason[] = "a stock pop-up shows in its place";

/* The HMI's route guidance (HmiMain3D's roadbook scenes,
 * CRbIcRemoteActivityBase::getSceneID): on the HUD 0x01000200, 0x01000201,
 * 0x01000210 (BMW's guidance) and 0x01000211; on the cluster 0x01000300,
 * 0x01000301, 0x01000310-0x01000314 (0x01000310: the turn box) and
 * 0x01000360. No target gives way to them in its yield slots. */
static bool is_guidance(uint64_t content)
{
    switch (content) {
    case 0x01000200: case 0x01000201: case 0x01000210: case 0x01000211:
    case 0x01000300: case 0x01000301: case 0x01000310: case 0x01000311:
    case 0x01000312: case 0x01000313: case 0x01000314: case 0x01000360:
        return true;
    default:
        return false;
    }
}

bool SlotManager::popup_blocks(int i, int64_t now_ms)
{
    Target &t = t_[i];
    const TargetSpec &spec = specs_[i];
    if (!t.yield) {
        if (t.popup_ms >= 0)
            log_info("target %s: a layer keeps it shown (yield 0); asking again", spec.name.c_str());
        t.popup_ms = -1;
        return false;
    }
    bool shown = false;
    /* Stock content parked behind ours in our own slot (the HMI mapped its
     * content after ours: Google Maps' turn cards on the HUD) - a target
     * that doesn't contend gives way to it, or the HMI moves that content
     * elsewhere (the cluster). A contending one (the Waze map) keeps it. */
    if (!t.contend) {
        auto rows = slots_.find(spec.slot);
        if (rows != slots_.end())
            for (const SlotInfo &r : rows->second)
                shown = shown || (r.content != 0 && r.content != spec.scene_id && r.map_status == 3);
    }
    for (uint64_t slot : spec.yield_slots) {
        auto rows = slots_.find(slot);
        if (rows == slots_.end())
            continue;
        for (const SlotInfo &r : rows->second)
            shown = shown || (r.content != 0 && !is_guidance(r.content) && (r.map_status == 1 || r.map_status == 2));
    }
    if (shown) {
        if (t.popup_ms < 0)
            log_info("target %s: stock content shows in a slot it gives way to (a pop-up, or parked behind it)", spec.name.c_str());
        t.popup_ms = now_ms;
        return true;
    }
    if (t.popup_ms < 0)
        return false;
    if (now_ms - t.popup_ms < spec.yield_ms)
        return true;
    log_info("target %s: the pop-up has gone; asking again", spec.name.c_str());
    t.popup_ms = -1;
    return false;
}

/* A slot the table names as someone else's is not asked for, unless a
 * contending layer takes it from the stock HMI: the target waits for it to
 * be free. */
bool SlotManager::yields(int i, int64_t now_ms)
{
    const Holder h = other_holder(i);
    if (h != Holder::Foreign && (h != Holder::Stock || t_[i].contend))
        return false;
    if (t_[i].status.state != TargetState::Waiting || t_[i].status.holder != h || t_[i].status.retry_ms >= 0)
        set(i, TargetState::Waiting, h, "slot in use", now_ms, -1);
    return true;
}

void SlotManager::want(int i, bool wanted, bool contend, bool guide_hold, bool yield)
{
    t_[i].wanted = wanted && !t_[i].refused;
    t_[i].contend = t_[i].wanted && contend;
    t_[i].guide_hold = guide_hold;
    t_[i].yield = yield;
}

bool SlotManager::fall_back(int i, const std::string &why, int64_t now_ms)
{
    Target &t = t_[i];
    if (!specs_[i].fallback_scene_id)
        return false;
    log_warn("target %s: scene 0x%llx %s; falling back to 0x%llx until cluster-render restarts",
             specs_[i].name.c_str(), (unsigned long long)specs_[i].scene_id, why.c_str(),
             (unsigned long long)specs_[i].fallback_scene_id);
    take_down(i, now_ms);
    drop_scene(i);
    released_.erase(specs_[i].scene_id);
    specs_[i].scene_id = specs_[i].fallback_scene_id;
    specs_[i].fallback_scene_id = 0;
    t.ladder = 0;
    return true;
}

void SlotManager::follow(int64_t now_ms)
{
    for (size_t k = 0; k < specs_.size(); k++) {
        const int i = (int)k;
        Target &t = t_[i];
        if (specs_[i].follows.empty())
            continue;
        const int lead = index(specs_[i].follows);
        const bool declined = lead >= 0 && !t_[lead].guide_hold;
        if (declined != t.declined)
            log_info("target %s: %s", specs_[i].name.c_str(),
                     declined ? "not held: a layer of the target it follows has guide_hold 0"
                              : "held again while the target it follows is shown");
        t.declined = declined;
        const bool shown = lead >= 0 && t_[lead].status.state == TargetState::Shown && !declined;

        SlotInfo row;
        const uint64_t y = yield_to(i, &row);
        if (y != t.yielding) {
            if (y)
                log_info("target %s: hold yields to 0x%llx (class %u, map status %u); slot 0x%llx %s",
                         specs_[i].name.c_str(), (unsigned long long)y, row.klass, row.map_status,
                         (unsigned long long)specs_[i].slot, rows_text(specs_[i].slot).c_str());
            else
                log_info("target %s: hold no longer yields: 0x%llx is not shown in slot 0x%llx",
                         specs_[i].name.c_str(), (unsigned long long)t.yielding, (unsigned long long)specs_[i].slot);
            t.yielding = y;
        }
        if (y && t.requested && !t.lost_parked)
            note_loss(i, "yielded to other content", now_ms);
        if ((y || t.given_up) && t.requested)
            release(i, now_ms);
        want(i, shown && !y && !t.given_up, true);
    }
}

bool SlotManager::takes(int i, uint64_t content) const
{
    const std::vector<uint64_t> &c = specs_[i].take_over_contents;
    return std::find(c.begin(), c.end(), content) != c.end();
}

uint64_t SlotManager::yield_to(int i, SlotInfo *row) const
{
    auto it = slots_.find(specs_[i].slot);
    if (it == slots_.end())
        return 0;
    for (const SlotInfo &s : it->second)
        if (s.content && (s.map_status == 1 || s.map_status == 2) && s.content != specs_[i].scene_id &&
            !takes(i, s.content)) {
            *row = s;
            return s.content;
        }
    return 0;
}

void SlotManager::note_loss(int i, const char *how, int64_t now_ms, bool move)
{
    Target &t = t_[i];
    if (specs_[i].follows.empty() || (locked(i) && !move) || t.given_up)
        return;
    t.losses.push_back(now_ms);
    while (now_ms - t.losses.front() > kHoldLossMs)
        t.losses.erase(t.losses.begin());
    if ((int)t.losses.size() <= kHoldLosses)
        return;
    t.given_up = true;
    log_warn("target %s: hold given up: its scene %s, %zu losses within %lld s; not asked for again until "
             "cluster-render restarts; slot 0x%llx %s",
             specs_[i].name.c_str(), how, t.losses.size(), (long long)(kHoldLossMs / 1000),
             (unsigned long long)specs_[i].slot, rows_text(specs_[i].slot).c_str());
}

uint64_t SlotManager::reported_slot(int i, const std::map<uint64_t, std::vector<SlotInfo>> &rows,
                                    std::string *why) const
{
    /* A disabled slot's rows are as the lock found them, not where the box
     * goes. */
    const std::vector<uint64_t> &allowed = specs_[i].table_slots;
    int best = 0;
    const SlotInfo *box = nullptr;
    for (uint64_t id : allowed) {
        auto r = rows.find(id);
        if (r == rows.end())
            continue;
        for (const SlotInfo &s : r->second) {
            if (!s.status || !s.map_status || !takes(i, s.content))
                continue;
            const int rank = (s.map_status == 1 || s.map_status == 2 ? 2 : 1) * 2 + (id == specs_[i].slot);
            if (rank > best) {
                best = rank;
                box = &s;
            }
        }
    }
    char b[96];
    if (box) {
        std::snprintf(b, sizeof b, "where the slot table reports 0x%llx (class %u, map status %u)",
                      (unsigned long long)box->content, box->klass, box->map_status);
        *why = b;
        return box->slot;
    }
    if (specs_[i].slot)
        return 0;
    /* No box yet: the HMI's choice, the first of them reported enabled. */
    for (uint64_t id : allowed) {
        auto r = rows.find(id);
        if (r == rows.end() || r->second.empty() || !r->second.front().status)
            continue;
        std::snprintf(b, sizeof b, "the first of the box's slots the slot table reports enabled (status %u)",
                      r->second.front().status);
        *why = b;
        return id;
    }
    return 0;
}

void SlotManager::move_hold(int i, uint64_t slot, const std::string &why, int64_t now_ms)
{
    Target &t = t_[i];
    TargetSpec &spec = specs_[i];
    char was[48] = "no slot before";
    if (spec.slot)
        std::snprintf(was, sizeof was, "was slot 0x%llx%s", (unsigned long long)spec.slot,
                      t.requested ? ", released there" : "");
    log_info("target %s: hold moves to slot 0x%llx, %s (%s); ours 0x%llx; slot 0x%llx %s", spec.name.c_str(),
             (unsigned long long)slot, why.c_str(), was, (unsigned long long)spec.scene_id, (unsigned long long)slot,
             rows_text(slot).c_str());
    if (spec.slot)
        note_loss(i, "moved with the box to another slot", now_ms, true);
    take_down(i, now_ms);
    spec.slot = slot;
    t.verdict = false;
    t.removed_ms = -1;
    t.not_ready = 0;
    t.slot_away = t.lock_away = t.lock_seen = t.lost_parked = false;
    t.ahead.clear();
    t.fresh_retry = false;
    const TargetStatus &st = t.status;
    if (st.state == TargetState::Waiting && st.retry_ms > now_ms)
        set(i, TargetState::Waiting, Holder::None, st.reason, now_ms, st.retry_ms);
    else
        set(i, TargetState::Idle, Holder::None, "", now_ms);
}

void SlotManager::note_ahead(int i)
{
    auto it = slots_.find(specs_[i].slot);
    if (it == slots_.end())
        return;
    for (const SlotInfo &s : it->second)
        if (s.map_status != 0 && holder_of(s.content, s.slot) == Holder::Stock)
            t_[i].ahead.insert(s.content);
}

std::string SlotManager::rows_text(uint64_t slot) const
{
    auto it = slots_.find(slot);
    if (it == slots_.end() || it->second.empty())
        return "not reported";
    std::string s = "status " + std::to_string(it->second.front().status) + ":";
    char b[80];
    for (const SlotInfo &r : it->second) {
        std::snprintf(b, sizeof b, "%s 0x%llx class %u map %u", &r == &it->second.front() ? "" : ",",
                      (unsigned long long)r.content, r.klass, r.map_status);
        s += b;
    }
    return s;
}

std::string SlotManager::table_text() const
{
    std::string s;
    char b[40];
    for (const auto &kv : slots_) {
        if (kv.second.empty())
            continue;
        std::snprintf(b, sizeof b, "slot 0x%llx ", (unsigned long long)kv.first);
        s += b + rows_text(kv.first) + "\n";
    }
    return s;
}

void SlotManager::set(int i, TargetState s, Holder h, const std::string &reason, int64_t now_ms, int64_t retry_ms)
{
    TargetStatus &st = t_[i].status;
    if (st.state != s && !specs_[i].follows.empty())
        log_info("target %s: hold %s (was %s)%s%s; ours 0x%llx; slot 0x%llx %s", specs_[i].name.c_str(),
                 target_state_name(s), target_state_name(st.state), reason.empty() ? "" : ": ", reason.c_str(),
                 (unsigned long long)specs_[i].scene_id, (unsigned long long)specs_[i].slot,
                 rows_text(specs_[i].slot).c_str());
    if (st.state != s)
        t_[i].since_ms = now_ms;
    st.state = s;
    st.holder = h;
    st.reason = reason;
    st.retry_ms = retry_ms;
    if (s != TargetState::Waiting)
        t_[i].fresh_retry = false;   /* only the fresh-retry wait may request early */
}

/* Out of the display's slot; the scene stays published. */
void SlotManager::take_down(int i, int64_t now_ms)
{
    Target &t = t_[i];
    if (!t.requested)
        return;
    if (port_->available()) {
        port_->release(specs_[i]);
        released_[specs_[i].scene_id] = now_ms;
    }
    t.requested = false;
    t.unmapped_ms = -1;
    t.status.rendering = Rendering::None;
}

void SlotManager::drop_scene(int i)
{
    if (!t_[i].scene)
        return;
    scenes_->unpublish(specs_[i]);
    subs_.erase(specs_[i].scene_id);
    t_[i].scene = false;
    t_[i].resend_due = false;
}

void SlotManager::republish(int i, int64_t now_ms)
{
    const TargetSpec &spec = specs_[i];
    Target &t = t_[i];
    log_info("target %s: its scene file replaced: scene 0x%llx published again in place, the slot kept",
             spec.name.c_str(), (unsigned long long)spec.scene_id);
    scenes_->unpublish(spec);
    subs_[spec.scene_id] = Subscribers();   /* its renderer subscribes again */
    t.scene_ms = now_ms;
    std::string err;
    if (scenes_->publish(spec, &err))
        return;
    t.scene = false;
    subs_.erase(spec.scene_id);
    take_down(i, now_ms);
    wait(i, Holder::None, true, "scene: " + err, now_ms);
}

void SlotManager::unpublish_all()
{
    for (size_t i = 0; i < t_.size(); i++)
        drop_scene((int)i);
}

bool SlotManager::subscribed(uint64_t scene_id) const
{
    auto s = subs_.find(scene_id);
    return s != subs_.end() && !s->second.empty();
}

bool SlotManager::settled(int i, int64_t now_ms) const
{
    auto r = released_.find(specs_[i].scene_id);
    return r == released_.end() || now_ms - r->second >= kConfirmMs ||
           (port_->confirmed(specs_[i]) && !subscribed(specs_[i].scene_id));
}

/* Released because nothing wants it: the stock HMI's content is shown again
 * because of us, so the take-overs' count starts over (their 1.5 s spacing
 * stays). A hold's starts over only after kTakeOverSettleMs shown: the map
 * it follows can come and go faster than that. */
void SlotManager::release(int i, int64_t now_ms)
{
    Target &t = t_[i];
    take_down(i, now_ms);
    t.ladder = 0;
    t.removed_ms = -1;
    if (specs_[i].follows.empty()) {
        t.takeovers = 0;
        t.takeover_ms = t.takeover_sent_ms + kTakeOverMs[0];
    }
    set(i, TargetState::Idle, Holder::None, "", now_ms);
}

void SlotManager::wait(int i, Holder h, bool use_ladder, const std::string &reason, int64_t now_ms, int64_t from_ms,
                       int64_t min_ms)
{
    Target &t = t_[i];
    int64_t retry = -1;
    if (use_ladder) {
        retry = (from_ms >= 0 ? from_ms : now_ms) + std::max(kLadder[std::min(t.ladder, kLadderSteps - 1)], min_ms);
        t.ladder++;
    }
    /* A slot already reported away (the report can come before the loss)
     * counts from the start of the wait. */
    auto a = away_.find(specs_[i].slot);
    t.slot_away = a != away_.end() && a->second;
    t.lock_away = locked(i);
    set(i, TargetState::Waiting, h, reason, now_ms, retry);
}

void SlotManager::acquire(int i, int64_t now_ms)
{
    Target &t = t_[i];
    const TargetSpec &spec = specs_[i];
    if (scene_busy(i)) {
        set(i, TargetState::Waiting, Holder::None, "scene id in use by another target", now_ms, now_ms);
        return;
    }
    if (t.scene) {
        t.resend_due = true;
    } else {
        const int other = by_published(spec.scene_id);   /* released: scene_busy */
        if (other >= 0) {
            log_info("target %s: unpublishing its released scene 0x%llx for target %s", specs_[other].name.c_str(),
                     (unsigned long long)spec.scene_id, spec.name.c_str());
            drop_scene(other);
        }
        std::string err;
        if (!scenes_->publish(spec, &err)) {
            wait(i, Holder::None, true, "scene: " + err, now_ms);
            return;
        }
        t.scene = true;
        t.scene_ms = now_ms;
        t.fresh_retries = 0;
        subs_[spec.scene_id] = Subscribers();
    }
    auto r = released_.find(spec.scene_id);
    if (r != released_.end()) {
        const bool confirmed = port_->confirmed(spec), unsubscribed = !subscribed(spec.scene_id);
        if (confirmed && unsubscribed)
            log_info("target %s: asking for scene 0x%llx %lld ms after its release, which the display confirmed%s",
                     spec.name.c_str(), (unsigned long long)spec.scene_id, (long long)(now_ms - r->second),
                     sub_lines_ ? " and its renderer unsubscribed" : "");
        else
            log_warn("target %s: asking for scene 0x%llx %lld ms after its release, without %s",
                     spec.name.c_str(), (unsigned long long)spec.scene_id, (long long)(now_ms - r->second),
                     !confirmed && !unsubscribed ? "the display's confirmation or its renderer unsubscribing"
                     : !confirmed                ? "the display's confirmation"
                                                 : "its renderer unsubscribing");
        released_.erase(r);
    }
    t.requested = true;
    t.last_request_ms = t.mapped_ms = now_ms;
    t.mapped_seq = ++mapped_seq_;
    t.first_show = true;
    t.lock_seen = locked(i);
    t.lost_parked = false;
    t.unmapped_ms = -1;
    t.ahead.clear();
    if (!spec.follows.empty())
        note_ahead(i);
    port_->request(spec);
    set(i, TargetState::Acquiring, Holder::None, "", now_ms);
}

void SlotManager::on_availability(bool up, int64_t now_ms)
{
    slots_.clear();
    away_.clear();
    disabled_.clear();
    if (up) {
        available_since_ms_ = now_ms;
        return;
    }
    available_since_ms_ = -1;
    released_.clear();
    /* A cluster restart takes the display service with it: logged with the
     * target mapped last, for reading the log. */
    int last = -1;
    for (size_t i = 0; i < t_.size(); i++)
        if (now_ms - t_[i].mapped_ms <= kMappingLogMs && (last < 0 || t_[i].mapped_seq > t_[last].mapped_seq))
            last = (int)i;
    if (last >= 0) {
        log_warn("target %s: the display service went away %lld ms after its mapping (target %s)",
                 specs_[last].name.c_str(), (long long)(now_ms - t_[last].mapped_ms),
                 target_state_name(t_[last].status.state));
        /* One incident, one line: the event is not logged again. */
        t_[last].mapped_ms = kNoMapping;
        t_[last].first_show = false;
    }
    for (Target &t : t_)
        t.verdict = false;
    /* The display is gone, and with it every mapping: nothing to release. */
    for (size_t i = 0; i < t_.size(); i++) {
        t_[i].requested = false;
        t_[i].status.rendering = Rendering::None;
        set((int)i, t_[i].wanted ? TargetState::Unavailable : TargetState::Idle, Holder::None,
            t_[i].wanted ? "display service not available" : "", now_ms);
    }
}

void SlotManager::on_shown(uint64_t scene_id, int64_t now_ms)
{
    int i = by_scene(scene_id);
    if (i < 0)
        return;
    const TargetState s = t_[i].status.state;
    if (s != TargetState::Acquiring && s != TargetState::Parked)
        return;   /* a late answer to a request taken down since */
    if (s == TargetState::Parked)
        log_info("target %s: shown, after %lld ms parked", specs_[i].name.c_str(),
                 (long long)(now_ms - t_[i].since_ms));
    charge_resend(i, now_ms);
    t_[i].lost_parked = false;
    t_[i].unmapped_ms = -1;
    set(i, TargetState::Shown, Holder::Us, "", now_ms);
    Target &t = t_[i];
    t.shown_ms = t.check_ms = now_ms;
    if (t.first_show) {
        t.first_show = false;
        t.mapped_ms = now_ms;
        t.mapped_seq = ++mapped_seq_;
    }
    t.removed_ms = -1;
    verify(i, now_ms);
}

void SlotManager::on_parked(uint64_t scene_id, int64_t now_ms)
{
    int i = by_scene(scene_id);
    if (i < 0)
        return;
    const TargetState s = t_[i].status.state;
    if (s == TargetState::Lingering) {
        release(i, now_ms);   /* nobody wants it and nothing of it is on screen */
        return;
    }
    if (s == TargetState::Shown && !specs_[i].follows.empty()) {
        note_loss(i, "parked again behind other content", now_ms);
        t_[i].lost_parked = true;   /* the yield it may lead to is the same loss */
    }
    if (s == TargetState::Acquiring || s == TargetState::Shown)
        park(i, now_ms);
}

/* A warm scene the display has fetched again went whole to its renderer
 * when it subscribed: charged to the byte budget then, once, since a fetch
 * the display fails sends nothing. */
void SlotManager::charge_resend(int i, int64_t now_ms)
{
    if (!t_[i].resend_due)
        return;
    t_[i].resend_due = false;
    scenes_->resent(specs_[i], now_ms);
}

/* Mapped and fetched, invisible behind the content the slot shows, until
 * the display shows it: no timeout. The table names who is shown. */
void SlotManager::park(int i, int64_t now_ms)
{
    charge_resend(i, now_ms);
    t_[i].unmapped_ms = -1;
    const Holder h = other_holder(i);
    log_info("target %s: %s in slot 0x%llx", specs_[i].name.c_str(), parked_reason(h).c_str(),
             (unsigned long long)specs_[i].slot);
    set(i, TargetState::Parked, h, parked_reason(h), now_ms);
    t_[i].status.rendering = Rendering::None;
    port_->refresh_slots();
}

/* Parked behind the stock HMI's content while a layer contends: that
 * content is unmapped as the HMI, so the display shows ours (and the HMI's,
 * mapped again, parks behind it). Only content of the stock HMI, only in the
 * target's own slot, and spaced by kTakeOverMs[]. */
void SlotManager::take_over(int i, int64_t now_ms)
{
    Target &t = t_[i];
    const TargetSpec &spec = specs_[i];
    if (!spec.take_over_stock || !t.contend || now_ms < t.takeover_ms)
        return;
    const SlotInfo *row = holder_row(i);
    if (!row || holder_of(row->content, row->slot) != Holder::Stock)
        return;
    /* Copies: a take-over's answer can replace the rows. */
    std::vector<SlotInfo> unmapped(1, *row), behind;
    if (!spec.follows.empty()) {
        if (!takes(i, row->content))
            return;   /* follow() yields to it */
        for (const SlotInfo &s : slots_[spec.slot])
            if (s.map_status == 3 && holder_of(s.content, s.slot) == Holder::Stock && takes(i, s.content))
                (t.ahead.count(s.content) ? unmapped : behind).push_back(s);
    }
    const int64_t gap = kTakeOverMs[std::min(t.takeovers, kTakeOverSteps - 1)];
    t.takeovers++;
    t.takeovers_total++;
    t.takeover_sent_ms = now_ms;
    t.takeover_ms = now_ms + gap;
    auto list = [](const std::vector<SlotInfo> &rows) {
        std::string s;
        char b[96];
        for (const SlotInfo &r : rows) {
            std::snprintf(b, sizeof b, "%s0x%llx (class %u, map status %u)", s.empty() ? "" : ", ",
                          (unsigned long long)r.content, r.klass, r.map_status);
            s += b;
        }
        return s;
    };
    log_info("target %s: take-over %d (%llu since start): unmapping the stock HMI's %s in slot 0x%llx as the HMI%s%s; "
             "the next no sooner than %g s; ours 0x%llx; slot %s",
             spec.name.c_str(), t.takeovers, (unsigned long long)t.takeovers_total, list(unmapped).c_str(),
             (unsigned long long)spec.slot, behind.empty() ? "" : "; left parked, mapped after ours: ",
             list(behind).c_str(), gap / 1000.0, (unsigned long long)spec.scene_id,
             rows_text(spec.slot).c_str());
    for (const SlotInfo &s : unmapped) {
        t.ahead.erase(s.content);   /* mapped again, it is behind ours */
        port_->take_over(spec, s.content);
    }
}

/* Released and asked for again as a loss (settle_loss): a lock keeps the
 * ladder and is waited out; not evidence for the governor, and not a hold's
 * loss (the display took nothing from it). */
void SlotManager::not_mapped(int i, int64_t now_ms)
{
    Target &t = t_[i];
    const TargetSpec &spec = specs_[i];
    const TargetState s = t.status.state;
    log_warn("target %s: the display accepted scene 0x%llx and has not mapped it for %lld ms (target %s, %s); "
             "releasing it and asking again; slot 0x%llx %s",
             spec.name.c_str(), (unsigned long long)spec.scene_id, (long long)(now_ms - t.unmapped_ms),
             target_state_name(s), s == TargetState::Acquiring ? "no map status" : "the slot table reports it unmapped",
             (unsigned long long)spec.slot, rows_text(spec.slot).c_str());
    if (s == TargetState::Acquiring)
        port_->no_answer(spec, now_ms);
    const Holder h = other_holder(i);
    take_down(i, now_ms);
    if (spec.follows.empty()) {   /* as at any release (release()) */
        t.takeovers = 0;
        t.takeover_ms = t.takeover_sent_ms + kTakeOverMs[0];
    }
    if (s == TargetState::Lingering) {
        set(i, TargetState::Idle, Holder::None, "", now_ms);
        return;
    }
    settle_loss(i, h, false, "accepted, not mapped", now_ms, now_ms);
}

void SlotManager::on_refused(uint64_t scene_id, Refusal r, const std::string &why, int64_t now_ms)
{
    int i = by_scene(scene_id);
    if (i < 0 || t_[i].status.state != TargetState::Acquiring)
        return;
    /* Who holds the slot, read while our scene still counts as ours. */
    Target &t = t_[i];
    if (r == Refusal::FetchFailed && now_ms - t.scene_ms <= kFreshSceneMs && t.fresh_retries < kFreshRetries) {
        t.fresh_retries++;
        log_info("target %s: fetch failed %lld ms after its scene was published; asking again in %lld ms (%d of %d)",
                 specs_[i].name.c_str(), (long long)(now_ms - t.scene_ms), (long long)kFreshRetryMs,
                 t.fresh_retries, kFreshRetries);
        take_down(i, now_ms);
        t.fresh_retry = true;
        set(i, TargetState::Waiting, Holder::None, "refused: " + why, now_ms, now_ms + kFreshRetryMs);
        return;
    }
    if (r == Refusal::FetchFailed && t.removed_ms >= 0 && t.last_request_ms - t.removed_ms <= kNotReadyWindowMs &&
        now_ms - t.last_request_ms <= kNotReadyMs && t.not_ready < kNotReadyRetries) {
        t.not_ready++;
        log_info("target %s: fetch failed %lld ms after the request, %lld ms after the display removed the scene: "
                 "not ready; asking again in %lld ms (%d of %d)",
                 specs_[i].name.c_str(), (long long)(now_ms - t.last_request_ms), (long long)(now_ms - t.removed_ms),
                 (long long)kNotReadyRetryMs, t.not_ready, kNotReadyRetries);
        take_down(i, now_ms);
        set(i, TargetState::Waiting, Holder::None, "refused: " + why, now_ms, now_ms + kNotReadyRetryMs);
        return;
    }
    if (r == Refusal::NoSlot && !specs_[i].follows.empty()) {
        log_warn("target %s: refused (%s): not asked for again until cluster-render restarts", specs_[i].name.c_str(),
                 why.c_str());
        take_down(i, now_ms);
        t.refused = true;
        set(i, TargetState::Idle, Holder::None, "refused: " + why, now_ms);
        return;
    }
    /* The display does not know the scene: the id is refused. */
    if (r == Refusal::UnknownContent && fall_back(i, "refused: " + why, now_ms)) {
        wait(i, Holder::None, true, "refused: " + why, now_ms);
        return;
    }
    Holder h = Holder::None;
    if (r == Refusal::Busy) {
        h = other_holder(i);
        if (h == Holder::None)
            h = Holder::Stock;   /* held by someone the table has not named yet */
    }
    take_down(i, now_ms);
    if (h == Holder::Foreign || (h == Holder::Stock && !t_[i].contend))
        wait(i, h, false, "refused: " + why, now_ms);
    else
        wait(i, h, true, "refused: " + why, now_ms);
}

void SlotManager::on_lost(uint64_t scene_id, const std::string &why, int64_t now_ms)
{
    int i = by_scene(scene_id);
    if (i < 0)
        return;
    const TargetState s = t_[i].status.state;
    if (s != TargetState::Shown && s != TargetState::Lingering && s != TargetState::Acquiring &&
        s != TargetState::Parked)
        return;
    const bool ours_named = named(i);
    const Holder h = other_holder(i);
    take_down(i, now_ms);
    note_loss(i, why.c_str(), now_ms);
    /* A scene removed while shown is evidence for the governor; one lost
     * before it showed is not, and neither is one the display took with a
     * lock (the slot reported disabled: a door-open warning). */
    const bool removal = (s == TargetState::Shown || s == TargetState::Lingering) && !locked(i);
    if (s == TargetState::Lingering) {
        if (h == Holder::None && !ours_named && removal)
            removal_evidence(i, why, now_ms);
        set(i, TargetState::Idle, Holder::None, "", now_ms);
        return;
    }
    if (ours_named) {
        /* The table still names our scene id: a report from before the
         * loss, or someone else's content with that id (0x01000400 is also
         * the HMI's speech dialog). The next report, after our unfetch,
         * tells which; without one in kVerdictMs it was ours. */
        Target &t = t_[i];
        t.verdict = true;
        t.verdict_seq = slots_seq_;
        t.verdict_removal = removal;
        t.lost_ms = now_ms;
        port_->refresh_slots();
        set(i, TargetState::Waiting, Holder::None, "taken: " + why, now_ms, now_ms + kVerdictMs);
        return;
    }
    settle_loss(i, h, removal, "taken: " + why, now_ms, now_ms);
}

bool SlotManager::locked(int i) const
{
    auto d = disabled_.find(specs_[i].slot);
    return d != disabled_.end() && d->second >= 0;
}

bool SlotManager::lock_holds(int i, int64_t now_ms)
{
    const bool hold = !specs_[i].follows.empty();
    auto d = disabled_.find(specs_[i].slot);
    const bool lock = d != disabled_.end() && d->second >= 0 && (hold || now_ms - d->second < kLockTrustMs);
    const bool unlisted = hold && (!specs_[i].slot || slot_status(specs_[i].slot) < 0);
    if (!lock && !unlisted)
        return false;
    std::string why = lock ? "slot locked" : "slot not reported";
    if (!specs_[i].slot) {
        why = "no slot:";
        char b[24];
        for (uint64_t s : specs_[i].table_slots) {
            std::snprintf(b, sizeof b, " 0x%llx", (unsigned long long)s);
            why += b;
        }
        why += " not reported enabled";
    }
    const TargetStatus &st = t_[i].status;
    set(i, TargetState::Waiting, st.holder, why, now_ms, st.state == TargetState::Waiting ? st.retry_ms : now_ms);
    return true;
}

void SlotManager::removal_evidence(int i, const std::string &why, int64_t ms)
{
    if (specs_[i].share)
        log_info("target %s: %s (the HUD share's: the upload cap stays)", specs_[i].name.c_str(), why.c_str());
    else if (on_evidence)
        on_evidence(Evidence::Removal, specs_[i].name + ": " + why, ms);
}

/* The ladder and the evidence count from lost_ms, when the scene went. */
void SlotManager::settle_loss(int i, Holder h, bool removal, const std::string &reason, int64_t now_ms,
                              int64_t lost_ms)
{
    Target &t = t_[i];
    const bool by_display = h == Holder::None && removal && !locked(i);
    t.removed_ms = by_display ? lost_ms : -1;
    t.not_ready = 0;
    if (by_display)
        removal_evidence(i, reason, lost_ms);
    if (h == Holder::Foreign || (h == Holder::Stock && !t.contend)) {
        wait(i, h, false, reason, now_ms);
        return;
    }
    const int ladder = t.ladder;
    wait(i, h, true, reason, now_ms, lost_ms, by_display ? kRemovalWaitMs : 0);
    if (locked(i))
        t.ladder = ladder;   /* the display's lock, not a contest for the slot */
}

void SlotManager::on_slots(const std::vector<SlotInfo> &slots, int64_t now_ms)
{
    /* A slot named in the report is replaced by its rows there; one it
     * leaves out keeps its last rows. */
    std::map<uint64_t, std::vector<SlotInfo>> rows;
    for (const SlotInfo &s : slots)
        rows[s.slot].push_back(s);
    /* A hold whose slot comes from the table goes to the slot reported_slot
     * names, once the rows are in. */
    std::map<int, std::pair<uint64_t, std::string>> moves;
    for (size_t i = 0; i < specs_.size(); i++) {
        std::string why;
        const uint64_t to = specs_[i].table_slots.empty() ? 0 : reported_slot((int)i, rows, &why);
        if (to && to != specs_[i].slot)
            moves[(int)i] = {to, why};
    }
    /* The stock HMI's content newly parked in a requested take-over target's
     * slot: mapped again behind ours. (target, content, its map status
     * before: -1 not listed) */
    struct Parked {
        int target;
        uint64_t content;
        int was;
    };
    std::vector<Parked> parked;
    for (size_t i = 0; i < specs_.size(); i++) {
        auto now_rows = rows.find(specs_[i].slot);
        if (!specs_[i].take_over_stock || !t_[i].requested || now_rows == rows.end() || moves.count((int)i))
            continue;
        auto old = slots_.find(specs_[i].slot);
        for (const SlotInfo &s : now_rows->second) {
            if (s.map_status != 3 || holder_of(s.content, s.slot) != Holder::Stock)
                continue;
            int was = -1;
            if (old != slots_.end())
                for (const SlotInfo &o : old->second)
                    if (o.content == s.content)
                        was = (int)o.map_status;
            if (was != 3)
                parked.push_back(Parked{(int)i, s.content, was});
        }
    }
    for (auto &kv : rows)
        slots_[kv.first].swap(kv.second);
    for (const auto &mv : moves)
        move_hold(mv.first, mv.second.first, mv.second.second, now_ms);
    /* A hold's contents ahead of ours: joined while it acquires, left once
     * unmapped or gone. */
    for (size_t i = 0; i < specs_.size(); i++) {
        auto it = slots_.find(specs_[i].slot);
        if (specs_[i].follows.empty() || !t_[i].requested || !rows.count(specs_[i].slot))
            continue;
        if (t_[i].status.state == TargetState::Acquiring)
            note_ahead((int)i);
        for (auto a = t_[i].ahead.begin(); a != t_[i].ahead.end();) {
            bool mapped = false;
            for (const SlotInfo &s : it->second)
                mapped = mapped || (s.content == *a && s.map_status != 0);
            a = mapped ? std::next(a) : t_[i].ahead.erase(a);
        }
    }
    for (const Parked &p : parked)
        log_info("target %s: the stock HMI's 0x%llx is mapped again, parked behind %s (map status 3, was %s); "
                 "ours 0x%llx; slot 0x%llx %s",
                 specs_[p.target].name.c_str(), (unsigned long long)p.content,
                 named(p.target) ? "ours" : "other content",
                 p.was < 0 ? "not listed" : std::to_string(p.was).c_str(),
                 (unsigned long long)specs_[p.target].scene_id, (unsigned long long)specs_[p.target].slot,
                 rows_text(specs_[p.target].slot).c_str());
    std::map<uint64_t, bool> away;
    std::map<uint64_t, int64_t> disabled;
    for (const SlotInfo &s : slots) {
        bool ours = false;
        for (const TargetSpec &spec : specs_)
            ours = ours || (spec.slot == s.slot && spec.scene_id == s.content);
        /* The slot's status is the same in each of its rows; who holds it
         * is the row that is mapped. */
        bool &a = away[s.slot];
        a = a || s.status == 0 || (!ours && (s.map_status == 1 || s.map_status == 2));
        int64_t &d = disabled.emplace(s.slot, -1).first->second;
        if (s.status == 0)
            d = now_ms;
    }
    for (const auto &kv : away)
        away_[kv.first] = kv.second;
    for (const auto &kv : disabled)
        disabled_[kv.first] = kv.second;
    slots_seq_++;
    /* Accepted, not mapped: a parked, shown or lingering target's own row
     * unmapped, none of its rows mapped. A report without its row says
     * nothing. */
    for (size_t i = 0; i < t_.size(); i++) {
        Target &t = t_[i];
        const TargetState s = t.status.state;
        if (!t.requested || !rows.count(specs_[i].slot) ||
            (s != TargetState::Parked && s != TargetState::Shown && s != TargetState::Lingering))
            continue;
        bool listed = false, mapped = false;
        for (const SlotInfo &r : slots_[specs_[i].slot])
            if (r.content == specs_[i].scene_id) {
                listed = true;
                mapped = mapped || r.map_status != 0;
            }
        if (mapped) {
            t.unmapped_ms = -1;
        } else if (listed && t.unmapped_ms < 0) {
            t.unmapped_ms = now_ms;
            log_info("target %s: the slot table reports scene 0x%llx unmapped (target %s); asked for again if it "
                     "stays so %lld ms; slot 0x%llx %s",
                     specs_[i].name.c_str(), (unsigned long long)specs_[i].scene_id, target_state_name(s),
                     (long long)kNotMappedMs, (unsigned long long)specs_[i].slot, rows_text(specs_[i].slot).c_str());
        }
    }
    /* A slot that comes back (disabled or held, then free) ends the wait of
     * a target on the ladder: it is asked for at once, not at the next
     * step. Only a slot seen away counts, so a display that drops our scene
     * while reporting the slot free still meets the ladder. */
    for (size_t i = 0; i < t_.size(); i++) {
        Target &t = t_[i];
        TargetStatus &st = t.status;
        if (st.state == TargetState::Parked) {
            const Holder h = other_holder((int)i);
            if (h != st.holder)
                set((int)i, TargetState::Parked, h, parked_reason(h), now_ms);
            continue;
        }
        auto d = disabled.find(specs_[i].slot);
        if (st.state == TargetState::Acquiring && d != disabled.end()) {
            if (d->second >= 0) {
                t.lock_seen = true;
            } else if (t.lock_seen) {
                /* The display never shows a map into a locked slot. */
                log_info("target %s: slot 0x%llx free again, its request from before or during the lock unanswered; "
                         "asking again",
                         specs_[i].name.c_str(), (unsigned long long)specs_[i].slot);
                take_down((int)i, now_ms);
                set((int)i, TargetState::Waiting, Holder::None, "slot free again", now_ms, now_ms);
            }
            continue;
        }
        if (st.state != TargetState::Waiting || st.retry_ms < 0 || t.verdict)
            continue;
        auto a = away.find(specs_[i].slot);
        if (a == away.end())
            continue;
        if (a->second)
            t.slot_away = true;
        const bool lock = disabled[specs_[i].slot] >= 0;
        t.lock_away = t.lock_away || lock;
        /* After a lock, a take-over target takes over the stock content
         * shown there: the slot is back for it once it is not locked. */
        const bool back = !a->second || (specs_[i].take_over_stock && t.lock_away && !lock);
        if (back && t.slot_away && st.retry_ms > now_ms) {
            st.retry_ms = t.removed_ms >= 0 ? std::max(now_ms, t.removed_ms + kRemovalWaitMs) : now_ms;
            t.slot_away = t.lock_away = false;
        }
    }
}

void SlotManager::tick(int64_t now_ms)
{
    const bool up = port_->available();
    const bool settling = available_since_ms_ >= 0 && now_ms - available_since_ms_ < kSettleMs;
    bool poll_slots = false;

    for (size_t k = 0; k < specs_.size(); k++) {
        const int i = (int)k;
        Target &t = t_[i];
        const TargetSpec &spec = specs_[i];

        /* A scene file came or went: published again, the stale scene
         * shown until then while nothing wants the target (its linger). */
        if (t.scene && (!t.requested || t.wanted) && scenes_->stale(spec)) {
            log_info("target %s: a scene file came or went: its scene goes and is published again", spec.name.c_str());
            if (t.requested)
                release(i, now_ms);
            drop_scene(i);
        } else if (t.scene && scenes_->outdated(spec)) {
            if (t.requested)
                republish(i, now_ms);
            else
                drop_scene(i);
        }
        const TargetState s = t.status.state;

        if (!up) {
            if (s != TargetState::Idle && s != TargetState::Unavailable) {
                take_down(i, now_ms);
                set(i, t.wanted ? TargetState::Unavailable : TargetState::Idle, Holder::None,
                    t.wanted ? "display service not available" : "", now_ms);
            } else if (s == TargetState::Idle && t.wanted) {
                set(i, TargetState::Unavailable, Holder::None, "display service not available", now_ms);
            } else if (s == TargetState::Unavailable && !t.wanted) {
                set(i, TargetState::Idle, Holder::None, "", now_ms);
            }
            continue;
        }

        /* A stock pop-up in a slot the target gives way to, or (on such a
         * target: the HUD) stock content parked behind it while it doesn't
         * contend: out of the display at once and not asked for until it has
         * gone. */
        if (!spec.yield_slots.empty()) {
            if (t.wanted && popup_blocks(i, now_ms)) {
                if (s == TargetState::Acquiring || s == TargetState::Parked || s == TargetState::Shown ||
                    s == TargetState::Lingering)
                    release(i, now_ms);
                if (t.status.state != TargetState::Waiting || t.status.reason != kPopupReason)
                    set(i, TargetState::Waiting, Holder::Stock, kPopupReason, now_ms, -1);
                t.verdict = false;
                continue;
            }
            if (s == TargetState::Waiting && t.status.reason == kPopupReason) {
                set(i, TargetState::Idle, Holder::None, "", now_ms);
                continue;
            }
        }

        if ((s == TargetState::Parked || s == TargetState::Shown || s == TargetState::Lingering) &&
            t.unmapped_ms >= 0 && now_ms - t.unmapped_ms >= kNotMappedMs) {
            not_mapped(i, now_ms);
            continue;
        }

        switch (s) {
        case TargetState::Unavailable:
            set(i, TargetState::Idle, Holder::None, "", now_ms);
            /* fall through - it may be requested at once */
        case TargetState::Idle: {
            if (!t.wanted || settling)
                break;
            if (scene_busy(i)) {
                set(i, TargetState::Waiting, Holder::None, "scene id in use by another target", now_ms, now_ms);
                break;
            }
            if (yields(i, now_ms)) {
                poll_slots = true;
                break;
            }
            if (now_ms - t.last_request_ms < gap(i) || !settled(i, now_ms))
                break;
            if (lock_holds(i, now_ms)) {
                poll_slots = true;
                break;
            }
            acquire(i, now_ms);
            break;
        }
        case TargetState::Acquiring:
            if (t.unmapped_ms < 0)
                t.unmapped_ms = port_->accepted_ms(spec);
            if (t.unmapped_ms >= 0 && now_ms - t.unmapped_ms >= kNotMappedMs) {
                not_mapped(i, now_ms);
            } else if (t.unmapped_ms < 0 && now_ms - t.since_ms >= kStartTimeoutMs) {
                port_->no_answer(spec, now_ms);
                take_down(i, now_ms);
                wait(i, Holder::None, true, "no answer from the display", now_ms);
            } else {
                port_->remind(spec, now_ms);
            }
            break;
        case TargetState::Parked:
            if (!t.wanted) {
                release(i, now_ms);   /* nothing of it is on screen: no linger */
                break;
            }
            poll_slots = true;
            take_over(i, now_ms);
            break;
        case TargetState::Shown:
            if (!t.wanted) {
                set(i, TargetState::Lingering, Holder::Us, "", now_ms);
                break;
            }
            if (now_ms - t.since_ms >= kLadderResetMs)
                t.ladder = 0;
            if (t.takeovers && now_ms - t.since_ms >= kTakeOverSettleMs) {
                t.takeovers = 0;
                t.takeover_ms = t.takeover_sent_ms + kTakeOverMs[0];
            }
            verify(i, now_ms);
            break;
        case TargetState::Lingering:
            if (t.wanted) {
                set(i, TargetState::Shown, Holder::Us, "", now_ms);
                verify(i, now_ms);
            } else if (now_ms - t.since_ms >= spec.linger_ms) {
                release(i, now_ms);
            } else {
                verify(i, now_ms);
            }
            break;
        case TargetState::Waiting: {
            TargetStatus &st = t.status;
            if (t.verdict) {
                if (slots_seq_ == t.verdict_seq && now_ms < st.retry_ms)
                    break;
                const bool reported = slots_seq_ != t.verdict_seq;
                t.verdict = false;
                const Holder h = named(i) ? (reported ? Holder::Stock : Holder::None) : other_holder(i);
                settle_loss(i, h, t.verdict_removal, st.reason, now_ms, t.lost_ms);
                break;
            }
            if (st.retry_ms < 0 && !t.wanted) {
                /* Idle checks the slot table again before any request. */
                set(i, TargetState::Idle, Holder::None, "", now_ms);
                break;
            }
            const int slot = slot_status(spec.slot);
            bool ready;
            if (st.retry_ms >= 0) {
                ready = now_ms >= st.retry_ms && !scene_busy(i);
            } else if (st.holder == Holder::Stock && t.contend) {
                /* A contending layer arrived while yielding: retry on the ladder. */
                wait(i, Holder::Stock, true, st.reason, now_ms);
                ready = false;
            } else {
                ready = slot == 1 || (slot == 2 && other_holder(i) == Holder::None);
                if (!ready)
                    poll_slots = true;
            }
            if (!ready)
                break;
            if (!t.wanted) {
                set(i, TargetState::Idle, Holder::None, "", now_ms);
                break;
            }
            if (settling || now_ms - t.last_request_ms < gap(i) || !settled(i, now_ms))
                break;
            if (yields(i, now_ms) || lock_holds(i, now_ms)) {
                poll_slots = true;
                break;
            }
            acquire(i, now_ms);
            break;
        }
        }
    }

    if (poll_slots && now_ms >= slot_poll_ms_) {
        port_->refresh_slots();
        slot_poll_ms_ = now_ms + kSlotPollMs;
    }
}

int64_t SlotManager::wake_in_ms(int64_t now_ms) const
{
    /* Only timers still ahead: one already due and still pending waits on
     * something else (a busy scene id, the settle time), which is counted
     * on its own or falls to the caller's regular tick. */
    int64_t best = -1;
    auto at = [&](int64_t when) {
        int64_t d = when - now_ms;
        if (d > 0 && (best < 0 || d < best))
            best = d;
    };
    if (available_since_ms_ >= 0)
        at(available_since_ms_ + kSettleMs);
    for (size_t i = 0; i < specs_.size(); i++) {
        const Target &t = t_[i];
        switch (t.status.state) {
        case TargetState::Acquiring:
            if (port_->accepted_ms(specs_[i]) >= 0)
                at(now_ms + kRemindMs);
            else
                at(t.since_ms + kStartTimeoutMs);
            break;
        case TargetState::Parked:
            if (specs_[i].take_over_stock && t.contend)
                at(t.takeover_ms);
            at(slot_poll_ms_);
            break;
        case TargetState::Lingering: at(t.since_ms + specs_[i].linger_ms); break;
        case TargetState::Waiting:
            if (t.status.retry_ms >= 0)
                at(std::max(t.status.retry_ms, t.last_request_ms + gap((int)i)));
            else
                at(slot_poll_ms_);
            break;
        case TargetState::Idle:
            if (t.wanted)
                at(t.last_request_ms + gap((int)i));
            break;
        case TargetState::Shown:
            if (t.status.rendering == Rendering::Checking)
                at(t.check_ms + kVerifyMs);
            break;
        default:
            break;
        }
        if (t.unmapped_ms >= 0)
            at(t.unmapped_ms + kNotMappedMs);
        auto r = released_.find(specs_[i].scene_id);
        if (t.wanted && r != released_.end() && !settled((int)i, now_ms))
            at(r->second + kConfirmMs);
    }
    return best;
}

void SlotManager::verify(int i, int64_t now_ms)
{
    Target &t = t_[i];
    TargetStatus &st = t.status;
    const TargetSpec &spec = specs_[i];
    auto s = subs_.find(spec.scene_id);
    const Rendering r = subscribed(spec.scene_id) ? Rendering::Verified : sub_lines_ ? Rendering::Checking : Rendering::Unobservable;
    if (r != st.rendering) {
        if (r == Rendering::Verified) {
            const auto &first = *s->second.begin();
            const int64_t d = t.shown_ms - first.second.since_ms;
            log_info("target %s: rendered: renderer %s subscribed to scene 0x%llx %lld ms %s it was shown",
                     spec.name.c_str(), first.first.c_str(), (unsigned long long)spec.scene_id,
                     (long long)(d >= 0 ? d : -d), d >= 0 ? "before" : "after");
            t.recoveries = 0;
        } else if (r == Rendering::Checking && st.rendering != Rendering::None) {
            t.check_ms = now_ms;
            log_warn("target %s: shown, and its renderer is not subscribed to scene 0x%llx", spec.name.c_str(),
                     (unsigned long long)spec.scene_id);
        } else if (r == Rendering::Unobservable && !unobservable_logged_) {
            unobservable_logged_ = true;
            log_warn("target %s: renderer subscription not observable: no RAMSES subscription line read since "
                     "start; shown targets are not verified",
                     spec.name.c_str());
        }
        st.rendering = r;
    }
    if (r != Rendering::Checking || st.state != TargetState::Shown || now_ms - t.check_ms < kVerifyMs ||
        !spec.follows.empty())
        return;
    /* The display shows the scene, and nothing draws it: out of the slot and
     * back in, at once the first time; the scene stays published. */
    t.recoveries++;
    const std::string reason = "not rendered: the renderer is not subscribed";
    log_warn("target %s: the display reports scene 0x%llx mapped but its renderer is not subscribed (%lld ms); "
             "unmapping, unfetching and asking again%s",
             spec.name.c_str(), (unsigned long long)spec.scene_id, (long long)(now_ms - t.check_ms),
             t.recoveries == 1 ? " at once" : " on the ladder");
    take_down(i, now_ms);
    /* From the second recovery on the publication itself may be what the renderer missed: unpublished, so the
     * next acquire publishes it again (a fresh offer); then, now and again, the framework reconnected */
    if (t.recoveries >= 2)
        drop_scene(i);
    if (on_renderer_stuck && t.recoveries % kReconnectAfter == 0)
        on_renderer_stuck(now_ms);
    if (t.recoveries == 1)
        set(i, TargetState::Waiting, Holder::None, reason, now_ms, now_ms);
    else
        wait(i, Holder::None, true, reason, now_ms);
}

std::string SlotManager::timing(int i, int64_t now_ms) const
{
    const Target &t = t_[i];
    char b[160];
    const TargetState s = t.status.state;
    if (s == TargetState::Shown || s == TargetState::Lingering)
        std::snprintf(b, sizeof b, ", %lld ms after the fetch, %lld ms after the map, target %s",
                      (long long)(now_ms - t.last_request_ms), (long long)(now_ms - t.shown_ms),
                      target_state_name(s));
    else if (t.requested)
        std::snprintf(b, sizeof b, ", %lld ms after the fetch, target %s", (long long)(now_ms - t.last_request_ms),
                      target_state_name(s));
    else
        std::snprintf(b, sizeof b, ", target %s (released)", target_state_name(s));
    return b;
}

void SlotManager::on_subscription(uint64_t scene, bool subscribed, bool unknown, const std::string &guid,
                                  int64_t now_ms)
{
    sub_lines_++;
    const int i = by_published(scene);
    const std::string who = guid.empty() ? std::string("(no guid)") : guid;
    const char *what = subscribed ? "subscribed to" : "unsubscribed from";
    if (unknown || i < 0) {
        log_warn("renderer %s: %s scene 0x%llx, which %s", who.c_str(), what, (unsigned long long)scene,
                 unknown ? "the RAMSES client does not know" : "no target publishes");
        return;
    }
    Subscribers &subs = subs_[scene];
    if (subscribed) {
        Subscriber &sub = subs[who];
        sub.since_ms = now_ms;
        sub.asked = false;
    } else {
        subs.erase(who);
    }
    log_info("renderer %s: %s scene 0x%llx (target %s)%s", who.c_str(), what, (unsigned long long)scene,
             specs_[i].name.c_str(), timing(i, now_ms).c_str());
}

void SlotManager::on_renderer_gone(const std::string &guid, int64_t now_ms)
{
    for (auto &kv : subs_)
        if (kv.second.erase(guid)) {
            const int i = by_published(kv.first);
            log_warn("renderer %s: gone, and with it its subscription to scene 0x%llx%s", guid.c_str(),
                     (unsigned long long)kv.first, i >= 0 ? timing(i, now_ms).c_str() : "");
        }
}

void SlotManager::on_resource_request(const std::string &guid, unsigned resources, int64_t now_ms)
{
    for (auto &kv : subs_) {
        auto sub = kv.second.find(guid);
        if (sub == kv.second.end() || sub->second.asked)
            continue;
        sub->second.asked = true;
        const int i = by_published(kv.first);
        log_info("renderer %s: requested %u resources, the first request since it subscribed to scene 0x%llx %lld ms "
                 "ago%s",
                 guid.c_str(), resources, (unsigned long long)kv.first, (long long)(now_ms - sub->second.since_ms),
                 i >= 0 ? timing(i, now_ms).c_str() : "");
    }
}

std::string SlotManager::rendering_text(int i, int64_t now_ms) const
{
    const Target &t = t_[i];
    char b[160];
    switch (t.status.rendering) {
    case Rendering::Verified: {
        auto s = subs_.find(specs_[i].scene_id);
        if (s == subs_.end() || s->second.empty())
            return "rendered";
        const auto &first = *s->second.begin();
        std::snprintf(b, sizeof b, "rendered: renderer %s subscribed for %lld s", first.first.c_str(),
                      (long long)((now_ms - first.second.since_ms) / 1000));
        return b;
    }
    case Rendering::Checking:
        std::snprintf(b, sizeof b, "renderer not subscribed for %lld ms", (long long)(now_ms - t.check_ms));
        return b;
    case Rendering::Unobservable: return "renderer subscription not observable";
    default: return t.scene && !t.requested ? "scene published" : "";
    }
}

std::string SlotManager::renderer_text(int64_t now_ms) const
{
    const std::string console = console_text ? "renderer console: " + console_text(now_ms) + "\n" : std::string();
    if (!sub_lines_)
        return "renderer: subscription not observable: no RAMSES subscription line read since start\n" + console;
    return "renderer: " + std::to_string(sub_lines_) + " subscription lines read since start\n" + console;
}

}  // namespace crs
