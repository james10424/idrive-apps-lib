/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* cluster-render-idrive7: the service on iDrive 7, the RAMSES 19
 * participant and the Dcsm2 client (README.md). */
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "dcsm2_link.h"
#include "dcsm2_port.h"
#include "graph_ramses19.h"
#include "log.h"
#include "ramses19/ramses.h"
#include "ramses_console.h"
#include "unit.h"

namespace crs {

namespace {

/* The console's lines and events since the last call: the lines into the
 * log, the events to the slot manager. */
void renderer_events(RamsesConsole *console, SlotManager *slots)
{
    std::vector<ConsoleEvent> events;
    std::vector<std::string> raw;
    uint64_t dropped = 0;
    console->take(&events, &raw, &dropped);
    for (const std::string &line : raw)
        log_info("ramses: %.400s", line.c_str());
    if (dropped)
        log_info("ramses: %llu more such lines not logged (at most %u a minute)", (unsigned long long)dropped,
                 RamsesConsole::kRawPerMinute);
    for (const ConsoleEvent &e : events) {
        switch (e.kind) {
        case ConsoleEvent::Subscribed:
        case ConsoleEvent::Unsubscribed:
            slots->on_subscription(e.scene, e.kind == ConsoleEvent::Subscribed, e.unknown, e.guid, e.ms);
            break;
        case ConsoleEvent::RendererGone: slots->on_renderer_gone(e.guid, e.ms); break;
        case ConsoleEvent::ResourceRequest: slots->on_resource_request(e.guid, e.resources, e.ms); break;
        }
    }
}

}  // namespace

int start(const cld::Config &cfg, char **)
{
    ServiceConfig sc;
    std::string err;
    if (!service_config(cfg, 7, &sc, &err)) {
        log_error("%s", err.c_str());
        return 1;
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    const int sig_fd = signalfd(-1, &sigs, SFD_CLOEXEC);
    if (sig_fd < 0) {
        log_error("signalfd: %s", strerror(errno));
        return 1;
    }

    /* The renderer's subscriptions, from RAMSES's console at info
     * (ramses_console.h): stdout goes to the reader before RAMSES starts. */
    std::vector<uint64_t> scenes;
    for (const TargetSpec &t : sc.targets)
        scenes.push_back(t.scene_id);
    RamsesConsole *console = new RamsesConsole(scenes);   /* for the process: its thread reads until exit */
    if (!console->start(&err))
        log_warn("ramses: console not read (%s): renderer subscription not observable", err.c_str());
    const char *args[] = {"cluster-render", "--log-level-console", "info"};
    ramses::RamsesFrameworkConfig fwc(3, args);
    fwc.setDLTApplicationID("CLRD");
    fwc.setDLTApplicationDescription("cluster-render");
    const uint32_t user = (uint32_t)cfg.num("ramses_user");
    if (fwc.enableSomeIPCommunication(user) != ramses::StatusOK) {
        log_error("ramses: SOME/IP participant %u refused", user);
        return 1;
    }
    ramses::RamsesFramework framework(fwc);
    ramses::RamsesClient client("cluster-render", framework);
    if (framework.connect() != ramses::StatusOK) {
        log_error("ramses: connect failed (participant %u)", user);
        return 1;
    }
    log_info("ramses: connected as participant %u", user);
    Ramses19Gpu gpu(client);
    if (!gpu.init(&err)) {
        log_error("ramses: %s", err.c_str());
        framework.disconnect();
        return 1;
    }

    Dcsm2Link link;
    Dcsm2Port port(&link);
    Service service(&gpu, &port, sc);
    port.set_manager(&service.slots());
    port.set_hmi_client_handle(std::strtoull(cfg.str("dcsm_hmi_client_handle").c_str(), nullptr, 0));
    service.slots().console_text = [console](int64_t now_ms) { return console->counts_text(now_ms); };
    Governor *gov = &service.compositor().governor();
    port.on_link = [gov](bool silent, int64_t now_ms) {
        if (silent)
            gov->evidence(Evidence::LinkStall, now_ms);
    };
    Dcsm2Link::Options lo;
    lo.app_name = cfg.str("vsomeip_app");
    lo.client_handle = cfg.has("dcsm_client_handle")
                           ? std::strtoull(cfg.str("dcsm_client_handle").c_str(), nullptr, 0)
                           : 0;
    lo.heartbeat_replies = cfg.num("dcsm_heartbeat_replies") != 0;
    if (!service.start(&err) || !link.start(lo, &err)) {
        log_error("%s", err.c_str());
        framework.disconnect();
        return 1;
    }
    for (const TargetSpec &t : sc.targets)
        log_info("target %s: slot 0x%llx class %u scene 0x%llx %ux%u, linger %d ms%s%s%s%s", t.name.c_str(),
                 (unsigned long long)t.slot, t.content_class, (unsigned long long)t.scene_id, t.width, t.height,
                 t.linger_ms, t.take_over_stock ? ", takes over from the stock HMI" : "",
                 t.scene_files ? ", takes scene files" : "", t.follows.empty() ? "" : ", held while shown: ",
                 t.follows.c_str());

    std::vector<pollfd> fds;
    for (;;) {
        fds.clear();
        fds.push_back(pollfd{sig_fd, POLLIN, 0});
        fds.push_back(pollfd{link.fd(), POLLIN, 0});
        fds.push_back(pollfd{console->fd(), POLLIN, 0});
        const size_t first = service.poll_fds(&fds);
        const int n = poll(fds.data(), fds.size(), service.timeout_ms(cld::now_ms()));
        if (n < 0 && errno != EINTR)
            break;
        if (fds[0].revents & POLLIN)
            break;
        const int64_t now = cld::now_ms();
        if (fds[1].revents & POLLIN)
            link.drain(&port, now);
        if (fds[2].revents & POLLIN)
            renderer_events(console, &service.slots());
        service.after_poll(fds, first, now);
    }
    service.shutdown(cld::now_ms());
    usleep(300 * 1000);   /* the unmap and unfetch on their way */
    link.stop();
    framework.disconnect();
    log_info("stopped");
    return 0;
}

}  // namespace crs
