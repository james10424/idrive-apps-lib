/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
#include "graph_ramses27.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "ramses-client-api/AnimatedProperty.h"
#include "ramses-client-api/Animation.h"
#include "ramses-client-api/AnimationSequence.h"
#include "ramses-client-api/AnimationSystemRealTime.h"
#include "ramses-client-api/Appearance.h"
#include "ramses-client-api/ArrayResource.h"
#include "ramses-client-api/AttributeInput.h"
#include "ramses-client-api/Effect.h"
#include "ramses-client-api/EffectDescription.h"
#include "ramses-client-api/GeometryBinding.h"
#include "ramses-client-api/MeshNode.h"
#include "ramses-client-api/OrthographicCamera.h"
#include "ramses-client-api/PerspectiveCamera.h"
#include "ramses-client-api/RamsesClient.h"
#include "ramses-client-api/RenderGroup.h"
#include "ramses-client-api/RenderPass.h"
#include "ramses-client-api/Scene.h"
#include "ramses-client-api/SceneConfig.h"
#include "ramses-client-api/SplineLinearFloat.h"
#include "ramses-client-api/SplineLinearVector3f.h"
#include "ramses-client-api/SplineLinearVector4f.h"
#include "ramses-client-api/SplineStepFloat.h"
#include "ramses-client-api/SplineStepVector3f.h"
#include "ramses-client-api/SplineStepVector4f.h"
#include "ramses-client-api/Texture2D.h"
#include "ramses-client-api/Texture2DBuffer.h"
#include "ramses-client-api/TextureSampler.h"
#include "ramses-client-api/UniformInput.h"

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
    app.setDepthWrite(ramses::EDepthWrite_Disabled);
    app.setCullingMode(ramses::ECullMode_Disabled);
}

bool part_look(ramses::Appearance &app, const PartLook &look)
{
    bool good = ok(app.setDepthFunction(look.depth ? ramses::EDepthFunc_LessEqual : ramses::EDepthFunc_Disabled)) &&
                ok(app.setDepthWrite(look.depth ? ramses::EDepthWrite_Enabled : ramses::EDepthWrite_Disabled)) &&
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

ramses::EVisibilityMode visible(bool on) { return on ? ramses::EVisibilityMode::Visible : ramses::EVisibilityMode::Invisible; }

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

/* The effect, with the renderer filling the uniform semantics it mentions
 * (graph.h Gpu::effect), compiled in scene. */
ramses::Effect *compile(ramses::Scene &scene, const char *vs, const char *fs)
{
    static const struct {
        const char *name;
        ramses::EEffectUniformSemantic semantic;
    } semantics[] = {{"u_mv", ramses::EEffectUniformSemantic::ModelViewMatrix},
                     {"u_m", ramses::EEffectUniformSemantic::ModelMatrix},
                     {"u_v", ramses::EEffectUniformSemantic::ViewMatrix},
                     {"u_p", ramses::EEffectUniformSemantic::ProjectionMatrix},
                     {"u_normal", ramses::EEffectUniformSemantic::NormalMatrix}};
    ramses::EffectDescription d;
    d.setVertexShader(vs);
    d.setFragmentShader(fs);
    d.setUniformSemantic("u_mvp", ramses::EEffectUniformSemantic::ModelViewProjectionMatrix);
    for (const auto &s : semantics)
        if (mentions(vs, s.name) || mentions(fs, s.name))
            d.setUniformSemantic(s.name, s.semantic);
    return scene.createEffect(d, ramses::ResourceCacheFlag_DoNotCache);
}

}  // namespace

class Ramses27Graph : public Graph {
public:
    Ramses27Graph(ramses::RamsesClient &client, Ramses27Gpu &gpu) : client_(client), gpu_(gpu) { gpu_.attach(this); }

    ~Ramses27Graph() override
    {
        gpu_.detach(this);
        if (!scene_)
            return;
        if (scene_->isPublished())
            gpu_.retire(scene_);   /* until its offer has stopped */
        else
            client_.destroy(*scene_);   /* and every object in it, its effects and resources too */
    }

    bool build(const TargetSpec &t, std::string *err)
    {
        scene_ = client_.createScene(ramses::sceneId_t(t.scene_id), ramses::SceneConfig(), t.name.c_str());
        if (!scene_) {
            *err = "createScene failed";
            return false;
        }
        w_ = t.width;
        h_ = t.height;
        static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
        static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
        pos_ = scene_->createArrayResource(ramses::EDataType::Vector2F, 4, quad);
        idx_ = scene_->createArrayResource(ramses::EDataType::UInt16, 6, idx);
        cam2d_ = scene_->createOrthographicCamera("cam");
        Segment s;
        s.pass = scene_->createRenderPass("pass");
        s.group = scene_->createRenderGroup("group");
        if (!pos_ || !idx_ || !cam2d_ || !s.pass || !s.group) {
            *err = "scene objects failed";
            return false;
        }
        /* Pixel space over the whole target, y up; rects are turned over in
         * place(). No pass clears colour (graph_ramses27.h). */
        cam2d_->setFrustum(0.0f, (float)w_, 0.0f, (float)h_, 0.1f, 10.0f);
        cam2d_->setViewport(0, 0, w_, h_);
        s.pass->setCamera(*cam2d_);
        s.pass->addRenderGroup(*s.group);
        s.pass->setClearFlags(ramses::EClearFlags_None);
        segments_.push_back(s);
        if (!ok(scene_->flush()) || !ok(scene_->publish(ramses::EScenePublicationMode_LocalAndRemote))) {
            *err = "publish failed";
            return false;
        }
        return true;
    }

    /* The effect dropped from the Gpu: no quad or part uses it any more
     * (the compositor drops those first), so this scene's copy goes. */
    void forget(int effect)
    {
        auto it = fx_.find(effect);
        if (it == fx_.end())
            return;
        scene_->destroy(*it->second);
        fx_.erase(it);
    }

    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s) override { return buffer(f, w, h, s, Wrap::Clamp); }

    int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap) override
    {
        const ramses::ETextureAddressMode mode =
            wrap == Wrap::Repeat ? ramses::ETextureAddressMode_Repeat : ramses::ETextureAddressMode_Clamp;
        const ramses::ETextureSamplingMethod method =
            s == Sampling::Nearest ? ramses::ETextureSamplingMethod_Nearest : ramses::ETextureSamplingMethod_Linear;
        ramses::Texture2DBuffer *b = scene_->createTexture2DBuffer(
            f == TexFormat::R8 ? ramses::ETextureFormat::R8 : ramses::ETextureFormat::RGBA8, w, h, 1);
        ramses::TextureSampler *smp = b ? scene_->createTextureSampler(mode, mode, method, method, *b) : nullptr;
        if (!smp) {
            if (b)
                scene_->destroy(*b);
            log_error("ramses: %ux%u %s texture buffer refused", w, h, f == TexFormat::R8 ? "R8" : "RGBA8");
            return 0;
        }
        buffers_[next_] = Buffer{b, smp};
        return next_++;
    }

    /* ETC2RGBA: a scene resource, sent with the next flush. */
    int immutable_texture(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap, const uint8_t *data,
                          size_t n) override
    {
        if (f != TexFormat::ETC2RGBA)
            return 0;
        const ramses::ETextureAddressMode mode =
            wrap == Wrap::Repeat ? ramses::ETextureAddressMode_Repeat : ramses::ETextureAddressMode_Clamp;
        const ramses::ETextureSamplingMethod method =
            s == Sampling::Nearest ? ramses::ETextureSamplingMethod_Nearest : ramses::ETextureSamplingMethod_Linear;
        const ramses::MipLevelData mip((uint32_t)n, data);
        ramses::Texture2D *tex = scene_->createTexture2D(ramses::ETextureFormat::ETC2RGBA, w, h, 1, &mip);
        ramses::TextureSampler *smp = tex ? scene_->createTextureSampler(mode, mode, method, method, *tex) : nullptr;
        if (!smp) {
            log_error("ramses: %ux%u ETC2RGBA texture of %zu bytes refused by %s", w, h, n,
                      tex ? "createTextureSampler" : "createTexture2D");
            if (tex)
                scene_->destroy(*tex);
            return 0;
        }
        Buffer b{nullptr, smp};
        b.tex = tex;
        buffers_[next_] = b;
        return next_++;
    }

    bool write(int buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px) override
    {
        auto it = buffers_.find(buf);
        if (it == buffers_.end() || !it->second.buffer)
            return false;
        const ramses::status_t s = it->second.buffer->updateData(0, x, y, w, h, px);
        if (!ok(s) && !write_failing_)
            log_error("ramses: updateData %ux%u at %u,%u: %s", w, h, x, y, client_.getStatusMessage(s));
        write_failing_ = !ok(s);
        return ok(s);
    }

    /* A quad is its mesh under two nodes: place (translation to the pivot's
     * place in the target) and turn (rotation and scale about the pivot,
     * and an animated offset); the mesh is offset by the pivot and scaled
     * to the rect. */
    int quad(int effect, const Rect &r) override
    {
        ramses::Effect *fx = effect_of(effect);
        if (!fx)
            return 0;
        ramses::Appearance *app = scene_->createAppearance(*fx);
        ramses::GeometryBinding *geo = scene_->createGeometryBinding(*fx);
        ramses::MeshNode *mesh = scene_->createMeshNode();
        ramses::Node *placed = scene_->createNode();
        ramses::Node *turned = scene_->createNode();
        ramses::AttributeInput ain;
        if (!app || !geo || !mesh || !placed || !turned || !ok(fx->findAttributeInput("a_position", ain)) ||
            !ok(geo->setInputBuffer(ain, *pos_)) || !ok(geo->setIndices(*idx_)) || !ok(mesh->setParent(*turned)) ||
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
        mesh->setVisibility(visible(false));
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
        q.effect = fx;
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
            return ok(v->second.root->setVisibility(visible(on && !v->second.offscreen)));
        }
        auto q = quads_.find(quad);
        return q != quads_.end() && ok(q->second.mesh->setVisibility(visible(on)));
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
            scene_->destroy(*b->second.tex);
        buffers_.erase(b);
    }

    int view(const Rect &r, const Projection &p, const Pose &camera) override
    {
        View v;
        v.pass = scene_->createRenderPass();
        v.group = scene_->createRenderGroup();
        v.root = scene_->createNode();
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
        ramses::Effect *fx = v == views_.end() ? nullptr : effect_of(effect);
        if (!fx)
            return 0;
        ramses::Appearance *app = scene_->createAppearance(*fx);
        ramses::GeometryBinding *geo = scene_->createGeometryBinding(*fx);
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
        p.effect = fx;
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
        auto array = [this](ramses::EDataType t, uint32_t n, const void *data) {
            return scene_->createArrayResource(t, n, data);
        };
        ramses::ArrayResource *pos = array(ramses::EDataType::Vector3F, g.vertices, g.position);
        ramses::ArrayResource *nor = reads & kAttrNormal ? array(ramses::EDataType::Vector3F, g.vertices, g.normal) : nullptr;
        ramses::ArrayResource *uv =
            reads & kAttrTexcoord ? array(ramses::EDataType::Vector2F, g.vertices, g.texcoord) : nullptr;
        ramses::ArrayResource *col = reads & kAttrColor ? array(ramses::EDataType::Vector4F, g.vertices, g.color) : nullptr;
        ramses::ArrayResource *idx = array(ramses::EDataType::UInt16, g.indices, g.index);
        std::vector<ramses::ArrayResource *> res;
        for (ramses::ArrayResource *r : {pos, nor, uv, col, idx})
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
            for (ramses::ArrayResource *r : res)
                scene_->destroy(*r);
            for (ramses::ArrayResource *r : p.res)
                scene_->destroy(*r);
            p.res.clear();
            return false;
        }
        /* The binding holds the new ones: the old ones go. */
        for (ramses::ArrayResource *r : p.res)
            scene_->destroy(*r);
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
        for (ramses::ArrayResource *r : p.res)
            scene_->destroy(*r);
        const int view = p.view;
        parts_.erase(it);
        if (v != views_.end())
            clear_depth(v->second, view);
    }

    int animate(int node, const Animation &a) override
    {
        if (a.t.empty() || a.v.size() != a.t.size() * a.width)
            return 0;
        if (!anim_)
            anim_ = scene_->createRealTimeAnimationSystem(ramses::EAnimationSystemFlags_Default, "anim");
        if (!anim_)
            return 0;
        auto q = quads_.find(node);
        auto p = parts_.find(node);
        auto v = views_.find(node);
        ramses::Node *target = q != quads_.end()   ? q->second.turned
                               : p != parts_.end() ? (ramses::Node *)p->second.mesh
                               : v != views_.end() ? (ramses::Node *)v->second.cam
                                                   : nullptr;
        if (!target)
            return 0;
        const bool on_quad = q != quads_.end();
        /* The renderer's conventions: y up, so down is negative y, and its
         * rotations (the three-angle setRotation, as RAMSES 19's) are
         * left-handed, so a quad's clockwise turn is positive z and the
         * right-handed angles of parts and cameras are negated. */
        std::vector<float> vals = a.v;
        if (!on_quad && a.property == Animated::Rotate)
            for (float &x : vals)
                x = -x;
        if (on_quad && a.property == Animated::Translate)
            for (size_t k = 1; k < vals.size(); k += 3)
                vals[k] = -vals[k];

        Anim an;
        an.node = node;
        an.spline = spline(a, vals);
        if (a.property == Animated::Uniform) {
            ramses::Appearance *app = nullptr;
            const ramses::Effect *fx = look(node, &app);
            ramses::UniformInput in;
            if (an.spline && fx && ok(fx->findUniformInput(a.uniform.c_str(), in)))
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
            !ok(anim_->updateLocalTime()) || !ok(an.seq->start())) {
            log_error("ramses: animation refused");
            destroy_anim(an);
            return 0;
        }
        anims_[next_] = an;
        return next_++;
    }

    void stop(int anim) override
    {
        auto it = anims_.find(anim);
        if (it == anims_.end())
            return;
        it->second.seq->stop();
        destroy_anim(it->second);
        anims_.erase(it);
    }

    bool flush(uint64_t tag) override
    {
        if (layout_dirty_)
            relayout();
        const ramses::status_t s = scene_->flush(tag);
        if (!ok(s) && !flush_failing_)
            log_error("ramses: flush %llu: %s", (unsigned long long)tag, client_.getStatusMessage(s));
        flush_failing_ = !ok(s);
        return ok(s);
    }

private:
    struct Buffer {
        ramses::Texture2DBuffer *buffer;   /* null for an immutable texture */
        ramses::TextureSampler *sampler;
        ramses::Texture2D *tex = nullptr;
    };
    struct Quad {
        ramses::MeshNode *mesh = nullptr;
        ramses::Node *placed = nullptr, *turned = nullptr;
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
        ramses::Camera *cam = nullptr;
        ramses::Node *root = nullptr;
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
        std::vector<ramses::ArrayResource *> res;
    };
    struct Anim {
        int node = 0;
        ramses::AnimationSequence *seq = nullptr;
        ramses::Animation *anim = nullptr;
        ramses::AnimatedProperty *prop = nullptr;
        ramses::Spline *spline = nullptr;
    };

    /* This scene's copy of a Gpu effect, compiled at its first use. */
    ramses::Effect *effect_of(int effect)
    {
        auto it = fx_.find(effect);
        if (it != fx_.end())
            return it->second;
        const Ramses27Gpu::Source *src = gpu_.source(effect);
        ramses::Effect *fx = src ? compile(*scene_, src->vs.c_str(), src->fs.c_str()) : nullptr;
        if (!fx) {
            if (src)
                log_error("ramses: effect %d did not compile in scene %s", effect, scene_->getName());
            return nullptr;
        }
        fx_[effect] = fx;
        return fx;
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
                   ok(v.cam->setViewport(0, 0, 1, 1)) && ok(v.root->setVisibility(visible(false)));
        const float l = -right + 2.0f * right * (float)(x0 - v.r.x) / (float)v.r.w;
        const float rr = -right + 2.0f * right * (float)(x1 - v.r.x) / (float)v.r.w;
        const float t = top - 2.0f * top * (float)(y0 - v.r.y) / (float)v.r.h;
        const float b = top - 2.0f * top * (float)(y1 - v.r.y) / (float)v.r.h;
        return good && ok(v.cam->setFrustum(l, rr, b, t, v.p.near, v.p.far)) &&
               ok(v.cam->setViewport((int32_t)x0, (int32_t)(h_ - y1), (uint32_t)(x1 - x0), (uint32_t)(y1 - y0))) &&
               ok(v.root->setVisibility(visible(v.shown)));
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
                return s && good ? s : drop_spline(s);
            }
            ramses::SplineLinearFloat *s = anim_->createSplineLinearFloat();
            for (size_t k = 0; s && good && k < n; k++)
                good = ok(s->setKey(a.t[k], f[k]));
            return s && good ? s : drop_spline(s);
        }
        if (a.width == 3) {
            if (a.step) {
                ramses::SplineStepVector3f *s = anim_->createSplineStepVector3f();
                for (size_t k = 0; s && good && k < n; k++)
                    good = ok(s->setKey(a.t[k], f[k * 3], f[k * 3 + 1], f[k * 3 + 2]));
                return s && good ? s : drop_spline(s);
            }
            ramses::SplineLinearVector3f *s = anim_->createSplineLinearVector3f();
            for (size_t k = 0; s && good && k < n; k++)
                good = ok(s->setKey(a.t[k], f[k * 3], f[k * 3 + 1], f[k * 3 + 2]));
            return s && good ? s : drop_spline(s);
        }
        if (a.width == 4) {
            if (a.step) {
                ramses::SplineStepVector4f *s = anim_->createSplineStepVector4f();
                for (size_t k = 0; s && good && k < n; k++)
                    good = ok(s->setKey(a.t[k], f[k * 4], f[k * 4 + 1], f[k * 4 + 2], f[k * 4 + 3]));
                return s && good ? s : drop_spline(s);
            }
            ramses::SplineLinearVector4f *s = anim_->createSplineLinearVector4f();
            for (size_t k = 0; s && good && k < n; k++)
                good = ok(s->setKey(a.t[k], f[k * 4], f[k * 4 + 1], f[k * 4 + 2], f[k * 4 + 3]));
            return s && good ? s : drop_spline(s);
        }
        return nullptr;
    }

    ramses::Spline *drop_spline(ramses::Spline *s)
    {
        if (s)
            anim_->destroy(*s);
        return nullptr;
    }

    void destroy_anim(Anim &an)
    {
        if (an.seq) anim_->destroy(*an.seq);
        if (an.anim) anim_->destroy(*an.anim);
        if (an.prop) anim_->destroy(*an.prop);
        if (an.spline) anim_->destroy(*an.spline);
        an = Anim();
    }

    void stop_on(int node)
    {
        std::vector<int> going;
        for (const auto &kv : anims_)
            if (kv.second.node == node)
                going.push_back(kv.first);
        for (int a : going)
            stop(a);
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
            if (s.render_order != kv.first && ok(s.pass->setRenderOrder(kv.first)))
                s.render_order = kv.first;
        }
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].view) {
                View &v = views_[items[i].handle];
                if (v.render_order != pass[i] && ok(v.pass->setRenderOrder(pass[i])))
                    v.render_order = pass[i];
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
    Ramses27Gpu &gpu_;
    ramses::Scene *scene_ = nullptr;
    ramses::ArrayResource *pos_ = nullptr, *idx_ = nullptr;   /* the unit quad */
    ramses::OrthographicCamera *cam2d_ = nullptr;
    ramses::AnimationSystemRealTime *anim_ = nullptr;
    std::vector<Segment> segments_;
    uint32_t w_ = 0, h_ = 0;
    bool write_failing_ = false, flush_failing_ = false;   /* log the first of a run */
    bool layout_dirty_ = false;
    std::map<int, ramses::Effect *> fx_;   /* by Gpu effect handle */
    std::map<int, Buffer> buffers_;
    std::map<int, Quad> quads_;
    std::map<int, View> views_;
    std::map<int, Part> parts_;
    std::map<int, Anim> anims_;
    int next_ = 1;
};

Ramses27Gpu::Ramses27Gpu(ramses::RamsesClient &client) : client_(client)
{
}

Ramses27Gpu::~Ramses27Gpu()
{
    while (!retired_.empty())
        drop_retired(retired_.begin()->first);
    if (scratch_)
        client_.destroy(*scratch_);
}

void Ramses27Gpu::retire(ramses::Scene *scene)
{
    const uint64_t id = scene->getSceneId().getValue();
    drop_retired(id);
    retired_[id] = scene;
    if (offered && !offered(id))
        drop_retired(id);   /* no offer to stop: no stop to wait for */
}

void Ramses27Gpu::drop_retired(uint64_t scene_id)
{
    auto it = retired_.find(scene_id);
    if (it == retired_.end())
        return;
    it->second->unpublish();
    client_.destroy(*it->second);   /* and every object in it */
    retired_.erase(it);
}

bool Ramses27Gpu::init(std::string *err)
{
    scratch_ = client_.createScene(ramses::sceneId_t(kScratchScene), ramses::SceneConfig(), "effects");
    if (!scratch_) {
        *err = "scratch scene refused";
        return false;
    }
    return true;
}

int Ramses27Gpu::effect(const char *vs, const char *fs, std::string *err)
{
    ramses::Effect *fx = compile(*scratch_, vs, fs);
    if (!fx) {
        *err = "effect did not compile";
        const std::string why = scratch_->getLastEffectErrorMessages();
        if (!why.empty())
            *err += ": " + why;
        return 0;
    }
    Source s;
    s.vs = vs;
    s.fs = fs;
    s.attributes = effect_attributes(*fx);
    scratch_->destroy(*fx);
    effects_[next_] = s;
    return next_++;
}

void Ramses27Gpu::drop_effect(int effect)
{
    if (!effects_.erase(effect))
        return;
    for (Ramses27Graph *g : graphs_)
        g->forget(effect);
}

unsigned Ramses27Gpu::attributes(int effect) const
{
    auto it = effects_.find(effect);
    return it == effects_.end() ? 0 : it->second.attributes;
}

const Ramses27Gpu::Source *Ramses27Gpu::source(int effect) const
{
    auto it = effects_.find(effect);
    return it == effects_.end() ? nullptr : &it->second;
}

std::unique_ptr<Graph> Ramses27Gpu::scene(const TargetSpec &t, std::string *err)
{
    if (retired_.count(t.scene_id)) {
        *err = "its last offer is still stopping";
        return nullptr;
    }
    std::unique_ptr<Ramses27Graph> g(new Ramses27Graph(client_, *this));
    if (!g->build(t, err))
        return nullptr;
    return std::unique_ptr<Graph>(g.release());
}

}  // namespace crs
