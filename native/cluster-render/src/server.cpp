/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "server.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"

namespace crs {

namespace {

/* Non-blocking and close-on-exec, the portable way (the host tests run on
 * macOS). */
void nonblocking(int fd)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;   /* the process ignores SIGPIPE */
#endif

bool num(const cr::Json &j, const char *key, double lo, double hi, double *out, std::string *err)
{
    auto it = j.find(key);
    if (it == j.end())
        return true;
    char *end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    if (it->second.empty() || *end || !(v >= lo && v <= hi)) {
        *err = std::string(key) + " must be a number from " + std::to_string((long long)lo) + " to " +
               std::to_string((long long)hi);
        return false;
    }
    *out = v;
    return true;
}

std::string str(const cr::Json &j, const char *key, const std::string &def = "")
{
    auto it = j.find(key);
    return it == j.end() ? def : it->second;
}

bool fnum(const cr::Json &j, const char *key, double lo, double hi, float *out, std::string *err)
{
    double v = *out;
    if (!num(j, key, lo, hi, &v, err))
        return false;
    *out = (float)v;
    return true;
}

/* One of names (its index in *out), or the default when absent. */
bool choice(const cr::Json &j, const char *key, const std::vector<std::string> &names, int *out,
            std::string *err)
{
    auto it = j.find(key);
    if (it == j.end())
        return true;
    for (size_t i = 0; i < names.size(); i++)
        if (it->second == names[i]) {
            *out = (int)i;
            return true;
        }
    *err = std::string(key) + " must be ";
    for (size_t i = 0; i < names.size(); i++)
        *err += (i ? (i + 1 == names.size() ? " or " : ", ") : "") + names[i];
    return false;
}

/* A mesh layer's projection and camera fields present in j, over *mp. */
bool mesh_params(const cr::Json &j, MeshParams *mp, std::string *err)
{
    MeshParams m = *mp;
    int ortho = m.projection.ortho ? 1 : 0;
    if (!choice(j, "projection", {"perspective", "ortho"}, &ortho, err))
        return false;
    m.projection.ortho = ortho == 1;
    Projection &p = m.projection;
    if (!fnum(j, "fov", 1, 179, &p.fov, err) || !fnum(j, "ortho_h", 0, 1000000, &p.ortho_h, err) ||
        !fnum(j, "near", 0, 1000000, &p.near, err) || !fnum(j, "far", 0, 1000000, &p.far, err))
        return false;
    if (!(p.near > 0 && p.far > p.near && p.ortho_h > 0)) {
        *err = "near and ortho_h must be above 0, and far above near";
        return false;
    }
    const char *const pos[3] = {"cx", "cy", "cz"}, *const rot[3] = {"crx", "cry", "crz"};
    for (int i = 0; i < 3; i++)
        if (!fnum(j, pos[i], -1000000, 1000000, &m.camera.t[i], err) ||
            !fnum(j, rot[i], -36000, 36000, &m.camera.r[i], err))
            return false;
    *mp = m;
    return true;
}

/* A part's pose fields present in j, over *p. */
bool pose_fields(const cr::Json &j, Pose *p, std::string *err)
{
    Pose n = *p;
    const char *const t[3] = {"tx", "ty", "tz"}, *const r[3] = {"rx", "ry", "rz"}, *const sc[3] = {"sx", "sy", "sz"};
    for (int i = 0; i < 3; i++)
        if (!fnum(j, t[i], -1000000, 1000000, &n.t[i], err) || !fnum(j, r[i], -36000, 36000, &n.r[i], err) ||
            !fnum(j, sc[i], -1000000, 1000000, &n.s[i], err))
            return false;
    *p = n;
    return true;
}

/* Up to max finite numbers separated by spaces, the whole text. */
bool numbers(const std::string &text, size_t max, std::vector<float> *out)
{
    out->clear();
    const char *p = text.c_str();
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            return !out->empty();
        char *end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p || !std::isfinite(f) || out->size() >= max)
            return false;
        out->push_back(f);
        p = end;
    }
}

/* "t:v[,v,v] t:v ...": times in ms, ascending, every key the same number
 * of values (1 to 4). */
bool parse_keys(const std::string &text, Animation *a, std::string *err)
{
    static const size_t kMaxKeys = 256;
    *err = "keys are t:v[,v,v,v] separated by spaces, t in ms ascending, every key as many values";
    a->t.clear();
    a->v.clear();
    a->width = 0;
    const char *p = text.c_str();
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p < '0' || *p > '9')
            return false;
        char *end = nullptr;
        const unsigned long long t = std::strtoull(p, &end, 10);
        if (*end != ':' || t > 0xffffffffull || (!a->t.empty() && t <= a->t.back()) || a->t.size() >= kMaxKeys)
            return false;
        p = end + 1;
        uint32_t n = 0;
        for (;;) {
            const float f = std::strtof(p, &end);
            if (end == p || !std::isfinite(f) || n >= 4)
                return false;
            a->v.push_back(f);
            n++;
            p = end;
            if (*p != ',')
                break;
            p++;
        }
        if (*p && *p != ' ' && *p != '\t')
            return false;
        if (a->width && n != a->width)
            return false;
        a->width = n;
        a->t.push_back((uint32_t)t);
    }
    if (a->t.empty())
        return false;
    err->clear();
    return true;
}

/* The client-settable parameters present in j, over *p. */
bool layer_params(const cr::Json &j, LayerParams *p, std::string *err)
{
    LayerParams n = *p;
    if (j.count("class") && !klass_from_name(j.at("class"), &n.klass)) {
        *err = "class must be ambient, content, overlay or alert";
        return false;
    }
    double v;
    v = n.z;
    if (!num(j, "z", -1000, 1000, &v, err)) return false;
    n.z = (int)v;
    v = n.rect.x;
    if (!num(j, "x", -4096, 4096, &v, err)) return false;
    n.rect.x = (int32_t)v;
    v = n.rect.y;
    if (!num(j, "y", -4096, 4096, &v, err)) return false;
    n.rect.y = (int32_t)v;
    v = n.rect.w;
    if (!num(j, "w", 1, 4096, &v, err)) return false;
    n.rect.w = (uint32_t)v;
    v = n.rect.h;
    if (!num(j, "h", 1, 4096, &v, err)) return false;
    n.rect.h = (uint32_t)v;
    v = n.opacity;
    if (!num(j, "opacity", 0, 1, &v, err)) return false;
    n.opacity = (float)v;
    if (!fnum(j, "rotate", -36000, 36000, &n.rotate, err) || !fnum(j, "sx", -1000, 1000, &n.sx, err) ||
        !fnum(j, "sy", -1000, 1000, &n.sy, err) || !fnum(j, "px", -8192, 8192, &n.px, err) ||
        !fnum(j, "py", -8192, 8192, &n.py, err))
        return false;
    n.pivot_x = n.pivot_x || j.count("px");
    n.pivot_y = n.pivot_y || j.count("py");
    struct Flag { const char *key; bool *field; } flags[] = {
        {"visible", &n.visible}, {"exclusive", &n.exclusive}, {"prewarm", &n.prewarm}, {"contend", &n.contend},
        {"guide_hold", &n.guide_hold}};
    for (const Flag &f : flags) {
        v = *f.field ? 1 : 0;
        if (!num(j, f.key, 0, 1, &v, err)) return false;
        *f.field = v != 0;
    }
    *p = n;
    return true;
}

std::string state_line(LayerState s, const std::string &reason)
{
    return std::string(layer_state_name(s)) + "\n" + reason;
}

}  // namespace

Server::Server(Compositor *comp, SlotManager *slots, const std::string &backend)
    : comp_(comp), slots_(slots), backend_(backend)
{
}

Server::~Server()
{
    for (Client &c : clients_)
        close(c.fd);
    if (listen_fd_ >= 0)
        close(listen_fd_);
    if (own_path_)
        unlink(path_.c_str());
}

bool Server::listen(const std::string &path, unsigned mode, const std::string &group, std::string *err)
{
    /* systemd socket activation: fd 3, LISTEN_PID = us. */
    const char *pid = getenv("LISTEN_PID");
    const char *fds = getenv("LISTEN_FDS");
    if (pid && fds && atoi(pid) == (int)getpid() && atoi(fds) >= 1) {
        listen_fd_ = 3;
        nonblocking(listen_fd_);
        log_info("socket from systemd");
        return true;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        *err = "socket path too long";
        return false;
    }
    std::strcpy(addr.sun_path, path.c_str());
    const std::string dir = path.substr(0, path.rfind('/'));
    if (!dir.empty())
        mkdir(dir.c_str(), 0755);
    unlink(path.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ >= 0)
        nonblocking(listen_fd_);
    if (listen_fd_ < 0 || bind(listen_fd_, (struct sockaddr *)&addr, sizeof addr) < 0 || ::listen(listen_fd_, 8) < 0) {
        *err = "socket " + path + ": " + strerror(errno);
        return false;
    }
    own_path_ = true;
    path_ = path;
    if (!group.empty()) {
        struct group *g = getgrnam(group.c_str());
        if (!g || chown(path.c_str(), (uid_t)-1, g->gr_gid) < 0)
            log_warn("cannot give %s to group %s", path.c_str(), group.c_str());
    }
    chmod(path.c_str(), mode);
    log_info("listening on %s", path.c_str());
    return true;
}

void Server::poll_fds(std::vector<pollfd> *fds) const
{
    fds->push_back(pollfd{listen_fd_, POLLIN, 0});
    for (const Client &c : clients_)
        fds->push_back(pollfd{c.fd, (short)(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
}

void Server::handle(const std::vector<pollfd> &fds, size_t first, int64_t now_ms)
{
    const size_t n = clients_.size();
    for (size_t i = 0; i < n && first + 1 + i < fds.size(); i++) {
        Client &c = clients_[i];
        const short re = fds[first + 1 + i].revents;
        if (re & POLLOUT)
            write_client(c);
        if (re & (POLLIN | POLLHUP | POLLERR))
            read_client(c, now_ms);
    }
    for (size_t i = clients_.size(); i-- > 0;)
        if (clients_[i].closing)
            drop_client(i);
    if (fds.size() > first && (fds[first].revents & POLLIN))
        accept_client(now_ms);
}

void Server::accept_client(int64_t)
{
    for (;;) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
            return;
        nonblocking(fd);
        if (clients_.size() >= kMaxClients) {
            log_warn("client refused: %zu connected", clients_.size());
            close(fd);
            continue;
        }
        /* Room for a whole video frame in flight (the kernel caps it). */
        int buf = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
        Client c;
        c.fd = fd;
        c.id = next_client_++;
        clients_.push_back(std::move(c));
    }
}

void Server::drop_client(size_t i)
{
    Client &c = clients_[i];
    if (!c.app.empty() || !c.refs.empty())   /* not a status request */
        log_info("client %llu (%s) gone, %zu layers and %zu assets removed", (unsigned long long)c.id,
                 c.app.empty() ? "?" : c.app.c_str(), c.refs.size(), c.assets.size());
    comp_->remove_owner(c.id);
    close(c.fd);
    clients_.erase(clients_.begin() + i);
}

void Server::read_client(Client &c, int64_t now_ms)
{
    /* At most a few messages' worth per pass, so one busy producer cannot
     * hold the render loop. */
    size_t budget = 4 << 20;
    for (;;) {
        ssize_t r = c.in.fill(c.fd);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            c.closing = true;
            break;
        }
        if (r < 0)
            break;
        for (;;) {
            size_t body_len = 0;
            uint8_t type = 0;
            const char *body = nullptr;
            bool bad = false;
            if (!c.in.next(&type, &body, &body_len, &bad)) {
                if (bad) {
                    log_warn("client %llu: malformed message, disconnected", (unsigned long long)c.id);
                    c.closing = true;
                }
                break;
            }
            if (type == cr::kJson)
                on_json(c, std::string(body, body_len), now_ms);
            else if (type == cr::kFrame)
                on_frame(c, body, body_len, now_ms);
            else if (type == cr::kMesh)
                on_mesh(c, body, body_len, now_ms);
            if (c.closing)
                break;
        }
        c.in.settle();
        if (c.closing || (size_t)r >= budget)
            break;
        budget -= (size_t)r;
    }
}

void Server::write_client(Client &c)
{
    while (!c.out.empty()) {
        ssize_t w = ::send(c.fd, c.out.data(), c.out.size(), kSendFlags);
        if (w < 0) {
            if (errno != EAGAIN && errno != EINTR)
                c.closing = true;
            return;
        }
        c.out.erase(0, (size_t)w);
    }
}

void Server::send(Client &c, const std::string &json)
{
    if (c.closing)
        return;
    c.out += cr::encode(cr::kJson, json);
    if (c.out.size() > kMaxOutput) {
        log_warn("client %llu (%s) does not read its events, disconnected", (unsigned long long)c.id, c.app.c_str());
        c.closing = true;
        return;
    }
    write_client(c);
}

void Server::error(Client &c, const std::string &op, uint32_t ref, const std::string &why)
{
    send(c, cr::json_object({{"ev", cr::json_quote("error")},
                             {"op", cr::json_quote(op)},
                             {"ref", std::to_string(ref)},
                             {"reason", cr::json_quote(why)}}));
}

bool Server::apply(Layer *l, const cr::Json &j, std::string *err)
{
    LayerParams p = l->params;
    if (!layer_params(j, &p, err))
        return false;
    MeshLayer *m = std::string(l->kind()) == "mesh" ? static_cast<MeshLayer *>(l) : nullptr;
    MeshParams mp;
    if (m) {
        mp = m->mesh;
        if (j.count("projection") && (j.at("projection") == "ortho") != mp.projection.ortho) {
            *err = "a mesh layer's projection is set when it is made";
            return false;
        }
        if (!mesh_params(j, &mp, err))
            return false;
        m->mesh = mp;
    }
    l->params = p;
    comp_->changed(l);
    return true;
}

void Server::op_layer(Client &c, const cr::Json &j, int64_t now_ms)
{
    double ref_d = 0;
    std::string err;
    if (!num(j, "ref", 1, 4294967295.0, &ref_d, &err) || ref_d == 0) {
        error(c, "layer", 0, err.empty() ? "ref missing" : err);
        return;
    }
    const uint32_t ref = (uint32_t)ref_d;
    if (c.refs.count(ref) || c.assets.count(ref)) {
        error(c, "layer", ref, "ref in use");
        return;
    }
    const int target = slots_->index(str(j, "target"));
    if (target < 0 || !slots_->targets()[target].follows.empty()) {
        error(c, "layer", ref, "unknown target " + str(j, "target"));
        return;
    }
    const TargetSpec &spec = slots_->targets()[target];
    LayerParams lp;
    lp.rect.w = spec.width;
    lp.rect.h = spec.height;
    if (!layer_params(j, &lp, &err)) {
        error(c, "layer", ref, err);
        return;
    }
    const std::string kind = str(j, "kind");
    uint32_t id = 0;
    double v;
    if (kind == "video") {
        VideoParams vp;
        struct { const char *key; double lo, hi; double cur; } k[] = {
            {"vw", 64, 1920, (double)vp.w},           {"vh", 32, 720, (double)vp.h},
            {"fps", 0, 60, (double)vp.fps},           {"level_bits", 0, 3, (double)vp.level_bits},
            {"dirty_threshold", 0, 64, (double)vp.dirty_threshold},
            {"settle_ms", 0, 3600000, (double)vp.dirty_settle_ms},
            {"pan_blocks", 0, 1, vp.pan_blocks ? 1.0 : 0.0}, {"stall_ms", 1000, 600000, (double)vp.stall_ms}};
        for (auto &e : k)
            if (!num(j, e.key, e.lo, e.hi, &e.cur, &err)) {
                error(c, "layer", ref, err);
                return;
            }
        vp.w = (uint32_t)k[0].cur;
        vp.h = (uint32_t)k[1].cur;
        vp.fps = (uint32_t)k[2].cur;
        vp.level_bits = (uint32_t)k[3].cur;
        vp.dirty_threshold = (uint32_t)k[4].cur;
        vp.dirty_settle_ms = (int)k[5].cur;
        vp.pan_blocks = k[6].cur != 0;
        vp.stall_ms = (int)k[7].cur;
        id = comp_->add_video(c.id, target, lp, vp, now_ms, &err);
    } else if (kind == "image") {
        double iw = lp.rect.w, ih = lp.rect.h, fps = 30;
        if (!num(j, "iw", 1, 1920, &iw, &err) || !num(j, "ih", 1, 720, &ih, &err) ||
            !num(j, "fps", 0, 60, &fps, &err)) {
            error(c, "layer", ref, err);
            return;
        }
        const std::string s = str(j, "sampling", "nearest");
        if (s != "nearest" && s != "bilinear") {
            error(c, "layer", ref, "sampling must be nearest or bilinear");
            return;
        }
        id = comp_->add_image(c.id, target, lp, (uint32_t)iw, (uint32_t)ih, (uint32_t)fps,
                              s == "nearest" ? Sampling::Nearest : Sampling::Bilinear, now_ms, &err);
    } else if (kind == "effect") {
        v = 0;
        double clock = 0;
        if (!num(j, "time_fps", 0, 60, &v, &err) || !num(j, "time", 0, 1, &clock, &err)) {
            error(c, "layer", ref, err);
            return;
        }
        if (v && clock) {
            error(c, "layer", ref, "time and time_fps exclude each other");
            return;
        }
        if (str(j, "fs").empty()) {
            error(c, "layer", ref, "fs (the fragment shader) missing");
            return;
        }
        id = comp_->add_effect(c.id, target, lp, str(j, "vs"), str(j, "fs"), (uint32_t)v, now_ms, &err);
        if (id && clock)
            static_cast<EffectLayer *>(comp_->layer(c.id, id))->cluster_time();
    } else if (kind == "mesh") {
        MeshParams mp;
        if (!mesh_params(j, &mp, &err)) {
            error(c, "layer", ref, err);
            return;
        }
        id = comp_->add_mesh(c.id, target, lp, mp, now_ms, &err);
    } else {
        error(c, "layer", ref, "kind must be video, image, effect or mesh");
        return;
    }
    if (!id) {
        error(c, "layer", ref, err);
        return;
    }
    c.refs[ref] = id;
    log_info("client %llu (%s): %s layer %u on %s (%s, z %d)", (unsigned long long)c.id, c.app.c_str(), kind.c_str(),
             id, spec.name.c_str(), klass_name(lp.klass), lp.z);
}

void Server::on_json(Client &c, const std::string &body, int64_t now_ms)
{
    cr::Json j;
    if (!cr::parse_json(body, &j)) {
        error(c, "?", 0, "not a flat JSON object");
        return;
    }
    const std::string op = str(j, "op");
    if (op == "hello") {
        c.app = str(j, "app", "?").substr(0, 64);
        std::string targets;
        for (const TargetSpec &t : slots_->targets())
            if (t.follows.empty())
                targets += (targets.empty() ? "" : ",") + t.name + ":" + std::to_string(t.width) + "x" +
                           std::to_string(t.height);
        send(c, cr::json_object({{"ev", cr::json_quote("hello")},
                                 {"proto", std::to_string(cr::kProtocolVersion)},
                                 {"backend", cr::json_quote(backend_)},
                                 {"targets", cr::json_quote(targets)},
                                 {"repeat", "1"}}));
        c.target_sent.clear();
        log_info("client %llu: %s", (unsigned long long)c.id, c.app.c_str());
        return;
    }
    if (op == "layer") {
        op_layer(c, j, now_ms);
        return;
    }
    if (op == "status") {
        send(c, cr::json_object({{"ev", cr::json_quote("status")}, {"text", cr::json_quote(status_text(now_ms))}}));
        return;
    }
    if (op == "asset") {
        op_asset(c, j);
        return;
    }
    double ref_d = 0;
    std::string err;
    num(j, "ref", 0, 4294967295.0, &ref_d, &err);
    const uint32_t ref = (uint32_t)ref_d;
    auto it = c.refs.find(ref);
    Layer *l = it == c.refs.end() ? nullptr : comp_->layer(c.id, it->second);
    if (op == "repeat") {
        const bool video = l && std::string(l->kind()) == "video";
        if (video)
            static_cast<VideoLayer *>(l)->repeat(now_ms);
        /* Answered at most once per ref, as frames are (on_frame). */
        const std::string why = video ? "" : l ? "only a video layer repeats a frame" : "no such layer";
        std::string &last = c.frame_error[l ? ref : 0];
        if (!why.empty() && why != last)
            error(c, op, ref, why);
        last = why;
        return;
    }
    if (op == "drop") {
        auto a = c.assets.find(ref);
        if (a != c.assets.end()) {
            if (!comp_->remove_asset(c.id, a->second, &err)) {
                error(c, op, ref, err);
                return;
            }
            c.assets.erase(a);
            c.frame_error.erase(ref);
            return;
        }
    }
    static const char *const ops[] = {"set", "uniform", "drop", "bind", "part", "transform", "animate", "stop"};
    if (std::find(std::begin(ops), std::end(ops), op) == std::end(ops)) {
        error(c, op, ref, "unknown op");
        return;
    }
    if (!l) {
        error(c, op, ref, "no such layer");
        return;
    }
    if (op == "drop") {
        comp_->remove(c.id, it->second);
        c.refs.erase(it);
        c.layer_sent.erase(ref);
        c.frame_error.erase(ref);
        return;
    }
    op_on_layer(c, op, ref, l, j);
}

void Server::op_asset(Client &c, const cr::Json &j)
{
    double ref_d = 0, w = 0, h = 0;
    std::string err;
    if (!num(j, "ref", 1, 4294967295.0, &ref_d, &err) || ref_d == 0) {
        error(c, "asset", 0, err.empty() ? "ref missing" : err);
        return;
    }
    const uint32_t ref = (uint32_t)ref_d;
    if (c.refs.count(ref) || c.assets.count(ref)) {
        error(c, "asset", ref, "ref in use");
        return;
    }
    const int target = slots_->index(str(j, "target"));
    if (target < 0 || !slots_->targets()[target].follows.empty()) {
        error(c, "asset", ref, "unknown target " + str(j, "target"));
        return;
    }
    AssetParams ap;
    int format = 0, sampling = 0, wrap = 0;
    if (!num(j, "w", 1, 2048, &w, &err) || !num(j, "h", 1, 2048, &h, &err) ||
        !choice(j, "format", {"rgba8", "r8"}, &format, &err) ||
        !choice(j, "sampling", {"nearest", "bilinear"}, &sampling, &err) ||
        !choice(j, "wrap", {"clamp", "repeat"}, &wrap, &err)) {
        error(c, "asset", ref, err);
        return;
    }
    if (!w || !h) {
        error(c, "asset", ref, "an asset needs w and h");
        return;
    }
    ap.w = (uint32_t)w;
    ap.h = (uint32_t)h;
    ap.format = format ? TexFormat::R8 : TexFormat::RGBA8;
    ap.sampling = sampling ? Sampling::Bilinear : Sampling::Nearest;
    ap.wrap = wrap ? Wrap::Repeat : Wrap::Clamp;
    const uint32_t id = comp_->add_asset(c.id, target, ap, &err);
    if (!id) {
        error(c, "asset", ref, err);
        return;
    }
    c.assets[ref] = id;
    log_info("client %llu (%s): asset %u on %s (%s %ux%u)", (unsigned long long)c.id, c.app.c_str(), id,
             slots_->targets()[target].name.c_str(), format ? "r8" : "rgba8", ap.w, ap.h);
}

void Server::op_on_layer(Client &c, const std::string &op, uint32_t ref, Layer *l, const cr::Json &j)
{
    std::string err;
    double part_d = 0;
    if (!num(j, "part", 0, 4294967295.0, &part_d, &err)) {
        error(c, op, ref, err);
        return;
    }
    const uint32_t part = (uint32_t)part_d;
    const bool mesh = std::string(l->kind()) == "mesh";
    if (op == "set") {
        if (!apply(l, j, &err))
            error(c, op, ref, err);
    } else if (op == "uniform") {
        op_uniform(c, ref, l, j);
    } else if (op == "bind") {
        const std::string sampler = str(j, "sampler");
        double asset_d = 0;
        num(j, "asset", 0, 4294967295.0, &asset_d, &err);
        auto a = c.assets.find((uint32_t)asset_d);
        if (sampler.empty() || sampler.size() > 64)
            error(c, op, ref, "a bind needs a sampler name");
        else if (a == c.assets.end())
            error(c, op, ref, "no such asset");
        else if (!comp_->bind(c.id, l->id(), part, sampler, a->second, &err))
            error(c, op, ref, err);
    } else if (op == "part") {
        PartLook look;
        double depth = 1;
        int cull = 0, blend = 0;
        if (!mesh)
            err = "parts are for mesh layers";
        else if (part == 0)
            err = "parts are numbered from 1";
        else if (str(j, "vs").empty() || str(j, "fs").empty())
            err = "a part needs vs and fs";
        else if (num(j, "depth", 0, 1, &depth, &err) && choice(j, "cull", {"back", "front", "none"}, &cull, &err) &&
                 choice(j, "blend", {"alpha", "add", "none"}, &blend, &err)) {
            look.depth = depth != 0;
            look.cull = cull == 0 ? Cull::Back : cull == 1 ? Cull::Front : Cull::None;
            look.blend = blend == 0 ? Blend::Alpha : blend == 1 ? Blend::Add : Blend::None;
            comp_->add_part(c.id, l->id(), part, str(j, "vs"), str(j, "fs"), look, &err);
        }
        if (!err.empty())
            error(c, op, ref, err);
    } else if (op == "transform") {
        MeshLayer *m = mesh ? static_cast<MeshLayer *>(l) : nullptr;
        if (!m || !m->has_part(part)) {
            error(c, op, ref, m ? "no such part" : "transforms are for mesh parts");
            return;
        }
        Pose pose = m->pose(part);
        if (!pose_fields(j, &pose, &err)) {
            error(c, op, ref, err);
            return;
        }
        m->set_pose(part, pose);
        comp_->touch(l);
    } else if (op == "animate") {
        double anim_d = 0, loop = 0;
        int property = 0, interp = 0;
        Animation a;
        if (!num(j, "anim", 1, 4294967295.0, &anim_d, &err) || !anim_d) {
            error(c, op, ref, err.empty() ? "anim (the animation's number, 1 or more) missing" : err);
            return;
        }
        if (!choice(j, "property", {"rotate", "translate", "scale", "uniform"}, &property, &err) ||
            !num(j, "loop", 0, 1, &loop, &err) || !choice(j, "interp", {"linear", "step"}, &interp, &err) ||
            !parse_keys(str(j, "keys"), &a, &err)) {
            error(c, op, ref, err);
            return;
        }
        if (!j.count("property")) {
            error(c, op, ref, "property must be rotate, translate, scale or uniform");
            return;
        }
        static const Animated props[] = {Animated::Rotate, Animated::Translate, Animated::Scale, Animated::Uniform};
        a.property = props[property];
        a.uniform = str(j, "name");
        a.loop = loop != 0;
        a.step = interp == 1;
        if (a.property == Animated::Uniform && (a.uniform.empty() || a.uniform.size() > 64)) {
            error(c, op, ref, "a uniform's animation needs its name");
            return;
        }
        if (!l->animate((uint32_t)anim_d, part, a, &err))
            error(c, op, ref, err);
        else
            comp_->touch(l);
    } else if (op == "stop") {
        double anim_d = 0;
        num(j, "anim", 0, 4294967295.0, &anim_d, &err);
        if (!anim_d || !l->stop((uint32_t)anim_d))
            error(c, op, ref, "no such animation");
        else
            comp_->touch(l);
    }
}

void Server::op_uniform(Client &c, uint32_t ref, Layer *l, const cr::Json &j)
{
    const std::string op = "uniform";
    const std::string kind = l->kind();
    double part_d = 0;
    std::string err;
    num(j, "part", 0, 4294967295.0, &part_d, &err);
    const uint32_t part = (uint32_t)part_d;
    if (kind != "effect" && kind != "mesh") {
        error(c, op, ref, "uniforms are for effect layers and mesh parts");
        return;
    }
    if (kind == "effect" && part) {
        error(c, op, ref, "effect layers have no parts");
        return;
    }
    MeshLayer *m = kind == "mesh" ? static_cast<MeshLayer *>(l) : nullptr;
    if (m && !m->has_part(part)) {
        error(c, op, ref, "no such part");
        return;
    }
    const std::string name = str(j, "name");
    if (j.count("type")) {
        int type = 0;
        std::vector<float> v;
        if (!choice(j, "type", {"float", "vec2", "vec3", "vec4", "mat4"}, &type, &err)) {
            error(c, op, ref, err);
            return;
        }
        static const UniformType types[] = {UniformType::Float, UniformType::Vec2, UniformType::Vec3,
                                            UniformType::Vec4, UniformType::Mat4};
        const UniformType t = types[type];
        const uint32_t width = uniform_width(t);
        if (name.empty() || name.size() > 64 || !numbers(str(j, "v"), 1024, &v) || v.size() % width) {
            error(c, op, ref, "a uniform needs a name and 1 to 1024 finite numbers in v, whole elements of its type");
            return;
        }
        const uint32_t elements = (uint32_t)(v.size() / width);
        const bool ok = m ? m->set_uniform(part, name, t, v.data(), elements)
                          : static_cast<EffectLayer *>(l)->set_uniform(name, t, v.data(), elements);
        if (!ok)
            error(c, op, ref,
                  "at most " + std::to_string(Uniforms::kMaxNames) + " uniforms and " +
                      std::to_string(Uniforms::kMaxFloats) + " values a quad or part");
        return;
    }
    float vals[4];
    int n = 0;
    const std::string text = str(j, "v");
    const char *p = text.c_str();
    char *end = nullptr;
    bool finite = true;
    while (n < 4) {
        const float f = std::strtof(p, &end);
        if (end == p)
            break;
        finite = finite && std::isfinite(f);
        vals[n++] = f;
        p = end;
    }
    if (n == 0 || !finite || name.empty() || name.size() > 64) {
        error(c, op, ref, "a uniform needs a name and 1 to 4 finite numbers in v");
        return;
    }
    const bool ok = m ? m->set_uniform(part, name, vals, n) : static_cast<EffectLayer *>(l)->set_uniform(name, vals, n);
    if (!ok)
        error(c, op, ref, "at most " + std::to_string(EffectLayer::kMaxUniforms) + " uniforms a layer");
}
void Server::on_frame(Client &c, const char *body, size_t n, int64_t now_ms)
{
    cr::FrameHeader h;
    if (!cr::parse_frame(body, n, &h)) {
        error(c, "frame", 0, "malformed frame");
        return;
    }
    auto it = c.refs.find(h.ref);
    Layer *l = it == c.refs.end() ? nullptr : comp_->layer(c.id, it->second);
    auto ai = c.assets.find(h.ref);
    Asset *asset = ai == c.assets.end() ? nullptr : comp_->asset(c.id, ai->second);
    std::string err;
    if (asset) {
        std::vector<crk::PixelRect> rects;
        for (const cr::FrameRect &r : h.rects)
            rects.push_back(crk::PixelRect{r.x, r.y, r.w, r.h});
        asset->update(rects, h.pixels, h.pixel_bytes, &err);
    } else if (!l) {
        err = "no such layer";
    } else if (std::string(l->kind()) == "video") {
        VideoLayer *v = static_cast<VideoLayer *>(l);
        if (!h.rects.empty() || h.pixel_bytes != v->frame_bytes())
            err = "a video frame is one whole atlas of " + std::to_string(v->frame_bytes()) + " bytes";
        else
            v->offer(h.pixels, now_ms);
    } else if (std::string(l->kind()) == "image") {
        std::vector<crk::PixelRect> rects;
        for (const cr::FrameRect &r : h.rects)
            rects.push_back(crk::PixelRect{r.x, r.y, r.w, r.h});
        static_cast<ImageLayer *>(l)->update(rects, h.pixels, h.pixel_bytes, &err);
    } else {
        err = std::string(l->kind()) + " layers take no frames";
    }
    /* One error line per layer until it changes, so a producer sending bad
     * frames at 30 a second does not flood its own socket; frames for refs
     * it does not have share one line. */
    std::string &last = c.frame_error[l || asset ? h.ref : 0];
    if (!err.empty() && err != last)
        error(c, "frame", h.ref, err);
    last = err;
}

namespace {

void floats_le(const uint8_t *src, size_t n, std::vector<float> *out)
{
    out->resize(n);
    for (size_t i = 0; i < n; i++) {
        const uint8_t *b = src + i * 4;
        const uint32_t u = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
        std::memcpy(&(*out)[i], &u, 4);
    }
}

}  // namespace

void Server::on_mesh(Client &c, const char *body, size_t n, int64_t now_ms)
{
    cr::MeshHeader h;
    std::string err;
    if (!cr::parse_mesh(body, n, &h, &err)) {
        const unsigned char *p = reinterpret_cast<const unsigned char *>(body);
        error(c, "mesh", n >= 4 ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) : 0, err);
        return;
    }
    auto it = c.refs.find(h.ref);
    if (it == c.refs.end()) {
        error(c, "mesh", h.ref, "no such layer");
        return;
    }
    MeshData d;
    d.vertices = h.vertices;
    floats_le(h.position, (size_t)h.vertices * 3, &d.position);
    if (h.normal)
        floats_le(h.normal, (size_t)h.vertices * 3, &d.normal);
    if (h.texcoord)
        floats_le(h.texcoord, (size_t)h.vertices * 2, &d.texcoord);
    if (h.color)
        floats_le(h.color, (size_t)h.vertices * 4, &d.color);
    d.index.resize(h.indices);
    for (uint32_t i = 0; i < h.indices; i++)
        d.index[i] = (uint16_t)(h.index[i * 2] | h.index[i * 2 + 1] << 8);
    if (!comp_->geometry(c.id, it->second, h.part, std::move(d), now_ms, &err))
        error(c, "mesh", h.ref, err);
}

void Server::report(int64_t now_ms)
{
    for (Client &c : clients_) {
        if (c.closing || c.app.empty())
            continue;
        for (size_t t = 0; t < slots_->targets().size(); t++) {
            if (!slots_->targets()[t].follows.empty())
                continue;   /* a hold: no client's target */
            const TargetStatus &s = slots_->status((int)t);
            const bool hold_off = slots_->hold_disabled((int)t), off = slots_->disabled((int)t);
            const std::string line = std::string(target_state_name(s.state)) + "\n" + holder_name(s.holder) + "\n" +
                                     s.reason + "\n" + std::to_string(s.retry_ms) + "\n" +
                                     rendering_name(s.rendering) + "\n" + (hold_off ? "1" : "0") + (off ? "1" : "0");
            std::string &sent = c.target_sent[slots_->targets()[t].name];
            if (line == sent)
                continue;
            sent = line;
            send(c, cr::json_object({{"ev", cr::json_quote("target")},
                                     {"target", cr::json_quote(slots_->targets()[t].name)},
                                     {"state", cr::json_quote(target_state_name(s.state))},
                                     {"holder", cr::json_quote(holder_name(s.holder))},
                                     {"reason", cr::json_quote(s.reason)},
                                     {"retry_in_ms", std::to_string(s.retry_ms >= 0 ? std::max<int64_t>(0, s.retry_ms - now_ms)
                                                                                   : -1)},
                                     {"rendering", cr::json_quote(rendering_name(s.rendering))},
                                     {"guide_disabled", hold_off ? "1" : "0"},
                                     {"disabled", off ? "1" : "0"}}));
        }
        const Governor &gov = comp_->governor();
        const std::string budget = std::to_string(gov.cap()) + "\n" + gov.reason();
        if (budget != c.budget_sent) {
            c.budget_sent = budget;
            send(c, cr::json_object({{"ev", cr::json_quote("budget")},
                                     {"cap_kb_per_s", std::to_string(gov.cap() / 1024)},
                                     {"max_kb_per_s", std::to_string(gov.max() / 1024)},
                                     {"floor_kb_per_s", std::to_string(gov.floor() / 1024)},
                                     {"reason", cr::json_quote(gov.reason())}}));
        }
        for (const auto &kv : c.refs) {
            std::string reason;
            const LayerState s = comp_->state(kv.second, &reason);
            const std::string line = state_line(s, reason);
            std::string &sent = c.layer_sent[kv.first];
            if (line == sent)
                continue;
            sent = line;
            send(c, cr::json_object({{"ev", cr::json_quote("layer")},
                                     {"ref", std::to_string(kv.first)},
                                     {"state", cr::json_quote(layer_state_name(s))},
                                     {"reason", cr::json_quote(reason)}}));
        }
    }
    for (size_t i = clients_.size(); i-- > 0;)
        if (clients_[i].closing)
            drop_client(i);
}

std::string Server::status_text(int64_t now_ms) const
{
    std::string s;
    char line[320];
    const Governor &gov = comp_->governor();
    std::snprintf(line, sizeof line, "upload: cap %llu KB/s (ceiling %llu, floor %llu): %s\n",
                  (unsigned long long)(gov.cap() / 1024), (unsigned long long)(gov.max() / 1024),
                  (unsigned long long)(gov.floor() / 1024), gov.reason().c_str());
    s += line;
    for (size_t t = 0; t < slots_->targets().size(); t++) {
        const TargetSpec &spec = slots_->targets()[t];
        const TargetStatus &st = slots_->status((int)t);
        const std::string rendering = slots_->rendering_text((int)t, now_ms);
        std::snprintf(line, sizeof line, "target %s: slot 0x%llx scene 0x%llx %ux%u: %s%s%s%s%s%s%s\n",
                      spec.name.c_str(), (unsigned long long)spec.slot, (unsigned long long)spec.scene_id, spec.width,
                      spec.height, target_state_name(st.state), st.holder != Holder::None ? " (" : "",
                      st.holder != Holder::None ? holder_name(st.holder) : "", st.holder != Holder::None ? ")" : "",
                      st.reason.empty() ? "" : (" " + st.reason).c_str(), rendering.empty() ? "" : "; ",
                      rendering.c_str());
        s += line;
    }
    s += slots_->guard_text();
    s += slots_->renderer_text(now_ms);
    s += slots_->port().status_text(now_ms);
    s += slots_->table_text();
    for (const Client &c : clients_)
        for (const auto &kv : c.assets)
            for (const Asset *a : comp_->assets())
                if (a->id() == kv.second) {
                    std::snprintf(line, sizeof line, "asset %u %s %ux%u of %s on %s: %s\n", a->id(),
                                  a->params().format == TexFormat::R8 ? "r8" : "rgba8", a->params().w, a->params().h,
                                  c.app.c_str(), slots_->targets()[a->target()].name.c_str(), a->stats().c_str());
                    s += line;
                }
    for (const Client &c : clients_)
        for (const auto &kv : c.refs) {
            std::string reason;
            const LayerState st = comp_->state(kv.second, &reason);
            const Layer *l = nullptr;
            for (const Layer *x : comp_->layers())
                if (x->id() == kv.second)
                    l = x;
            if (!l)
                continue;
            std::snprintf(line, sizeof line, "layer %u %s of %s on %s, %s z %d%s: %s%s%s\n", l->id(), l->kind(),
                          c.app.c_str(), slots_->targets()[l->target()].name.c_str(), klass_name(l->params.klass),
                          l->params.z, l->params.guide_hold ? "" : ", guide_hold 0", layer_state_name(st),
                          reason.empty() ? "" : " - ", reason.c_str());
            s += line;
            const std::string stats = l->stats();
            if (!stats.empty())
                s += "  " + stats + "\n";
        }
    return s;
}

}  // namespace crs
