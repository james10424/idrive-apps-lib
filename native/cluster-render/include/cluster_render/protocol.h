/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Robert Nisipeanu */
/* The cluster render service's wire protocol, version 1 (PROTOCOL.md).
 *
 * A stream socket carrying messages in both directions, each
 *
 *     u32 length (little-endian, the bytes after it) | u8 type | body
 *
 * of one of three types:
 * - 'J': a flat JSON object (string and number values only), the control
 *   messages and the service's events;
 * - 'F': a frame of pixels for a layer or an asset:
 *       u32 ref | u64 frame id | u16 rects | rects x u16 {x, y, w, h} | pixels
 *   all little-endian; a video frame has no rects and is the whole 4:2:0
 *   atlas; an image or asset frame's pixels are its rects' rows one rect
 *   after another, or with no rects the whole image;
 * - 'M': a mesh part's geometry:
 *       u32 ref | u32 part | u32 vertices | u32 indices | u8 mask |
 *       f32 positions (xyz) | [f32 normals (xyz)] | [f32 texcoords (uv)] |
 *       [f32 colors (rgba)] | u16 indices
 *   all little-endian; mask 1: normals, 2: texcoords, 4: colors.
 *
 * The parsing here is shared by the service and the C++ client. */
#ifndef CLUSTER_RENDER_PROTOCOL_H
#define CLUSTER_RENDER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace cr {

const int kProtocolVersion = 1;
const char *const kDefaultSocket = "/dev/shm/cluster-render/render.sock";
/* The largest message: a whole 1920x720 RGBA image and its header. */
const size_t kMaxMessage = 1920 * 720 * 4 + 64;

const uint8_t kJson = 'J';
const uint8_t kFrame = 'F';
const uint8_t kMesh = 'M';

struct FrameRect {
    uint16_t x, y, w, h;
};

/* A frame message's header, decoded; pixels point into the message. */
struct FrameHeader {
    uint32_t ref = 0;
    uint64_t id = 0;
    std::vector<FrameRect> rects;
    const uint8_t *pixels = nullptr;
    size_t pixel_bytes = 0;
};

/* An M message's mask bits, and its bounds. */
const uint8_t kMeshNormals = 1, kMeshTexcoords = 2, kMeshColors = 4;
const uint32_t kMaxMeshVertices = 65535;
const uint32_t kMaxMeshIndices = 3 * 65535;
const size_t kMaxMeshBytes = 4u << 20;

/* An M message, decoded: its arrays point into the message (little-endian,
 * unaligned); an attribute the mask leaves out is null. */
struct MeshHeader {
    uint32_t ref = 0, part = 0, vertices = 0, indices = 0;
    uint8_t mask = 0;
    const uint8_t *position = nullptr, *normal = nullptr, *texcoord = nullptr, *color = nullptr, *index = nullptr;
};

/* A whole message: the 4-byte length, the type and the body. */
std::string encode(uint8_t type, const std::string &body);
/* A frame message's bytes before its pixels (length and type included). */
std::string frame_head(uint32_t ref, uint64_t id, const std::vector<FrameRect> &rects, size_t pixel_bytes);

/* The first whole message in buf[0, len): false when there is none yet;
 * *bad when buf starts with a message too long or empty. */
bool next_message(const char *buf, size_t len, size_t *consumed, uint8_t *type, const char **body,
                  size_t *body_len, bool *bad);
bool parse_frame(const char *body, size_t n, FrameHeader *out);
/* A whole M message. The arrays hold vertices or indices entries; normal,
 * texcoord and color may be null. */
std::string mesh_message(uint32_t ref, uint32_t part, uint32_t vertices, const float *position, const float *normal,
                         const float *texcoord, const float *color, uint32_t indices, const uint16_t *index);
/* False with *err when the body is malformed: counts out of bounds, sizes
 * that do not add up, a value that is not finite, an index past the
 * vertices, or indices that are not whole triangles. */
bool parse_mesh(const char *body, size_t n, MeshHeader *out, std::string *err);

/* Flat JSON: an object of string, number, true/false/null values. The
 * values are kept as text (strings unescaped). */
typedef std::map<std::string, std::string> Json;
bool parse_json(const std::string &s, Json *out);
std::string json_quote(const std::string &s);
/* {"k":v,...} from pairs whose values are already JSON (json_quote a string). */
std::string json_object(const std::vector<std::pair<std::string, std::string>> &kv);

/* A scene edit line's key (PROTOCOL.md "edit"): its kind, --show counted as
 * --hide and --pass-on as --pass-off, and the rest of the line, up to the
 * last '=' for the kinds that take a value. A line replaces the kept line
 * with the same key. */
std::string edit_key(const std::string &line);

}  // namespace cr

#endif
