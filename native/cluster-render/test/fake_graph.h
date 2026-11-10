/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* A renderer for host tests (graph.h): keeps every object's pending state
 * and, at each flush, copies it to what the display shows, as RAMSES
 * applies a flush whole. A test can refuse a write after some bytes or the
 * next flush, and read back what is shown: buffers, quads, views, parts and
 * the animations playing. Scene files are text the test writes
 * (FakeFileScene), read back the same way. */
#ifndef CRS_FAKE_GRAPH_H
#define CRS_FAKE_GRAPH_H

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../src/graph.h"

namespace crs {

struct FakeBuffer {
    TexFormat f;
    uint32_t w, h;
    std::vector<uint8_t> px;
    Sampling sampling = Sampling::Nearest;
    Wrap wrap = Wrap::Clamp;
};

struct FakeQuad {
    int effect = 0;
    Rect r;
    Turn turn;
    bool visible = false;
    int32_t order = 0;
    std::map<std::string, int> textures;
    std::map<std::string, std::vector<float>> uniforms;
    std::map<std::string, UniformType> types;   /* uniforms set as arrays */
};

struct FakeView {
    Rect r;
    Projection p;
    Pose camera;
    bool visible = false;
    int32_t order = 0;
};

struct FakePart {
    int view = 0, effect = 0;
    PartLook look;
    Pose pose;
    bool geometry = false;
    uint32_t vertices = 0, indices = 0;
    unsigned attributes = 0;
    int geometries = 0;   /* geometry() calls */
    std::map<std::string, int> textures;
    std::map<std::string, std::vector<float>> uniforms;
    std::map<std::string, UniformType> types;
};

struct FakeAnimation {
    int node = 0;
    Animation a;
};

struct FakeState {
    std::map<int, FakeBuffer> buffers;
    std::map<int, FakeQuad> quads;
    std::map<int, FakeView> views;
    std::map<int, FakePart> parts;
    std::map<int, FakeAnimation> animations;
};

class FakeGraph : public Graph {
public:
    FakeState pending, shown;
    int next = 1;
    uint64_t flushes = 0, last_tag = 0;
    size_t written = 0;            /* bytes written since the last flush */
    uint64_t flushed_bytes = 0;    /* bytes carried by flushes that went */
    int64_t *clock_us = nullptr;   /* advanced by *flush_cost_us at each flush */
    const int64_t *flush_cost_us = nullptr;
    long write_budget = -1;        /* refuse writes once this many bytes went; -1: never */
    bool fail_flush = false;
    std::string name;
    std::map<std::string, FakeGraph *> *registry = nullptr;
    const std::map<int, unsigned> *effect_attributes = nullptr;   /* the Gpu's */

    ~FakeGraph() override
    {
        if (registry)
            registry->erase(name);
    }

    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s) override { return buffer(f, w, h, s, Wrap::Clamp); }
    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap) override
    {
        FakeBuffer b{f, w, h, std::vector<uint8_t>((size_t)w * h * (f == TexFormat::R8 ? 1 : 4), 0), s, wrap};
        pending.buffers[next] = b;
        return next++;
    }
    bool write(int buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px) override
    {
        auto it = pending.buffers.find(buf);
        if (it == pending.buffers.end() || x + w > it->second.w || y + h > it->second.h)
            return false;
        const size_t bpp = it->second.f == TexFormat::R8 ? 1 : 4;
        if (write_budget >= 0 && (long)(written + (size_t)w * h * bpp) > write_budget)
            return false;
        for (uint32_t row = 0; row < h; row++)
            std::memcpy(&it->second.px[((size_t)(y + row) * it->second.w + x) * bpp], px + (size_t)row * w * bpp,
                        (size_t)w * bpp);
        written += (size_t)w * h * bpp;
        return true;
    }
    int quad(int effect, const Rect &r) override
    {
        FakeQuad q;
        q.effect = effect;
        q.r = r;
        pending.quads[next] = q;
        return next++;
    }
    bool place(int quad, const Rect &r) override
    {
        if (pending.views.count(quad))
            return pending.views[quad].r = r, true;
        return has(quad) && (pending.quads[quad].r = r, true);
    }
    bool turn(int quad, const Turn &t) override { return has(quad) && (pending.quads[quad].turn = t, true); }
    bool texture(int quad, const char *u, int buf) override
    {
        if (!pending.buffers.count(buf) || !known_sampler(u))
            return false;
        if (pending.parts.count(quad))
            return pending.parts[quad].textures[u] = buf, true;
        if (!has(quad))
            return false;
        pending.quads[quad].textures[u] = buf;
        return true;
    }
    bool uniform(int quad, const char *name, const float *v, int n) override
    {
        if (n < 1 || n > 4 || unknown_uniform == name)
            return false;
        if (pending.parts.count(quad))
            return pending.parts[quad].uniforms[name] = std::vector<float>(v, v + n), true;
        if (!has(quad))
            return false;
        pending.quads[quad].uniforms[name] = std::vector<float>(v, v + n);
        return true;
    }
    bool uniform_array(int quad, const char *name, UniformType t, const float *v, uint32_t elements) override
    {
        if (!elements || unknown_uniform == name)
            return false;
        const std::vector<float> vals(v, v + elements * uniform_width(t));
        if (pending.parts.count(quad)) {
            pending.parts[quad].uniforms[name] = vals;
            pending.parts[quad].types[name] = t;
            return true;
        }
        if (!has(quad))
            return false;
        pending.quads[quad].uniforms[name] = vals;
        pending.quads[quad].types[name] = t;
        return true;
    }
    bool show(int quad, bool on) override
    {
        if (pending.views.count(quad))
            return pending.views[quad].visible = on, true;
        return has(quad) && (pending.quads[quad].visible = on, true);
    }
    bool order(int quad, int32_t o) override
    {
        if (pending.views.count(quad))
            return pending.views[quad].order = o, true;
        return has(quad) && (pending.quads[quad].order = o, true);
    }
    void drop_quad(int quad) override
    {
        stop_on(quad);
        pending.quads.erase(quad);
    }
    void drop_buffer(int buf) override { pending.buffers.erase(buf); }

    int view(const Rect &r, const Projection &p, const Pose &camera) override
    {
        FakeView v;
        v.r = r;
        v.p = p;
        v.camera = camera;
        pending.views[next] = v;
        return next++;
    }
    bool camera(int view, const Projection &p, const Pose &camera) override
    {
        auto it = pending.views.find(view);
        if (it == pending.views.end())
            return false;
        it->second.p = p;
        it->second.camera = camera;
        return true;
    }
    void drop_view(int view) override
    {
        stop_on(view);
        pending.views.erase(view);
    }
    int part(int view, int effect, const PartLook &look) override
    {
        if (!pending.views.count(view))
            return 0;
        FakePart p;
        p.view = view;
        p.effect = effect;
        p.look = look;
        pending.parts[next] = p;
        return next++;
    }
    bool geometry(int part, const Geometry &g) override
    {
        auto it = pending.parts.find(part);
        if (it == pending.parts.end())
            return false;
        const unsigned has_attrs = kAttrPosition | (g.normal ? kAttrNormal : 0) | (g.texcoord ? kAttrTexcoord : 0) |
                                   (g.color ? kAttrColor : 0);
        unsigned reads = kAttrPosition;
        if (effect_attributes && effect_attributes->count(it->second.effect))
            reads = effect_attributes->at(it->second.effect);
        if (reads & ~has_attrs)
            return false;
        it->second.geometry = true;
        it->second.vertices = g.vertices;
        it->second.indices = g.indices;
        it->second.attributes = has_attrs;
        it->second.geometries++;
        geometry_bytes += (size_t)g.vertices * 4 * (3 + (g.normal ? 3 : 0) + (g.texcoord ? 2 : 0) + (g.color ? 4 : 0)) +
                          (size_t)g.indices * 2;
        return true;
    }
    bool pose(int part, const Pose &p) override
    {
        auto it = pending.parts.find(part);
        return it != pending.parts.end() && (it->second.pose = p, true);
    }
    void drop_part(int part) override
    {
        stop_on(part);
        pending.parts.erase(part);
    }
    int animate(int node, const Animation &a) override
    {
        if (!has(node) && !pending.parts.count(node) && !pending.views.count(node))
            return 0;
        if (a.property == Animated::Uniform && unknown_uniform == a.uniform)
            return 0;
        pending.animations[next] = FakeAnimation{node, a};
        return next++;
    }
    void stop(int anim) override { pending.animations.erase(anim); }

    bool flush(uint64_t tag) override
    {
        if (clock_us && flush_cost_us)
            *clock_us += *flush_cost_us;
        if (fail_flush)
            return false;
        flushed_bytes += written;
        shown = pending;
        flushes++;
        last_tag = tag;
        written = 0;
        return true;
    }

    std::string unknown_uniform;
    std::string unknown_sampler;
    size_t geometry_bytes = 0;   /* geometry made, over the scene's life */

    bool has(int quad) const { return pending.quads.count(quad) != 0; }
    bool known_sampler(const char *u) const { return unknown_sampler.empty() || unknown_sampler != u; }
    /* Visible quads as shown, bottom to top. */
    std::vector<int> visible() const
    {
        std::vector<std::pair<int32_t, int>> v;
        for (const auto &kv : shown.quads)
            if (kv.second.visible)
                v.push_back({kv.second.order, kv.first});
        std::sort(v.begin(), v.end());
        std::vector<int> out;
        for (auto &p : v)
            out.push_back(p.second);
        return out;
    }
    /* Visible quads and views as shown, bottom to top. */
    std::vector<int> drawn() const
    {
        std::vector<std::pair<int32_t, int>> v;
        for (const auto &kv : shown.quads)
            if (kv.second.visible)
                v.push_back({kv.second.order, kv.first});
        for (const auto &kv : shown.views)
            if (kv.second.visible)
                v.push_back({kv.second.order, kv.first});
        std::sort(v.begin(), v.end());
        std::vector<int> out;
        for (auto &p : v)
            out.push_back(p.second);
        return out;
    }
    bool is_view(int h) const { return shown.views.count(h) != 0; }
    /* The animations playing on node, as shown. */
    std::vector<Animation> playing(int node) const
    {
        std::vector<Animation> out;
        for (const auto &kv : shown.animations)
            if (kv.second.node == node)
                out.push_back(kv.second.a);
        return out;
    }

private:
    void stop_on(int node)
    {
        for (auto it = pending.animations.begin(); it != pending.animations.end();)
            it = it->second.node == node ? pending.animations.erase(it) : std::next(it);
    }
};

/* What a file scene shows, by object name: a node's visibility, its pose
 * ("NAME t", "NAME s", "NAME r": xyz), a camera's viewport, a pass enabled,
 * and values ("MESH:UNIFORM", or a data object's name). */
struct FakeFileState {
    std::map<std::string, bool> visible, enabled;
    std::map<std::string, std::vector<float>> pose, values;
    std::map<std::string, std::vector<uint32_t>> viewport;
};

/* A scene file of the host tests (FakeGpu::load), lines of text: "scene ID",
 * then "node NAME PARENT", "mesh NAME PARENT UNIFORM...", "camera NAME
 * PARENT" (PARENT - for none; meshes and cameras are nodes too), "pass
 * NAME", "data NAME COMPONENTS". */
class FakeFileScene : public FileScene {
public:
    std::vector<std::string> names[kKinds];
    std::vector<int> parent;   /* per node: its parent's handle; -1: none */
    std::map<std::string, std::vector<std::string>> uniforms;   /* mesh -> its uniforms */
    std::map<std::string, size_t> data_width;
    FakeFileState pending, shown;
    uint64_t flushes = 0;
    int listed = 0;   /* objects() calls */
    std::string name;
    std::map<std::string, FakeFileScene *> *registry = nullptr;

    ~FakeFileScene() override
    {
        if (registry)
            registry->erase(name);
    }
    std::vector<std::string> objects(Kind k) override
    {
        listed++;
        return names[k];
    }
    bool visible(int node, bool on) override
    {
        for (size_t n = 0; n < names[Node].size(); n++)
            for (int a = (int)n; a >= 0; a = parent[a])
                if (a == node) {
                    pending.visible[names[Node][n]] = on;
                    break;
                }
        return node >= 0 && node < (int)names[Node].size();
    }
    bool transform(int node, Transform what, const float v[3]) override
    {
        pending.pose[names[Node].at(node) + " " + "tsr"[what]] = std::vector<float>(v, v + 3);
        return true;
    }
    bool viewport(int camera, const uint32_t v[4]) override
    {
        pending.viewport[names[Camera].at(camera)] = std::vector<uint32_t>(v, v + 4);
        return true;
    }
    bool enable(int pass, bool on) override
    {
        pending.enabled[names[Pass].at(pass)] = on;
        return true;
    }
    bool uniform(int mesh, const std::string &u, const std::vector<float> &v, std::string *why) override
    {
        const std::vector<std::string> &has = uniforms[names[Mesh].at(mesh)];
        if (std::find(has.begin(), has.end(), u) == has.end()) {
            *why = "no uniform " + u;
            return false;
        }
        pending.values[names[Mesh][mesh] + ":" + u] = v;
        return true;
    }
    bool data(const std::string &d, const std::vector<float> &v, std::string *why) override
    {
        auto it = data_width.find(d);
        if (it == data_width.end() || v.size() < it->second) {
            *why = it == data_width.end() ? "no data object " + d : "too few values";
            return false;
        }
        pending.values[d] = std::vector<float>(v.begin(), v.begin() + it->second);
        return true;
    }
    bool flush(uint64_t) override
    {
        shown = pending;
        flushes++;
        return true;
    }
};

class FakeGpu : public Gpu {
public:
    int next_effect = 1;
    std::map<std::string, FakeFileScene *> files;   /* loaded file scenes by target name */
    int loads = 0;
    std::map<int, std::string> effects;   /* handle -> fragment shader */
    std::map<int, unsigned> attrs;        /* handle -> attributes its vertex shader reads */
    std::string reject;                   /* a fragment shader containing this fails to compile */
    std::map<std::string, FakeGraph *> live;   /* published scenes by target name */
    int scenes = 0;
    int64_t *clock_us = nullptr;
    int64_t flush_cost_us = 0;

    int effect(const char *vs, const char *fs, std::string *err) override
    {
        if (!reject.empty() && std::string(fs).find(reject) != std::string::npos) {
            *err = "shader did not compile";
            return 0;
        }
        const std::string v = vs;
        unsigned a = 0;
        const struct { const char *name; unsigned bit; } names[] = {
            {"a_position", kAttrPosition}, {"a_normal", kAttrNormal}, {"a_texcoord", kAttrTexcoord},
            {"a_color", kAttrColor}};
        for (const auto &n : names)
            if (v.find(std::string("attribute vec3 ") + n.name) != std::string::npos ||
                v.find(std::string("attribute vec2 ") + n.name) != std::string::npos ||
                v.find(std::string("attribute vec4 ") + n.name) != std::string::npos)
                a |= n.bit;
        effects[next_effect] = fs;
        attrs[next_effect] = a;
        return next_effect++;
    }
    void drop_effect(int e) override
    {
        effects.erase(e);
        attrs.erase(e);
    }
    unsigned attributes(int e) const override
    {
        auto it = attrs.find(e);
        return it == attrs.end() ? 0 : it->second;
    }
    std::unique_ptr<Graph> scene(const TargetSpec &t, std::string *) override
    {
        FakeGraph *g = new FakeGraph();
        g->name = t.name;
        g->registry = &live;
        g->clock_us = clock_us;
        g->flush_cost_us = &flush_cost_us;
        g->effect_attributes = &attrs;
        live[t.name] = g;
        scenes++;
        return std::unique_ptr<Graph>(g);
    }
    FakeGraph *graph(const std::string &target)
    {
        auto it = live.find(target);
        return it == live.end() ? nullptr : it->second;
    }
    std::unique_ptr<FileScene> load(const TargetSpec &t, const std::string &file, const std::vector<std::string> &res,
                                    std::string *err) override
    {
        std::ifstream in(file);
        std::string word, id;
        if (!(in >> word >> id) || word != "scene") {
            *err = "the scene file did not load";
            return nullptr;
        }
        for (const std::string &r : res)
            if (!std::ifstream(r)) {
                *err = "the scene file did not load";
                return nullptr;
            }
        if (std::strtoull(id.c_str(), nullptr, 0) != t.scene_id) {
            *err = "the file's scene id is " + id + ", not the target's";
            return nullptr;
        }
        std::unique_ptr<FakeFileScene> s(new FakeFileScene());
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string kind, name, up;
            if (!(ls >> kind >> name))
                continue;
            if (kind == "pass") {
                s->names[FileScene::Pass].push_back(name);
                continue;
            }
            if (kind == "data") {
                ls >> s->data_width[name];
                continue;
            }
            ls >> up;
            const auto &nodes = s->names[FileScene::Node];
            const auto p = std::find(nodes.begin(), nodes.end(), up);
            s->parent.push_back(p == nodes.end() ? -1 : (int)(p - nodes.begin()));
            s->names[FileScene::Node].push_back(name);
            if (kind == "camera")
                s->names[FileScene::Camera].push_back(name);
            if (kind == "mesh") {
                s->names[FileScene::Mesh].push_back(name);
                for (std::string u; ls >> u;)
                    s->uniforms[name].push_back(u);
            }
        }
        s->name = t.name;
        s->registry = &files;
        files[t.name] = s.get();
        loads++;
        return std::unique_ptr<FileScene>(s.release());
    }
    FakeFileScene *file(const std::string &target)
    {
        auto it = files.find(target);
        return it == files.end() ? nullptr : it->second;
    }
};

}  // namespace crs

#endif
