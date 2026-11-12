/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The renderer as the layers see it: quads drawn by effects, persistent
 * texture buffers written in place, uniforms, 3D views of meshes, animations
 * the renderer plays by itself, and one flush that carries a tick's changes.
 * All content the service draws is built from these, or is a scene file a
 * client has it load whole (FileScene), so a display generation
 * is one implementation of Gpu and Graph: graph_ramses19.cpp for iDrive 7
 * (RAMSES 19, the KOMBI's renderer), graph_ramses27.cpp for iDrive 8
 * (RAMSES 27, the cluster's; README.md "iDrive 8").
 *
 * Coordinates are the target's pixels from its top-left corner. A buffer is
 * written in rectangles of tightly packed rows. Every handle is a positive
 * int, unique within its Graph across every kind of object; 0 means refused.
 * Nothing a call changes is shown before flush(). */
#ifndef CRS_GRAPH_H
#define CRS_GRAPH_H

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

#include "slots.h"

namespace crs {

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
};
inline bool operator==(const Rect &a, const Rect &b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
inline bool operator!=(const Rect &a, const Rect &b) { return !(a == b); }

/* ETC2RGBA: ETC2 colour with EAC alpha, 16 bytes a 4x4 block
 * (GL_COMPRESSED_RGBA8_ETC2_EAC); an immutable_texture(), never a buffer(). */
enum class TexFormat { R8, RGBA8, ETC2RGBA };
/* Bytes of a w x h texture of format f (ETC2RGBA: w and h multiples of 4). */
inline size_t tex_bytes(TexFormat f, uint32_t w, uint32_t h)
{
    if (f == TexFormat::ETC2RGBA)
        return (size_t)(w / 4) * (h / 4) * 16;
    return (size_t)w * h * (f == TexFormat::R8 ? 1 : 4);
}
enum class Sampling { Nearest, Bilinear };
enum class Wrap { Clamp, Repeat };

enum class UniformType { Float, Vec2, Vec3, Vec4, Mat4 };
/* Floats in one element of t (a mat4 is 16, in GLSL's column order). */
inline uint32_t uniform_width(UniformType t)
{
    switch (t) {
    case UniformType::Vec2: return 2;
    case UniformType::Vec3: return 3;
    case UniformType::Vec4: return 4;
    case UniformType::Mat4: return 16;
    default: return 1;
    }
}

/* How a quad turns and scales about its pivot: degrees clockwise on screen,
 * scale factors, the pivot in pixels from the quad's top-left. */
struct Turn {
    float rotate = 0;
    float sx = 1, sy = 1;
    float px = 0, py = 0;
};

/* A 3D view's projection, its aspect that of the view's rect: perspective
 * over fov degrees vertically, or orthographic over ortho_h units. */
struct Projection {
    bool ortho = false;
    float fov = 30, ortho_h = 2;
    float near = 0.1f, far = 100;
};

/* A node's place in its view: translation, rotation about x, y and z in
 * degrees (right-handed: positive turns counter-clockwise seen from the
 * axis's positive end), scale. A camera looks down its -z. */
struct Pose {
    float t[3] = {0, 0, 0};
    float r[3] = {0, 0, 0};
    float s[3] = {1, 1, 1};
};

enum class Cull { None, Back, Front };
enum class Blend { Alpha, Add, None };
/* How a part is drawn: depth tested (less or equal) and written, which faces
 * are culled, and how it blends over what is below. */
struct PartLook {
    bool depth = true;
    Cull cull = Cull::Back;
    Blend blend = Blend::Alpha;
};

/* Vertex attributes, as bits of a mask: what a part's geometry carries and
 * what its vertex shader reads (a_position, a_normal, a_texcoord, a_color). */
const unsigned kAttrNormal = 1, kAttrTexcoord = 2, kAttrColor = 4, kAttrPosition = 8;

/* A part's vertices and triangles. Positions and normals are xyz, texture
 * coordinates uv, colours rgba, one per vertex; an attribute it lacks is
 * null. */
struct Geometry {
    uint32_t vertices = 0, indices = 0;
    const float *position = nullptr, *normal = nullptr, *texcoord = nullptr, *color = nullptr;
    const uint16_t *index = nullptr;
};

enum class Animated { Rotate, Translate, Scale, Uniform };
/* Keys the renderer interpolates by itself: width floats a key, at times in
 * milliseconds from the start, ascending. */
struct Animation {
    Animated property = Animated::Rotate;
    std::string uniform;   /* property Uniform */
    uint32_t width = 1;
    bool loop = false;     /* from the first key again after the last */
    bool step = false;     /* each key's value held until the next */
    std::vector<uint32_t> t;
    std::vector<float> v;
};

/* One target's scene. */
class Graph {
public:
    virtual ~Graph() {}
    /* A persistent texture buffer and its sampler (clamped). */
    virtual int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s) = 0;
    /* Likewise, with the sampler's wrap. */
    virtual int buffer(TexFormat f, uint32_t w, uint32_t h, Sampling s, Wrap wrap) = 0;
    virtual bool write(int buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *px) = 0;
    /* An immutable texture of n bytes (one mip level) and its sampler,
     * handled as a buffer (texture(), drop_buffer()) but never written.
     * Only ETC2RGBA, only where the backend takes it: 0 otherwise. */
    virtual int immutable_texture(TexFormat, uint32_t, uint32_t, Sampling, Wrap, const uint8_t *, size_t) { return 0; }
    /* A quad over r drawn by effect, hidden until show(). Blended over what
     * is below (source alpha), no depth test. */
    virtual int quad(int effect, const Rect &r) = 0;
    virtual bool place(int quad, const Rect &r) = 0;
    /* Turns and scales the quad within its rect's place. */
    virtual bool turn(int quad, const Turn &t) = 0;
    /* texture(), uniform() and uniform_array() take a quad or a part. */
    virtual bool texture(int quad, const char *uniform, int buf) = 0;
    /* A float, vec2, vec3 or vec4 uniform (n 1-4). */
    virtual bool uniform(int quad, const char *name, const float *v, int n) = 0;
    /* elements values of type t: an array uniform, or one value. */
    virtual bool uniform_array(int quad, const char *name, UniformType t, const float *v, uint32_t elements) = 0;
    /* show(), order() and place() take a quad or a view. */
    virtual bool show(int quad, bool on) = 0;
    /* Draw order within the scene: higher is drawn later, on top. Quads and
     * views share it; a view drawn between quads splits them into separate
     * passes when the backend needs that, so the order holds exactly. */
    virtual bool order(int quad, int32_t order) = 0;
    virtual void drop_quad(int quad) = 0;
    virtual void drop_buffer(int buf) = 0;

    /* A 3D view over r with its own camera, drawn in a pass of its own that
     * clears depth (when a part tests it), never colour. Hidden until
     * show(). */
    virtual int view(const Rect &r, const Projection &p, const Pose &camera) = 0;
    virtual bool camera(int view, const Projection &p, const Pose &camera) = 0;
    virtual void drop_view(int view) = 0;
    /* A part of the view drawn by effect, at the identity pose; drawn once
     * it has geometry. */
    virtual int part(int view, int effect, const PartLook &look) = 0;
    /* Replaces the part's geometry: immutable resources, made once a call.
     * False when the effect reads an attribute g lacks. */
    virtual bool geometry(int part, const Geometry &g) = 0;
    virtual bool pose(int part, const Pose &p) = 0;
    virtual void drop_part(int part) = 0;

    /* Plays a by the renderer, nothing sent per frame, on:
     * - a quad: Rotate 1 value (degrees clockwise about its pivot), Translate
     *   3 (pixels right, down, out, from its place), Scale 3 (about its
     *   pivot);
     * - a part: Rotate, Translate, Scale 3 values, as pose();
     * - a view's camera: Rotate, Translate 3 values, as camera();
     * - the uniform of a quad or a part: 1, 3 or 4 values (float, vec3,
     *   vec4).
     * Returns the animation's handle, 0 when refused. A stopped animation
     * leaves its property at its last value; dropping its node stops it. */
    virtual int animate(int node, const Animation &a) = 0;
    virtual void stop(int anim) = 0;

    /* Sends everything changed since the last flush as one update, tagged. */
    virtual bool flush(uint64_t tag) = 0;
};

/* A target's scene loaded from a scene file (Gpu::load): drawn as the file
 * has it, with its own cameras and passes, its objects changed by name
 * (scene_edit.h). objects() lists each kind's names; an object's handle is
 * its place in that list, valid while the scene is. Nothing a call changes
 * is shown before flush(). */
class FileScene {
public:
    enum Kind { Node, Mesh, Camera, Pass, kKinds };
    enum Transform { Translation, Scaling, Rotation };
    virtual ~FileScene() {}
    virtual std::vector<std::string> objects(Kind k) = 0;
    /* The node and every node under it. */
    virtual bool visible(int node, bool on) = 0;
    /* Rotation in degrees, about x, y and z as the scene's own nodes turn. */
    virtual bool transform(int node, Transform what, const float v[3]) = 0;
    /* x, y, w, h in pixels from the bottom-left, as the file's own. */
    virtual bool viewport(int camera, const uint32_t v[4]) = 0;
    virtual bool enable(int pass, bool on) = 0;
    /* The uniform called name of the mesh's appearance: as many whole
     * elements of v as it takes, ints rounded. False with *why: the mesh has
     * no such uniform, it is bound to a data object, or v is too short. */
    virtual bool uniform(int mesh, const std::string &name, const std::vector<float> &v, std::string *why) = 0;
    /* The data object called name: 1 to 4 values, as its type takes. */
    virtual bool data(const std::string &name, const std::vector<float> &v, std::string *why) = 0;
    virtual bool flush(uint64_t tag) = 0;
};

/* The participant: effects (shared by every scene) and the scenes. */
class Gpu {
public:
    virtual ~Gpu() {}
    /* The target's scene loaded from a scene file and its resource files
     * (real paths, checked: scene_file.h), flushed and published; null with
     * *err when it does not load or its scene id is not the target's. */
    virtual std::unique_ptr<FileScene> load(const TargetSpec &, const std::string &,
                                            const std::vector<std::string> &, std::string *err)
    {
        *err = "this display takes no scene files";
        return nullptr;
    }
    /* Compiles an effect: GLSL ES 1.00 (the HMI's own effects are). The
     * renderer fills these uniforms where a shader declares them: u_mvp,
     * u_mv, u_m, u_v, u_p (mat4: model-view-projection, model-view, model,
     * view, projection) and u_normal (mat4: the inverse transpose of u_mv).
     * A quad's vertex shader gets a_position (the unit quad, 0-1); the
     * service's default one passes v_uv with v = 0 at the quad's top. */
    virtual int effect(const char *vs, const char *fs, std::string *err) = 0;
    virtual void drop_effect(int effect) = 0;
    /* The vertex attributes the effect reads (kAttr* bits). */
    virtual unsigned attributes(int effect) const = 0;
    /* The target's scene, empty, flushed and published. */
    virtual std::unique_ptr<Graph> scene(const TargetSpec &t, std::string *err) = 0;
};

/* The default vertex shader: a_position over the quad, v_uv from its
 * top-left. */
extern const char *const kQuadVertexShader;

/* The render passes a backend with ordered passes draws the scene in: each
 * view in a pass of its own, and the quads between two views sharing one.
 * views lists, for the scene's quads and views sorted by draw order, which
 * are views; returns each one's pass, 0 the first, in render order. */
std::vector<int> plan_passes(const std::vector<bool> &views);

}  // namespace crs

#endif
