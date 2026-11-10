/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
#include "scene_file.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace crs {

namespace {

/* FIPS 180-4. */
const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

class Sha256 {
public:
    void update(const uint8_t *p, size_t n)
    {
        total_ += n;
        while (n) {
            const size_t take = std::min(n, sizeof buf_ - used_);
            std::memcpy(buf_ + used_, p, take);
            used_ += take;
            p += take;
            n -= take;
            if (used_ == sizeof buf_) {
                block(buf_);
                used_ = 0;
            }
        }
    }
    std::string hex()
    {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used_ != 56)
            update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++)
            len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++)
            std::snprintf(out + i * 8, 9, "%08x", h_[i]);
        return std::string(out, 64);
    }

private:
    void block(const uint8_t *p)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        const uint32_t add[8] = {a, b, c, d, e, f, g, h};
        for (int i = 0; i < 8; i++)
            h_[i] += add[i];
    }

    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

/* Owned by the owner, and nobody else may write it. */
bool owned(const struct stat &st, const SceneFiles &w)
{
    return st.st_uid == w.owner && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

/* path's real path into *real: under root (a real path ending in '/'), it
 * and every folder below root owned (owned()), and a regular file, whose
 * size goes into *bytes. */
bool checked(const std::string &root, const SceneFiles &w, const std::string &path, std::string *real, uint64_t *bytes,
             std::string *err)
{
    if (path.empty() || path[0] != '/' || path.size() >= PATH_MAX) {
        *err = "a scene file's path must be absolute: " + path;
        return false;
    }
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf)) {
        *err = path + ": " + std::strerror(errno);
        return false;
    }
    const std::string r = buf;
    if (r.size() <= root.size() || r.compare(0, root.size(), root) != 0) {
        *err = path + " is not under " + w.root;
        return false;
    }
    for (size_t at = r.find('/', root.size());; at = r.find('/', at + 1)) {
        const std::string part = at == std::string::npos ? r : r.substr(0, at);
        struct stat st;
        if (lstat(part.c_str(), &st) != 0) {
            *err = part + ": " + std::strerror(errno);
            return false;
        }
        if (!owned(st, w)) {
            *err = part + " is not owned by uid " + std::to_string((unsigned long)w.owner) +
                   " or is writable by others";
            return false;
        }
        if (at == std::string::npos) {
            if (!S_ISREG(st.st_mode)) {
                *err = path + " is not a regular file";
                return false;
            }
            *bytes = (uint64_t)st.st_size;
            break;
        }
    }
    *real = r;
    return true;
}

}  // namespace

std::string sha256_hex(const uint8_t *data, size_t n)
{
    Sha256 s;
    s.update(data, n);
    return s.hex();
}

std::string sha256_file(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return std::string();
    Sha256 s;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        s.update(buf, n);
    const bool good = !std::ferror(f);
    std::fclose(f);
    return good ? s.hex() : std::string();
}

bool check_scene_source(const SceneFiles &where, const std::string &file, const std::string &res,
                        const std::string &sha256, SceneSource *out, std::string *err)
{
    std::vector<std::string> names, hashes;
    std::istringstream rs(res), hs(sha256);
    for (std::string w; rs >> w;)
        names.push_back(w);
    for (std::string w; hs >> w;) {
        for (char &c : w)
            c = (char)std::tolower((unsigned char)c);
        hashes.push_back(w);
    }
    if (names.size() > kMaxResourceFiles) {
        *err = "at most " + std::to_string(kMaxResourceFiles) + " resource files";
        return false;
    }
    bool hex = hashes.size() == names.size() + 1;
    for (const std::string &h : hashes)
        hex = hex && h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string::npos;
    if (!hex) {
        *err = "sha256 must be " + std::to_string(names.size() + 1) +
               " SHA-256s (64 hex digits each, separated by spaces): the scene file's, then each resource file's";
        return false;
    }
    names.insert(names.begin(), file);
    char rb[PATH_MAX];
    if (where.root.empty() || !realpath(where.root.c_str(), rb)) {
        *err = "scene files: the folder " + where.root + " is missing";
        return false;
    }
    std::string root = rb;
    if (root.back() != '/')
        root += '/';
    SceneSource s;
    std::vector<std::string> real(names.size());
    for (size_t i = 0; i < names.size(); i++) {
        uint64_t bytes = 0;
        if (!checked(root, where, names[i], &real[i], &bytes, err))
            return false;
        s.bytes += bytes;
    }
    if (s.bytes > kMaxSceneBytes) {
        *err = "the files are larger than " + std::to_string(kMaxSceneBytes >> 20) + " MB together";
        return false;
    }
    /* Checked first, hashed then: no file can change after its check. */
    for (size_t i = 0; i < names.size(); i++) {
        const std::string got = sha256_file(real[i]);
        if (got != hashes[i]) {
            *err = got.empty() ? names[i] + " does not read" : names[i] + "'s SHA-256 is " + got + ", not " + hashes[i];
            return false;
        }
    }
    s.file = real[0];
    s.res.assign(real.begin() + 1, real.end());
    *out = s;
    return true;
}

}  // namespace crs
