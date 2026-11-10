/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
/* The iDrive 8 renderer: graph.h on the unit's RAMSES 27 client library
 * (../abi/ABI27.md), whose scenes the cluster's renderer draws once the
 * cluster takes the content they are offered as (dcsm27_port.h).
 *
 * The scene is built as graph_ramses19.cpp builds it: a pixel-space
 * orthographic camera over the target, each quad a mesh node under two
 * nodes that place and turn it, each view a render pass of its own, the
 * passes in draw order (plan_passes), one real-time animation system per
 * scene. What RAMSES 27 changes:
 * - effects and array resources belong to a scene, not to the client. An
 *   effect is compiled once in a scratch scene that is never published (to
 *   check it and read its attributes) and again in each scene that draws
 *   it, at its first quad or part; dropped, it goes from every scene;
 * - texture buffers are written with updateData, the flush takes only a
 *   tag, visibility is a mode (hidden quads are Invisible: they keep their
 *   resources);
 * - no pass clears colour, the first one included: the cluster draws the
 *   scene into its own area (VERIFIED on the car with icwidget, whose
 *   moving text left no trail);
 * - a scene taken down while its content is offered, or its offer is
 *   stopping, stays published as it was last flushed until the cluster has
 *   accepted the stop (retired, then dropped by drop_retired()), as icwidget
 *   did on the car: unpublished at once, a test unit's stand-in
 *   cluster logged errors unassigning it. No new scene with its id is built
 *   meanwhile: scene() refuses, and the target waits on the slot manager's
 *   ladder. A scene taken down with no offer left (a warm scene, slots.h)
 *   goes at once. */
#ifndef CRS_GRAPH_RAMSES27_H
#define CRS_GRAPH_RAMSES27_H

#include <functional>
#include <map>
#include <set>

#include "graph.h"

namespace ramses {
class RamsesClient;
class Scene;
}  // namespace ramses

namespace crs {

class Ramses27Graph;

class Ramses27Gpu : public Gpu {
public:
    explicit Ramses27Gpu(ramses::RamsesClient &client);
    ~Ramses27Gpu() override;
    /* The scratch scene effects are compiled in. */
    bool init(std::string *err);

    int effect(const char *vs, const char *fs, std::string *err) override;
    void drop_effect(int effect) override;
    unsigned attributes(int effect) const override;
    std::unique_ptr<Graph> scene(const TargetSpec &t, std::string *err) override;

    /* The scene id of the scratch scene: never published, so it collides
     * with no content. */
    static const uint64_t kScratchScene = 0x4d4755ffffffull;

    /* For the graphs. */
    struct Source {
        std::string vs, fs;
        unsigned attributes = 0;
    };
    const Source *source(int effect) const;
    void attach(Ramses27Graph *g) { graphs_.insert(g); }
    void detach(Ramses27Graph *g) { graphs_.erase(g); }
    void retire(ramses::Scene *scene);

    /* The offer of the scene's content is gone: its retired scene is
     * unpublished and destroyed. */
    void drop_retired(uint64_t scene_id);
    /* Whether the scene's content is offered or its offer is stopping
     * (Dcsm27Port::holds); unset: always. */
    std::function<bool(uint64_t scene_id)> offered;

private:
    ramses::RamsesClient &client_;
    ramses::Scene *scratch_ = nullptr;
    std::map<int, Source> effects_;
    std::set<Ramses27Graph *> graphs_;
    std::map<uint64_t, ramses::Scene *> retired_;   /* by scene id */
    int next_ = 1;
};

}  // namespace crs

#endif
