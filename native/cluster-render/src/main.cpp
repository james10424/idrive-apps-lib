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
 *   cluster-render [-c FILE] [-s key=value]... status   print the service's status */
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cluster_render/protocol.h"
#include "config.h"
#include "head_unit.h"
#include "log.h"
#include "refused.h"
#include "settings.h"
#include "unit.h"

using namespace crs;

/* component.json's version, given by the target build. */
#ifndef CR_VERSION
#define CR_VERSION ""
#endif

namespace {

/* The drop-ins read with the settings (main()), for the log and the
 * status once the service starts (service_config). */
std::vector<std::string> drop_in_report;

/* The mapping guard's strikes, as cluster-render 0.5.11 and older keep
 * them: nothing reads them, and the service removes the file at start. */
const char kOldStateFile[] = "/var/opt/cluster-render/state/guard.txt";

}  // namespace

bool crs::service_config(const cld::Config &cfg, int generation, ServiceConfig *out, std::string *err)
{
    if (cfg.has("log_file"))
        cld::log_to_file(cfg.str("log_file"), cld::log_file_cap(cfg.num("log_file_kb")), "cluster-render", CR_VERSION);
    ServiceConfig &sc = *out;
    for (const std::string &line : drop_in_report)
        cld::log(line.compare(0, 8, "skipped ") ? 'I' : 'W', "drop-in %s", line.c_str());
    sc.drop_ins = drop_in_report;
    if (cfg.has("guard_path"))
        cld::log('W', "setting guard_path is not used; ignored");
    if (unlink(kOldStateFile) == 0)
        cld::log('I', "%s removed: its strikes are not used", kOldStateFile);
    else if (errno != ENOENT)
        cld::log('W', "%s: not removed: %s", kOldStateFile, strerror(errno));
    if (!load_targets(cfg, generation, &sc.targets, err))
        return false;
    const long max_kb = cfg.num("upload_kb_per_s"), step_kb = cfg.num("upload_step_kb_per_s");
    long floor_kb = cfg.num("upload_floor_kb_per_s");
    if (max_kb < 0 || max_kb > 1048576 || floor_kb < 0 || step_kb < 1 || cfg.num("upload_burst_ms") < 1 ||
        cfg.num("upload_burst_ms") > 1000 || cfg.num("fps") < 1 || cfg.num("fps") > 60 ||
        cfg.num("pixel_mib") < 1 || cfg.num("pixel_mib") > 256 ||
        cfg.num("slow_flush_ms") < 10 || cfg.num("slow_flush_ms") > 10000 || cfg.num("fade_ms") < 0 ||
        cfg.num("fade_ms") > 5000) {
        *err = "upload_kb_per_s (0-1048576), upload_floor_kb_per_s (0 and up), upload_step_kb_per_s (1 and up), "
               "upload_burst_ms (1-1000), slow_flush_ms (10-10000), fps (1-60), fade_ms (0-5000) or pixel_mib (1-256) "
               "out of range";
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
    sc.pixel_bytes = (uint64_t)cfg.num("pixel_mib") << 20;
    sc.fps = (int)cfg.num("fps");
    sc.fade_ms = (int)cfg.num("fade_ms");
    sc.socket_path = cfg.str("socket_path");
    sc.socket_mode = (unsigned)std::strtoul(cfg.str("socket_mode").c_str(), nullptr, 8);
    sc.socket_group = cfg.str("socket_group");
    sc.status_path = cfg.str("status_path");
    sc.backend = generation == 8 ? "id8" : "id7";
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
    bool conf = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const bool has_arg = i + 1 < argc;
        if (!std::strcmp(argv[i], "-c") && has_arg) {
            if (!cfg.load(argv[++i], &err) || !load_local(&cfg, &err)) {
                std::fprintf(stderr, "cluster-render: %s\n", err.c_str());
                return 2;
            }
            conf = true;
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
        /* The drop-ins with a settings file, as local_conf, but after the
         * whole command line: -s drop_ins= reads none (settings.h). */
        if (conf)
            load_drop_ins(&cfg, 0, &drop_in_report);
        /* Before anything that reaches the cluster: the generation's
         * service, Dcsm2, vsomeip, RAMSES. */
        const std::string why = cld::head_unit_refused();
        return why.empty() ? start(cfg, argv) : serve_refused(cfg, why);
    }
    if (i + 1 == argc && !std::strcmp(argv[i], "status"))
        return status(cfg);
    std::fprintf(stderr, "usage: cluster-render [-c FILE] [-s key=value]... [status]\n");
    return 2;
}
