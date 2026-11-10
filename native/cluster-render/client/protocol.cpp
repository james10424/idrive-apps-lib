/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Robert Nisipeanu */
#include "cluster_render/protocol.h"

#include <cmath>
#include <cstring>

namespace cr {

namespace {

void put32(std::string *s, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        s->push_back((char)(v >> (8 * i)));
}

void put16(std::string *s, uint16_t v)
{
    s->push_back((char)v);
    s->push_back((char)(v >> 8));
}

uint32_t get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

void skip_ws(const std::string &s, size_t *i)
{
    while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n'))
        (*i)++;
}

int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_string(const std::string &s, size_t *i, std::string *out)
{
    if (*i >= s.size() || s[*i] != '"')
        return false;
    (*i)++;
    out->clear();
    while (*i < s.size()) {
        char c = s[(*i)++];
        if (c == '"')
            return true;
        if (c != '\\') {
            out->push_back(c);
            continue;
        }
        if (*i >= s.size())
            return false;
        char e = s[(*i)++];
        switch (e) {
        case '"': case '\\': case '/': out->push_back(e); break;
        case 'n': out->push_back('\n'); break;
        case 't': out->push_back('\t'); break;
        case 'r': out->push_back('\r'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
            if (*i + 4 > s.size())
                return false;
            unsigned cp = 0;
            for (int k = 0; k < 4; k++) {
                int h = hex(s[*i + k]);
                if (h < 0)
                    return false;
                cp = cp << 4 | (unsigned)h;
            }
            *i += 4;
            /* UTF-8 of the code point; a surrogate half stands alone. */
            if (cp < 0x80) {
                out->push_back((char)cp);
            } else if (cp < 0x800) {
                out->push_back((char)(0xc0 | cp >> 6));
                out->push_back((char)(0x80 | (cp & 0x3f)));
            } else {
                out->push_back((char)(0xe0 | cp >> 12));
                out->push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
                out->push_back((char)(0x80 | (cp & 0x3f)));
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

}  // namespace

std::string encode(uint8_t type, const std::string &body)
{
    std::string m;
    m.reserve(5 + body.size());
    put32(&m, (uint32_t)(body.size() + 1));
    m.push_back((char)type);
    m += body;
    return m;
}

std::string frame_head(uint32_t ref, uint64_t id, const std::vector<FrameRect> &rects, size_t pixel_bytes)
{
    std::string m;
    const size_t body = 4 + 8 + 2 + rects.size() * 8 + pixel_bytes;
    put32(&m, (uint32_t)(body + 1));
    m.push_back((char)kFrame);
    put32(&m, ref);
    put32(&m, (uint32_t)id);
    put32(&m, (uint32_t)(id >> 32));
    put16(&m, (uint16_t)rects.size());
    for (const FrameRect &r : rects) {
        put16(&m, r.x);
        put16(&m, r.y);
        put16(&m, r.w);
        put16(&m, r.h);
    }
    return m;
}

bool next_message(const char *buf, size_t n, size_t *consumed, uint8_t *type, const char **body,
                  size_t *body_len, bool *bad)
{
    *bad = false;
    if (n < 4)
        return false;
    const uint32_t len = get32(reinterpret_cast<const unsigned char *>(buf));
    if (len < 1 || len > kMaxMessage) {
        *bad = true;
        return false;
    }
    if (n < 4 + (size_t)len)
        return false;
    *type = (uint8_t)buf[4];
    *body = buf + 5;
    *body_len = len - 1;
    *consumed = 4 + len;
    return true;
}

bool parse_frame(const char *body, size_t n, FrameHeader *out)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(body);
    if (n < 14)
        return false;
    out->ref = get32(p);
    out->id = get32(p + 4) | (uint64_t)get32(p + 8) << 32;
    const uint16_t nr = get16(p + 12);
    if (n < 14 + (size_t)nr * 8)
        return false;
    out->rects.resize(nr);
    for (uint16_t i = 0; i < nr; i++) {
        const unsigned char *r = p + 14 + i * 8;
        out->rects[i] = FrameRect{get16(r), get16(r + 2), get16(r + 4), get16(r + 6)};
    }
    out->pixels = p + 14 + (size_t)nr * 8;
    out->pixel_bytes = n - 14 - (size_t)nr * 8;
    return true;
}

namespace {

void put_floats(std::string *s, const float *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        std::memcpy(&u, &v[i], 4);
        put32(s, u);
    }
}

uint32_t mesh_floats(uint8_t mask)
{
    return 3 + (mask & kMeshNormals ? 3 : 0) + (mask & kMeshTexcoords ? 2 : 0) + (mask & kMeshColors ? 4 : 0);
}

}  // namespace

std::string mesh_message(uint32_t ref, uint32_t part, uint32_t vertices, const float *position, const float *normal,
                         const float *texcoord, const float *color, uint32_t indices, const uint16_t *index)
{
    const uint8_t mask = (uint8_t)((normal ? kMeshNormals : 0) | (texcoord ? kMeshTexcoords : 0) |
                                   (color ? kMeshColors : 0));
    std::string body;
    body.reserve(17 + (size_t)vertices * mesh_floats(mask) * 4 + (size_t)indices * 2);
    put32(&body, ref);
    put32(&body, part);
    put32(&body, vertices);
    put32(&body, indices);
    body.push_back((char)mask);
    put_floats(&body, position, (size_t)vertices * 3);
    if (normal)
        put_floats(&body, normal, (size_t)vertices * 3);
    if (texcoord)
        put_floats(&body, texcoord, (size_t)vertices * 2);
    if (color)
        put_floats(&body, color, (size_t)vertices * 4);
    for (uint32_t i = 0; i < indices; i++)
        put16(&body, index[i]);
    return encode(kMesh, body);
}

bool parse_mesh(const char *body, size_t n, MeshHeader *out, std::string *err)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(body);
    if (n < 17) {
        *err = "a mesh message's header is 17 bytes";
        return false;
    }
    MeshHeader m;
    m.ref = get32(p);
    m.part = get32(p + 4);
    m.vertices = get32(p + 8);
    m.indices = get32(p + 12);
    m.mask = p[16];
    if (m.mask & ~(kMeshNormals | kMeshTexcoords | kMeshColors)) {
        *err = "mesh mask bits are 1 (normals), 2 (texcoords) and 4 (colors)";
        return false;
    }
    if (m.vertices < 1 || m.vertices > kMaxMeshVertices) {
        *err = "a mesh has 1 to 65535 vertices";
        return false;
    }
    if (m.indices < 3 || m.indices > kMaxMeshIndices || m.indices % 3) {
        *err = "a mesh's indices are whole triangles, at most 196605";
        return false;
    }
    const size_t floats = (size_t)m.vertices * mesh_floats(m.mask);
    const size_t need = 17 + floats * 4 + (size_t)m.indices * 2;
    if (n != need || n > kMaxMeshBytes) {
        *err = "this mesh is " + std::to_string(need) + " bytes";
        return false;
    }
    const unsigned char *f = p + 17;
    for (size_t i = 0; i < floats; i++) {
        const uint32_t u = get32(f + i * 4);
        float v;
        std::memcpy(&v, &u, 4);
        if (!std::isfinite(v)) {
            *err = "mesh values must be finite";
            return false;
        }
    }
    const unsigned char *idx = f + floats * 4;
    for (uint32_t i = 0; i < m.indices; i++)
        if (get16(idx + (size_t)i * 2) >= m.vertices) {
            *err = "a mesh index is past its vertices";
            return false;
        }
    const unsigned char *a = f;
    m.position = a;
    a += (size_t)m.vertices * 12;
    if (m.mask & kMeshNormals) {
        m.normal = a;
        a += (size_t)m.vertices * 12;
    }
    if (m.mask & kMeshTexcoords) {
        m.texcoord = a;
        a += (size_t)m.vertices * 8;
    }
    if (m.mask & kMeshColors)
        m.color = a;
    m.index = idx;
    *out = m;
    return true;
}

bool parse_json(const std::string &s, Json *out)
{
    size_t i = 0;
    out->clear();
    skip_ws(s, &i);
    if (i >= s.size() || s[i++] != '{')
        return false;
    skip_ws(s, &i);
    if (i < s.size() && s[i] == '}') {
        i++;
        skip_ws(s, &i);
        return i == s.size();
    }
    for (;;) {
        std::string key, value;
        skip_ws(s, &i);
        if (!parse_string(s, &i, &key))
            return false;
        skip_ws(s, &i);
        if (i >= s.size() || s[i++] != ':')
            return false;
        skip_ws(s, &i);
        if (i < s.size() && s[i] == '"') {
            if (!parse_string(s, &i, &value))
                return false;
        } else {
            size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\n' && s[i] != '\t' &&
                   s[i] != '\r')
                i++;
            value = s.substr(start, i - start);
            if (value.empty() || value[0] == '{' || value[0] == '[')
                return false;
            if (value == "true") value = "1";
            else if (value == "false" || value == "null") value = "0";
        }
        (*out)[key] = value;
        skip_ws(s, &i);
        if (i >= s.size())
            return false;
        if (s[i] == ',') { i++; continue; }
        if (s[i] == '}') { i++; break; }
        return false;
    }
    skip_ws(s, &i);
    return i == s.size();
}

std::string json_quote(const std::string &s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if ((unsigned char)c < 0x20) o += ' ';
        else o.push_back(c);
    }
    return o + "\"";
}

std::string json_object(const std::vector<std::pair<std::string, std::string>> &kv)
{
    std::string o = "{";
    for (const auto &p : kv) {
        if (o.size() > 1)
            o += ",";
        o += json_quote(p.first) + ":" + p.second;
    }
    return o + "}";
}

std::string edit_key(const std::string &line)
{
    const size_t sp = line.find(' ');
    std::string kind = line.substr(0, sp);
    std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    if (kind == "--show")
        kind = "--hide";
    else if (kind == "--pass-on")
        kind = "--pass-off";
    else if (kind == "--uniform" || kind == "--move" || kind == "--scale" || kind == "--rotate" ||
             kind == "--viewport" || kind == "--set")
        rest = rest.substr(0, rest.rfind('='));
    return kind + " " + rest;
}

}  // namespace cr
