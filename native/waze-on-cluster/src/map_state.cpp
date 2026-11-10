// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "map_state.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "log.h"

namespace woc {

const char *const kMapStateKeys[6] = {"cluster",      "hud",          "hud_width_pct",
                                      "hud_offset_x", "hud_offset_y", "hud_keep_shown"};

namespace {

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

/* A whole number in the key's range: the map on (1) or off (0), 0 or 20-100 %
 * wide, -800..800 px from the centre, kept shown (1) or not (0). */
bool valid(const std::string &key, const std::string &value)
{
    char *end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end)
        return false;
    if (key == "hud_width_pct")
        return v == 0 || (v >= 20 && v <= 100);
    if (key == "hud_offset_x" || key == "hud_offset_y")
        return v >= -800 && v <= 800;
    return v == 0 || v == 1;
}

}  // namespace

void load_map_state(cld::Config *cfg, const std::string &path)
{
    if (path.empty() || access(path.c_str(), F_OK) != 0)
        return;
    std::ifstream f(path);
    if (!f) {
        log_warn("%s: cannot read it: the map settings of the configuration instead", path.c_str());
        return;
    }
    std::string line, applied, err;
    while (std::getline(f, line)) {
        if (trim(line).empty())
            continue;
        const size_t eq = line.find('=');
        const std::string key = trim(line.substr(0, eq)), value = eq == std::string::npos ? "" : trim(line.substr(eq + 1));
        const bool known = std::find_if(std::begin(kMapStateKeys), std::end(kMapStateKeys),
                                        [&](const char *k) { return key == k; }) != std::end(kMapStateKeys);
        if (!known || !valid(key, value) || !cfg->set_pair(key + "=" + value, &err)) {
            log_warn("%s: \"%s\" left out", path.c_str(), trim(line).c_str());
            continue;
        }
        applied += (applied.empty() ? "" : ", ") + key + " " + value;
    }
    if (!applied.empty())
        log_info("map settings from %s: %s", path.c_str(), applied.c_str());
}

}  // namespace woc
