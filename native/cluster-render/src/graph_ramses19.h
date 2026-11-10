/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The iDrive 7 renderer: graph.h on the unit's RAMSES 19 client library
 * (../abi/ABI.md), whose scenes the KOMBI's renderer draws.
 *
 * Each target is one scene with a pixel-space orthographic camera over the
 * target. Every quad is a mesh node (the shared unit quad, an appearance of
 * its effect) under two group nodes that place and turn it, ordered within
 * the render group of its 2D pass. Without views there is one 2D pass. Each
 * view is a render pass of its own with a perspective or orthographic camera
 * and a render group of its parts' mesh nodes, whose geometry is immutable
 * resources; the passes' render order follows the draw order (plan_passes).
 * Buffers are Texture2DBuffers with one sampler each. Animations are one
 * real-time animation system per scene, made at the first one, played by the
 * renderer; an animation that stops takes the system with it, and the others
 * are made again in a new one. A scene file is the client library's own
 * scene and resources, as the file has them. */
#ifndef CRS_GRAPH_RAMSES19_H
#define CRS_GRAPH_RAMSES19_H

#include <map>

#include "graph.h"

namespace ramses {
class RamsesClient;
class Effect;
class Vector2fArray;
class UInt16Array;
}  // namespace ramses

namespace crs {

/* The library's object types and uniform data types, which its headers do
 * not give, learned once (graph_ramses19.cpp). */
struct Ramses19Types {
    uint32_t node = 0, mesh = 0, persp = 0, ortho = 0, pass = 0, resource = 0;
    uint32_t data[5] = {};      /* DataFloat, DataInt32, DataVector2f, DataVector3f, DataVector4f */
    uint32_t uniform[11] = {};  /* float, vec2-4, int, ivec2-4, mat2-4 */
};

class Ramses19Gpu : public Gpu {
public:
    explicit Ramses19Gpu(ramses::RamsesClient &client);
    ~Ramses19Gpu() override;
    /* The unit quad's vertices and indices, shared by every scene. */
    bool init(std::string *err);

    int effect(const char *vs, const char *fs, std::string *err) override;
    void drop_effect(int effect) override;
    unsigned attributes(int effect) const override;
    std::unique_ptr<Graph> scene(const TargetSpec &t, std::string *err) override;
    /* The first one learns the types. */
    std::unique_ptr<FileScene> load(const TargetSpec &t, const std::string &file, const std::vector<std::string> &res,
                                    std::string *err) override;
    /* What the first load learned (for tests). */
    const Ramses19Types &types() const { return types_; }

private:
    ramses::RamsesClient &client_;
    const ramses::Vector2fArray *pos_ = nullptr;
    const ramses::UInt16Array *idx_ = nullptr;
    std::map<int, ramses::Effect *> effects_;
    int next_ = 1;
    Ramses19Types types_;
    bool typed_ = false;
};

}  // namespace crs

#endif
