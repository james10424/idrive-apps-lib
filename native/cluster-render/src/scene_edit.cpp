/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
#include "scene_edit.h"

#include <regex.h>

#include <cmath>
#include <cstdlib>
#include <iterator>

#include "cluster_render/protocol.h"

namespace crs {

namespace {

/* lo to hi finite numbers separated by commas or spaces, the whole text. */
bool numbers(const std::string &text, size_t lo, size_t hi, std::vector<float> *out)
{
    out->clear();
    const char *p = text.c_str();
    for (;;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            return out->size() >= lo;
        char *end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p || !std::isfinite(f) || out->size() >= hi)
            return false;
        out->push_back(f);
        p = end;
    }
}

/* A line for a message: at most 80 characters of it. */
std::string quoted(const std::string &line)
{
    return "'" + (line.size() > 80 ? line.substr(0, 77) + "..." : line) + "'";
}

const char *kind_name(FileScene::Kind k)
{
    switch (k) {
    case FileScene::Mesh: return "mesh";
    case FileScene::Camera: return "camera";
    case FileScene::Pass: return "render pass";
    default: return "node";
    }
}

}  // namespace

bool parse_edit(const std::string &line, SceneEdit *out, std::string *err)
{
    static const struct {
        const char *kind;
        SceneEdit::Op op;
        bool valued;
        const char *form;
    } kinds[] = {
        {"--hide", SceneEdit::Hide, false, "--hide RE"},
        {"--show", SceneEdit::Show, false, "--show RE"},
        {"--pass-off", SceneEdit::PassOff, false, "--pass-off RE"},
        {"--pass-on", SceneEdit::PassOn, false, "--pass-on RE"},
        {"--uniform", SceneEdit::Uniform, true, "--uniform MESH_RE:NAME=v[,v..]"},
        {"--move", SceneEdit::Move, true, "--move NODE_RE=x,y,z"},
        {"--scale", SceneEdit::Scale, true, "--scale NODE_RE=x,y,z"},
        {"--rotate", SceneEdit::Rotate, true, "--rotate NODE_RE=x,y,z"},
        {"--viewport", SceneEdit::Viewport, true, "--viewport CAMERA_RE=x,y,w,h"},
        {"--set", SceneEdit::Set, true, "--set DATA=v[,v..]"},
    };
    if (line.size() > SceneEdits::kMaxLine) {
        *err = "longer than " + std::to_string(SceneEdits::kMaxLine) + " characters";
        return false;
    }
    const size_t sp = line.find(' ');
    const std::string kind = line.substr(0, sp);
    const auto *k = std::begin(kinds);
    while (k != std::end(kinds) && kind != k->kind)
        k++;
    if (k == std::end(kinds)) {
        *err = "unknown kind " + kind.substr(0, 32);
        return false;
    }
    SceneEdit e;
    e.op = k->op;
    e.line = line;
    e.pattern = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    std::string values;
    bool good = true;
    if (k->valued) {
        const size_t eq = e.pattern.rfind('=');
        good = eq != std::string::npos;
        if (good) {
            values = e.pattern.substr(eq + 1);
            e.pattern.erase(eq);
        }
    }
    if (good && e.op == SceneEdit::Uniform) {
        const size_t colon = e.pattern.rfind(':');
        good = colon != std::string::npos && colon + 1 < e.pattern.size() && e.pattern.size() - colon - 1 <= 64;
        if (good) {
            e.name = e.pattern.substr(colon + 1);
            e.pattern.erase(colon);
        }
    }
    if (good) {
        switch (e.op) {
        case SceneEdit::Uniform: good = numbers(values, 1, 1024, &e.v); break;
        case SceneEdit::Move:
        case SceneEdit::Scale:
        case SceneEdit::Rotate: good = numbers(values, 3, 3, &e.v); break;
        case SceneEdit::Set: good = numbers(values, 1, 4, &e.v); break;
        case SceneEdit::Viewport:
            good = numbers(values, 4, 4, &e.v);
            for (size_t i = 0; good && i < 4; i++)
                good = e.v[i] == std::floor(e.v[i]) && e.v[i] >= (i < 2 ? 0 : 1) && e.v[i] <= 16384;
            break;
        default: break;
        }
    }
    if (!good || e.pattern.empty()) {
        *err = std::string("not ") + k->form;
        return false;
    }
    if (e.pattern.size() > SceneEdits::kMaxPattern) {
        *err = "a pattern or name longer than " + std::to_string(SceneEdits::kMaxPattern) + " characters";
        return false;
    }
    /* Back-references make matching exponential. */
    for (size_t i = 0; e.op != SceneEdit::Set && i + 1 < e.pattern.size(); i++)
        if (e.pattern[i] == '\\' && e.pattern[i + 1] >= '1' && e.pattern[i + 1] <= '9') {
            *err = "back-references are not taken";
            return false;
        }
    *out = e;
    return true;
}

std::vector<EditProblem> SceneEdits::add(const std::string &lines)
{
    std::vector<EditProblem> refused;
    size_t at = 0;
    while (at <= lines.size()) {
        size_t nl = lines.find('\n', at);
        if (nl == std::string::npos)
            nl = lines.size();
        std::string line = lines.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        SceneEdit e;
        std::string err;
        if (!parse_edit(line, &e, &err)) {
            refused.push_back(EditProblem{line, quoted(line) + ": " + err});
            continue;
        }
        const std::string key = cr::edit_key(line);
        auto it = by_key_.find(key);
        if (it != by_key_.end()) {
            lines_.erase(it->second);
        } else if (lines_.size() >= kMaxKept) {
            refused.push_back(EditProblem{line, quoted(line) + ": at most " + std::to_string(kMaxKept) + " lines are kept"});
            continue;
        }
        Kept k;
        k.e = e;
        by_key_[key] = lines_.insert(lines_.end(), k);
    }
    return refused;
}

void SceneEdits::reset()
{
    for (Kept &k : lines_)
        k.due = true;
    for (int k = 0; k < FileScene::kKinds; k++) {
        names_[k].clear();
        listed_[k] = false;
    }
    cache_.clear();
    reported_.clear();
}

bool SceneEdits::due() const
{
    for (const Kept &k : lines_)
        if (k.due)
            return true;
    return false;
}

bool SceneEdits::stage(FileScene *s)
{
    bool any = false;
    for (Kept &k : lines_) {
        if (!k.due)
            continue;
        k.due = false;
        std::string why;
        if (apply(s, k.e, &why)) {
            any = true;
            continue;
        }
        if (reported_.count(k.e.line))
            continue;
        if (reported_.size() >= kMaxKept)
            reported_.clear();
        reported_.insert(k.e.line);
        problems_.push_back(EditProblem{k.e.line, quoted(k.e.line) + " changed nothing: " + why});
    }
    return any;
}

std::vector<EditProblem> SceneEdits::take_problems()
{
    std::vector<EditProblem> p;
    p.swap(problems_);
    return p;
}

const SceneEdits::Match &SceneEdits::resolve(FileScene *s, FileScene::Kind k, const std::string &pattern)
{
    const std::pair<int, std::string> key(k, pattern);
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;
    /* ponytail: a client making patterns up starts the cache over; an LRU if
     * a real scene ever needs more. */
    if (cache_.size() >= kMaxCached)
        cache_.clear();
    if (!listed_[k]) {
        names_[k] = s->objects(k);
        listed_[k] = true;
    }
    Match &m = cache_[key];
    regex_t re;
    if (regcomp(&re, pattern.c_str(), REG_EXTENDED | REG_NOSUB) != 0) {
        m.error = "not a regular expression";
        return m;
    }
    for (size_t i = 0; i < names_[k].size(); i++)
        if (regexec(&re, names_[k][i].c_str(), 0, nullptr, 0) == 0)
            m.hits.push_back((int)i);
    regfree(&re);
    return m;
}

bool SceneEdits::apply(FileScene *s, const SceneEdit &e, std::string *why)
{
    if (e.op == SceneEdit::Set)
        return s->data(e.pattern, e.v, why);
    const FileScene::Kind kind = e.op == SceneEdit::Uniform    ? FileScene::Mesh
                                 : e.op == SceneEdit::Viewport ? FileScene::Camera
                                 : e.op == SceneEdit::PassOff || e.op == SceneEdit::PassOn ? FileScene::Pass
                                                                                          : FileScene::Node;
    const Match &m = resolve(s, kind, e.pattern);
    if (!m.error.empty() || m.hits.empty()) {
        *why = m.error.empty() ? std::string("no ") + kind_name(kind) + " matches" : m.error;
        return false;
    }
    const uint32_t vp[4] = {e.op == SceneEdit::Viewport ? (uint32_t)e.v[0] : 0,
                            e.op == SceneEdit::Viewport ? (uint32_t)e.v[1] : 0,
                            e.op == SceneEdit::Viewport ? (uint32_t)e.v[2] : 0,
                            e.op == SceneEdit::Viewport ? (uint32_t)e.v[3] : 0};
    bool any = false;
    for (int h : m.hits) {
        bool done = false;
        switch (e.op) {
        case SceneEdit::Hide:
        case SceneEdit::Show: done = s->visible(h, e.op == SceneEdit::Show); break;
        case SceneEdit::Move: done = s->transform(h, FileScene::Translation, e.v.data()); break;
        case SceneEdit::Scale: done = s->transform(h, FileScene::Scaling, e.v.data()); break;
        case SceneEdit::Rotate: done = s->transform(h, FileScene::Rotation, e.v.data()); break;
        case SceneEdit::Viewport: done = s->viewport(h, vp); break;
        case SceneEdit::PassOff:
        case SceneEdit::PassOn: done = s->enable(h, e.op == SceneEdit::PassOn); break;
        case SceneEdit::Uniform: done = s->uniform(h, e.name, e.v, why); break;
        default: break;
        }
        any = any || done;
    }
    if (!any && why->empty())
        *why = "the renderer refused it";
    return any;
}

}  // namespace crs
