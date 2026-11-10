// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Where the map shows and the HUD map's size, place and whether it stays
 * shown over BMW's pop-ups, as the app chose them: cluster, hud,
 * hud_width_pct, hud_offset_x, hud_offset_y and hud_keep_shown, which
 * patchd saves in map_state_file (key = value lines), a file of its own so
 * that waze-on-cluster.local.conf stays one an earlier version of this tool
 * starts with. Each valid value replaces the configuration's; one out of
 * range, another key or a line that does not read is left out with a
 * warning, as is a file that cannot be read. */
#ifndef WOC_MAP_STATE_H
#define WOC_MAP_STATE_H

#include <string>

#include "config.h"

namespace woc {

/* The keys map_state_file holds. */
extern const char *const kMapStateKeys[6];

void load_map_state(cld::Config *cfg, const std::string &path);

}  // namespace woc

#endif
