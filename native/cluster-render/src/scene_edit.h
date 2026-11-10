/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
/* A scene content's edits (PROTOCOL.md "edit"): lines that change a loaded
 * scene's objects by name, kept so that the scene loaded again gets them
 * all.
 *
 * A line is parsed when it arrives. One with the key of a kept line
 * (cr::edit_key) replaces it and goes to the end, so the kept lines, applied
 * in order, leave every object as the lines sent so far did: each line sets
 * a value, and lines of one key set the same values of the same objects.
 * New lines are applied at the next stage(), once a tick, in the target's
 * one flush. A pattern is a POSIX extended regular expression found anywhere
 * in an object's name (as grep -E). It is resolved once per scene and kind
 * and cached by its text: a scene of thousands of nodes with a few dozen
 * lines 30 times a second would otherwise match every name every tick. A
 * line that changes nothing (no object matches, or none takes it) is
 * reported once per scene, and a tick whose lines all change nothing has
 * nothing to flush (a flush of no change still sends the renderer its
 * flush). */
#ifndef CRS_SCENE_EDIT_H
#define CRS_SCENE_EDIT_H

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "graph.h"

namespace crs {

struct SceneEdit {
    enum Op { Hide, Show, Uniform, Move, Scale, Rotate, Viewport, PassOff, PassOn, Set };
    Op op = Hide;
    std::string pattern;   /* a regular expression of names; Set: the data object's name */
    std::string name;      /* Uniform: the uniform's */
    std::vector<float> v;
    std::string line;      /* as it came */
};

/* False with *err when the line is not one of PROTOCOL.md's. */
bool parse_edit(const std::string &line, SceneEdit *out, std::string *err);

/* A line refused or that changed nothing: the line, and why, naming it. */
struct EditProblem {
    std::string line, reason;
};

class SceneEdits {
public:
    static const size_t kMaxKept = 1024;
    static const size_t kMaxLine = 1024;
    static const size_t kMaxPattern = 256;
    static const size_t kMaxCached = 1024;

    /* The lines (separated by newlines; empty ones skipped), each kept and
     * due at the next stage(). Returns the lines refused. */
    std::vector<EditProblem> add(const std::string &lines);
    /* A new scene: every kept line due again, every name resolved again. */
    void reset();
    /* Applies the due lines to s, in order; true when any changed s (then
     * it wants a flush). */
    bool stage(FileScene *s);
    /* The lines that changed nothing since the last call. */
    std::vector<EditProblem> take_problems();
    size_t kept() const { return lines_.size(); }
    /* A line waits for the next stage(). */
    bool due() const;

private:
    struct Kept {
        SceneEdit e;
        bool due = true;
    };
    struct Match {
        std::vector<int> hits;
        std::string error;
    };
    bool apply(FileScene *s, const SceneEdit &e, std::string *why);
    const Match &resolve(FileScene *s, FileScene::Kind k, const std::string &pattern);

    std::list<Kept> lines_;
    std::map<std::string, std::list<Kept>::iterator> by_key_;
    std::vector<std::string> names_[FileScene::kKinds];
    bool listed_[FileScene::kKinds] = {};
    std::map<std::pair<int, std::string>, Match> cache_;
    std::set<std::string> reported_;
    std::vector<EditProblem> problems_;
};

}  // namespace crs

#endif
