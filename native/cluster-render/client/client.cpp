/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Robert Nisipeanu */
#include "cluster_render/client.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace cr {

namespace {

#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;   /* the app ignores SIGPIPE */
#endif

std::string num(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

std::string flag(bool b) { return b ? "1" : "0"; }

/* After each failed connection: 0.25, 0.5, 1, then every 2 s. */
const int64_t kBackoff[] = {250, 500, 1000, 2000};

}  // namespace

Client::Client(const std::string &app, const std::string &socket_path) : app_(app), path_(socket_path)
{
}

Client::~Client()
{
    if (fd_ >= 0)
        close(fd_);
}

void Client::connect_now(int64_t now_ms)
{
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path_.c_str());
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    /* Room for a whole video frame in flight (the kernel caps it). */
    int buf = 1 << 20;
    if (fd_ >= 0)
        setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    if (fd_ < 0 || ::connect(fd_, (struct sockaddr *)&addr, sizeof addr) < 0) {
        disconnect(now_ms);
        return;
    }
    fcntl(fd_, F_SETFD, FD_CLOEXEC);
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    queue(json_object({{"op", json_quote("hello")}, {"app", json_quote(app_)}, {"proto", num(kProtocolVersion)}}));
    /* Assets first, so binds find them; binds and animations once every
     * layer and part is made. */
    for (int pass = 0; pass < 2; pass++)
        for (auto &kv : layers_) {
            Layer &l = kv.second;
            if ((l.kind == "asset") != (pass == 0))
                continue;
            queue(layer_json(kv.first, l));
            if (!l.edits.empty())
                queue(edits_json(kv.first, l));
            for (const auto &h : l.hosts)
                queue(h.second);
            for (const auto &p : l.parts) {
                queue(p.second.json);
                if (!p.second.geometry.empty())
                    queue_message(p.second.geometry);
                if (!p.second.transform.empty())
                    queue(p.second.transform);
            }
            for (const auto &u : l.uniforms)
                queue(u.second);
            if ((l.kind == "image" || l.kind == "asset") && l.have) {
                l.whole = true;   /* sent once the service says hello */
                l.dirty.clear();
                l.dirty_bytes = 0;
            }
        }
    for (const auto &kv : layers_)
        for (const auto &b : kv.second.binds)
            queue(b.second.second);
    for (const auto &kv : layers_)
        for (const auto &a : kv.second.animations)
            queue(a.second);
    write_out();
}

void Client::disconnect(int64_t now_ms)
{
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
    ready_ = false;
    in_.clear();
    hosts.clear();
    /* The newest frame each video layer was given stays its last: the one
     * on its way, then any still waiting. */
    if (!cur_px_.empty())
        keep_last(last_frame_ref_, &cur_px_);
    for (auto &f : frames_)
        keep_last(f.first, &f.second.px);
    for (auto &kv : layers_)
        kv.second.sent = false;
    repeats_ = false;
    cur_.clear();
    cur_px_.clear();
    cur_off_ = 0;
    ctl_.clear();
    frames_.clear();
    repeats_due_.clear();
    retry_ms_ = now_ms + kBackoff[backoff_ < 3 ? backoff_ : 3];
    if (backoff_ < 3)
        backoff_++;
}

bool Client::wants_write() const
{
    if (fd_ < 0)
        return false;
    if (!cur_.empty() || !ctl_.empty() || !frames_.empty() || !repeats_due_.empty())
        return true;
    for (const auto &kv : layers_)
        if ((kv.second.kind == "image" || kv.second.kind == "asset") && (kv.second.whole || !kv.second.dirty.empty()))
            return ready_;
    return false;
}

void Client::poll(int64_t now_ms)
{
    if (fd_ < 0) {
        if (now_ms >= retry_ms_)
            connect_now(now_ms);
        return;
    }
    read_events();
    if (fd_ >= 0)
        write_out();
    if (fd_ < 0)
        retry_ms_ = now_ms + kBackoff[backoff_ < 3 ? backoff_ : 3];
}

void Client::read_events()
{
    char buf[16384];
    for (;;) {
        ssize_t r = read(fd_, buf, sizeof buf);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            disconnect(0);
            return;
        }
        if (r < 0)
            break;
        in_.append(buf, (size_t)r);
    }
    size_t off = 0;
    for (;;) {
        size_t used = 0, n = 0;
        uint8_t type = 0;
        const char *body = nullptr;
        bool bad = false;
        if (!next_message(in_.data() + off, in_.size() - off, &used, &type, &body, &n, &bad)) {
            if (bad) {
                disconnect(0);
                return;
            }
            break;
        }
        off += used;
        Json j;
        if (type != kJson || !parse_json(std::string(body, n), &j))
            continue;
        const std::string ev = j["ev"];
        if (ev == "hello") {
            backend = j["backend"];
            targets = j["targets"];
            repeats_ = j["repeat"] == "1";
            ready_ = true;
            backoff_ = 0;
            if (on_ready)
                on_ready();
        } else if (ev == "layer" && on_layer) {
            on_layer((uint32_t)std::strtoul(j["ref"].c_str(), nullptr, 10), j["state"], j["reason"]);
        } else if (ev == "target") {
            guide_disabled[j["target"]] = j["guide_disabled"] == "1";
            disabled[j["target"]] = j["disabled"] == "1";
            if (on_target)
                on_target(j["target"], j["state"], j["holder"], j["reason"]);
        } else if (ev == "error") {
            const uint32_t ref = (uint32_t)std::strtoul(j["ref"].c_str(), nullptr, 10);
            /* A layer the service refused or lost: its next frame goes whole,
             * which the service answers at most once. */
            auto l = layers_.find(ref);
            if (l != layers_.end())
                l->second.sent = false;
            /* a host refused is not sent again after a reconnect */
            if (l != layers_.end() && j["op"] == "host")
                l->second.hosts.erase(j["target"]);
            if (on_error)
                on_error(j["op"], ref, j["reason"]);
        } else if (ev == "budget" && on_budget) {
            on_budget((unsigned)std::strtoul(j["cap_kb_per_s"].c_str(), nullptr, 10),
                      (unsigned)std::strtoul(j["max_kb_per_s"].c_str(), nullptr, 10), j["reason"]);
        } else if (ev == "status" && on_status) {
            on_status(j["text"]);
        } else if (ev == "host") {
            Host h;
            h.target = j["target"];
            h.into = j["into"];
            h.shown = j["shown"] == "1";
            int *ints[] = {&h.x, &h.y, &h.src_x, &h.src_y};
            unsigned *sizes[] = {&h.w, &h.h, &h.src_w, &h.src_h};
            const char *const ik[] = {"x", "y", "src_x", "src_y"}, *const sk[] = {"w", "h", "src_w", "src_h"};
            for (int i = 0; i < 4; i++) {
                *ints[i] = (int)std::strtol(j[ik[i]].c_str(), nullptr, 10);
                *sizes[i] = (unsigned)std::strtoul(j[sk[i]].c_str(), nullptr, 10);
            }
            if (h.into.empty())
                hosts.erase(h.target);
            else
                hosts[h.target] = h;
            if (on_host)
                on_host(h);
        }
        if (fd_ < 0)
            return;
    }
    in_.erase(0, off);
}

void Client::write_out()
{
    for (;;) {
        if (cur_off_ >= cur_.size() + cur_px_.size()) {
            cur_.clear();
            cur_off_ = 0;
            if (!cur_px_.empty()) {
                auto l = layers_.find(last_frame_ref_);
                if (l != layers_.end())
                    l->second.sent = true;
                keep_last(last_frame_ref_, &cur_px_);
            }
            if (!repeats_due_.empty()) {
                cur_ = encode(kJson, json_object({{"op", json_quote("repeat")}, {"ref", num(*repeats_due_.begin())}}));
                repeats_due_.erase(repeats_due_.begin());
            } else if (!ctl_.empty()) {
                cur_.swap(ctl_.front());
                ctl_.pop_front();
            } else if (ready_ && next_image(&cur_)) {
                /* an image layer's changes */
            } else if (!frames_.empty()) {
                /* Round robin over the layers with a frame waiting. */
                auto it = frames_.upper_bound(last_frame_ref_);
                if (it == frames_.end())
                    it = frames_.begin();
                last_frame_ref_ = it->first;
                cur_.swap(it->second.head);
                cur_px_.swap(it->second.px);
                frames_.erase(it);
            } else {
                return;
            }
        }
        struct iovec iov[2];
        struct msghdr mh;
        std::memset(&mh, 0, sizeof mh);
        mh.msg_iov = iov;
        if (cur_off_ < cur_.size()) {
            iov[mh.msg_iovlen].iov_base = const_cast<char *>(cur_.data() + cur_off_);
            iov[mh.msg_iovlen++].iov_len = cur_.size() - cur_off_;
        }
        const size_t px_off = cur_off_ > cur_.size() ? cur_off_ - cur_.size() : 0;
        if (px_off < cur_px_.size()) {
            iov[mh.msg_iovlen].iov_base = cur_px_.data() + px_off;
            iov[mh.msg_iovlen++].iov_len = cur_px_.size() - px_off;
        }
        ssize_t w = ::sendmsg(fd_, &mh, kSendFlags);
        if (w < 0) {
            if (errno != EAGAIN && errno != EINTR)
                disconnect(0);
            return;
        }
        cur_off_ += (size_t)w;
    }
}

void Client::queue(const std::string &json)
{
    if (fd_ < 0)
        return;
    ctl_.push_back(encode(kJson, json));
}

void Client::queue_message(const std::string &msg)
{
    if (fd_ >= 0)
        ctl_.push_back(msg);
}

void Client::send_now()
{
    if (fd_ >= 0)
        write_out();
}

void Client::turn_fields(std::vector<std::pair<std::string, std::string>> *kv, const LayerOptions &lo) const
{
    kv->push_back({"rotate", num(lo.rotate)});
    kv->push_back({"sx", num(lo.sx)});
    kv->push_back({"sy", num(lo.sy)});
    if (lo.pivot) {
        kv->push_back({"px", num(lo.px)});
        kv->push_back({"py", num(lo.py)});
    }
}

void Client::mesh_fields(std::vector<std::pair<std::string, std::string>> *kv, const MeshOptions &mo) const
{
    const std::pair<const char *, float> f[] = {{"fov", mo.fov}, {"ortho_h", mo.ortho_h}, {"near", mo.near},
                                                {"far", mo.far},  {"cx", mo.cx},           {"cy", mo.cy},
                                                {"cz", mo.cz},    {"crx", mo.crx},         {"cry", mo.cry},
                                                {"crz", mo.crz}};
    kv->push_back({"projection", json_quote(mo.ortho ? "ortho" : "perspective")});
    for (const auto &p : f)
        kv->push_back({p.first, num(p.second)});
}

std::vector<std::pair<std::string, std::string>> Client::fields(const char *op, uint32_t ref,
                                                                const LayerOptions &lo) const
{
    std::vector<std::pair<std::string, std::string>> kv = {
        {"op", json_quote(op)},          {"ref", num(ref)},
        {"class", json_quote(lo.klass)}, {"z", num(lo.z)},
        {"x", num(lo.x)},                {"y", num(lo.y)},
        {"visible", flag(lo.visible)},   {"exclusive", flag(lo.exclusive)},
        {"prewarm", flag(lo.prewarm)},   {"contend", flag(lo.contend)},
        {"guide_hold", flag(lo.guide_hold)}, {"yield", flag(lo.yield)},
        {"opacity", num(lo.opacity)}};
    if (lo.w && lo.h) {
        kv.push_back({"w", num(lo.w)});
        kv.push_back({"h", num(lo.h)});
    }
    return kv;
}

std::string Client::edits_json(uint32_t ref, const Layer &l) const
{
    std::vector<std::pair<uint64_t, std::string>> lines;
    for (const auto &kv : l.edits)
        lines.push_back(kv.second);
    std::sort(lines.begin(), lines.end());
    std::string text;
    for (const auto &line : lines)
        text += (text.empty() ? "" : "\n") + line.second;
    return json_object({{"op", json_quote("edit")}, {"ref", num(ref)}, {"lines", json_quote(text)}});
}

std::string Client::layer_json(uint32_t ref, const Layer &l) const
{
    if (l.kind == "scene") {
        std::string res, sha;
        for (const std::string &r : l.so.res)
            res += (res.empty() ? "" : " ") + r;
        for (const std::string &h : l.so.sha256)
            sha += (sha.empty() ? "" : " ") + h;
        std::vector<std::pair<std::string, std::string>> kv = {{"op", json_quote("scene")},
                                                               {"ref", num(ref)},
                                                               {"target", json_quote(l.so.target)},
                                                               {"file", json_quote(l.so.file)},
                                                               {"res", json_quote(res)},
                                                               {"sha256", json_quote(sha)},
                                                               {"contend", flag(l.so.contend)}};
        if (l.so.layers)
            kv.push_back({"layers", "1"});
        if (l.so.layers && l.so.layers_order_set)
            kv.push_back({"layers_order", num(l.so.layers_order)});
        return json_object(kv);
    }
    if (l.kind == "asset")
        return json_object({{"op", json_quote("asset")},
                            {"ref", num(ref)},
                            {"target", json_quote(l.ao.target)},
                            {"format", json_quote(l.ao.r8 ? "r8" : "rgba8")},
                            {"w", num(l.ao.w)},
                            {"h", num(l.ao.h)},
                            {"sampling", json_quote(l.ao.bilinear ? "bilinear" : "nearest")},
                            {"wrap", json_quote(l.ao.repeat ? "repeat" : "clamp")}});
    std::vector<std::pair<std::string, std::string>> kv = fields("layer", ref, l.lo);
    kv.push_back({"target", json_quote(l.lo.target)});
    kv.push_back({"kind", json_quote(l.kind)});
    if (l.kind == "video") {
        turn_fields(&kv, l.lo);
        kv.push_back({"vw", num(l.vo.w)});
        kv.push_back({"vh", num(l.vo.h)});
        kv.push_back({"fps", num(l.vo.fps)});
        kv.push_back({"level_bits", num(l.vo.level_bits)});
        kv.push_back({"dirty_threshold", num(l.vo.dirty_threshold)});
        kv.push_back({"settle_ms", num(l.vo.settle_ms)});
        kv.push_back({"pan_blocks", flag(l.vo.pan_blocks)});
        kv.push_back({"stall_ms", num(l.vo.stall_ms)});
    } else if (l.kind == "image") {
        kv.push_back({"iw", num(l.io.w)});
        kv.push_back({"ih", num(l.io.h)});
        kv.push_back({"fps", num(l.io.fps)});
        kv.push_back({"sampling", json_quote(l.io.bilinear ? "bilinear" : "nearest")});
        turn_fields(&kv, l.lo);
    } else if (l.kind == "mesh") {
        mesh_fields(&kv, l.mo);
    } else {
        kv.push_back({"fs", json_quote(l.eo.fs)});
        if (!l.eo.vs.empty())
            kv.push_back({"vs", json_quote(l.eo.vs)});
        kv.push_back({"time_fps", num(l.eo.time_fps)});
        if (l.cluster_time)
            kv.push_back({"time", "1"});
        turn_fields(&kv, l.lo);
    }
    return json_object(kv);
}

std::string Client::uniform_json(uint32_t ref, const std::string &name, const std::vector<float> &v) const
{
    std::string vals;
    for (float f : v)
        vals += (vals.empty() ? "" : " ") + num(f);
    return json_object({{"op", json_quote("uniform")}, {"ref", num(ref)}, {"name", json_quote(name)},
                        {"v", json_quote(vals)}});
}

uint32_t Client::add(const Layer &l)
{
    const uint32_t ref = next_ref_++;
    layers_[ref] = l;
    queue(layer_json(ref, l));
    if (fd_ >= 0)
        write_out();
    return ref;
}

uint32_t Client::video(const LayerOptions &lo, const VideoOptions &vo)
{
    Layer l;
    l.kind = "video";
    l.lo = lo;
    l.vo = vo;
    return add(l);
}

uint32_t Client::image(const LayerOptions &lo, const ImageOptions &io)
{
    Layer l;
    l.kind = "image";
    l.lo = lo;
    l.io = io;
    if (!l.io.w || !l.io.h) {
        l.io.w = lo.w;
        l.io.h = lo.h;
    }
    if (!l.io.w || !l.io.h || l.io.w > 1920 || l.io.h > 720)
        return 0;
    l.w = l.io.w;
    l.h = l.io.h;
    l.image.assign((size_t)l.io.w * l.io.h * 4, 0);
    return add(l);
}

uint32_t Client::effect(const LayerOptions &lo, const EffectOptions &eo)
{
    return effect(lo, eo, false);
}

uint32_t Client::effect(const LayerOptions &lo, const EffectOptions &eo, bool cluster_time)
{
    Layer l;
    l.kind = "effect";
    l.lo = lo;
    l.eo = eo;
    l.cluster_time = cluster_time;
    return add(l);
}

uint32_t Client::mesh(const LayerOptions &lo, const MeshOptions &mo)
{
    Layer l;
    l.kind = "mesh";
    l.lo = lo;
    l.mo = mo;
    return add(l);
}

uint32_t Client::asset(const AssetOptions &ao)
{
    Layer l;
    l.kind = "asset";
    l.ao = ao;
    l.w = ao.w;
    l.h = ao.h;
    l.bpp = ao.r8 ? 1 : 4;
    if (!l.w || !l.h || l.w > 2048 || l.h > 2048 || (size_t)l.w * l.h * l.bpp + 64 > kMaxMessage)
        return 0;
    l.image.assign((size_t)l.w * l.h * l.bpp, 0);
    return add(l);
}

uint32_t Client::scene(const SceneOptions &so)
{
    if (so.file.empty() || so.sha256.size() != so.res.size() + 1)
        return 0;
    Layer l;
    l.kind = "scene";
    l.so = so;
    return add(l);
}

void Client::scene(uint32_t ref, const SceneOptions &so)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || it->second.kind != "scene" || so.file.empty() || so.sha256.size() != so.res.size() + 1)
        return;
    const std::string target = it->second.so.target;   /* set when it is made */
    it->second.so = so;
    it->second.so.target = target;
    it->second.edits.clear();
    it->second.hosts.clear();   /* the service ends them with the file it replaces */
    queue(layer_json(ref, it->second));
    send_now();
}

bool Client::host(uint32_t ref, const Host &h)
{
    auto it = layers_.find(ref);
    /* what the service refuses whatever it holds is not sent or kept */
    if (it == layers_.end() || it->second.kind != "scene" || !it->second.so.layers || h.target.empty() ||
        h.target == it->second.so.target)
        return false;
    std::vector<std::pair<std::string, std::string>> kv = {
        {"op", json_quote("host")}, {"ref", num(ref)},     {"target", json_quote(h.target)}, {"x", num(h.x)},
        {"y", num(h.y)},            {"w", num(h.w)},       {"h", num(h.h)},                  {"shown", flag(h.shown)}};
    if (h.src_w && h.src_h)
        kv.insert(kv.end(), {{"src_x", num(h.src_x)}, {"src_y", num(h.src_y)}, {"src_w", num(h.src_w)},
                             {"src_h", num(h.src_h)}});
    const std::string json = json_object(kv);
    it->second.hosts[h.target] = json;
    queue(json);
    send_now();
    return true;
}

void Client::edit(uint32_t ref, const std::string &lines)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || it->second.kind != "scene")
        return;
    size_t at = 0;
    while (at <= lines.size()) {
        size_t nl = lines.find('\n', at);
        if (nl == std::string::npos)
            nl = lines.size();
        std::string line = lines.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            it->second.edits[edit_key(line)] = std::make_pair(++edits_, line);
    }
    queue(json_object({{"op", json_quote("edit")}, {"ref", num(ref)}, {"lines", json_quote(lines)}}));
    send_now();
}

void Client::set(uint32_t ref, const LayerOptions &lo)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || it->second.kind == "asset" || it->second.kind == "scene")
        return;
    const std::string target = it->second.lo.target;
    it->second.lo = lo;
    it->second.lo.target = target;
    std::vector<std::pair<std::string, std::string>> kv = fields("set", ref, it->second.lo);
    if (it->second.kind != "mesh")
        turn_fields(&kv, it->second.lo);
    queue(json_object(kv));
    send_now();
}

void Client::camera(uint32_t ref, const MeshOptions &mo)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || it->second.kind != "mesh")
        return;
    it->second.mo.fov = mo.fov;
    it->second.mo.ortho_h = mo.ortho_h;
    it->second.mo.near = mo.near;
    it->second.mo.far = mo.far;
    it->second.mo.cx = mo.cx;
    it->second.mo.cy = mo.cy;
    it->second.mo.cz = mo.cz;
    it->second.mo.crx = mo.crx;
    it->second.mo.cry = mo.cry;
    it->second.mo.crz = mo.crz;
    std::vector<std::pair<std::string, std::string>> kv = {{"op", json_quote("set")}, {"ref", num(ref)}};
    mesh_fields(&kv, it->second.mo);
    queue(json_object(kv));
    send_now();
}

void Client::uniform(uint32_t ref, const std::string &name, const std::vector<float> &v)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || v.empty() || v.size() > 4)
        return;
    std::string &m = it->second.uniforms[std::make_pair(0u, name)];
    m = uniform_json(ref, name, v);
    queue(m);
    send_now();
}

void Client::uniform(uint32_t ref, const std::string &name, UniformType type, const std::vector<float> &v,
                     uint32_t part)
{
    static const char *const names[] = {"float", "vec2", "vec3", "vec4", "mat4"};
    static const size_t widths[] = {1, 2, 3, 4, 16};
    const int t = (int)type;
    auto it = layers_.find(ref);
    if (it == layers_.end() || v.empty() || v.size() > 1024 || v.size() % widths[t])
        return;
    std::string vals;
    for (float f : v)
        vals += (vals.empty() ? "" : " ") + num(f);
    std::vector<std::pair<std::string, std::string>> kv = {{"op", json_quote("uniform")}, {"ref", num(ref)},
                                                           {"name", json_quote(name)},   {"type", json_quote(names[t])},
                                                           {"v", json_quote(vals)}};
    if (part)
        kv.push_back({"part", num(part)});
    std::string &m = it->second.uniforms[std::make_pair(part, name)];
    m = json_object(kv);
    queue(m);
    send_now();
}

void Client::bind(uint32_t ref, const std::string &sampler, uint32_t asset, uint32_t part)
{
    auto it = layers_.find(ref);
    auto a = layers_.find(asset);
    if (it == layers_.end() || a == layers_.end() || a->second.kind != "asset")
        return;
    std::vector<std::pair<std::string, std::string>> kv = {{"op", json_quote("bind")}, {"ref", num(ref)},
                                                           {"sampler", json_quote(sampler)}, {"asset", num(asset)}};
    if (part)
        kv.push_back({"part", num(part)});
    auto &b = it->second.binds[std::make_pair(part, sampler)];
    b = std::make_pair(asset, json_object(kv));
    queue(b.second);
    send_now();
}

bool Client::part(uint32_t ref, uint32_t part, const PartOptions &po)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || it->second.kind != "mesh" || !part || it->second.parts.count(part))
        return false;
    Part &p = it->second.parts[part];
    p.json = json_object({{"op", json_quote("part")},
                          {"ref", num(ref)},
                          {"part", num(part)},
                          {"vs", json_quote(po.vs)},
                          {"fs", json_quote(po.fs)},
                          {"depth", flag(po.depth)},
                          {"cull", json_quote(po.cull)},
                          {"blend", json_quote(po.blend)}});
    queue(p.json);
    send_now();
    return true;
}

bool Client::geometry(uint32_t ref, uint32_t part, const Geometry &g)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || !it->second.parts.count(part))
        return false;
    const size_t v = g.positions.size() / 3;
    if (g.positions.size() % 3 || v < 1 || v > kMaxMeshVertices || (!g.normals.empty() && g.normals.size() != v * 3) ||
        (!g.texcoords.empty() && g.texcoords.size() != v * 2) || (!g.colors.empty() && g.colors.size() != v * 4) ||
        g.indices.size() < 3 || g.indices.size() % 3 || g.indices.size() > kMaxMeshIndices)
        return false;
    for (uint16_t i : g.indices)
        if (i >= v)
            return false;
    std::string &m = it->second.parts[part].geometry;
    m = mesh_message(ref, part, (uint32_t)v, g.positions.data(), g.normals.empty() ? nullptr : g.normals.data(),
                     g.texcoords.empty() ? nullptr : g.texcoords.data(), g.colors.empty() ? nullptr : g.colors.data(),
                     (uint32_t)g.indices.size(), g.indices.data());
    queue_message(m);
    send_now();
    return true;
}

void Client::transform(uint32_t ref, uint32_t part, const Transform &t)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || !it->second.parts.count(part))
        return;
    std::string &m = it->second.parts[part].transform;
    m = json_object({{"op", json_quote("transform")},
                     {"ref", num(ref)},
                     {"part", num(part)},
                     {"tx", num(t.tx)},
                     {"ty", num(t.ty)},
                     {"tz", num(t.tz)},
                     {"rx", num(t.rx)},
                     {"ry", num(t.ry)},
                     {"rz", num(t.rz)},
                     {"sx", num(t.sx)},
                     {"sy", num(t.sy)},
                     {"sz", num(t.sz)}});
    queue(m);
    send_now();
}

uint32_t Client::animate(uint32_t ref, const AnimationOptions &a)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || a.property.empty() || a.keys.empty())
        return 0;
    const uint32_t anim = it->second.next_anim++;
    std::vector<std::pair<std::string, std::string>> kv = {
        {"op", json_quote("animate")},       {"ref", num(ref)},
        {"anim", num(anim)},                 {"property", json_quote(a.property)},
        {"keys", json_quote(a.keys)},        {"loop", flag(a.loop)},
        {"interp", json_quote(a.step ? "step" : "linear")}};
    if (a.part)
        kv.push_back({"part", num(a.part)});
    if (!a.name.empty())
        kv.push_back({"name", json_quote(a.name)});
    std::string &m = it->second.animations[anim];
    m = json_object(kv);
    queue(m);
    send_now();
    return anim;
}

void Client::stop(uint32_t ref, uint32_t anim)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || !it->second.animations.erase(anim))
        return;
    queue(json_object({{"op", json_quote("stop")}, {"ref", num(ref)}, {"anim", num(anim)}}));
    send_now();
}

void Client::request_status()
{
    queue(json_object({{"op", json_quote("status")}}));
    if (fd_ >= 0)
        write_out();
}

void Client::drop(uint32_t ref)
{
    auto it = layers_.find(ref);
    if (it == layers_.end())
        return;
    bool sampled = false;
    for (const auto &kv : layers_)
        for (const auto &b : kv.second.binds)
            sampled = sampled || b.second.first == ref;
    if (it->second.kind == "video")
        spare_.clear();   /* buffers of its size may be needed no more */
    if (!sampled)
        layers_.erase(it);
    frames_.erase(ref);
    repeats_due_.erase(ref);
    queue(json_object({{"op", json_quote("drop")}, {"ref", num(ref)}}));
    send_now();
}

size_t Client::frame_bytes(uint32_t ref) const
{
    auto it = layers_.find(ref);
    if (it == layers_.end())
        return 0;
    if (it->second.kind == "video")
        return (size_t)it->second.vo.w * (it->second.vo.h + it->second.vo.h / 2);
    if (it->second.kind == "image" || it->second.kind == "asset")
        return it->second.image.size();
    return 0;
}

bool Client::video_frame_ok(uint32_t ref, size_t n) const
{
    auto it = layers_.find(ref);
    return ready_ && it != layers_.end() && it->second.kind == "video" && n == frame_bytes(ref);
}

void Client::take_spare(std::vector<uint8_t> *v, size_t n)
{
    for (size_t i = spare_.size(); i-- > 0;) {
        if (spare_[i].size() == n) {
            v->swap(spare_[i]);
            spare_.erase(spare_.begin() + (long)i);
            return;
        }
    }
    v->resize(n);
}

void Client::keep_last(uint32_t ref, std::vector<uint8_t> *px)
{
    auto l = layers_.find(ref);
    if (l != layers_.end() && l->second.kind == "video")
        px->swap(l->second.last);
    if (!px->empty()) {
        if (spare_.size() >= kMaxSpare)
            spare_.erase(spare_.begin());
        spare_.push_back(std::move(*px));
    }
    px->clear();
}

bool Client::frame(uint32_t ref, const uint8_t *atlas, size_t n, uint64_t id)
{
    if (!video_frame_ok(ref, n))
        return false;
    repeats_due_.erase(ref);
    Frame &f = frames_[ref];
    f.head = frame_head(ref, id, {}, n);
    if (f.px.size() != n)
        take_spare(&f.px, n);
    std::memcpy(f.px.data(), atlas, n);
    write_out();
    return true;
}

bool Client::frame(uint32_t ref, std::vector<uint8_t> *atlas, uint64_t id)
{
    if (!atlas || !video_frame_ok(ref, atlas->size()))
        return false;
    const size_t n = atlas->size();
    repeats_due_.erase(ref);
    Frame &f = frames_[ref];
    f.head = frame_head(ref, id, {}, n);
    f.px.swap(*atlas);
    if (atlas->size() != n)
        take_spare(atlas, n);   /* else it is the unsent frame this one replaces */
    write_out();
    return true;
}

bool Client::repeat(uint32_t ref, uint64_t id)
{
    const size_t n = frame_bytes(ref);
    if (!video_frame_ok(ref, n))
        return false;
    Layer &l = layers_[ref];
    auto f = frames_.find(ref);
    if (f != frames_.end()) {
        f->second.head = frame_head(ref, id, {}, n);   /* the frame waiting is this picture */
        return true;
    }
    const bool on_its_way = !cur_px_.empty() && last_frame_ref_ == ref;
    if (repeats_ && (l.sent || on_its_way)) {
        repeats_due_.insert(ref);
        write_out();
        return true;
    }
    if (on_its_way)
        return true;
    if (l.last.size() != n)
        return false;
    repeats_due_.erase(ref);
    Frame &w = frames_[ref];
    w.head = frame_head(ref, id, {}, n);
    w.px.swap(l.last);
    write_out();
    return true;
}

bool Client::latest(uint32_t ref, std::vector<uint8_t> *out) const
{
    auto l = layers_.find(ref);
    if (l == layers_.end() || l->second.kind != "video")
        return false;
    auto f = frames_.find(ref);
    const std::vector<uint8_t> &px = f != frames_.end()                            ? f->second.px
                                     : !cur_px_.empty() && last_frame_ref_ == ref ? cur_px_
                                                                                   : l->second.last;
    if (px.empty())
        return false;
    *out = px;
    return true;
}

bool Client::pixels(uint32_t ref, const std::vector<FrameRect> &rects, const uint8_t *px, size_t n)
{
    auto it = layers_.find(ref);
    if (it == layers_.end() || (it->second.kind != "image" && it->second.kind != "asset"))
        return false;
    Layer &l = it->second;
    const unsigned w = l.w, h = l.h, bpp = l.bpp;
    if (rects.empty()) {
        if (n != l.image.size())
            return false;
        std::memcpy(l.image.data(), px, n);
        l.whole = true;
        l.dirty.clear();
        l.dirty_bytes = 0;
    } else {
        size_t need = 0;
        for (const FrameRect &r : rects) {
            if (!r.w || !r.h || (unsigned)r.x + r.w > w || (unsigned)r.y + r.h > h)
                return false;
            need += (size_t)r.w * r.h * bpp;
        }
        if (need != n)
            return false;
        for (const FrameRect &r : rects) {
            for (unsigned y = 0; y < r.h; y++)
                std::memcpy(&l.image[((size_t)(r.y + y) * w + r.x) * bpp], px + (size_t)y * r.w * bpp,
                            (size_t)r.w * bpp);
            px += (size_t)r.w * r.h * bpp;
            if (l.whole)
                continue;
            /* An update of rects (8 bytes of header each) never outgrows the
             * whole image, which is within the service's message limit. */
            l.dirty_bytes += (size_t)r.w * r.h * bpp;
            if (l.dirty.size() >= kMaxDirty || l.dirty_bytes + 8 * (l.dirty.size() + 1) >= l.image.size()) {
                l.whole = true;
                l.dirty.clear();
                l.dirty_bytes = 0;
            } else {
                l.dirty.push_back(r);
            }
        }
    }
    l.have = true;
    if (fd_ >= 0)
        write_out();
    return true;
}

/* The changed part of one image layer or asset, as one frame message;
 * these go before video frames. */
bool Client::next_image(std::string *msg)
{
    for (auto &kv : layers_) {
        Layer &l = kv.second;
        if ((l.kind != "image" && l.kind != "asset") || (!l.whole && l.dirty.empty()))
            continue;
        std::vector<FrameRect> rects = l.whole ? std::vector<FrameRect>() : l.dirty;
        size_t n = 0;
        if (l.whole) {
            n = l.image.size();
        } else {
            for (const FrameRect &r : rects)
                n += (size_t)r.w * r.h * l.bpp;
        }
        *msg = frame_head(kv.first, 0, rects, n);
        if (l.whole) {
            msg->append(reinterpret_cast<const char *>(l.image.data()), n);
        } else {
            for (const FrameRect &r : rects)
                for (unsigned y = 0; y < r.h; y++)
                    msg->append(reinterpret_cast<const char *>(&l.image[((size_t)(r.y + y) * l.w + r.x) * l.bpp]),
                                (size_t)r.w * l.bpp);
        }
        l.whole = false;
        l.dirty.clear();
        l.dirty_bytes = 0;
        return true;
    }
    return false;
}

}  // namespace cr
