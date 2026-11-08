/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "holds.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace crs {

namespace {

/* The whole of s as an unsigned number in base 0 (decimal, or hex after
 * 0x); hex_only: 0x and hex digits only. */
bool number(const std::string &s, bool hex_only, uint64_t *v)
{
    if (s.empty() || !std::isdigit((unsigned char)s[0]))
        return false;
    if (hex_only && (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') ||
                     s.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos))
        return false;
    char *end = nullptr;
    *v = std::strtoull(s.c_str(), &end, 0);
    return *end == '\0';
}

std::string hex(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

/* The tokens of a list separated by commas or spaces. */
std::vector<std::string> tokens(const std::string &list)
{
    std::vector<std::string> out;
    for (size_t at = 0; at < list.size();) {
        const size_t end = std::min(list.find_first_of(", \t", at), list.size());
        if (end > at)
            out.push_back(list.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

/* The slots the stock HMI maps its CarPlay box in (VERIFIED on one car
 * each), in its order: 0x105 when the KOMBI reports it enabled, else 0x103
 * (read from HmiMain3D). guide_slots = auto holds no other. */
const std::vector<uint64_t> kBoxSlots = {0x105, 0x103};

/* Adds why to *warn; false. */
bool refuse(std::string *warn, const std::string &why)
{
    *warn += (warn->empty() ? "" : "; ") + why;
    return false;
}

}  // namespace

bool load_holds(const HoldSettings &cfg, const std::vector<uint64_t> &allowed, std::vector<TargetSpec> *targets,
                std::string *warn)
{
    warn->clear();
    if (cfg.hold == "0")
        return true;
    if (cfg.hold != "1")
        *warn = "guide_hold must be 0 or 1, not \"" + cfg.hold + "\": taken as 1";
    if (std::none_of(targets->begin(), targets->end(), [](const TargetSpec &t) { return t.name == "map"; }))
        return refuse(warn, "guide_hold: there is no map target to follow");
    uint64_t scene = 0, klass = 0;
    if (!number(cfg.scene_id, false, &scene) || !number(cfg.content_class, false, &klass) || klass > 0xffffffffu)
        return refuse(warn, "guide_scene_id and guide_content_class must be numbers, not \"" + cfg.scene_id +
                                "\" and \"" + cfg.content_class + "\"");
    std::vector<uint64_t> takes;
    for (const std::string &c : tokens(cfg.take_over)) {
        uint64_t v = 0;
        if (!number(c, true, &v) || !v)
            return refuse(warn, "guide_take_over: \"" + c + "\" is not a hex content id");
        if (v == 0x01000314)
            return refuse(warn, "guide_take_over: 0x1000314 is the seat-belt-warning panel, never taken over");
        takes.push_back(v);
    }
    if (takes.empty())
        return refuse(warn, "guide_take_over names no content");
    std::vector<TargetSpec> holds;
    const std::vector<std::string> slots = tokens(cfg.slots);
    const bool from_table = slots.size() == 1 && slots[0] == "auto";
    for (const std::string &id : slots) {
        TargetSpec t;
        if (!from_table && id == "auto")
            return refuse(warn, "guide_slots: auto is given alone, without slot ids");
        if (!from_table && (!number(id, true, &t.slot) || t.slot < 1 || t.slot > 0xfff))
            return refuse(warn, "guide_slots: \"" + id + "\" is not a slot id from 0x1 to 0xfff");
        t.name = from_table ? "guide_auto" : "guide_" + hex(t.slot);
        if (from_table)
            t.table_slots = kBoxSlots;
        t.content_class = (uint32_t)klass;
        t.scene_id = scene + holds.size();
        t.width = t.height = 16;
        t.linger_ms = 0;
        t.take_over_stock = true;
        t.follows = "map";
        t.take_over_contents = takes;
        t.strike_limit = 2;
        for (const std::vector<TargetSpec> *v : {targets, &holds})
            for (const TargetSpec &o : *v) {
                if (o.scene_id == t.scene_id)
                    return refuse(warn, "guide_slots: " + t.name + "'s scene id " + hex(t.scene_id) +
                                            " is target " + o.name + "'s");
                if (from_table ? std::count(kBoxSlots.begin(), kBoxSlots.end(), o.slot) > 0 : o.slot == t.slot)
                    return refuse(warn, "guide_slots: slot " + hex(from_table ? o.slot : t.slot) +
                                            (o.follows.empty() ? " is target " + o.name + "'s"
                                                               : std::string(" is listed twice")));
            }
        if (std::find(allowed.begin(), allowed.end(), t.scene_id) == allowed.end())
            return refuse(warn,
                          "guide_slots: " + t.name + "'s scene id " + hex(t.scene_id) + " is not in scene_ids_allowed");
        holds.push_back(t);
    }
    if (holds.empty())
        return refuse(warn, "guide_hold: guide_slots names no slot");
    targets->insert(targets->end(), holds.begin(), holds.end());
    return true;
}

bool hud_rules(TargetSpec *t, std::string *why)
{
    if (t->slot != kHudSlot || (t->scene_id != kHudScene && t->scene_id != kHudOwnScene) || t->take_over_stock) {
        char b[240];
        std::snprintf(b, sizeof b, "only slot 0x%llx with scene 0x%llx or 0x%llx, without hud_take_over_stock "
                      "(hud_slot 0x%llx, hud_scene_id 0x%llx%s)",
                      (unsigned long long)kHudSlot, (unsigned long long)kHudScene, (unsigned long long)kHudOwnScene,
                      (unsigned long long)t->slot,
                      (unsigned long long)t->scene_id, t->take_over_stock ? ", hud_take_over_stock 1" : "");
        *why = b;
        return false;
    }
    t->share = true;
    t->strike_limit = 2;
    return true;
}

}  // namespace crs
