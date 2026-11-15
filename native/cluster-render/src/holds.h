/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The rules the service adds to iDrive 7's targets: the guide hold's
 * settings, the HUD's and the panel's fallback.
 *
 * The guide hold's settings (cluster-render.conf "guide hold", iDrive 7):
 * unless guide_hold = 0, a hold (slots.h) that follows the map, appended to
 * the targets: one in 0x105 or 0x103, as the slot table reports them and
 * the box in them (guide_slots = auto), or one per slot of guide_slots. */
#ifndef CRS_HOLDS_H
#define CRS_HOLDS_H

#include <stdint.h>
#include <string>
#include <vector>

#include "slots.h"

namespace crs {

/* The settings as written. */
struct HoldSettings {
    std::string hold, slots, scene_id, content_class, take_over;   /* guide_hold, guide_slots, ... */
};

/* Appends a hold per slot of slots unless hold is 0; hold other than 0 or 1
 * counts as 1, with *warn saying so. slots: auto, one hold guide_auto with
 * scene_id whose slot is 0x105 or 0x103 (TargetSpec::table_slots),
 * neither another target's; or hex slot ids 0x1-0xfff, separated by commas
 * or spaces, each once and no other target's. Scene ids from scene_id up,
 * one per slot, each in allowed and no other target's.
 * take_over: the hex content ids the holds' take-overs may unmap, at least
 * one, separated the same way, never 0x01000314 (the seat-belt-warning
 * panel). Anything else adds no hold: false, with *warn naming it, and
 * targets left as they were. */
bool load_holds(const HoldSettings &cfg, const std::vector<uint64_t> &allowed, std::vector<TargetSpec> *targets,
                std::string *warn);

/* The HUD's slot and scene on iDrive 7 (README.md "The HUD"). */
const uint64_t kHudSlot = 0x323, kHudScene = 0x01000400;

/* The iDrive 7 HUD's rules for t, the hud target: only slot kHudSlot with
 * scene kHudScene, never
 * taking a slot from the stock HMI; then its layers write from the HUD's
 * share of the budget. False,
 * with *why, for any other slot or a take-over: the HUD is left out. */
bool hud_rules(TargetSpec *t, std::string *why);

/* The panel target t's rules (README.md "Scene ids"): its fallback scene
 * id, as written (panel_fallback_scene_id), a number in allowed, becomes
 * t's fallback unless it is t's own id or empty.
 * False, with *err, for any other fallback: the service does not start. */
bool fallback_rules(TargetSpec *t, const std::string &fallback, const std::vector<uint64_t> &allowed,
                    std::string *err);

}  // namespace crs

#endif
