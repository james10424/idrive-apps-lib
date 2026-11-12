/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* An asset: a texture a client uploads once and its effect layers and mesh
 * parts on the same target sample (bind). It is not drawn and not in the
 * layer stack, so it never makes its target wanted.
 *
 * Like an image layer it keeps its pixels on the CPU: a new scene of its
 * target gets a new buffer, written whole; a client's changes are written
 * in place in the rectangles that changed. Its writes are staged before the
 * layers' in the same flush and spend the shared byte budget. A failed
 * write leaves the buffer unlike the asset (unclean) until it is written
 * whole again; the layers sampling it are not drawn meanwhile.
 *
 * An etc2rgba asset is compressed: it is sent
 * whole, once, and each scene of its target gets an immutable texture made
 * from it at the asset's first stage there. No rectangles, no scratch. A
 * texture the renderer refuses leaves the asset refused(). */
#ifndef CRS_ASSET_H
#define CRS_ASSET_H

#include <stdint.h>
#include <string>
#include <vector>

#include "dirty_tiles.h"
#include "graph.h"
#include "layer.h"
#include "upload_pacer.h"

namespace crs {

struct AssetParams {
    TexFormat format = TexFormat::RGBA8;
    uint32_t w = 0, h = 0;
    Sampling sampling = Sampling::Nearest;
    Wrap wrap = Wrap::Clamp;
};

class Asset {
public:
    static const uint32_t kMaxRects = 16;   /* more pending: written whole */

    Asset(uint32_t id, int target, crk::UploadPacer &pacer, const AssetParams &p);
    /* Why an asset of this size cannot be: it must fit one frame message. */
    static const char *size_error(TexFormat f, uint32_t w, uint32_t h);

    uint32_t id() const { return id_; }
    int target() const { return target_; }
    const AssetParams &params() const { return p_; }
    uint32_t bpp() const { return p_.format == TexFormat::R8 ? 1 : 4; }
    bool compressed() const { return p_.format == TexFormat::ETC2RGBA; }
    size_t bytes() const { return px_.size(); }
    /* What it holds of the service's pixel memory: its pixels, and a
     * rectangle scratch as large unless compressed. */
    uint64_t memory() const { return (uint64_t)px_.size() * (compressed() ? 1 : 2); }
    static uint64_t memory(const AssetParams &p)
    {
        return (uint64_t)tex_bytes(p.format, p.w, p.h) * (p.format == TexFormat::ETC2RGBA ? 1 : 2);
    }
    /* Why the renderer refused its texture; empty when it did not. */
    const std::string &refused() const { return refused_; }

    /* Rectangles of new pixels, each's rows packed one after another;
     * none: data is the whole asset. False with *err when a rectangle is
     * outside it or data does not match. */
    bool update(const std::vector<crk::PixelRect> &rects, const uint8_t *data, size_t n, std::string *err);
    bool have() const { return have_; }

    /* Its buffer in a published scene; false when refused. */
    bool attach(Graph *g);
    /* The scene goes, or the asset: dropped from the scene first when drop. */
    void detach(bool drop);
    bool attached() const { return g_ != nullptr; }
    int buffer() const { return buf_; }
    /* The buffer holds the asset whole. */
    bool clean() const { return g_ && clean_; }

    /* Writes what changed, when the budget allows; the caller flushes. */
    Stage stage(int64_t now_ms);
    void committed(bool ok);
    size_t staged_bytes() const { return staged_; }
    std::string stats() const;

private:
    uint32_t id_;
    int target_;
    crk::UploadPacer &pacer_;
    AssetParams p_;
    std::vector<uint8_t> px_, scratch_;
    std::string refused_;
    std::vector<crk::PixelRect> dirty_;
    bool whole_ = true, have_ = false, clean_ = false, staged_whole_ = false;
    Graph *g_ = nullptr;
    int buf_ = 0;
    size_t staged_ = 0;
    uint64_t writes_ = 0, bytes_ = 0, failed_ = 0;
};

}  // namespace crs

#endif
