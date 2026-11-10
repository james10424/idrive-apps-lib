/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
/* The checks a scene file passes before the service loads it (PROTOCOL.md
 * "Scene files"). The service runs as hmi_5000 and every client of the
 * socket may name a file, and libramses trusts what it reads: a malformed
 * file that crashes it takes the service down with it. So a file is taken
 * only from a real path (symlinks and ".." resolved) under the root, the
 * file and every folder below the root owned by the owner (root on the unit)
 * and writable by it alone, so no client can change it after the check; and
 * every file only when its SHA-256 is the one the client names for it. */
#ifndef CRS_SCENE_FILE_H
#define CRS_SCENE_FILE_H

#include <stdint.h>
#include <sys/types.h>

#include <string>
#include <vector>

namespace crs {

/* Where scene files may come from. */
struct SceneFiles {
    std::string root = "/var/opt/";
    uid_t owner = 0;
};

/* A scene file and its resource files, checked: their real paths. */
struct SceneSource {
    std::string file;
    std::vector<std::string> res;
    uint64_t bytes = 0;   /* the files' sizes together */
};

const size_t kMaxResourceFiles = 16;
/* The files are hashed in the render loop: at most this much together
 * (about a second). */
const uint64_t kMaxSceneBytes = 256u << 20;

/* file and the resource files res names (separated by spaces) checked as
 * above into *out, sha256 their SHA-256s in that order (hex, separated by
 * spaces); false with *err. */
bool check_scene_source(const SceneFiles &where, const std::string &file, const std::string &res,
                        const std::string &sha256, SceneSource *out, std::string *err);

/* SHA-256 of n bytes, lowercase hex. */
std::string sha256_hex(const uint8_t *data, size_t n);
/* Of a file's contents; empty when it does not read. */
std::string sha256_file(const std::string &path);

}  // namespace crs

#endif
