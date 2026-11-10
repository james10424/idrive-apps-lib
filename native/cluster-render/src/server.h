/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The service's socket: any number of clients (apps, native producers), each
 * owning the layers it makes, which go when it disconnects (PROTOCOL.md).
 *
 * A client names its layers with its own refs; the service tells it each
 * layer's state (shown, waiting and why, suspended, stalled, hidden, error)
 * and each target's (slots.h) whenever they change. Frames are taken as
 * they arrive and each replaces the layer's frame not yet written, so a
 * producer faster than the display skips ahead; a client that stops
 * reading its events is disconnected rather than buffered without bound. */
#ifndef CRS_SERVER_H
#define CRS_SERVER_H

#include <poll.h>

#include <map>
#include <string>
#include <vector>

#include "cluster_render/protocol.h"
#include "compositor.h"
#include "recv_buffer.h"
#include "slots.h"

namespace crs {

class Server {
public:
    static const size_t kMaxClients = 16;
    static const size_t kMaxOutput = 1 << 20;

    Server(Compositor *comp, SlotManager *slots, const std::string &backend);
    ~Server();

    /* The systemd-passed socket (fd 3) if there is one, else path. */
    bool listen(const std::string &path, unsigned mode, const std::string &group, std::string *err);

    /* Appends the listening socket and every client to fds (POLLOUT where
     * output waits); handle() takes the same entries back from first on. */
    void poll_fds(std::vector<pollfd> *fds) const;
    void handle(const std::vector<pollfd> &fds, size_t first, int64_t now_ms);
    /* Sends what changed since the last call. */
    void report(int64_t now_ms);

    size_t clients() const { return clients_.size(); }
    std::string status_text(int64_t now_ms) const;
    /* Where the scene files clients name may come from (scene_file.h). */
    void set_scene_files(const SceneFiles &where) { scene_files_ = where; }

private:
    struct Client {
        int fd = -1;
        uint64_t id = 0;
        std::string app;
        RecvBuffer in;
        std::string out;
        std::map<uint32_t, uint32_t> refs;            /* the client's ref -> layer or scene content id */
        std::map<uint32_t, uint32_t> assets;          /* the client's ref -> asset id */
        std::map<uint32_t, std::string> layer_sent;   /* by ref: the state line last sent */
        std::map<std::string, std::string> target_sent;
        std::map<uint32_t, std::string> frame_error;  /* by ref: the last frame error sent */
        std::string budget_sent;
        bool closing = false;
    };

    void accept_client(int64_t now_ms);
    void read_client(Client &c, int64_t now_ms);
    void write_client(Client &c);
    void drop_client(size_t i);
    void on_json(Client &c, const std::string &body, int64_t now_ms);
    void on_frame(Client &c, const char *body, size_t n, int64_t now_ms);
    void on_mesh(Client &c, const char *body, size_t n, int64_t now_ms);
    void send(Client &c, const std::string &json);
    void error(Client &c, const std::string &op, uint32_t ref, const std::string &why);
    /* With the edit line it is about. */
    void error(Client &c, const std::string &op, uint32_t ref, const std::string &why, const std::string &line);
    /* A layer of c waiting for the scene file on its target ("scene"): the
     * scene's ref when it is c's, else its client's app. */
    std::string scene_reason(const Client &c, uint32_t layer) const;
    void op_layer(Client &c, const cr::Json &j, int64_t now_ms);
    void op_asset(Client &c, const cr::Json &j);
    /* A scene file on a target (PROTOCOL.md "Scene files"), and its edits. */
    void op_scene(Client &c, const cr::Json &j);
    void op_edit(Client &c, uint32_t ref, const cr::Json &j);
    /* set, uniform, bind, part, transform, animate and stop. */
    void op_on_layer(Client &c, const std::string &op, uint32_t ref, Layer *l, const cr::Json &j);
    void op_uniform(Client &c, uint32_t ref, Layer *l, const cr::Json &j);
    bool apply(Layer *l, const cr::Json &j, std::string *err);

    Compositor *comp_;
    SlotManager *slots_;
    std::string backend_;
    SceneFiles scene_files_;
    int listen_fd_ = -1;
    bool own_path_ = false;
    std::string path_;
    std::vector<Client> clients_;
    uint64_t next_client_ = 1;
};

}  // namespace crs

#endif
