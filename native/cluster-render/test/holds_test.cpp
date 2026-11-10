/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for the guide hold's settings (holds.h): on as shipped
 * (cluster-render.conf), its slot from the slot table (guide_slots = auto)
 * and its scene id in the shipped scene_ids_allowed, and with 1, off with
 * 0; auto alone, one hold with guide_scene_id in 0x105 or 0x103, never
 * with slot ids or with another target in either; one
 * hold per slot of guide_slots following the map, scene ids from
 * guide_scene_id up;
 * separators commas or spaces; guide_hold other than 0 or 1 taken as 1 with
 * a warning; every other malformed value refused with what was wrong and no
 * target added: a slot that is not whole hex from 0x1 to 0xfff, a slot
 * listed twice or another target's, a scene id not allowed or another
 * target's, a take-over content that is not whole hex, none, no map, no
 * slot. The panel: on an id other than the HUD's struck out at 2; its
 * fallback scene id another allowed id, none when empty or its own id,
 * anything else refused. */
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/holds.h"

using namespace crs;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static const std::vector<uint64_t> kAllowed = {0x010004f0, 0x010004f1, 0x010004f2};

/* key's value in the shipped cluster-render.conf. */
static std::string shipped(const std::string &key)
{
    std::ifstream f("cluster-render.conf");
    for (std::string line; std::getline(f, line);)
        if (line.compare(0, key.size() + 3, key + " = ") == 0)
            return line.substr(key.size() + 3);
    return "?";
}

/* The targets with the map, after load_holds with the given settings over
 * the shipped defaults (key=value). */
static bool load(const std::vector<std::string> &pairs, std::vector<TargetSpec> *t, std::string *err)
{
    HoldSettings h{shipped("guide_hold"), "0x105", "0x010004f1", "0", "0x01000310"};
    for (const std::string &p : pairs) {
        const std::string k = p.substr(0, p.find('=')), v = p.substr(p.find('=') + 1);
        (k == "guide_hold"       ? h.hold
         : k == "guide_slots"    ? h.slots
         : k == "guide_scene_id" ? h.scene_id
         : k == "guide_take_over" ? h.take_over
                                  : h.content_class) = v;
    }
    TargetSpec map;
    map.name = "map";
    map.slot = 0x100;
    map.scene_id = 0x010004f0;
    *t = {map};
    err->clear();
    return load_holds(h, kAllowed, t, err);
}

static bool refused(const std::vector<std::string> &pairs, const std::string &why)
{
    std::vector<TargetSpec> t;
    std::string err;
    const bool good = !load(pairs, &t, &err) && t.size() == 1 && err.find(why) != std::string::npos;
    if (!good)
        std::printf("     got \"%s\", %zu targets\n", err.c_str(), t.size());
    return good;
}

int main()
{
    std::vector<TargetSpec> t;
    std::string err;
    {
        TargetSpec h;
        h.name = "hud"; h.slot = 0x323; h.scene_id = 0x01000400;
        ok("hud: slot 0x323 taken, with the HUD's share, struck out at 2", hud_rules(&h, &err) && h.share && h.strike_limit == 2);
        TargetSpec scene;
        scene.name = "hud"; scene.slot = 0x323; scene.scene_id = 0x010004f0;
        ok("hud: its own scene id (the map's) taken, with the HUD's share, struck out at 2",
           hud_rules(&scene, &err) && scene.share && scene.strike_limit == 2);
        TargetSpec guide;
        guide.name = "hud"; guide.slot = 0x323; guide.scene_id = 0x010004f1;
        ok("hud: any other scene id (the guide hold's) refused, nothing set",
           !hud_rules(&guide, &err) && !guide.share && err.find("hud_scene_id 0x10004f1") != std::string::npos);
        TargetSpec other;
        other.name = "hud"; other.slot = 0x330; other.scene_id = 0x01000400;
        ok("hud: any other slot refused, nothing set", !hud_rules(&other, &err) && !other.share && other.strike_limit == 0 &&
                                                        err.find("hud_slot 0x330") != std::string::npos);
        TargetSpec taking;
        taking.name = "hud"; taking.slot = 0x323; taking.scene_id = 0x01000400; taking.take_over_stock = true;
        ok("hud: a take-over from the stock HMI refused", !hud_rules(&taking, &err));
    }
    {
        const std::vector<uint64_t> allowed = {0x010004f0, 0x01000400, 0x010004f1, 0x010004f8};
        const auto panel = [](uint64_t scene) {
            TargetSpec p;
            p.name = "panel"; p.slot = 0x203; p.scene_id = scene;
            return p;
        };
        TargetSpec p = panel(0x010004f8);
        ok("panel: a fallback of another allowed id, its own id struck out at 2",
           fallback_rules(&p, "0x01000400", allowed, &err) && p.fallback_scene_id == 0x01000400 && p.strike_limit == 2);
        p = panel(0x010004f8);
        ok("panel: empty, no fallback, its own id still struck out at 2",
           fallback_rules(&p, "", allowed, &err) && p.fallback_scene_id == 0 && p.strike_limit == 2);
        p = panel(0x01000400);
        ok("panel: on the HUD's id with it as fallback: no fallback, strikes only logged",
           fallback_rules(&p, "0x01000400", allowed, &err) && p.fallback_scene_id == 0 && p.strike_limit == 0);
        p = panel(0x010004f8);
        ok("panel: its own id as fallback: none, struck out at 2",
           fallback_rules(&p, "0x010004f8", allowed, &err) && p.fallback_scene_id == 0 && p.strike_limit == 2);
        p = panel(0x010004f8);
        ok("panel: a decimal id taken", fallback_rules(&p, "16778240", allowed, &err) && p.fallback_scene_id == 0x01000400);
        p = panel(0x010004f8);
        ok("panel: an id not allowed refused, no fallback set",
           !fallback_rules(&p, "0x010004fa", allowed, &err) && p.fallback_scene_id == 0 &&
               err == "panel_fallback_scene_id 0x10004fa is not in scene_ids_allowed");
        for (const char *bad : {"0x0100040g", "panel", "-1", " 0x01000400"}) {
            p = panel(0x010004f8);
            ok(("panel: not a number refused: \"" + std::string(bad) + "\"").c_str(),
               !fallback_rules(&p, bad, allowed, &err) && p.fallback_scene_id == 0 &&
                   err.find("must be a number") != std::string::npos);
        }
        std::vector<uint64_t> shipped_allowed;
        std::stringstream ss(shipped("scene_ids_allowed"));
        for (std::string id; std::getline(ss, id, ',');)
            shipped_allowed.push_back(std::strtoull(id.c_str(), nullptr, 0));
        p = panel(std::strtoull(shipped("panel_scene_id").c_str(), nullptr, 0));
        ok("shipped: the panel on 0x10004f8, falling back to the HUD's 0x1000400, both allowed",
           p.scene_id == 0x010004f8 && shipped("hud_scene_id") == "0x01000400" &&
               std::find(shipped_allowed.begin(), shipped_allowed.end(), 0x010004f8) != shipped_allowed.end() &&
               fallback_rules(&p, shipped("panel_fallback_scene_id"), shipped_allowed, &err) &&
               p.fallback_scene_id == 0x01000400 && p.strike_limit == 2);
    }
    ok("shipped: a hold", shipped("guide_hold") == "1" && load({}, &t, &err) && t.size() == 2 && err.empty());
    {
        std::vector<uint64_t> allowed;
        std::stringstream ss(shipped("scene_ids_allowed"));
        for (std::string id; std::getline(ss, id, ',');)
            allowed.push_back(std::strtoull(id.c_str(), nullptr, 0));
        TargetSpec map;
        map.name = "map";
        map.slot = std::strtoull(shipped("map_slot").c_str(), nullptr, 0);
        map.scene_id = std::strtoull(shipped("map_scene_id").c_str(), nullptr, 0);
        std::vector<TargetSpec> s = {map};
        ok("shipped, with the shipped scene_ids_allowed and guide_ settings: a hold from the slot table with "
           "0x10004f1",
           std::find(allowed.begin(), allowed.end(), 0x010004f1) != allowed.end() &&
               load_holds(HoldSettings{shipped("guide_hold"), shipped("guide_slots"), shipped("guide_scene_id"),
                                       shipped("guide_content_class"), shipped("guide_take_over")},
                          allowed, &s, &err) &&
               s.size() == 2 && s[1].table_slots == std::vector<uint64_t>{0x105, 0x103} && s[1].slot == 0 &&
               s[1].scene_id == 0x010004f1 &&
               err.empty());
    }
    ok("off with 0: no hold", load({"guide_hold=0"}, &t, &err) && t.size() == 1 && err.empty());

    ok("on: a hold of 0x105 following the map, never drawn",
       load({"guide_hold=1"}, &t, &err) && t.size() == 2 && t[1].name == "guide_0x105" && t[1].slot == 0x105 &&
           t[1].scene_id == 0x010004f1 && t[1].follows == "map" && t[1].take_over_stock && t[1].linger_ms == 0 &&
           t[1].width == 16 && t[1].height == 16 && t[1].content_class == 0 && t[1].strike_limit == 2 &&
           t[0].strike_limit == 0 &&
           t[1].take_over_contents == std::vector<uint64_t>{0x01000310});
    ok("auto: one hold whose slot is 0x105 or 0x103 from the slot table, with guide_scene_id, following the map",
       load({"guide_hold=1", "guide_slots=auto"}, &t, &err) && t.size() == 2 && t[1].name == "guide_auto" &&
           t[1].slot == 0 && t[1].table_slots == std::vector<uint64_t>{0x105, 0x103} &&
           t[1].scene_id == 0x010004f1 && t[1].follows == "map" &&
           t[1].take_over_stock && t[1].strike_limit == 2 &&
           t[1].take_over_contents == std::vector<uint64_t>{0x01000310} && err.empty());
    ok("a slot id: no slot from the table", load({"guide_hold=1"}, &t, &err) && t[1].table_slots.empty());
    {
        TargetSpec map, panel;
        map.name = "map";
        map.slot = 0x100;
        map.scene_id = 0x010004f0;
        panel.name = "panel";
        panel.slot = 0x105;
        panel.scene_id = 0x010004f2;
        std::vector<TargetSpec> v = {map, panel};
        ok("auto with another target in 0x105: refused",
           !load_holds(HoldSettings{"1", "auto", "0x010004f1", "0", "0x01000310"}, kAllowed, &v, &err) &&
               v.size() == 2 && err == "guide_slots: slot 0x105 is target panel's");
    }
    ok("auto with slot ids: refused",
       refused({"guide_hold=1", "guide_slots=auto, 0x105"}, "guide_slots: auto is given alone, without slot ids") &&
           refused({"guide_hold=1", "guide_slots=0x105 auto"}, "auto is given alone"));
    ok("auto with a scene id not allowed: refused",
       refused({"guide_hold=1", "guide_slots=auto", "guide_scene_id=0x010004f3"},
               "guide_auto's scene id 0x10004f3 is not in scene_ids_allowed"));
    ok("guide_take_over: a list, as given", load({"guide_hold=1", "guide_take_over=0x01000310 0x01000315"}, &t, &err) &&
                                                t[1].take_over_contents ==
                                                    std::vector<uint64_t>{0x01000310, 0x01000315});
    ok("guide_take_over malformed or empty: refused",
       refused({"guide_hold=1", "guide_take_over=0x1000310x"}, "\"0x1000310x\" is not a hex content id") &&
           refused({"guide_hold=1", "guide_take_over=16777744"}, "is not a hex content id") &&
           refused({"guide_hold=1", "guide_take_over= "}, "guide_take_over names no content"));
    ok("the seat-belt-warning panel in guide_take_over: refused",
       refused({"guide_hold=1", "guide_take_over=0x01000310,0x01000314"}, "0x1000314 is the seat-belt-warning panel"));
    ok("two slots, by comma or space: the next scene id each",
       load({"guide_hold=1", "guide_slots=0x105, 0x103"}, &t, &err) && t.size() == 3 && t[2].slot == 0x103 &&
           t[2].scene_id == 0x010004f2 && load({"guide_hold=1", "guide_slots=0x105 0x103"}, &t, &err) &&
           t.size() == 3 && t[2].name == "guide_0x103");
    ok("the class as given", load({"guide_hold=1", "guide_content_class=0x400"}, &t, &err) &&
                                 t[1].content_class == 0x400);

    for (const char *v : {"guide_hold=yes", "guide_hold=2", "guide_hold="})
        ok(("taken as 1, with a warning: " + std::string(v)).c_str(),
           load({v}, &t, &err) && t.size() == 2 && err.find("guide_hold must be 0 or 1") != std::string::npos &&
               err.find("taken as 1") != std::string::npos);
    ok("a placeholder slot: refused", refused({"guide_hold=1", "guide_slots=<slot>"}, "\"<slot>\" is not a slot id"));
    ok("a slot that is not whole hex: refused",
       refused({"guide_hold=1", "guide_slots=0xO5"}, "\"0xO5\" is not a slot id") &&
           refused({"guide_hold=1", "guide_slots=261"}, "\"261\" is not a slot id") &&
           refused({"guide_hold=1", "guide_slots=0x105;0x103"}, "\"0x105;0x103\" is not a slot id"));
    ok("a slot out of 0x1-0xfff: refused", refused({"guide_hold=1", "guide_slots=0x0"}, "\"0x0\"") &&
                                               refused({"guide_hold=1", "guide_slots=0x1000"}, "\"0x1000\""));
    ok("a slot twice: refused", refused({"guide_hold=1", "guide_slots=0x105,0x105"}, "slot 0x105 is listed twice"));
    ok("another target's slot: refused", refused({"guide_hold=1", "guide_slots=0x100"}, "slot 0x100 is target map's"));
    ok("a scene id not allowed: refused, nothing added",
       refused({"guide_hold=1", "guide_slots=0x105,0x103,0x101"}, "guide_0x101's scene id 0x10004f3 is not in "
                                                                  "scene_ids_allowed"));
    ok("a scene id or class that is not a number: refused",
       refused({"guide_hold=1", "guide_scene_id=0x10004fg"}, "guide_scene_id and guide_content_class must be") &&
           refused({"guide_hold=1", "guide_content_class=-1"}, "guide_scene_id and guide_content_class must be"));
    ok("the map's scene id: refused", refused({"guide_hold=1", "guide_scene_id=0x010004f0"},
                                              "guide_0x105's scene id 0x10004f0 is target map's"));
    ok("no slot: refused", refused({"guide_hold=1", "guide_slots= , "}, "guide_slots names no slot"));
    std::vector<TargetSpec> none;
    ok("no map to follow: refused",
       !load_holds(HoldSettings{"1", "0x105", "0x010004f1", "0", "0x01000310"}, kAllowed, &none, &err) &&
                                       err.find("no map target") != std::string::npos);
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
