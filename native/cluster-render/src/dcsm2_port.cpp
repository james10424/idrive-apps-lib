/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "dcsm2_port.h"

#include <cstdio>

#include "dcsm/dcsm_client.h"
#include "log.h"

namespace crs {

namespace {

const char *phase_name(int phase)
{
    static const char *const names[] = {"fetching", "mapping", "parked", "mapped", "refused", "lost"};
    return phase >= 0 && phase < 6 ? names[phase] : "?";
}

std::string event_name(uint16_t event)
{
    if (event == 0xffff)
        return "the eventgroup";
    char b[24];
    std::snprintf(b, sizeof b, "event 0x%04x", event);
    return b;
}

std::string answer_text(uint16_t code)
{
    if (code == 0)
        return "accepted";
    char b[32];
    std::snprintf(b, sizeof b, "refused (code 0x%02x)", code);
    return b;
}

Refusal classify(bool ok, uint32_t status)
{
    if (!ok)
        return Refusal::Transient;
    switch (status) {
    case dcsm::E_DCSM_PRIOTOOLOW:
    case dcsm::E_DCSM_CONTENT_ALREADYMAPPED:
        return Refusal::Busy;
    case dcsm::E_DCSM_SLOTID_UNKNOWN:
        return Refusal::NoSlot;
    case dcsm::E_DCSM_CONTENTID_UNKNOWN:
        return Refusal::UnknownContent;
    case dcsm::E_DCSM_HANDLE_UNKNOWN:
    case dcsm::E_DCSM_CLIENT_ALREADYREGISTERED:
        return Refusal::Config;
    default:
        return Refusal::Transient;
    }
}

}  // namespace

std::string hex_bytes(const uint8_t *p, size_t n, size_t max)
{
    if (n == 0)
        return "(empty)";
    std::string out;
    char b[4];
    for (size_t i = 0; i < n && i < max; i++) {
        if (i && i % 4 == 0)
            out += ' ';
        std::snprintf(b, sizeof b, "%02x", p[i]);
        out += b;
    }
    if (n > max)
        out += " ...";
    return out;
}

std::string Dcsm2Port::counts_text(const Counts &c)
{
    char b[160];
    std::snprintf(b, sizeof b, "%u fetch, %u map, %u slot, %u undecoded events, %u replies", c.fetch, c.map, c.slots,
                  c.undecoded, c.replies);
    return b;
}

void Dcsm2Port::request(const TargetSpec &t)
{
    Content &c = contents_[t.scene_id];
    c = Content();
    c.spec = t;
    pending_.erase(t.scene_id);   /* confirmed or not, no longer awaited */
    asked_[t.scene_id] = total_;
    if (up_)
        fetches_.push_back(t.scene_id);
    wire_->fetch(t.scene_id);
}

void Dcsm2Port::release(const TargetSpec &t)
{
    auto it = contents_.find(t.scene_id);
    if (it == contents_.end())
        return;
    if (it->second.map_asked) {
        if (up_)
            unmaps_.push_back(Unmap{t.scene_id, 0});
        wire_->unmap(t.scene_id);
    }
    wire_->unfetch(t.scene_id);
    if (up_) {
        unfetched_[t.scene_id]++;
        unfetches_.push_back(Unfetch{t.scene_id, it->second.fetched});
        Pending &p = pending_[t.scene_id];
        p.replies++;
        p.removals += it->second.fetched;
    }
    contents_.erase(it);
}

void Dcsm2Port::take_over(const TargetSpec &, uint64_t content)
{
    if (up_)
        unmaps_.push_back(Unmap{content, hmi_handle_});
    wire_->unmap_as(hmi_handle_, content);
}

uint64_t Dcsm2Port::take(std::deque<uint64_t> *q)
{
    if (q->empty())
        return 0;
    uint64_t s = q->front();
    q->pop_front();
    return s;
}

void Dcsm2Port::settle(uint64_t scene, Pending &p)
{
    if (p.replies > 0 || p.removals > 0)
        return;
    log_info("dcsm: content 0x%llx: the release is confirmed", (unsigned long long)scene);
    pending_.erase(scene);
}

void Dcsm2Port::refuse(uint64_t scene, Content &c, Refusal r, const std::string &why, int64_t now_ms)
{
    c.phase = Phase::Refused;
    c.map_asked = false;
    if (manager_)
        manager_->on_refused(scene, r, why, now_ms);
}

void Dcsm2Port::on_availability(bool up, int64_t now_ms)
{
    log_info("dcsm: Dcsm2 %s", up ? "available" : "not available");
    up_ = up;
    changed_ms_ = now_ms;
    table_.clear();
    table_ms_ = -1;
    asked_.clear();
    if (!up)
        subscribed_.clear();
    fetches_.clear();
    maps_.clear();
    unmaps_.clear();
    unfetches_.clear();
    pending_.clear();
    contents_.clear();   /* gone with the service; nothing to release */
    unfetched_.clear();
    slots_.clear();
    if (manager_)
        manager_->on_availability(up, now_ms);
}

void Dcsm2Port::on_reply(uint16_t method, bool ok, uint32_t status, const std::string &status_text, int64_t now_ms)
{
    total_.replies++;
    if (method == dcsm::kReqUnmapContent) {
        Unmap u;
        if (!unmaps_.empty()) {
            u = unmaps_.front();
            unmaps_.pop_front();
        }
        if (u.handle)
            log_info("dcsm: reply to the take-over unmap of 0x%llx as client 0x%llx: %s", (unsigned long long)u.scene,
                     (unsigned long long)u.handle, ok ? status_text.c_str() : "no valid reply");
        else
            log_info("dcsm: reply to request 0x%04x (content 0x%llx): %s", method, (unsigned long long)u.scene,
                     ok ? status_text.c_str() : "no valid reply");
        return;   /* nothing waits on an unmap */
    }
    if (method == dcsm::kReqUnfetchContent) {
        Unfetch u;
        if (!unfetches_.empty()) {
            u = unfetches_.front();
            unfetches_.pop_front();
        }
        log_info("dcsm: reply to request 0x%04x (content 0x%llx): %s", method, (unsigned long long)u.scene,
                 ok ? status_text.c_str() : "no valid reply");
        auto p = pending_.find(u.scene);
        if (p == pending_.end())
            return;
        p->second.replies--;
        if (u.removal && !(ok && status == dcsm::E_DCSM_OK))
            p->second.removals--;   /* not unfetched: no REMOVED comes */
        settle(u.scene, p->second);
        return;
    }
    const uint64_t scene = method == dcsm::kReqFetchContent ? take(&fetches_)
                           : method == dcsm::kReqMapContent ? take(&maps_) : 0;
    const std::string why = ok ? status_text : "no valid reply";
    log_info("dcsm: reply to request 0x%04x (content 0x%llx): %s", method, (unsigned long long)scene, why.c_str());
    auto it = contents_.find(scene);
    if (ok && status == dcsm::E_DCSM_OK) {
        if (method == dcsm::kReqMapContent && it != contents_.end() && it->second.phase == Phase::Mapping &&
            it->second.accepted_ms < 0)
            it->second.accepted_ms = now_ms;
        return;   /* the status event follows */
    }
    if (it == contents_.end())
        return;   /* released since */
    Content &c = it->second;
    if (method == dcsm::kReqMapContent && c.phase == Phase::Mapping && c.accepted_ms >= 0) {
        /* A repeat refused: the first map stands, as accepted. */
        c.map_again = false;
        log_info("dcsm: content 0x%llx: the repeated map is not taken (%s); waiting on the first one",
                 (unsigned long long)scene, why.c_str());
        return;
    }
    if ((method == dcsm::kReqFetchContent && c.phase == Phase::Fetching) ||
        (method == dcsm::kReqMapContent && c.phase == Phase::Mapping))
        refuse(scene, c, classify(ok, status), (method == dcsm::kReqFetchContent ? "fetch " : "map ") + why, now_ms);
}

void Dcsm2Port::on_fetch_status(uint64_t scene, uint32_t status, int64_t now_ms)
{
    total_.fetch++;
    log_info("dcsm: content 0x%llx fetch status %u", (unsigned long long)scene, status);
    auto p = pending_.find(scene);
    if (p != pending_.end() && status == dcsm::E_FETCH_STATUS_REMOVED && p->second.removals > 0) {
        p->second.removals--;
        settle(scene, p->second);
    }
    auto un = unfetched_.find(scene);
    if (un != unfetched_.end()) {
        if (status == dcsm::E_FETCH_STATUS_REMOVED) {
            if (--un->second == 0)
                unfetched_.erase(un);
            return;   /* the answer to our unfetch */
        }
        if (status == dcsm::E_FETCH_STATUS_FETCHED)
            unfetched_.erase(un);   /* a later fetch: any earlier REMOVED has come */
    }
    auto it = contents_.find(scene);
    if (it == contents_.end())
        return;
    Content &c = it->second;
    switch (status) {
    case dcsm::E_FETCH_STATUS_FETCHED:
        if (c.phase != Phase::Fetching)
            break;
        c.phase = Phase::Mapping;
        c.map_asked = true;
        c.fetched = true;
        c.map_ms = now_ms;
        if (up_)
            maps_.push_back(scene);
        wire_->map(scene, c.spec.content_class, c.spec.slot);
        break;
    case dcsm::E_FETCH_STATUS_FAILED:
        if (c.phase == Phase::Fetching)
            refuse(scene, c, Refusal::FetchFailed, "E_FETCH_STATUS_FAILED", now_ms);
        break;
    case dcsm::E_FETCH_STATUS_REMOVED:
        /* The KOMBI took the slot back by removing the fetch, with no map
         * transition: the stock map re-mapping over ours, its watchdog, or
         * its renderer unsubscribing the scene. */
        c.fetched = false;
        if (c.phase == Phase::Mapped || c.phase == Phase::Parked) {
            c.phase = Phase::Lost;
            c.map_asked = false;
            if (manager_)
                manager_->on_lost(scene, "fetch removed", now_ms);
        } else if (c.phase == Phase::Fetching || c.phase == Phase::Mapping) {
            refuse(scene, c, Refusal::Transient, "fetch removed", now_ms);
        }
        break;
    default:
        break;
    }
}

void Dcsm2Port::on_map_status(uint64_t scene, uint32_t initial, uint32_t final_, int64_t now_ms)
{
    total_.map++;
    log_info("dcsm: content 0x%llx map status %u -> %u", (unsigned long long)scene, initial, final_);
    auto it = contents_.find(scene);
    if (it == contents_.end())
        return;
    Content &c = it->second;
    const bool showing = final_ == dcsm::E_MAP_STATUS_MAPPED_FOREGROUND || final_ == dcsm::E_MAP_STATUS_MAPPED_BACKGROUND;
    if (showing && (c.phase == Phase::Mapping || c.phase == Phase::Parked)) {
        c.phase = Phase::Mapped;
        if (manager_)
            manager_->on_shown(scene, now_ms);
    } else if (final_ == dcsm::E_MAP_STATUS_MAPPED_INVISIBLE &&
               (c.phase == Phase::Mapping || c.phase == Phase::Mapped)) {
        /* Accepted, behind the content the slot shows: the KOMBI shows it
         * when that goes. */
        c.phase = Phase::Parked;
        if (manager_)
            manager_->on_parked(scene, now_ms);
    } else if (final_ == dcsm::E_MAP_STATUS_UNMAPPED && (c.phase == Phase::Mapped || c.phase == Phase::Parked)) {
        c.phase = Phase::Lost;
        c.map_asked = false;
        if (manager_)
            manager_->on_lost(scene, "unmapped", now_ms);
    }
}

void Dcsm2Port::on_slots(const std::vector<SlotInfo> &slots, int64_t now_ms)
{
    /* An entry is logged when it is new to this report or changed since the
     * last one: the KOMBI re-reports every slot on each arbitration. */
    std::map<std::pair<uint64_t, uint64_t>, SlotInfo> now;
    for (const SlotInfo &s : slots) {
        auto it = slots_.find({s.slot, s.content});
        if (it == slots_.end() || it->second.status != s.status || it->second.klass != s.klass ||
            it->second.map_status != s.map_status)
            log_info("dcsm: slot 0x%llx status %u content 0x%llx class %u map %u", (unsigned long long)s.slot,
                     s.status, (unsigned long long)s.content, s.klass, s.map_status);
        now[{s.slot, s.content}] = s;
    }
    slots_.swap(now);
    total_.slots++;
    table_ = slots;
    table_ms_ = now_ms;
    if (manager_)
        manager_->on_slots(slots, now_ms);
}

void Dcsm2Port::on_undecoded(uint16_t event, size_t length, const std::string &hex, int64_t now_ms)
{
    total_.undecoded++;
    Undecoded &u = undecoded_[event];
    if (u.logged && now_ms - u.logged_ms < kUndecodedLogMs) {
        u.dropped++;
        return;
    }
    if (u.dropped)
        log_warn("dcsm: event 0x%04x (%zu bytes) not decoded: %s; %u more not logged since the last", event, length,
                 hex.c_str(), u.dropped);
    else
        log_warn("dcsm: event 0x%04x (%zu bytes) not decoded: %s", event, length, hex.c_str());
    u.logged = true;
    u.logged_ms = now_ms;
    u.dropped = 0;
}

void Dcsm2Port::on_subscription(uint16_t event, uint16_t code, int64_t)
{
    auto it = subscribed_.find(event);
    if (it != subscribed_.end() && it->second == code)
        return;
    subscribed_[event] = code;
    if (code == 0)
        log_info("dcsm: subscription to %s: accepted", event_name(event).c_str());
    else
        log_error("dcsm: subscription to %s: %s", event_name(event).c_str(), answer_text(code).c_str());
}

/* A display that is starting accepts a fetch and a map and then shows
 * nothing, with no map status and no refusal (seen on the car: the map
 * accepted, nothing for 5 s, and the same request made after a release
 * shown within 0.3 s). The map is asked again while that lasts: no
 * release, no unfetch, and the scene is not published again, so nothing
 * is uploaded twice. Not while the last table reports the slot disabled. */
bool Dcsm2Port::remind(const TargetSpec &t, int64_t now_ms)
{
    if (accepted_ms(t) < 0)
        return false;
    Content &c = contents_.find(t.scene_id)->second;
    if (!up_ || !c.map_again || now_ms - c.map_ms < kMapAgainMs)
        return true;
    for (const SlotInfo &s : table_)
        if (s.slot == c.spec.slot && s.status == 0)
            return true;
    c.map_ms = now_ms;
    if (c.map_repeats++ == 0)
        log_info("dcsm: content 0x%llx (target %s, slot 0x%llx): the map was accepted and nothing is shown; asking "
                 "again every %lld ms",
                 (unsigned long long)t.scene_id, t.name.c_str(), (unsigned long long)c.spec.slot, (long long)kMapAgainMs);
    maps_.push_back(t.scene_id);
    wire_->map(t.scene_id, c.spec.content_class, c.spec.slot);
    return true;
}

/* What arrived since the request, and the last slot table, so a log tells
 * events not delivered, events not decoded and a KOMBI that does not
 * answer apart. */
void Dcsm2Port::no_answer(const TargetSpec &t, int64_t now_ms)
{
    Counts since = total_;
    auto a = asked_.find(t.scene_id);
    if (a != asked_.end()) {
        since.fetch -= a->second.fetch;
        since.map -= a->second.map;
        since.slots -= a->second.slots;
        since.undecoded -= a->second.undecoded;
        since.replies -= a->second.replies;
    }
    auto c = contents_.find(t.scene_id);
    const char *phase = c == contents_.end() ? "released" : phase_name((int)c->second.phase);
    std::string table;
    if (table_ms_ < 0) {
        table = "no slot table since Dcsm2 came up";
    } else {
        char b[96];
        std::snprintf(b, sizeof b, "last slot table %lld ms ago, %zu rows (slot status content class map):",
                      (long long)(now_ms - table_ms_), table_.size());
        table = b;
        for (size_t i = 0; i < table_.size(); i++) {
            if (table.size() > 640) {
                std::snprintf(b, sizeof b, " +%zu more", table_.size() - i);
                table += b;
                break;
            }
            const SlotInfo &r = table_[i];
            std::snprintf(b, sizeof b, "%s 0x%llx %u 0x%llx %u %u", i ? "," : "", (unsigned long long)r.slot, r.status,
                          (unsigned long long)r.content, r.klass, r.map_status);
            table += b;
        }
    }
    log_warn("dcsm: no answer for content 0x%llx (target %s, slot 0x%llx, %s); since its request: %s; %s",
             (unsigned long long)t.scene_id, t.name.c_str(), (unsigned long long)t.slot, phase,
             counts_text(since).c_str(), table.c_str());
}

std::string Dcsm2Port::status_text(int64_t now_ms) const
{
    std::string s = "dcsm: Dcsm2 ";
    char b[96];
    if (changed_ms_ < 0)
        s += "not available yet";
    else {
        std::snprintf(b, sizeof b, "%s for %lld s", up_ ? "available" : "not available",
                      (long long)((now_ms - changed_ms_) / 1000));
        s += b;
    }
    s += "; subscription:";
    if (subscribed_.empty())
        s += " no answer yet";
    for (auto it = subscribed_.begin(); it != subscribed_.end(); ++it)
        s += (it == subscribed_.begin() ? " " : ", ") + event_name(it->first) + " " + answer_text(it->second);
    s += "\ndcsm: since start: " + counts_text(total_) + "; ";
    if (table_ms_ < 0)
        s += "no slot table since Dcsm2 came up\n";
    else {
        std::snprintf(b, sizeof b, "last slot table %lld s ago\n", (long long)((now_ms - table_ms_) / 1000));
        s += b;
    }
    return s;
}

}  // namespace crs
