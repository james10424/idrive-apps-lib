/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* cluster-render: the one process that draws on the instrument cluster and
 * the HUD for every app (README.md). It is the RAMSES participant and the
 * display client (Dcsm2 on iDrive 7, RAMSES 27's DCSM on iDrive 8); apps and
 * native producers are its clients over a socket. Built three times
 * (unit.h): cluster-render starts the service for the head unit's
 * generation, which takes the same arguments.
 *
 *   cluster-render [-c FILE] [-s key=value]...          serve
 *   cluster-render [-c FILE] [-s key=value]... status   print the service's status
 *   cluster-render [-c FILE] [-s key=value]... guard-reset
 *                     as root: remove the mapping guard's file (guard.h) and
 *                     restart the service if it runs */
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "cluster_render/protocol.h"
#include "config.h"
#include "head_unit.h"
#include "holds.h"
#include "log.h"
#include "refused.h"
#include "unit.h"

using namespace crs;

/* component.json's version, given by the target build. */
#ifndef CR_VERSION
#define CR_VERSION ""
#endif

namespace {

/* The keys this service reads beyond the shared Config's own
 * (cluster-render.conf documents each). */
void define_keys(cld::Config *cfg)
{
    cfg->define("generation", "auto");
    cfg->define("vsomeip_app", "cluster-demo");
    cfg->define("targets", "map,panel,hud");
    cfg->define("scene_ids_allowed", "0x010004f0,0x01000400,0x010004f1");
    cfg->define("map_slot", "0x100");
    cfg->define("map_content_class", "0");
    cfg->define("map_scene_id", "0x010004f0");
    cfg->define("map_size", "1088x480");
    cfg->define("map_linger_ms", "0");
    cfg->define("map_take_over_stock", "1");
    cfg->define("panel_slot", "0x203");
    cfg->define("panel_content_class", "0");
    cfg->define("panel_scene_id", "0x01000400");
    cfg->define("panel_size", "1920x720");
    cfg->define("panel_linger_ms", "3000");
    cfg->define("panel_take_over_stock", "0");
    cfg->define("hud_linger_ms", "3000");
    cfg->define("hud_take_over_stock", "0");
    /* the stock pop-up slots each target gives way to, and how long after */
    cfg->define("map_yield_slots", "");
    cfg->define("panel_yield_slots", "");
    cfg->define("hud_yield_slots", "0x322");
    cfg->define("map_yield_contents", "");
    cfg->define("panel_yield_contents", "");
    cfg->define("hud_yield_contents", "0x01000210");
    cfg->define("map_yield_ms", "1500");
    cfg->define("panel_yield_ms", "1500");
    cfg->define("hud_yield_ms", "1500");
    cfg->define("dcsm_hmi_client_handle", "1");
    cfg->define("guide_hold", "1");
    cfg->define("guide_slots", "auto");
    cfg->define("guide_scene_id", "0x010004f1");
    cfg->define("guide_content_class", "0");
    cfg->define("guide_take_over", "0x01000310");
    /* iDrive 8: a target's slot is a DCSM category, its content the content
     * offered there (README.md "iDrive 8"). */
    cfg->define("id8_targets", "map,hud,guide,widget,popup");
    cfg->define("id8_scene_ids_allowed",
                "0x4d4755000108,0x4d4755000110,0x4d4755000118,0x4d4755000001,0x4d4755000002");
    cfg->define("id8_map_category", "43100201");
    cfg->define("id8_map_content", "43204041");
    cfg->define("id8_map_scene_id", "0x4d4755000108");
    cfg->define("id8_map_size", "874x749");
    cfg->define("id8_map_linger_ms", "0");
    cfg->define("id8_hud_category", "43100202");
    cfg->define("id8_hud_content", "43204012");
    cfg->define("id8_hud_scene_id", "0x4d4755000110");
    cfg->define("id8_hud_size", "353x203");
    cfg->define("id8_hud_linger_ms", "3000");
    cfg->define("id8_guide_category", "43100401");
    cfg->define("id8_guide_content", "43204051");
    cfg->define("id8_guide_scene_id", "0x4d4755000118");
    cfg->define("id8_guide_size", "702x213");
    cfg->define("id8_guide_linger_ms", "3000");
    cfg->define("id8_widget_category", "43100301");
    cfg->define("id8_widget_content", "43204001");
    cfg->define("id8_widget_scene_id", "0x4d4755000001");
    cfg->define("id8_widget_size", "2240x896");
    cfg->define("id8_widget_linger_ms", "3000");
    cfg->define("id8_widget_description", "Cluster renderer");
    cfg->define("id8_widget_order", "50");
    cfg->define("id8_widget_preview", "");
    cfg->define("id8_widget_background_id", "");
    cfg->define("id8_widget_layouts", "");
    cfg->define("id8_widget_focus", "0");
    cfg->define("id8_popup_category", "43100301");
    cfg->define("id8_popup_content", "43204003");
    cfg->define("id8_popup_scene_id", "0x4d4755000002");
    cfg->define("id8_popup_size", "2240x896");
    cfg->define("id8_popup_linger_ms", "3000");
    cfg->define("id8_popup_description", "Pop-up");
    cfg->define("id8_popup_order", "51");
    cfg->define("id8_popup_preview", "");
    cfg->define("id8_popup_background_id", "43400005");
    cfg->define("id8_popup_layouts", "");
    cfg->define("id8_popup_focus", "1");
    cfg->define("fade_ms", "250");
    cfg->define("upload_kb_per_s", "5500");
    cfg->define("upload_floor_kb_per_s", "2000");
    cfg->define("upload_step_kb_per_s", "500");
    cfg->define("upload_burst_ms", "250");
    cfg->define("slow_flush_ms", "150");
    cfg->define("status_path", "/run/cluster-render/status.txt");
    cfg->define("guard_path", "/var/opt/cluster-render/state/guard.txt");
    cfg->define("log_file", "/var/opt/cluster-render/log/cluster-render.log");
    cfg->define("log_file_kb", "512");
    /* The shared Config's defaults are cluster-demo's (src/config.cpp). */
    std::string err;
    cfg->set_pair("socket_path=/dev/shm/cluster-render/render.sock", &err);
    cfg->set_pair("local_conf=/var/opt/cluster-render/cluster-render.local.conf", &err);
}

/* Generation 7's targets, or 8's from the id8_ keys: a category in place of
 * the slot, a content, no content class and no take-over. */
bool load_targets(const cld::Config &cfg, int generation, std::vector<TargetSpec> *out, std::string *err)
{
    const std::string g = generation == 8 ? "id8_" : "";
    const std::string slot = generation == 8 ? "category" : "slot";
    std::vector<uint64_t> allowed;
    {
        std::stringstream ss(cfg.str((g + "scene_ids_allowed").c_str()));
        std::string id;
        while (std::getline(ss, id, ','))
            allowed.push_back(std::strtoull(id.c_str(), nullptr, 0));
    }
    std::stringstream ss(cfg.str((g + "targets").c_str()));
    std::string name;
    while (std::getline(ss, name, ',')) {
        if (name.empty())
            continue;
        const std::string p = g + name + "_";
        TargetSpec t;
        t.name = name;
        if (!cfg.has((p + slot).c_str()) || !cfg.has((p + "scene_id").c_str()) ||
            (generation == 8 && !cfg.has((p + "content").c_str()))) {
            *err = "target " + name + ": " + p + slot + (generation == 8 ? ", " + p + "content" : std::string()) +
                   " and " + p + "scene_id must be set";
            return false;
        }
        t.slot = std::strtoull(cfg.str((p + slot).c_str()).c_str(), nullptr, 0);
        t.scene_id = std::strtoull(cfg.str((p + "scene_id").c_str()).c_str(), nullptr, 0);
        t.linger_ms = (int)cfg.num((p + "linger_ms").c_str());
        if (generation == 8) {
            t.content = std::strtoull(cfg.str((p + "content").c_str()).c_str(), nullptr, 0);
        } else {
            t.content_class = (uint32_t)cfg.num((p + "content_class").c_str());
            t.take_over_stock = cfg.num((p + "take_over_stock").c_str()) != 0;
            if (cfg.has((p + "yield_slots").c_str())) {
                std::stringstream ys(cfg.str((p + "yield_slots").c_str()));
                std::string ysl;
                while (std::getline(ys, ysl, ','))
                    if (!ysl.empty())
                        t.yield_slots.push_back(std::strtoull(ysl.c_str(), nullptr, 0));
            }
            if (cfg.has((p + "yield_contents").c_str())) {
                std::stringstream yc(cfg.str((p + "yield_contents").c_str()));
                std::string ycl;
                while (std::getline(yc, ycl, ','))
                    if (!ycl.empty())
                        t.yield_contents.push_back(std::strtoull(ycl.c_str(), nullptr, 0));
            }
            if (cfg.has((p + "yield_ms").c_str()))
                t.yield_ms = (int)cfg.num((p + "yield_ms").c_str());
            if (t.yield_ms < 0 || t.yield_ms > 60000) {
                *err = "target " + name + ": " + p + "yield_ms must be from 0 to 60000";
                return false;
            }
        }
        if (!cfg.size((p + "size").c_str(), &t.width, &t.height) || t.width < 16 || t.height < 16 ||
            t.width > 4096 || t.height > 4096) {
            *err = "target " + name + ": " + p + "size must be WxH";
            return false;
        }
        if (t.linger_ms < 0 || t.linger_ms > 600000) {
            *err = "target " + name + ": " + p + "linger_ms must be from 0 to 600000";
            return false;
        }
        std::string why;
        if (generation == 7 && name == "hud" && !hud_rules(&t, &why)) {
            log_warn("target hud left out: %s", why.c_str());
            continue;
        }
        /* Only scene ids the KOMBI is known to take: others (7702, 7703)
         * made it restart the cluster and the HUD. On iDrive 8, the ones
         * the cluster took. */
        if (std::find(allowed.begin(), allowed.end(), t.scene_id) == allowed.end()) {
            *err = "target " + name + ": scene id " + cfg.str((p + "scene_id").c_str()) + " is not in " + g +
                   "scene_ids_allowed";
            return false;
        }
        out->push_back(t);
    }
    if (out->empty()) {
        *err = "no targets";
        return false;
    }
    if (generation == 8)
        return true;
    std::string warn;
    const bool held = load_holds(HoldSettings{cfg.str("guide_hold"), cfg.str("guide_slots"), cfg.str("guide_scene_id"),
                                              cfg.str("guide_content_class"), cfg.str("guide_take_over")},
                                 allowed, out, &warn);
    if (!warn.empty())
        log_warn("guide hold%s: %s", held ? "" : " off", warn.c_str());
    return true;
}

}  // namespace

bool crs::service_config(const cld::Config &cfg, int generation, ServiceConfig *out, std::string *err)
{
    if (cfg.has("log_file"))
        cld::log_to_file(cfg.str("log_file"), cld::log_file_cap(cfg.num("log_file_kb")), "cluster-render", CR_VERSION);
    ServiceConfig &sc = *out;
    if (!load_targets(cfg, generation, &sc.targets, err))
        return false;
    const long max_kb = cfg.num("upload_kb_per_s"), step_kb = cfg.num("upload_step_kb_per_s");
    long floor_kb = cfg.num("upload_floor_kb_per_s");
    if (max_kb < 0 || max_kb > 1048576 || floor_kb < 0 || step_kb < 1 || cfg.num("upload_burst_ms") < 1 ||
        cfg.num("upload_burst_ms") > 1000 || cfg.num("fps") < 1 || cfg.num("fps") > 60 ||
        cfg.num("slow_flush_ms") < 10 || cfg.num("slow_flush_ms") > 10000 || cfg.num("fade_ms") < 0 ||
        cfg.num("fade_ms") > 5000) {
        *err = "upload_kb_per_s (0-1048576), upload_floor_kb_per_s (0 and up), upload_step_kb_per_s (1 and up), "
               "upload_burst_ms (1-1000), slow_flush_ms (10-10000), fps (1-60) or fade_ms (0-5000) out of range";
        return false;
    }
    /* A ceiling set below the floor is the cap: the governor then has no
     * room to lower it. 0 is no limit. */
    if (floor_kb > max_kb)
        floor_kb = max_kb;
    sc.budget.max_bps = (uint64_t)max_kb * 1024;
    sc.budget.floor_bps = (uint64_t)floor_kb * 1024;
    sc.budget.step_bps = (uint64_t)step_kb * 1024;
    sc.slow_flush_ms = (int)cfg.num("slow_flush_ms");
    sc.upload_burst_ms = (int)cfg.num("upload_burst_ms");
    sc.fps = (int)cfg.num("fps");
    sc.fade_ms = (int)cfg.num("fade_ms");
    sc.socket_path = cfg.str("socket_path");
    sc.socket_mode = (unsigned)std::strtoul(cfg.str("socket_mode").c_str(), nullptr, 8);
    sc.socket_group = cfg.str("socket_group");
    sc.status_path = cfg.str("status_path");
    sc.backend = generation == 8 ? "id8" : "id7";
    sc.guard_path = cfg.str("guard_path");
    sc.version = CR_VERSION;
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    std::getline(boot, sc.boot_id);
    return true;
}

namespace {

/* Prints the service's status: its targets and every client's layers. */
int status(const cld::Config &cfg)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", cfg.str("socket_path").c_str());
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        std::fprintf(stderr, "cluster-render: %s: %s\n", addr.sun_path, strerror(errno));
        return 1;
    }
    const std::string m = cr::encode(cr::kJson, "{\"op\":\"status\"}");
    if (write(fd, m.data(), m.size()) != (ssize_t)m.size())
        return 1;
    std::string in;
    char buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0)
            return 1;
        in.append(buf, (size_t)r);
        size_t used = 0, len = 0;
        uint8_t type = 0;
        const char *body = nullptr;
        bool bad = false;
        while (cr::next_message(in.data(), in.size(), &used, &type, &body, &len, &bad)) {
            cr::Json j;
            if (type == cr::kJson && cr::parse_json(std::string(body, len), &j) && j["ev"] == "status") {
                std::fputs(j["text"].c_str(), stdout);
                return 0;
            }
            in.erase(0, used);
        }
        if (bad)
            return 1;
    }
}

/* Root only: removes the mapping guard's file, then restarts the service
 * if it runs, so it starts with no strike. */
int guard_reset(const cld::Config &cfg)
{
    if (geteuid() != 0) {
        std::fprintf(stderr, "cluster-render: guard-reset runs as root\n");
        return 1;
    }
    const std::string path = cfg.str("guard_path");
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        std::fprintf(stderr, "cluster-render: %s: %s\n", path.c_str(), strerror(errno));
        return 1;
    }
    std::printf("cluster-render: %s removed\n", path.c_str());
    return std::system("systemctl try-restart cluster-render.service") == 0 ? 0 : 1;
}

bool load_local(cld::Config *cfg, std::string *err)
{
    const std::string path = cfg->str("local_conf");
    return path.empty() || access(path.c_str(), F_OK) != 0 || cfg->load(path, err);
}

}  // namespace

int main(int argc, char **argv)
{
    cld::log_name = "cluster-render";
    cld::Config cfg;
    define_keys(&cfg);
    std::string err;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const bool has_arg = i + 1 < argc;
        if (!std::strcmp(argv[i], "-c") && has_arg) {
            if (!cfg.load(argv[++i], &err) || !load_local(&cfg, &err)) {
                std::fprintf(stderr, "cluster-render: %s\n", err.c_str());
                return 2;
            }
        } else if (!std::strcmp(argv[i], "-s") && has_arg) {
            if (!cfg.set_pair(argv[++i], &err)) {
                std::fprintf(stderr, "cluster-render: %s\n", err.c_str());
                return 2;
            }
        } else {
            break;
        }
    }
    cld::log_verbose = (int)cfg.num("verbose");
    signal(SIGPIPE, SIG_IGN);
    if (i == argc) {
        /* Before anything that reaches the cluster: the generation's
         * service, Dcsm2, vsomeip, RAMSES. */
        const std::string why = cld::head_unit_refused();
        return why.empty() ? start(cfg, argv) : serve_refused(cfg, why);
    }
    if (i + 1 == argc && !std::strcmp(argv[i], "status"))
        return status(cfg);
    if (i + 1 == argc && !std::strcmp(argv[i], "guard-reset"))
        return guard_reset(cfg);
    std::fprintf(stderr, "usage: cluster-render [-c FILE] [-s key=value]... [status | guard-reset]\n");
    return 2;
}
