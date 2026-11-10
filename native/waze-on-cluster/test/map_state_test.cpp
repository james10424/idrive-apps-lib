// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of the map's saved settings (map_state.h): no state file, the
 * configuration's; each valid value of the state file over the
 * configuration's; a value out of range, another key or a line that does
 * not read left out, the rest taken; a state file that cannot be read, the
 * configuration's; the shipped waze-on-cluster.conf names the file patchd
 * writes. */
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "config.h"
#include "map_state.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

/* The configuration's map settings: the shipped defaults, the HUD 60 % wide. */
static cld::Config conf()
{
    cld::Config c;
    c.define("cluster", "1");
    c.define("hud", "0");
    c.define("hud_width_pct", "60");
    c.define("hud_offset_x", "0");
    c.define("hud_offset_y", "0");
    c.define("hud_keep_shown", "0");
    return c;
}

static std::string settings(const cld::Config &c)
{
    std::string s;
    for (const char *k : kMapStateKeys)
        s += std::string(s.empty() ? "" : " ") + c.str(k);
    return s;
}

int main()
{
    const std::string path = "/tmp/woc-map-state-test-" + std::to_string(getpid()) + ".conf";
    unlink(path.c_str());
    cld::Config c = conf();
    load_map_state(&c, path);
    ok("no state file: the configuration's", settings(c) == "1 0 60 0 0 0");
    std::ofstream(path) << "hud = 1\ncluster = 0\nhud_width_pct = 85\nhud_offset_x = -40\nhud_offset_y = 800\n"
                           "hud_keep_shown = 1\n";
    c = conf();
    load_map_state(&c, path);
    ok("the state file's, over the configuration's", settings(c) == "0 1 85 -40 800 1");
    std::ofstream(path) << "cluster = 2\nhud = 1\nhud_width_pct = 10\n\nhud_offset_x = 900\nhud_offset_y = -20\n"
                           "hud_fps = 30\ntarget = panel\ngarbage\nhud = yes\nhud_keep_shown = 2\n";
    c = conf();
    c.define("hud_fps", "10");
    c.define("target", "map");
    load_map_state(&c, path);
    ok("out of range, another key or a line that does not read: left out, the rest taken",
       settings(c) == "1 1 60 0 -20 0" && c.str("hud_fps") == "10" && c.str("target") == "map");
    std::ofstream(path) << "hud_width_pct = 0\nhud_offset_x = 1x\nhud_offset_y = \ncluster =1\n";
    c = conf();
    load_map_state(&c, path);
    ok("Standard (0) taken; a number with more after it, or none, left out", settings(c) == "1 0 0 0 0 0");
    std::ofstream(path) << "hud = 1\n";
    chmod(path.c_str(), 0);
    if (access(path.c_str(), R_OK) != 0) {   /* root reads it anyway */
        c = conf();
        load_map_state(&c, path);
        ok("a state file that cannot be read: the configuration's", settings(c) == "1 0 60 0 0 0");
    }
    unlink(path.c_str());
    std::ifstream shipped("waze-on-cluster.conf");
    std::string line, file;
    while (std::getline(shipped, line))
        if (line.compare(0, 17, "map_state_file = ") == 0)
            file = line.substr(17);
    ok("shipped: map_state_file is the file patchd writes", file == "/var/opt/waze-on-cluster/map.state.conf");
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
