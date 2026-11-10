/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The service on the host with fake display backends (fake_graph.h,
 * fake_port.h), for client tests in other languages: it serves the socket
 * given as its argument with the default targets (map, panel, hud), the
 * panel taking scene files from under the folder given second (the user's
 * own; fake_graph.h FakeFileScene), grants every request after 50 ms, and
 * takes commands on stdin:
 *
 *   steal <target>   the stock HMI maps its content over the target's slot
 *   free <target>    the slot is free again
 *   down / up        the display service goes away / comes back
 *   budget           prints the upload cap */
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <sstream>
#include <string>

#include "../src/service.h"
#include "fake_graph.h"
#include "fake_port.h"
#include "log.h"

using namespace crs;

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: host_service SOCKET [SCENE_ROOT]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    cld::log_name = "cluster-render";
    FakeGpu gpu;
    FakePort port;
    ServiceConfig sc;
    TargetSpec m, p, h;
    m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
    m.take_over_stock = true;
    p.name = "panel"; p.slot = 0x203; p.scene_id = 0x010004f8; p.width = 1920; p.height = 720; p.linger_ms = 3000;
    h.name = "hud"; h.slot = 0x323; h.scene_id = 0x01000400; h.width = 800; h.height = 480; h.linger_ms = 3000;
    p.scene_files = argc == 3;
    sc.targets = {m, p, h};
    if (argc == 3) {
        sc.scene_files.root = argv[2];
        sc.scene_files.owner = getuid();
    }
    sc.socket_path = argv[1];
    sc.backend = "host";
    Service svc(&gpu, &port, sc);
    port.m = &svc.slots();
    std::string err;
    if (!svc.start(&err)) {
        std::fprintf(stderr, "host_service: %s\n", err.c_str());
        return 1;
    }
    std::printf("ready\n");
    std::fflush(stdout);
    std::string line;
    for (;;) {
        std::vector<pollfd> fds{{0, POLLIN, 0}};
        const size_t first = svc.poll_fds(&fds);
        poll(fds.data(), fds.size(), svc.timeout_ms(cld::now_ms()));
        const int64_t now = cld::now_ms();
        port.tick(now);
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[256];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0)
                break;
            line.append(buf, (size_t)n);
            size_t nl;
            while ((nl = line.find('\n')) != std::string::npos) {
                std::istringstream in(line.substr(0, nl));
                line.erase(0, nl + 1);
                std::string cmd, target;
                in >> cmd >> target;
                const int t = svc.slots().index(target);
                if (cmd == "steal" && t >= 0)
                    port.steal(svc.slots().targets()[t]);
                else if (cmd == "free" && t >= 0)
                    port.free_slot(svc.slots().targets()[t]);
                else if (cmd == "down" || cmd == "up")
                    port.set_up(cmd == "up");
                else if (cmd == "budget")
                    std::printf("budget %llu\n", (unsigned long long)(svc.compositor().governor().cap() / 1024));
                std::fflush(stdout);
            }
        }
        svc.after_poll(fds, first, now);
    }
    return 0;
}
