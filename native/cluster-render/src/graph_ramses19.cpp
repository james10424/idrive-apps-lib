/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "graph_ramses19.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "log.h"
#include "ramses19/ramses.h"

namespace crs {

namespace {

bool ok(ramses::status_t s) { return s == ramses::StatusOK; }

/* Blend over what is below at the effect's alpha; no depth, no culling: flat
 * quads. */
void flat_blended(ramses::Appearance &app)
{
    app.setBlendingFactors(ramses::EBlendFactor_SrcAlpha, ramses::EBlendFactor_OneMinusSrcAlpha,
                           ramses::EBlendFactor_One, ramses::EBlendFactor_OneMinusSrcAlpha);
    app.setBlendingOperations(ramses::EBlendOperation_Add, ramses::EBlendOperation_Add);
    app.setDepthFunction(ramses::EDepthFunc_Disabled);
    app.setDepthWrite(false);
    app.setCullingMode(ramses::ECullMode_Disabled);
}

bool part_look(ramses::Appearance &app, const PartLook &look)
{
    bool good = ok(app.setDepthFunction(look.depth ? ramses::EDepthFunc_LessEqual : ramses::EDepthFunc_Disabled)) &&
                ok(app.setDepthWrite(look.depth)) &&
                ok(app.setCullingMode(look.cull == Cull::Back    ? ramses::ECullMode_BackFacing
                                      : look.cull == Cull::Front ? ramses::ECullMode_FrontFacing
                                                                 : ramses::ECullMode_Disabled));
    switch (look.blend) {
    case Blend::Alpha:
        return good &&
               ok(app.setBlendingFactors(ramses::EBlendFactor_SrcAlpha, ramses::EBlendFactor_OneMinusSrcAlpha,
                                         ramses::EBlendFactor_One, ramses::EBlendFactor_OneMinusSrcAlpha)) &&
               ok(app.setBlendingOperations(ramses::EBlendOperation_Add, ramses::EBlendOperation_Add));
    case Blend::Add:
        return good &&
               ok(app.setBlendingFactors(ramses::EBlendFactor_SrcAlpha, ramses::EBlendFactor_One,
                                         ramses::EBlendFactor_One, ramses::EBlendFactor_OneMinusSrcAlpha)) &&
               ok(app.setBlendingOperations(ramses::EBlendOperation_Add, ramses::EBlendOperation_Add));
    default:
        return good && ok(app.setBlendingOperations(ramses::EBlendOperation_Disabled, ramses::EBlendOperation_Disabled));
    }
}

/* name appears in src as a whole identifier. */
bool mentions(const char *src, const char *name)
{
    const size_t n = std::strlen(name);
    auto ident = [](char c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    for (const char *p = std::strstr(src, name); p; p = std::strstr(p + 1, name))
        if ((p == src || !ident(p[-1])) && !ident(p[n]))
            return true;
    return false;
}

const struct {
    const char *name;
    unsigned bit;
} kAttributes[] = {{"a_position", kAttrPosition}, {"a_normal", kAttrNormal}, {"a_texcoord", kAttrTexcoord},
                   {"a_color", kAttrColor}};

unsigned effect_attributes(const ramses::Effect &fx)
{
    unsigned bits = 0;
    const uint32_t n = fx.getAttributeInputCount();
    for (uint32_t i = 0; i < n; i++) {
        ramses::AttributeInput in;
        if (!ok(fx.getAttributeInput(i, in)) || !in.getName())
            continue;
        for (const auto &a : kAttributes)
            if (std::strcmp(in.getName(), a.name) == 0)
                bits |= a.bit;
    }
    return bits;
}

class Ramses19Graph : public Graph {
public:
    Ramses19Graph(ramses::RamsesClient &client, const std::map<int, ramses::Effect *> &effects,
                  const ramses::Vector2fArray &pos, const ramses::UInt16Array &idx)
        : client_(client), effects_(effects), pos_(pos), idx_(idx)
    {
    }

    ~Ramses19Graph() override
    {
        if (!scene_)
            return;
        /* A borrowed scene went first, with every object in it (graph.h
         * FileScene::layers): only the client's resources are left. */
        if (!borrowed_ && scene_->isPublished())
            scene_->unpublish();
        if (!borrowed_)
            client_.destroy(*scene_);   /* and every object in it */
        for (auto &kv : parts_)
            for (const ramses::Resource *r : kv.second.res)
                client_.destroy(*r);
        for (auto &kv : buffers_)
            if (kv.second.tex)
                client_.destroy(*kv.second.tex);
    }

    bool build(const TargetSpec &t, std::string *err)
    {
        ramses::SceneConfig sc;
        scene_ = client_.createScene(t.scene_id, sc, t.name.c_str());
        if (!scene_) {
            *err = "createScene failed";
            return false;
        }
        w_ = t.width;
        h_ = t.height;
        cam2d_ = scene_->createOrthographicCamera("cam");
        Segment s;
        s.pass = scene_->createRenderPass("pass");
        s.group = scene_->createRenderGroup("group");
        if (!cam2d_ || !s.pass || !s.group) {
            *err = "scene objects failed";
            return false;
        }
        /* Pixel space over the whole target, y up; rects are turned over in
         * place(). The first pass keeps the library's default clear; later
         * passes never clear colour. */
        cam2d_->setFrustum(0.0f, (float)w_, 0.0f, (float)h_, 0.1f, 10.0f);
        cam2d_->setViewport(0, 0, w_, h_);
        s.pass->setCamera(*cam2d_);
        s.pass->addRenderGroup(*s.group);
        segments_.push_back(s);
        if (!ok(scene_->flush()) || !ok(scene_->publish(ramses::EScenePublicationMode_LocalAndRemote))) {
            *err = "publish failed";
            return false;
        }
        return true;
    }

    /* In a scene file's scene, published already, over its own passes: the
     * passes at render orders from base up, none clearing. */
    bool over(ramses::Scene *scene, uint32_t w, uint32_t h, int32_t base, std::string *err)
    {
        scene_ = scene;
        borrowed_ = true;
        base_ = base;
        w_ = w;
        h_ = h;
        cam2d_ = scene_->createOrthographicCamera("cluster-render layers");
        Segment s;
        s.pass = scene_->createRenderPass("cluster-render layers");
        s.group = scene_->createRenderGroup();
        if (!cam2d_ || !s.pass || !s.group || !ok(cam2d_->setFrustum(0.0f, (float)w_, 0.0f, (float)h_, 0.1f, 10.0f)) ||
            !ok(cam2d_->setViewport(0, 0, w_, h_)) || !ok(s.pass->setCamera(*cam2d_)) ||
            !ok(s.pass->addRenderGroup(*s.group)) || !ok(s.pass->setClearFlags(ramses::EClearFlags_None)) ||
            !ok(s.pass->setRenderOrder(base_))) {
            /* nothing of ours left in the file's scene */
            if (s.pass) scene_->destroy(*s.pass);
            if (s.group) scene_->destroy(*s.group);
            if (cam2d_) scene_->destroy(*cam2d_);
            cam2d_ = nullptr;
            *err = "scene objects failed";
            return false;
        }
        s.render_order = base_;
        segments_.push_back(s);
        return true;
    }

    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s) override { return buffer(f, w, h, s, Wrap::Clamp); }

    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap) override
    {
        const ramses::ETextureAddressMode mode =
            wrap == Wrap::Repeat ? ramses::ETextureAddressMode_Repeat : ramses::ETextureAddressMode_Clamp;
        ramses::Texture2DBuffer *b = scene_->createTexture2DBuffer(
            1, w, h, f == TexFormat::R8 ? ramses::ETextureFormat_R8 : ramses::ETextureFormat_RGBA8);
        ramses::TextureSampler *smp =
            b ? scene_->createTextureSampler(mode, mode,
                                             s == Sampling::Nearest ? ramses::ETextureSamplingMethod_Nearest
                                                                    : ramses::ETextureSamplingMethod_Bilinear,
                                             *b)
              : nullptr;
        if (!smp) {
            if (b)
                scene_->destroy(*b);
            log_error("ramses: %ux%u %s texture buffer refused", w, h, f == TexFormat::R8 ? "R8" : "RGBA8");
            return 0;
        }
        buffers_[next_] = Buffer{b, smp, f == TexFormat::R8 ? 1u : 4u};
        return next_++;
    }

    /* ETC2RGBA or ETC2RGB: a client resource, sent with the next flush and
     * uploaded by the display's renderer. */
    int immutable_texture(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap, const uint8_t *data,
                          size_t n) override
    {
        if (!tex_compressed(f))
            return 0;
        const ramses::ETextureAddressMode mode =
            wrap == Wrap::Repeat ? ramses::ETextureAddressMode_Repeat : ramses::ETextureAddressMode_Clamp;
        const ramses::MipLevelData mip((uint32_t)n, data);
        ramses::Texture2D *tex = client_.createTexture2D(
            w, h, f == TexFormat::ETC2RGB ? ramses::ETextureFormat_ETC2RGB : ramses::ETextureFormat_ETC2RGBA, 1, &mip,
            false);
        ramses::TextureSampler *smp =
            tex ? scene_->createTextureSampler(mode, mode,
                                               s == Sampling::Nearest ? ramses::ETextureSamplingMethod_Nearest
                                                                      : ramses::ETextureSamplingMethod_Bilinear,
                                               *tex)
                : nullptr;
        if (!smp) {
            log_error("ramses: %ux%u %s texture of %zu bytes refused by %s", w, h, tex_format_name(f), n,
                      tex ? "createTextureSampler" : "createTexture2D");
            if (tex)
                client_.destroy(*tex);
            return 0;
        }
        Buffer b{nullptr, smp, 0};
        b.tex = tex;
        buffers_[next_] = b;
        return next_++;
    }

    bool write(int buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px) override
    {
        auto it = buffers_.find(buf);
        if (it == buffers_.end() || !it->second.buffer)
            return false;
        const ramses::status_t s = it->second.buffer->setData(reinterpret_cast<const char *>(px), 0, x, y, w, h);
        if (!ok(s) && !write_failing_)
            log_error("ramses: setData %ux%u at %u,%u: %s", w, h, x, y, client_.getStatusMessage(s));
        write_failing_ = !ok(s);
        return ok(s);
    }

    /* A quad is its mesh under two group nodes: place (translation to the
     * pivot's place in the target) and turn (rotation and scale about the
     * pivot, and an animated offset); the mesh is offset by the pivot and
     * scaled to the rect. */
    int quad(int effect, const Rect &r) override
    {
        auto fx = effects_.find(effect);
        if (fx == effects_.end())
            return 0;
        ramses::Appearance *app = scene_->createAppearance(*fx->second);
        ramses::GeometryBinding *geo = scene_->createGeometryBinding(*fx->second);
        ramses::MeshNode *mesh = scene_->createMeshNode();
        ramses::GroupNode *placed = scene_->createGroupNode();
        ramses::GroupNode *turned = scene_->createGroupNode();
        ramses::AttributeInput ain;
        if (!app || !geo || !mesh || !placed || !turned || !ok(fx->second->findAttributeInput("a_position", ain)) ||
            !ok(geo->setInputBuffer(ain, pos_)) || !ok(geo->setIndices(idx_)) || !ok(mesh->setParent(*turned)) ||
            !ok(turned->setParent(*placed))) {
            if (mesh) scene_->destroy(*mesh);
            if (turned) scene_->destroy(*turned);
            if (placed) scene_->destroy(*placed);
            if (geo) scene_->destroy(*geo);
            if (app) scene_->destroy(*app);
            return 0;
        }
        flat_blended(*app);
        mesh->setAppearance(*app);
        mesh->setGeometryBinding(*geo);
        mesh->setVisibility(false);
        if (!ok(segments_[0].group->addMeshNode(*mesh, 0))) {
            scene_->destroy(*mesh);
            scene_->destroy(*turned);
            scene_->destroy(*placed);
            scene_->destroy(*geo);
            scene_->destroy(*app);
            return 0;
        }
        Quad q;
        q.mesh = mesh;
        q.placed = placed;
        q.turned = turned;
        q.app = app;
        q.geo = geo;
        q.effect = fx->second;
        q.r = r;
        quads_[next_] = q;
        lay_out(quads_[next_]);
        layout_dirty_ = true;
        return next_++;
    }

    bool place(int quad, const Rect &r) override
    {
        auto v = views_.find(quad);
        if (v != views_.end()) {
            v->second.r = r;
            return frame(v->second);
        }
        auto it = quads_.find(quad);
        if (it == quads_.end())
            return false;
        it->second.r = r;
        return lay_out(it->second);
    }

    bool turn(int quad, const Turn &t) override
    {
        auto it = quads_.find(quad);
        if (it == quads_.end())
            return false;
        it->second.t = t;
        return lay_out(it->second);
    }

    bool texture(int quad, const char *uniform, int buf) override
    {
        ramses::Appearance *app = nullptr;
        const ramses::Effect *fx = look(quad, &app);
        auto b = buffers_.find(buf);
        ramses::UniformInput in;
        return fx && b != buffers_.end() && ok(fx->findUniformInput(uniform, in)) &&
               ok(app->setInputTexture(in, *b->second.sampler));
    }

    bool uniform(int quad, const char *name, const float *v, int n) override
    {
        ramses::Appearance *app = nullptr;
        const ramses::Effect *fx = look(quad, &app);
        ramses::UniformInput in;
        if (!fx || !ok(fx->findUniformInput(name, in)))
            return false;
        ramses::Appearance &a = *app;
        switch (n) {
        case 1: return ok(a.setInputValueFloat(in, v[0]));
        case 2: return ok(a.setInputValueVector2f(in, v[0], v[1]));
        case 3: return ok(a.setInputValueVector3f(in, v[0], v[1], v[2]));
        case 4: return ok(a.setInputValueVector4f(in, v[0], v[1], v[2], v[3]));
        default: return false;
        }
    }

    bool uniform_array(int quad, const char *name, UniformType t, const float *v, uint32_t elements) override
    {
        ramses::Appearance *app = nullptr;
        const ramses::Effect *fx = look(quad, &app);
        ramses::UniformInput in;
        if (!fx || !ok(fx->findUniformInput(name, in)))
            return false;
        switch (t) {
        case UniformType::Float: return ok(app->setInputValueFloat(in, elements, v));
        case UniformType::Vec2: return ok(app->setInputValueVector2f(in, elements, v));
        case UniformType::Vec3: return ok(app->setInputValueVector3f(in, elements, v));
        case UniformType::Vec4: return ok(app->setInputValueVector4f(in, elements, v));
        default: return ok(app->setInputValueMatrix44f(in, elements, v));
        }
    }

    bool show(int quad, bool on) override
    {
        auto v = views_.find(quad);
        if (v != views_.end()) {
            v->second.shown = on;
            return ok(v->second.root->setVisibility(on && !v->second.offscreen));
        }
        auto q = quads_.find(quad);
        return q != quads_.end() && ok(q->second.mesh->setVisibility(on));
    }

    bool order(int quad, int32_t order) override
    {
        auto v = views_.find(quad);
        if (v != views_.end()) {
            if (v->second.order != order)
                layout_dirty_ = true;
            v->second.order = order;
            return true;
        }
        auto q = quads_.find(quad);
        if (q == quads_.end())
            return false;
        if (q->second.order != order)
            layout_dirty_ = true;
        q->second.order = order;
        return true;
    }

    void drop_quad(int quad) override
    {
        auto q = quads_.find(quad);
        if (q == quads_.end())
            return;
        stop_on(quad);
        segments_[q->second.segment].group->removeMeshNode(*q->second.mesh);
        scene_->destroy(*q->second.mesh);
        scene_->destroy(*q->second.turned);
        scene_->destroy(*q->second.placed);
        scene_->destroy(*q->second.geo);
        scene_->destroy(*q->second.app);
        quads_.erase(q);
    }

    void drop_buffer(int buf) override
    {
        auto b = buffers_.find(buf);
        if (b == buffers_.end())
            return;
        scene_->destroy(*b->second.sampler);
        if (b->second.buffer)
            scene_->destroy(*b->second.buffer);
        if (b->second.tex)
            client_.destroy(*b->second.tex);
        buffers_.erase(b);
    }

    int view(const Rect &r, const Projection &p, const Pose &camera) override
    {
        View v;
        v.pass = scene_->createRenderPass();
        v.group = scene_->createRenderGroup();
        v.root = scene_->createGroupNode();
        if (p.ortho)
            v.cam = scene_->createOrthographicCamera();
        else
            v.cam = scene_->createPerspectiveCamera();
        v.r = r;
        v.p = p;
        v.camera = camera;
        /* A pass takes only a camera with its frustum and viewport set. */
        if (!v.pass || !v.group || !v.root || !v.cam || !frame(v) || !ok(v.pass->setCamera(*v.cam)) ||
            !ok(v.pass->addRenderGroup(*v.group)) || !ok(v.pass->setClearFlags(ramses::EClearFlags_None))) {
            destroy_view(v);
            return 0;
        }
        views_[next_] = v;
        layout_dirty_ = true;
        return next_++;
    }

    bool camera(int view, const Projection &p, const Pose &camera) override
    {
        auto it = views_.find(view);
        if (it == views_.end() || it->second.p.ortho != p.ortho)
            return false;
        it->second.p = p;
        it->second.camera = camera;
        return frame(it->second);
    }

    void drop_view(int view) override
    {
        auto it = views_.find(view);
        if (it == views_.end())
            return;
        std::vector<int> parts;
        for (const auto &kv : parts_)
            if (kv.second.view == view)
                parts.push_back(kv.first);
        for (int p : parts)
            drop_part(p);
        stop_on(view);
        destroy_view(it->second);
        views_.erase(it);
        layout_dirty_ = true;
    }

    int part(int view, int effect, const PartLook &look) override
    {
        auto v = views_.find(view);
        auto fx = effects_.find(effect);
        if (v == views_.end() || fx == effects_.end())
            return 0;
        ramses::Appearance *app = scene_->createAppearance(*fx->second);
        ramses::GeometryBinding *geo = scene_->createGeometryBinding(*fx->second);
        ramses::MeshNode *mesh = scene_->createMeshNode();
        if (!app || !geo || !mesh || !part_look(*app, look) || !ok(mesh->setAppearance(*app)) ||
            !ok(mesh->setGeometryBinding(*geo)) || !ok(mesh->setParent(*v->second.root))) {
            if (mesh) scene_->destroy(*mesh);
            if (geo) scene_->destroy(*geo);
            if (app) scene_->destroy(*app);
            return 0;
        }
        Part p;
        p.view = view;
        p.mesh = mesh;
        p.app = app;
        p.geo = geo;
        p.effect = fx->second;
        p.depth = look.depth;
        parts_[next_] = p;
        clear_depth(v->second, view);
        return next_++;
    }

    bool geometry(int part, const Geometry &g) override
    {
        auto it = parts_.find(part);
        if (it == parts_.end())
            return false;
        Part &p = it->second;
        const unsigned reads = effect_attributes(*p.effect);
        const unsigned has = kAttrPosition | (g.normal ? kAttrNormal : 0) | (g.texcoord ? kAttrTexcoord : 0) |
                             (g.color ? kAttrColor : 0);
        if (!(reads & kAttrPosition) || (reads & ~has))
            return false;
        std::vector<const ramses::Resource *> res;
        const ramses::Vector3fArray *pos = client_.createConstVector3fArray(g.vertices, g.position);
        const ramses::Vector3fArray *nor =
            reads & kAttrNormal ? client_.createConstVector3fArray(g.vertices, g.normal) : nullptr;
        const ramses::Vector2fArray *uv =
            reads & kAttrTexcoord ? client_.createConstVector2fArray(g.vertices, g.texcoord) : nullptr;
        const ramses::Vector4fArray *col =
            reads & kAttrColor ? client_.createConstVector4fArray(g.vertices, g.color) : nullptr;
        const ramses::UInt16Array *idx = client_.createConstUInt16Array(g.indices, g.index);
        for (const ramses::Resource *r : {(const ramses::Resource *)pos, (const ramses::Resource *)nor,
                                          (const ramses::Resource *)uv, (const ramses::Resource *)col,
                                          (const ramses::Resource *)idx})
            if (r)
                res.push_back(r);
        ramses::AttributeInput in;
        bool good = pos && idx && (!(reads & kAttrNormal) || nor) && (!(reads & kAttrTexcoord) || uv) &&
                    (!(reads & kAttrColor) || col) && ok(p.effect->findAttributeInput("a_position", in)) &&
                    ok(p.geo->setInputBuffer(in, *pos)) && ok(p.geo->setIndices(*idx)) &&
                    ok(p.mesh->setIndexCount(g.indices));   /* bound before its indices existed */
        if (good && nor)
            good = ok(p.effect->findAttributeInput("a_normal", in)) && ok(p.geo->setInputBuffer(in, *nor));
        if (good && uv)
            good = ok(p.effect->findAttributeInput("a_texcoord", in)) && ok(p.geo->setInputBuffer(in, *uv));
        if (good && col)
            good = ok(p.effect->findAttributeInput("a_color", in)) && ok(p.geo->setInputBuffer(in, *col));
        if (good && !p.grouped)
            good = p.grouped = ok(views_[p.view].group->addMeshNode(*p.mesh, part));
        if (!good) {
            log_error("ramses: part geometry of %u vertices refused", g.vertices);
            /* A binding left half set would point at what is destroyed. */
            if (p.grouped) {
                views_[p.view].group->removeMeshNode(*p.mesh);
                p.grouped = false;
            }
            for (const ramses::Resource *r : res)
                client_.destroy(*r);
            for (const ramses::Resource *r : p.res)
                client_.destroy(*r);
            p.res.clear();
            return false;
        }
        /* The binding holds the new ones: the old ones go. */
        for (const ramses::Resource *r : p.res)
            client_.destroy(*r);
        p.res = res;
        return true;
    }

    bool pose(int part, const Pose &p) override
    {
        auto it = parts_.find(part);
        if (it == parts_.end())
            return false;
        ramses::MeshNode &m = *it->second.mesh;
        return ok(m.setTranslation(p.t[0], p.t[1], p.t[2])) && ok(m.setRotation(-p.r[0], -p.r[1], -p.r[2])) &&
               ok(m.setScaling(p.s[0], p.s[1], p.s[2]));
    }

    void drop_part(int part) override
    {
        auto it = parts_.find(part);
        if (it == parts_.end())
            return;
        stop_on(part);
        Part &p = it->second;
        auto v = views_.find(p.view);
        if (p.grouped && v != views_.end())
            v->second.group->removeMeshNode(*p.mesh);
        scene_->destroy(*p.mesh);
        scene_->destroy(*p.geo);
        scene_->destroy(*p.app);
        for (const ramses::Resource *r : p.res)
            client_.destroy(*r);
        const int view = p.view;
        parts_.erase(it);
        if (v != views_.end())
            clear_depth(v->second, view);
    }

    int animate(int node, const Animation &a) override
    {
        if (a.t.empty() || a.v.size() != a.t.size() * a.width)
            return 0;
        Anim an;
        an.node = node;
        an.a = a;
        bool made = false;
        if (!build(an, &made)) {
            log_error("ramses: animation refused");
            if (made)
                rebuild();
            return 0;
        }
        anims_[next_] = an;
        return next_++;
    }

    /* The animation system goes whole, its objects with it, and the
     * animations that stay are made again in a new one, each from when it
     * started. VERIFIED on the unit's libramses 19: a destroyed spline or
     * animated property leaves its animation system one the library cannot
     * describe, and it crashes the process when a renderer subscribes to
     * the scene again, or when the scene is saved. */
    void stop(int anim) override
    {
        if (anims_.erase(anim))
            rebuild();
    }

    bool flush(uint64_t tag) override
    {
        if (layout_dirty_)
            relayout();
        const ramses::status_t s = scene_->flush(ramses::ESceneFlushMode_Synchronous, tag);
        if (!ok(s) && !flush_failing_)
            log_error("ramses: flush %llu: %s", (unsigned long long)tag, client_.getStatusMessage(s));
        flush_failing_ = !ok(s);
        return ok(s);
    }

private:
    struct Buffer {
        ramses::Texture2DBuffer *buffer;   /* null for an immutable texture */
        ramses::TextureSampler *sampler;
        uint32_t bpp;
        const ramses::Texture2D *tex = nullptr;
    };
    struct Quad {
        ramses::MeshNode *mesh = nullptr;
        ramses::GroupNode *placed = nullptr, *turned = nullptr;
        ramses::Appearance *app = nullptr;
        ramses::GeometryBinding *geo = nullptr;
        ramses::Effect *effect = nullptr;
        int32_t order = 0;
        int32_t grouped_order = 0;   /* its order within its segment's group */
        size_t segment = 0;
        Rect r;
        Turn t;
    };
    /* A 2D pass: the quads between two views. */
    struct Segment {
        ramses::RenderPass *pass = nullptr;
        ramses::RenderGroup *group = nullptr;
        int32_t render_order = 0;
    };
    struct View {
        ramses::RenderPass *pass = nullptr;
        ramses::RenderGroup *group = nullptr;
        ramses::LocalCamera *cam = nullptr;
        ramses::GroupNode *root = nullptr;
        Rect r;
        Projection p;
        Pose camera;
        int32_t order = 0, render_order = 0;
        bool shown = false, offscreen = false, depth = false;
    };
    struct Part {
        int view = 0;
        ramses::MeshNode *mesh = nullptr;
        ramses::Appearance *app = nullptr;
        ramses::GeometryBinding *geo = nullptr;
        ramses::Effect *effect = nullptr;
        bool depth = false, grouped = false;
        std::vector<const ramses::Resource *> res;
    };
    struct Anim {
        int node = 0;
        Animation a;
        ramses::globalTimeStamp_t start = 0;   /* 0: not started yet */
        ramses::AnimationSequence *seq = nullptr;
        ramses::Animation *anim = nullptr;
        ramses::AnimatedProperty *prop = nullptr;
        ramses::Spline *spline = nullptr;
    };

    /* Makes an's objects in the animation system (made when there is none)
     * and starts it at an.start, or now. An unknown node or uniform is
     * refused before anything is made; *made: objects were made, which a
     * refusal leaves to a new system (rebuild()) to take away. */
    bool build(Anim &an, bool *made)
    {
        const int node = an.node;
        const Animation &a = an.a;
        auto q = quads_.find(node);
        auto p = parts_.find(node);
        auto v = views_.find(node);
        ramses::Node *target = q != quads_.end()   ? (ramses::Node *)q->second.turned
                               : p != parts_.end() ? (ramses::Node *)p->second.mesh
                               : v != views_.end() ? (ramses::Node *)v->second.cam
                                                   : nullptr;
        ramses::Appearance *app = nullptr;
        const ramses::Effect *fx = look(node, &app);
        ramses::UniformInput in;
        if (!target || (a.property == Animated::Uniform && (!fx || !ok(fx->findUniformInput(a.uniform.c_str(), in)))))
            return false;
        if (!anim_)
            anim_ = scene_->createRealTimeAnimationSystem(ramses::EAnimationSystemFlags_Default, "anim");
        if (!anim_)
            return false;
        *made = true;
        const bool on_quad = q != quads_.end();
        /* The renderer's conventions: y up, so down is negative y, and its
         * rotations are left-handed, so a quad's clockwise turn is positive
         * z and the right-handed angles of parts and cameras are negated. */
        std::vector<float> vals = a.v;
        if (!on_quad && a.property == Animated::Rotate)
            for (float &x : vals)
                x = -x;
        if (on_quad && a.property == Animated::Translate)
            for (size_t k = 1; k < vals.size(); k += 3)
                vals[k] = -vals[k];

        an.spline = spline(a, vals);
        if (an.spline && a.property == Animated::Uniform) {
            an.prop = anim_->createAnimatedProperty(in, *app);
        } else if (an.spline) {
            const ramses::EAnimatedProperty prop = a.property == Animated::Rotate      ? ramses::EAnimatedProperty_Rotation
                                                   : a.property == Animated::Translate ? ramses::EAnimatedProperty_Translation
                                                                                       : ramses::EAnimatedProperty_Scaling;
            an.prop = anim_->createAnimatedProperty(*target, prop,
                                                    a.width == 1 ? ramses::EAnimatedPropertyComponent_Z
                                                                 : ramses::EAnimatedPropertyComponent_All);
        }
        an.anim = an.prop ? anim_->createAnimation(*an.prop, *an.spline) : nullptr;
        an.seq = an.anim ? anim_->createAnimationSequence() : nullptr;
        if (!an.seq || !ok(an.seq->addAnimation(*an.anim)) || (a.loop && !ok(an.seq->setAnimationLooping(*an.anim))) ||
            !ok(anim_->updateLocalTime()))
            return false;
        const ramses::globalTimeStamp_t at = an.start ? an.start : anim_->getTime();
        if (!ok(an.seq->startAt(at)))
            return false;
        an.start = at;
        return true;
    }

    void rebuild()
    {
        if (anim_)
            scene_->destroy(*anim_);
        anim_ = nullptr;
        bool left = false;
        for (auto it = anims_.begin(); it != anims_.end();) {
            bool made = false;
            if (build(it->second, &made)) {
                ++it;
                continue;
            }
            log_error("ramses: animation %d refused in a new animation system", it->first);
            left = left || made;
            it = anims_.erase(it);
        }
        if (left)
            rebuild();
    }

    /* The appearance and effect of a quad or a part. */
    const ramses::Effect *look(int handle, ramses::Appearance **app)
    {
        auto q = quads_.find(handle);
        if (q != quads_.end()) {
            *app = q->second.app;
            return q->second.effect;
        }
        auto p = parts_.find(handle);
        if (p != parts_.end()) {
            *app = p->second.app;
            return p->second.effect;
        }
        return nullptr;
    }

    bool lay_out(Quad &q)
    {
        const Rect &r = q.r;
        const Turn &t = q.t;
        return ok(q.placed->setTranslation((float)r.x + t.px, (float)h_ - (float)r.y - t.py, 0.0f)) &&
               ok(q.turned->setTranslation(0.0f, 0.0f, 0.0f)) && ok(q.turned->setRotation(0.0f, 0.0f, t.rotate)) &&
               ok(q.turned->setScaling(t.sx, t.sy, 1.0f)) &&
               ok(q.mesh->setTranslation(-t.px, t.py - (float)r.h, -1.0f)) &&
               ok(q.mesh->setScaling((float)r.w, (float)r.h, 1.0f));
    }

    /* The view's viewport is the part of its rect inside the target, and
     * its frustum the same part of the rect's whole frustum, so a rect
     * reaching past the target's edge is cut, not squeezed. */
    bool frame(View &v)
    {
        const int64_t x0 = std::max<int64_t>(v.r.x, 0), x1 = std::min<int64_t>((int64_t)v.r.x + v.r.w, w_);
        const int64_t y0 = std::max<int64_t>(v.r.y, 0), y1 = std::min<int64_t>((int64_t)v.r.y + v.r.h, h_);
        const bool good = ok(v.cam->setTranslation(v.camera.t[0], v.camera.t[1], v.camera.t[2])) &&
                          ok(v.cam->setRotation(-v.camera.r[0], -v.camera.r[1], -v.camera.r[2]));
        const float aspect = v.r.h ? (float)v.r.w / (float)v.r.h : 1.0f;
        const float top = v.p.ortho ? v.p.ortho_h / 2.0f : v.p.near * std::tan(v.p.fov * 3.14159265f / 360.0f);
        const float right = top * aspect;
        v.offscreen = x1 <= x0 || y1 <= y0 || !v.r.w || !v.r.h;
        if (v.offscreen)   /* nothing to draw; the camera stays valid */
            return good && ok(v.cam->setFrustum(-right, right, -top, top, v.p.near, v.p.far)) &&
                   ok(v.cam->setViewport(0, 0, 1, 1)) && ok(v.root->setVisibility(false));
        const float l = -right + 2.0f * right * (float)(x0 - v.r.x) / (float)v.r.w;
        const float rr = -right + 2.0f * right * (float)(x1 - v.r.x) / (float)v.r.w;
        const float t = top - 2.0f * top * (float)(y0 - v.r.y) / (float)v.r.h;
        const float b = top - 2.0f * top * (float)(y1 - v.r.y) / (float)v.r.h;
        return good && ok(v.cam->setFrustum(l, rr, b, t, v.p.near, v.p.far)) &&
               ok(v.cam->setViewport((uint32_t)x0, (uint32_t)(h_ - y1), (uint32_t)(x1 - x0), (uint32_t)(y1 - y0))) &&
               ok(v.root->setVisibility(v.shown));
    }

    /* A view clears depth when one of its parts tests it. */
    void clear_depth(View &v, int view)
    {
        bool depth = false;
        for (const auto &kv : parts_)
            depth = depth || (kv.second.view == view && kv.second.depth);
        if (depth != v.depth)
            v.pass->setClearFlags(depth ? ramses::EClearFlags_Depth : ramses::EClearFlags_None);
        v.depth = depth;
    }

    void destroy_view(View &v)
    {
        if (v.pass) scene_->destroy(*v.pass);
        if (v.group) scene_->destroy(*v.group);
        if (v.cam) scene_->destroy(*v.cam);
        if (v.root) scene_->destroy(*v.root);
    }

    ramses::Spline *spline(const Animation &a, const std::vector<float> &v)
    {
        const size_t n = a.t.size();
        const float *f = v.data();
        bool good = true;
        if (a.width == 1) {
            if (a.step) {
                ramses::SplineStepFloat *s = anim_->createSplineStepFloat();
                for (size_t k = 0; s && good && k < n; k++)
                    good = ok(s->setKey(a.t[k], f[k]));
                return s && good ? s : nullptr;
            }
            ramses::SplineLinearFloat *s = anim_->createSplineLinearFloat();
            for (size_t k = 0; s && good && k < n; k++)
                good = ok(s->setKey(a.t[k], f[k]));
            return s && good ? s : nullptr;
        }
        if (a.width == 3) {
            if (a.step) {
                ramses::SplineStepVector3f *s = anim_->createSplineStepVector3f();
                for (size_t k = 0; s && good && k < n; k++)
                    good = ok(s->setKey(a.t[k], f[k * 3], f[k * 3 + 1], f[k * 3 + 2]));
                return s && good ? s : nullptr;
            }
            ramses::SplineLinearVector3f *s = anim_->createSplineLinearVector3f();
            for (size_t k = 0; s && good && k < n; k++)
                good = ok(s->setKey(a.t[k], f[k * 3], f[k * 3 + 1], f[k * 3 + 2]));
            return s && good ? s : nullptr;
        }
        if (a.width == 4) {
            if (a.step) {
                ramses::SplineStepVector4f *s = anim_->createSplineStepVector4f();
                for (size_t k = 0; s && good && k < n; k++)
                    good = ok(s->setKey(a.t[k], f[k * 4], f[k * 4 + 1], f[k * 4 + 2], f[k * 4 + 3]));
                return s && good ? s : nullptr;
            }
            ramses::SplineLinearVector4f *s = anim_->createSplineLinearVector4f();
            for (size_t k = 0; s && good && k < n; k++)
                good = ok(s->setKey(a.t[k], f[k * 4], f[k * 4 + 1], f[k * 4 + 2], f[k * 4 + 3]));
            return s && good ? s : nullptr;
        }
        return nullptr;
    }

    void stop_on(int node)
    {
        const size_t before = anims_.size();
        for (auto it = anims_.begin(); it != anims_.end();)
            it = it->second.node == node ? anims_.erase(it) : std::next(it);
        if (anims_.size() != before)
            rebuild();
    }

    /* Puts every quad and view in its pass, in draw order (plan_passes). */
    void relayout()
    {
        layout_dirty_ = false;
        struct Item {
            int32_t order;
            bool view;
            int handle;
        };
        std::vector<Item> items;
        for (const auto &kv : quads_)
            items.push_back(Item{kv.second.order, false, kv.first});
        for (const auto &kv : views_)
            items.push_back(Item{kv.second.order, true, kv.first});
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            return a.order != b.order ? a.order < b.order : a.handle < b.handle;
        });
        std::vector<bool> kinds;
        for (const Item &it : items)
            kinds.push_back(it.view);
        const std::vector<int> pass = plan_passes(kinds);
        /* 2D passes in order of appearance; pass 0 is always the first
         * segment. */
        std::map<int, size_t> segment_of;
        segment_of[0] = 0;
        for (size_t i = 0; i < items.size(); i++)
            if (!items[i].view && !segment_of.count(pass[i]))
                segment_of.emplace(pass[i], segment_of.size());
        while (segments_.size() < segment_of.size()) {
            Segment s;
            s.pass = scene_->createRenderPass();
            s.group = scene_->createRenderGroup();
            if (!s.pass || !s.group || !ok(s.pass->setCamera(*cam2d_)) || !ok(s.pass->addRenderGroup(*s.group)) ||
                !ok(s.pass->setClearFlags(ramses::EClearFlags_None))) {
                log_error("ramses: render pass refused");
                if (s.pass) scene_->destroy(*s.pass);
                if (s.group) scene_->destroy(*s.group);
                layout_dirty_ = true;
                return;
            }
            segments_.push_back(s);
        }
        for (const auto &kv : segment_of) {
            Segment &s = segments_[kv.second];
            if (s.render_order != base_ + kv.first && ok(s.pass->setRenderOrder(base_ + kv.first)))
                s.render_order = base_ + kv.first;
        }
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].view) {
                View &v = views_[items[i].handle];
                if (v.render_order != base_ + pass[i] && ok(v.pass->setRenderOrder(base_ + pass[i])))
                    v.render_order = base_ + pass[i];
                continue;
            }
            Quad &q = quads_[items[i].handle];
            const size_t seg = segment_of[pass[i]];
            if (seg == q.segment && q.grouped_order == q.order)
                continue;
            segments_[q.segment].group->removeMeshNode(*q.mesh);
            q.segment = seg;
            q.grouped_order = q.order;
            segments_[seg].group->addMeshNode(*q.mesh, q.order);
        }
        /* Segments past the plan hold no quads: they go. */
        while (segments_.size() > segment_of.size()) {
            scene_->destroy(*segments_.back().pass);
            scene_->destroy(*segments_.back().group);
            segments_.pop_back();
        }
    }

    ramses::RamsesClient &client_;
    const std::map<int, ramses::Effect *> &effects_;
    const ramses::Vector2fArray &pos_;
    const ramses::UInt16Array &idx_;
    ramses::Scene *scene_ = nullptr;
    ramses::OrthographicCamera *cam2d_ = nullptr;
    ramses::AnimationSystemRealTime *anim_ = nullptr;
    std::vector<Segment> segments_;
    uint32_t w_ = 0, h_ = 0;
    /* In a scene file's scene (over()): not ours to publish or destroy; the
     * passes' render orders start at base_. */
    bool borrowed_ = false;
    int32_t base_ = 0;
    bool write_failing_ = false, flush_failing_ = false;   /* log the first of a run */
    bool layout_dirty_ = false;
    std::map<int, Buffer> buffers_;
    std::map<int, Quad> quads_;
    std::map<int, View> views_;
    std::map<int, Part> parts_;
    std::map<int, Anim> anims_;
    int next_ = 1;
};

/* Scene files on RAMSES 19 (graph.h FileScene): the scene and its resources
 * loaded by the client library (loadSceneFromFile), with the scene id the
 * file keeps, published as the target's scene. Its objects come from
 * SceneObjectIterator by object type, and a uniform's setter from its data
 * type; the headers give neither's values (../abi/ABI.md), so the first load
 * learns them from objects it makes and destroys at once: a probe effect
 * with a uniform of each type, a float array, and a scene of each object
 * kind, never published. The resource objects a load makes go with its
 * scene. */

/* Never published; no target's (they are allow-listed). */
const ramses::sceneId_t kProbeScene = 0xfffffff0u;

const char *const kProbeVs =
    "uniform float u_f1; uniform vec2 u_f2; uniform vec3 u_f3; uniform vec4 u_f4;\n"
    "uniform int u_i1; uniform ivec2 u_i2; uniform ivec3 u_i3; uniform ivec4 u_i4;\n"
    "uniform mat2 u_m2; uniform mat3 u_m3; uniform mat4 u_m4;\n"
    "attribute vec3 a_p;\n"
    "void main() {\n"
    "  float s = u_f1 + u_f2.x + u_f3.x + u_f4.x + float(u_i1 + u_i2.x + u_i3.x + u_i4.x) + u_m2[0][0] + u_m3[0][0];\n"
    "  gl_Position = u_m4 * vec4(a_p * s, 1.0);\n"
    "}\n";
const char *const kProbeFs = "precision mediump float;\nvoid main() { gl_FragColor = vec4(1.0); }\n";
const char *const kProbeUniforms[11] = {"u_f1", "u_f2", "u_f3", "u_f4", "u_i1", "u_i2",
                                        "u_i3", "u_i4", "u_m2", "u_m3", "u_m4"};
/* Floats or ints in one element of each, in Ramses19Types::uniform's order. */
const uint32_t kWidth[11] = {1, 2, 3, 4, 1, 2, 3, 4, 4, 9, 16};
const uint32_t kDataWidth[5] = {1, 1, 2, 3, 4};

bool learn(ramses::RamsesClient &client, Ramses19Types *t, std::string *err)
{
    ramses::EffectDescription d;
    d.setVertexShader(kProbeVs);
    d.setFragmentShader(kProbeFs);
    ramses::Effect *fx = client.createEffect(d, ramses::ResourceCacheFlag_DoNotCache, "cluster-render-probe");
    static const float one = 1;
    const ramses::FloatArray *arr = client.createConstFloatArray(1, &one);
    ramses::SceneConfig sc;
    ramses::Scene *s = client.createScene(kProbeScene, sc, "cluster-render-probe");
    bool good = fx && arr && s;
    for (int i = 0; good && i < 11; i++) {
        ramses::UniformInput u;
        good = ok(fx->findUniformInput(kProbeUniforms[i], u));
        t->uniform[i] = good ? (uint32_t)u.getDataType() : 0;
    }
    const ramses::RamsesObject *nodes[4] = {}, *others[8] = {};
    if (good) {
        const ramses::RamsesObject *n[4] = {s->createGroupNode(), s->createMeshNode(), s->createPerspectiveCamera(),
                                            s->createOrthographicCamera()};
        const ramses::RamsesObject *o[8] = {s->createRenderPass(),    s->createRenderGroup(),
                                            s->createAppearance(*fx), s->createDataFloat(),
                                            s->createDataInt32(),     s->createDataVector2f(),
                                            s->createDataVector3f(),  s->createDataVector4f()};
        std::copy(n, n + 4, nodes);
        std::copy(o, o + 8, others);
        for (const ramses::RamsesObject *x : nodes)
            good = good && x;
        for (const ramses::RamsesObject *x : others)
            good = good && x;
    }
    if (good) {
        t->mesh = (uint32_t)nodes[1]->getType();
        t->persp = (uint32_t)nodes[2]->getType();
        t->ortho = (uint32_t)nodes[3]->getType();
        t->pass = (uint32_t)others[0]->getType();
        for (int i = 0; i < 5; i++)
            t->data[i] = (uint32_t)others[3 + i]->getType();
        /* The bases: Node is every node's and nothing else's here; Resource
         * the effect's and the array's, and no scene object's. */
        t->node = t->resource = 0;
        for (uint32_t v = 1; v < 256; v++) {
            const ramses::ERamsesObjectType type = ramses::ERamsesObjectType(v);
            bool all_nodes = true, any_node = false, any_other = false;
            for (const ramses::RamsesObject *x : nodes) {
                all_nodes = all_nodes && x->isOfType(type);
                any_node = any_node || x->isOfType(type);
            }
            for (const ramses::RamsesObject *x : others)
                any_other = any_other || x->isOfType(type);
            const bool scene = s->isOfType(type);
            if (!t->node && all_nodes && !any_other && !scene && !fx->isOfType(type))
                t->node = v;
            if (!t->resource && fx->isOfType(type) && arr->isOfType(type) && !any_node && !any_other && !scene)
                t->resource = v;
        }
        good = t->node && t->resource;
    }
    if (s)
        client.destroy(*s);
    if (fx)
        client.destroy(*fx);
    if (arr)
        client.destroy(*arr);
    if (!good) {
        *err = "ramses: the object types could not be learned";
        log_error("%s", err->c_str());
        return false;
    }
    log_info("ramses: object types node %u mesh %u cameras %u %u pass %u resource %u", t->node, t->mesh, t->persp,
             t->ortho, t->pass, t->resource);
    return true;
}

std::set<const ramses::RamsesObject *> resources(const ramses::RamsesClient &client, uint32_t type)
{
    std::set<const ramses::RamsesObject *> out;
    ramses::ResourceIterator it(client, ramses::ERamsesObjectType(type));
    while (const ramses::RamsesObject *o = it.getNext())
        out.insert(o);
    return out;
}

/* A uniform by name, through the effect's list: findUniformInput reports a
 * name it lacks as an error, and a pattern may match meshes without it. */
bool find_uniform(const ramses::Effect &fx, const std::string &name, ramses::UniformInput *u)
{
    const uint32_t n = fx.getUniformInputCount();
    for (uint32_t i = 0; i < n; i++)
        if (ok(fx.getUniformInput(i, *u)) && u->getName() && name == u->getName())
            return true;
    return false;
}

bool set_tree(ramses::Node &n, bool on)
{
    bool good = ok(n.setVisibility(on));
    const uint32_t children = n.getChildCount();
    for (uint32_t i = 0; i < children; i++)
        if (ramses::Node *c = n.getChild(i))
            good = set_tree(*c, on) && good;
    return good;
}

class Ramses19FileScene : public FileScene {
public:
    /* effects, pos and idx: the participant's, for layers() */
    Ramses19FileScene(ramses::RamsesClient &client, const Ramses19Types &types,
                      const std::map<int, ramses::Effect *> &effects, const ramses::Vector2fArray &pos,
                      const ramses::UInt16Array &idx)
        : client_(client), types_(types), effects_(effects), pos_(pos), idx_(idx)
    {
    }

    ~Ramses19FileScene() override
    {
        if (scene_) {
            if (scene_->isPublished())
                scene_->unpublish();
            client_.destroy(*scene_);
        }
        for (const ramses::RamsesObject *r : made_)
            client_.destroy(*static_cast<const ramses::Resource *>(r));
    }

    bool load(const TargetSpec &t, const std::string &file, const std::vector<std::string> &res, std::string *err)
    {
        ramses::ResourceFileDescriptionSet set;
        for (const std::string &r : res) {
            files_.emplace_back(new ramses::ResourceFileDescription(r.c_str()));
            set.add(*files_.back());
        }
        const std::set<const ramses::RamsesObject *> before = resources(client_, types_.resource);
        scene_ = client_.loadSceneFromFile(file.c_str(), set);
        for (const ramses::RamsesObject *r : resources(client_, types_.resource))
            if (!before.count(r))
                made_.push_back(r);
        if (!scene_) {
            *err = "the scene file did not load";
            return false;
        }
        if (scene_->getSceneId() != t.scene_id) {
            char b[96];
            std::snprintf(b, sizeof b, "the file's scene id is 0x%llx, not the target's 0x%llx",
                          (unsigned long long)scene_->getSceneId(), (unsigned long long)t.scene_id);
            *err = b;
            return false;
        }
        w_ = t.width;
        h_ = t.height;
        collect(Node, types_.node);
        collect(Mesh, types_.mesh);
        collect(Camera, types_.persp);
        collect(Camera, types_.ortho);
        collect(Pass, types_.pass);
        if (!ok(scene_->flush()) || !ok(scene_->publish(ramses::EScenePublicationMode_LocalAndRemote))) {
            *err = "publish failed";
            return false;
        }
        log_info("ramses: scene file of %zu nodes, %zu meshes, %zu cameras, %zu passes, %zu resources made",
                 objs_[Node].size(), objs_[Mesh].size(), objs_[Camera].size(), objs_[Pass].size(), made_.size());
        return true;
    }

    std::vector<std::string> objects(Kind k) override { return names_[k]; }

    bool visible(int node, bool on) override { return set_tree(*static_cast<ramses::Node *>(objs_[Node][node]), on); }

    bool transform(int node, Transform what, const float v[3]) override
    {
        ramses::Node *n = static_cast<ramses::Node *>(objs_[Node][node]);
        return ok(what == Translation ? n->setTranslation(v[0], v[1], v[2])
                  : what == Scaling   ? n->setScaling(v[0], v[1], v[2])
                                      : n->setRotation(v[0], v[1], v[2]));
    }

    bool viewport(int camera, const uint32_t v[4]) override
    {
        return ok(static_cast<ramses::LocalCamera *>(objs_[Camera][camera])->setViewport(v[0], v[1], v[2], v[3]));
    }

    bool enable(int pass, bool on) override
    {
        return ok(static_cast<ramses::RenderPass *>(objs_[Pass][pass])->setEnabled(on));
    }

    bool uniform(int mesh, const std::string &name, const std::vector<float> &v, std::string *why) override
    {
        ramses::Appearance *app = static_cast<ramses::MeshNode *>(objs_[Mesh][mesh])->getAppearance();
        ramses::UniformInput u;
        if (!app || !find_uniform(app->getEffect(), name, &u)) {
            *why = "no uniform " + name;
            return false;
        }
        if (app->isInputBound(u)) {
            *why = name + " is bound to a data object (--set it)";
            return false;
        }
        const uint32_t type = (uint32_t)u.getDataType();
        int t = 0;
        while (t < 11 && types_.uniform[t] != type)
            t++;
        if (t == 11) {
            *why = name + " takes no numbers";
            return false;
        }
        const uint32_t n = std::min<uint32_t>((uint32_t)v.size() / kWidth[t], u.getElementCount());
        if (!n) {
            *why = name + " takes " + std::to_string(kWidth[t]) + " values";
            return false;
        }
        if (t >= 4 && t < 8) {
            std::vector<int32_t> iv(n * kWidth[t]);
            for (size_t i = 0; i < iv.size(); i++)
                iv[i] = (int32_t)std::lround(v[i]);
            const ramses::status_t s = t == 4   ? app->setInputValueInt32(u, n, iv.data())
                                       : t == 5 ? app->setInputValueVector2i(u, n, iv.data())
                                       : t == 6 ? app->setInputValueVector3i(u, n, iv.data())
                                                : app->setInputValueVector4i(u, n, iv.data());
            return ok(s);
        }
        const float *f = v.data();
        switch (t) {
        case 0: return ok(app->setInputValueFloat(u, n, f));
        case 1: return ok(app->setInputValueVector2f(u, n, f));
        case 2: return ok(app->setInputValueVector3f(u, n, f));
        case 3: return ok(app->setInputValueVector4f(u, n, f));
        case 8: return ok(app->setInputValueMatrix22f(u, n, f));
        case 9: return ok(app->setInputValueMatrix33f(u, n, f));
        default: return ok(app->setInputValueMatrix44f(u, n, f));
        }
    }

    bool data(const std::string &name, const std::vector<float> &v, std::string *why) override
    {
        auto it = data_.find(name);
        if (it == data_.end()) {
            /* ponytail: names a client makes up start the cache over. */
            if (data_.size() >= 1024)
                data_.clear();
            it = data_.emplace(name, scene_->findObjectByName(name.c_str())).first;
        }
        ramses::RamsesObject *o = it->second;
        const uint32_t type = o ? (uint32_t)o->getType() : 0;
        int t = 0;
        while (o && t < 5 && types_.data[t] != type)
            t++;
        if (!o || t == 5) {
            *why = o ? name + " is not a data object of numbers" : "no data object " + name;
            return false;
        }
        if (v.size() < kDataWidth[t]) {
            *why = name + " takes " + std::to_string(kDataWidth[t]) + " values";
            return false;
        }
        switch (t) {
        case 0: return ok(static_cast<ramses::DataFloat *>(o)->setValue(v[0]));
        case 1: return ok(static_cast<ramses::DataInt32 *>(o)->setValue((int32_t)std::lround(v[0])));
        case 2: return ok(static_cast<ramses::DataVector2f *>(o)->setValue(v[0], v[1]));
        case 3: return ok(static_cast<ramses::DataVector3f *>(o)->setValue(v[0], v[1], v[2]));
        default: return ok(static_cast<ramses::DataVector4f *>(o)->setValue(v[0], v[1], v[2], v[3]));
        }
    }

    bool flush(uint64_t tag) override
    {
        const ramses::status_t s = scene_->flush(ramses::ESceneFlushMode_Synchronous, tag);
        if (!ok(s) && !flush_failing_)
            log_error("ramses: flush %llu: %s", (unsigned long long)tag, client_.getStatusMessage(s));
        flush_failing_ = !ok(s);
        return ok(s);
    }

    std::unique_ptr<Graph> layers(const int32_t *order, std::string *err) override
    {
        int64_t base = 0;
        if (order) {
            base = *order;
        } else {
            for (ramses::RamsesObject *p : objs_[Pass])
                base = std::max<int64_t>(base, (int64_t) static_cast<ramses::RenderPass *>(p)->getRenderOrder() + 1);
        }
        /* ponytail: room for 1024 passes above it; a file at the top of the range draws nothing over itself */
        base = std::min<int64_t>(base, INT32_MAX - 1024);
        std::unique_ptr<Ramses19Graph> g(new Ramses19Graph(client_, effects_, pos_, idx_));
        if (!g->over(scene_, w_, h_, (int32_t)base, err))
            return nullptr;
        log_info("ramses: layers over the scene file from render order %lld", (long long)base);
        return std::unique_ptr<Graph>(g.release());
    }

private:
    void collect(Kind k, uint32_t type)
    {
        ramses::SceneObjectIterator it(*scene_, ramses::ERamsesObjectType(type));
        while (ramses::RamsesObject *o = it.getNext()) {
            objs_[k].push_back(o);
            names_[k].push_back(o->getName() ? o->getName() : "");
        }
    }

    ramses::RamsesClient &client_;
    const Ramses19Types &types_;
    const std::map<int, ramses::Effect *> &effects_;
    const ramses::Vector2fArray &pos_;
    const ramses::UInt16Array &idx_;
    uint32_t w_ = 0, h_ = 0;
    /* The descriptions outlive the scene loaded from them. */
    std::vector<std::unique_ptr<ramses::ResourceFileDescription>> files_;
    ramses::Scene *scene_ = nullptr;
    std::vector<const ramses::RamsesObject *> made_;
    std::vector<ramses::RamsesObject *> objs_[kKinds];
    std::vector<std::string> names_[kKinds];
    std::map<std::string, ramses::RamsesObject *> data_;
    bool flush_failing_ = false;
};

}  // namespace

Ramses19Gpu::Ramses19Gpu(ramses::RamsesClient &client) : client_(client)
{
}

Ramses19Gpu::~Ramses19Gpu()
{
    for (auto &kv : effects_)
        client_.destroy(*kv.second);
    if (pos_) client_.destroy(*pos_);
    if (idx_) client_.destroy(*idx_);
}

bool Ramses19Gpu::init(std::string *err)
{
    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    pos_ = client_.createConstVector2fArray(4, quad);
    idx_ = client_.createConstUInt16Array(6, idx);
    if (!pos_ || !idx_) {
        *err = "unit quad geometry refused";
        return false;
    }
    return true;
}

int Ramses19Gpu::effect(const char *vs, const char *fs, std::string *err)
{
    static const struct {
        const char *name;
        ramses::EEffectUniformSemantic semantic;
    } semantics[] = {{"u_mv", ramses::EEffectUniformSemantic_ModelViewMatrix},
                     {"u_m", ramses::EEffectUniformSemantic_ModelMatrix},
                     {"u_v", ramses::EEffectUniformSemantic_ViewMatrix},
                     {"u_p", ramses::EEffectUniformSemantic_ProjectionMatrix},
                     {"u_normal", ramses::EEffectUniformSemantic_NormalMatrix}};
    ramses::EffectDescription d;
    d.setVertexShader(vs);
    d.setFragmentShader(fs);
    d.setUniformSemantic("u_mvp", ramses::EEffectUniformSemantic_ModelViewProjectionMatrix);
    for (const auto &s : semantics)
        if (mentions(vs, s.name) || mentions(fs, s.name))
            d.setUniformSemantic(s.name, s.semantic);
    ramses::Effect *fx = client_.createEffect(d, ramses::ResourceCacheFlag_DoNotCache);
    if (!fx) {
        *err = "effect did not compile";
        return 0;
    }
    effects_[next_] = fx;
    return next_++;
}

void Ramses19Gpu::drop_effect(int effect)
{
    auto it = effects_.find(effect);
    if (it == effects_.end())
        return;
    client_.destroy(*it->second);
    effects_.erase(it);
}

unsigned Ramses19Gpu::attributes(int effect) const
{
    auto it = effects_.find(effect);
    return it == effects_.end() ? 0 : effect_attributes(*it->second);
}

std::unique_ptr<Graph> Ramses19Gpu::scene(const TargetSpec &t, std::string *err)
{
    std::unique_ptr<Ramses19Graph> g(new Ramses19Graph(client_, effects_, *pos_, *idx_));
    if (!g->build(t, err))
        return nullptr;
    return std::unique_ptr<Graph>(g.release());
}

std::unique_ptr<FileScene> Ramses19Gpu::load(const TargetSpec &t, const std::string &file,
                                             const std::vector<std::string> &res, std::string *err)
{
    if (!typed_)
        typed_ = learn(client_, &types_, err);
    if (!typed_)
        return nullptr;
    std::unique_ptr<Ramses19FileScene> s(new Ramses19FileScene(client_, types_, effects_, *pos_, *idx_));
    if (!s->load(t, file, res, err))
        return nullptr;
    return std::unique_ptr<FileScene>(s.release());
}

}  // namespace crs
