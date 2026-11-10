/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* One pass of the render loop, the same for the unit's binary (main.cpp) and
 * the host harness (test/host_service.cpp); only the backends differ.
 *
 * Each pass, after poll():
 *   1. the display's reports reach the slot manager (the caller drains them),
 *   2. clients' messages and frames are taken (server),
 *   3. each target's layers are arranged and its demand is set (compositor),
 *   4. targets are published, requested, kept or released (slot manager),
 *   5. each requested target's layers are staged and flushed once,
 *   6. changed states go to the clients, and once a second to the status
 *      file; once a minute each shown target's layers' writes since its
 *      last such line, and whether it is rendered, go to the log. */
#ifndef CRS_SERVICE_H
#define CRS_SERVICE_H

#include <poll.h>

#include <memory>
#include <string>
#include <vector>

#include "compositor.h"
#include "guard.h"
#include "governor.h"
#include "server.h"
#include "slots.h"

namespace crs {

struct ServiceConfig {
    std::vector<TargetSpec> targets;
    GovernorConfig budget;   /* ceiling 5500, floor 2000, step 500 KB/s */
    int upload_burst_ms = 250;
    int slow_flush_ms = 150;
    int fps = 30;                /* the loop's tick while a target is up */
    int fade_ms = 250;           /* a target's layers fade in when it is shown, and out before it is released */
    std::string socket_path = "/dev/shm/cluster-render/render.sock";
    unsigned socket_mode = 0660;
    std::string socket_group;
    std::string status_path;     /* empty: none */
    std::string backend = "id7";
    /* The mapping guard's file (guard.h); empty: no guard. */
    std::string guard_path;
    /* Where the scene files of targets that take them come from: under
     * /var/opt/, root's (scene_file.h). */
    SceneFiles scene_files;
    std::string version, boot_id;
};

class Service {
public:
    Service(Gpu *gpu, Port *port, const ServiceConfig &cfg);
    bool start(std::string *err);

    SlotManager &slots() { return slots_; }
    Guard &guard() { return guard_; }
    Compositor &compositor() { return *comp_; }
    Server &server() { return *server_; }

    /* The server's poll entries, appended to fds; returns where they start. */
    size_t poll_fds(std::vector<pollfd> *fds) const;
    int timeout_ms(int64_t now_ms) const;
    /* Steps 2-6 (README.md "The render loop"). */
    void after_poll(const std::vector<pollfd> &fds, size_t first, int64_t now_ms);
    /* Takes everything off the display and unpublishes the scenes, for a
     * clean exit. */
    void shutdown(int64_t now_ms);

private:
    ServiceConfig cfg_;
    SlotManager slots_;
    Guard guard_;
    std::unique_ptr<Compositor> comp_;
    std::unique_ptr<Server> server_;
    int64_t status_ms_ = 0;
    static const int64_t kMinuteMs = 60000;
    int64_t minute_ms_ = 0;
    /* Per target: its layers' writes and the time at its last minute line. */
    std::vector<WriteStats> stats_;
    std::vector<int64_t> stats_ms_;
    void log_minute(int64_t now_ms);
};

}  // namespace crs

#endif
