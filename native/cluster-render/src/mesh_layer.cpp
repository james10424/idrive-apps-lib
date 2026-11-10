/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "mesh_layer.h"

#include <cstdio>

#include "log.h"

namespace crs {

unsigned MeshData::attributes() const
{
    return kAttrPosition | (normal.empty() ? 0 : kAttrNormal) | (texcoord.empty() ? 0 : kAttrTexcoord) |
           (color.empty() ? 0 : kAttrColor);
}

size_t MeshData::bytes() const
{
    return (position.size() + normal.size() + texcoord.size() + color.size()) * 4 + index.size() * 2;
}

Geometry MeshData::geometry() const
{
    Geometry g;
    g.vertices = vertices;
    g.indices = (uint32_t)index.size();
    g.position = position.data();
    g.normal = normal.empty() ? nullptr : normal.data();
    g.texcoord = texcoord.empty() ? nullptr : texcoord.data();
    g.color = color.empty() ? nullptr : color.data();
    g.index = index.data();
    return g;
}

MeshLayer::MeshLayer(uint32_t id, int target, uint64_t seq, crk::UploadPacer &pacer, const MeshParams &mp)
    : Layer(id, target, seq, pacer), mesh(mp)
{
}

bool MeshLayer::create(uint32_t number, Part &p)
{
    p.handle = g_->part(view_, p.effect, p.look);
    if (!p.handle || !g_->pose(p.handle, p.pose)) {
        log_error("layer %u: part %u refused", id_, number);
        return false;
    }
    p.uniforms.dirty_all();
    p.upload = p.have;
    return true;
}

bool MeshLayer::add_part(uint32_t part, int effect, unsigned attributes, const PartLook &look, std::string *err)
{
    if (parts_.count(part)) {
        *err = "part " + std::to_string(part) + " exists";
        return false;
    }
    if (parts_.size() >= kMaxParts) {
        *err = "at most " + std::to_string(kMaxParts) + " parts a mesh layer";
        return false;
    }
    Part &p = parts_[part];
    p.effect = effect;
    p.attributes = attributes;
    p.look = look;
    if (g_ && !create(part, p)) {
        if (p.handle)
            g_->drop_part(p.handle);
        parts_.erase(part);
        *err = "the renderer refused the part";
        return false;
    }
    return true;
}

size_t MeshLayer::part_bytes(uint32_t part) const
{
    auto it = parts_.find(part);
    return it == parts_.end() ? 0 : it->second.data.bytes();
}

bool MeshLayer::set_geometry(uint32_t part, MeshData &&data, int64_t now_ms, std::string *err)
{
    auto it = parts_.find(part);
    if (it == parts_.end()) {
        *err = "no such part";
        return false;
    }
    Part &p = it->second;
    static const struct { unsigned bit; const char *name; } attrs[] = {
        {kAttrNormal, "a_normal"}, {kAttrTexcoord, "a_texcoord"}, {kAttrColor, "a_color"}};
    for (const auto &a : attrs)
        if ((p.attributes & a.bit) && !(data.attributes() & a.bit)) {
            *err = std::string("the part's vertex shader reads ") + a.name + ", which the geometry lacks";
            return false;
        }
    if (p.have && now_ms - p.geometry_ms < kGeometryMs) {
        *err = "a part's geometry is replaced at most once a second";
        return false;
    }
    p.data = std::move(data);
    p.have = true;
    p.geometry_ms = now_ms;
    p.upload = g_ != nullptr;
    refusal_.clear();
    return true;
}

bool MeshLayer::set_pose(uint32_t part, const Pose &pose)
{
    auto it = parts_.find(part);
    if (it == parts_.end())
        return false;
    it->second.pose = pose;
    if (g_ && it->second.handle)
        g_->pose(it->second.handle, pose);
    return true;
}

Pose MeshLayer::pose(uint32_t part) const
{
    auto it = parts_.find(part);
    return it == parts_.end() ? Pose() : it->second.pose;
}

bool MeshLayer::set_uniform(uint32_t part, const std::string &name, const float *v, int n)
{
    auto it = parts_.find(part);
    return it != parts_.end() && it->second.uniforms.set(name, v, n);
}

bool MeshLayer::set_uniform(uint32_t part, const std::string &name, UniformType t, const float *v, uint32_t elements)
{
    auto it = parts_.find(part);
    return it != parts_.end() && it->second.uniforms.set(name, t, v, elements);
}

std::vector<int> MeshLayer::effects() const
{
    std::vector<int> out;
    for (const auto &kv : parts_)
        out.push_back(kv.second.effect);
    return out;
}

bool MeshLayer::ready(int64_t) const
{
    for (const auto &kv : parts_)
        if (kv.second.have)
            return true;
    return false;
}

bool MeshLayer::attach(Graph *g, int64_t, std::string *err)
{
    g_ = g;
    view_ = g->view(params.rect, mesh.projection, mesh.camera);
    bool good = view_ && g->show(view_, false) && g->order(view_, order_);
    for (auto it = parts_.begin(); good && it != parts_.end(); ++it)
        good = create(it->first, it->second);
    if (!good) {
        *err = "mesh view or part refused";
        detach(true);
        return false;
    }
    drawn_ = false;
    refusal_.clear();
    start_animations();
    return true;
}

void MeshLayer::detach(bool drop)
{
    forget_animations();
    for (auto &kv : parts_) {
        if (g_ && drop && kv.second.handle)
            g_->drop_part(kv.second.handle);
        kv.second.handle = 0;
        kv.second.upload = false;
    }
    if (g_ && drop && view_)
        g_->drop_view(view_);
    g_ = nullptr;
    view_ = 0;
    drawn_ = false;
    staged_ = 0;
}

void MeshLayer::arrange(bool drawn, int32_t order)
{
    order_ = order;
    if (!g_)
        return;
    if (drawn != drawn_)
        g_->show(view_, drawn);
    g_->order(view_, order);
    drawn_ = drawn;
}

void MeshLayer::place()
{
    if (!g_)
        return;
    g_->place(view_, params.rect);
    g_->camera(view_, mesh.projection, mesh.camera);
}

Stage MeshLayer::stage(int64_t now_ms)
{
    staged_ = 0;
    if (!g_ || !drawn_)
        return Stage::Idle;
    bool wrote = false;
    for (auto &kv : parts_) {
        Part &p = kv.second;
        /* Geometry goes while the budget has room, charged at once;
         * resources are made whole, so a later tick takes the rest. */
        if (p.upload && pacer_.ready(now_ms)) {
            p.upload = false;
            if (g_->geometry(p.handle, p.data.geometry())) {
                pacer_.spend(p.data.bytes());
                staged_ += p.data.bytes();
                uploads_++;
                upload_bytes_ += p.data.bytes();
                wrote = true;
            } else {
                refusal_ = "the renderer refused part " + std::to_string(kv.first) + "'s geometry";
                log_error("layer %u: %s", id_, refusal_.c_str());
            }
        }
        wrote = p.uniforms.stage(g_, p.handle) || wrote;
    }
    return wrote ? Stage::Wrote : Stage::Idle;
}

void MeshLayer::committed(bool ok, int64_t)
{
    staged_ = 0;
    if (!ok)
        return;   /* uniforms still dirty: sent again with the next flush */
    for (auto &kv : parts_)
        kv.second.uniforms.committed();
}

std::string MeshLayer::stats() const
{
    char b[96];
    std::snprintf(b, sizeof b, "%zu parts, %llu geometry uploads, %llu KB", parts_.size(),
                  (unsigned long long)uploads_, (unsigned long long)(upload_bytes_ / 1024));
    return b;
}

int MeshLayer::drawable(uint32_t part) const
{
    auto it = parts_.find(part);
    return it == parts_.end() ? 0 : it->second.handle;
}

const char *MeshLayer::animation_error(uint32_t part, const Animation &a) const
{
    if (!part) {
        if (a.property == Animated::Uniform)
            return "uniforms belong to a mesh layer's parts";
        if (a.property == Animated::Scale)
            return "a camera has no scale";
        return a.width == 3 ? nullptr : "the camera's rotate and translate take 3 values";
    }
    if (!parts_.count(part))
        return "no such part";
    if (a.property == Animated::Uniform)
        return a.width == 1 || a.width == 3 || a.width == 4 ? nullptr : "a uniform takes 1, 3 or 4 values";
    return a.width == 3 ? nullptr : "a part's rotate, translate and scale take 3 values";
}

void MeshLayer::restore(uint32_t part, const Animation &a)
{
    if (!part) {
        g_->camera(view_, mesh.projection, mesh.camera);
        return;
    }
    auto it = parts_.find(part);
    if (it == parts_.end())
        return;
    if (a.property == Animated::Uniform)
        it->second.uniforms.dirty(a.uniform);
    else if (it->second.handle)
        g_->pose(it->second.handle, it->second.pose);
}

}  // namespace crs
