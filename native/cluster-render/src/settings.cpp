/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "settings.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>

#include "holds.h"
#include "log.h"

namespace crs {

namespace {

/* Generation 8's per-target fields (load_targets and unit_idrive8.cpp's
 * offer read them). */
const char *const kId8Fields[] = {"category", "content", "scene_id", "size",   "linger_ms",
                                  "description", "order", "preview", "background_id", "layouts", "focus"};

/* key is id8_<name>_<field> with a name of lowercase letters and digits:
 * its name. A name has no '_', so the field is the key's end. */
bool id8_target_key(const std::string &key, std::string *name)
{
    if (key.compare(0, 4, "id8_") != 0)
        return false;
    for (const char *f : kId8Fields) {
        const std::string tail = std::string("_") + f;
        if (key.size() <= 4 + tail.size() || key.compare(key.size() - tail.size(), tail.size(), tail) != 0)
            continue;
        *name = key.substr(4, key.size() - 4 - tail.size());
        return std::all_of(name->begin(), name->end(),
                           [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
    }
    return false;
}

/* A drop-in is a few lines. */
const off_t kMaxDropIn = 64 * 1024;

/* A comma-separated list's entries, trimmed, empty ones left out. */
std::vector<std::string> entries(const std::string &list)
{
    std::vector<std::string> out;
    std::stringstream ss(list);
    for (std::string e; std::getline(ss, e, ',');) {
        const size_t a = e.find_first_not_of(" \t"), b = e.find_last_not_of(" \t");
        if (a != std::string::npos)
            out.push_back(e.substr(a, b - a + 1));
    }
    return out;
}

/* id is one of ours: 0x4d4755xxxxxx, written whole. */
bool our_scene(const std::string &id)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(id.c_str(), &end, 0);
    return errno == 0 && end != id.c_str() && *end == '\0' && (v >> 24) == (kOurScenes >> 24);
}

/* The drop-in at path, read onto *cfg (a copy) as settings.h says, with
 * id8_targets at most max_targets long; false with why. */
bool drop_in(const std::string &path, uid_t owner, size_t max_targets, cld::Config *cfg, std::string *why)
{
    /* What is checked is what is read: the open file, no symlink. */
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        *why = errno == ELOOP ? "a symlink" : strerror(errno);
        return false;
    }
    struct stat st;
    std::string text;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = "not a regular file";
    } else if (st.st_uid != owner || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        *why = "not owned by uid " + std::to_string((unsigned long)owner) + " or writable by its group or others";
    } else if (st.st_size > kMaxDropIn) {
        *why = "larger than " + std::to_string(kMaxDropIn / 1024) + " KB";
    } else {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            text.append(buf, (size_t)n);
        if (n < 0)
            *why = strerror(errno);
    }
    close(fd);
    if (!why->empty())
        return false;
    const cld::Config before = *cfg;
    std::istringstream in(text);
    if (!cfg->load(in, path, why))
        return false;
    for (const char *list : {"id8_targets", "id8_scene_ids_allowed"}) {
        const std::vector<std::string> was = entries(before.str(list)), now = entries(cfg->str(list));
        if (now.size() < was.size() || !std::equal(was.begin(), was.end(), now.begin())) {
            *why = std::string(list) + " set: a drop-in only adds to it (+=)";
            return false;
        }
    }
    const std::vector<std::string> was = entries(before.str("id8_targets")), now = entries(cfg->str("id8_targets"));
    const std::vector<std::string> added(now.begin() + was.size(), now.end());
    for (auto it = added.begin(); it != added.end(); ++it)
        if (std::find(was.begin(), was.end(), *it) != was.end() || std::find(added.begin(), it, *it) != it) {
            *why = "target " + *it + ": a target already";
            return false;
        }
    if (now.size() > max_targets) {
        *why = "more than " + std::to_string(kMaxDropInTargets) + " targets from drop-ins";
        return false;
    }
    {
        const std::vector<std::string> allowed_was = entries(before.str("id8_scene_ids_allowed")),
                                       allowed_now = entries(cfg->str("id8_scene_ids_allowed"));
        for (size_t i = allowed_was.size(); i < allowed_now.size(); i++)
            if (!our_scene(allowed_now[i])) {
                *why = "id8_scene_ids_allowed: " + allowed_now[i] + " is not a scene id of ours (0x4d4755......)";
                return false;
            }
    }
    for (const auto &kv : cfg->values()) {
        std::string name;
        if (kv.first == "id8_targets" || kv.first == "id8_scene_ids_allowed" || before.str(kv.first.c_str()) == kv.second)
            continue;
        if (!id8_target_key(kv.first, &name) || std::find(added.begin(), added.end(), name) == added.end()) {
            *why = kv.first + " set: a drop-in sets only the id8_ keys of the targets it adds";
            return false;
        }
    }
    std::vector<TargetSpec> targets;
    if (!load_targets(*cfg, 8, &targets, why))
        return false;
    for (const TargetSpec &t : targets) {
        if (std::find(added.begin(), added.end(), t.name) == added.end())
            continue;
        if (t.slot != kWidgetCategory) {
            *why = "target " + t.name + ": its category is not the widget category " + std::to_string(kWidgetCategory);
            return false;
        }
        if (t.content < kWidgetContentFirst || t.content > kWidgetContentLast ||
            (t.content >= kHeadUnitContentFirst && t.content <= kHeadUnitContentLast)) {
            *why = "target " + t.name + ": its content is not a dynamic widget's (" + std::to_string(kWidgetContentFirst) +
                   "-" + std::to_string(kWidgetContentLast) + ", not " + std::to_string(kHeadUnitContentFirst) + "-" +
                   std::to_string(kHeadUnitContentLast) + ")";
            return false;
        }
        if ((t.scene_id >> 24) != (kOurScenes >> 24)) {
            *why = "target " + t.name + ": its scene id is not one of ours (0x4d4755......)";
            return false;
        }
        for (const TargetSpec &o : targets)
            if (o.name != t.name && (o.content == t.content || o.scene_id == t.scene_id)) {
                *why = "target " + t.name + ": its " + (o.content == t.content ? "content" : "scene id") +
                       " is target " + o.name + "'s";
                return false;
            }
    }
    return true;
}

}  // namespace

void define_keys(cld::Config *cfg)
{
    cfg->define("generation", "auto");
    cfg->define("vsomeip_app", "cluster-demo");
    cfg->define("targets", "map,panel,hud");
    cfg->define("scene_ids_allowed", "0x010004f0,0x01000400,0x010004f1,0x010004f8");
    cfg->define("map_slot", "0x100");
    cfg->define("map_content_class", "0");
    cfg->define("map_scene_id", "0x010004f0");
    cfg->define("map_size", "1088x480");
    cfg->define("map_linger_ms", "0");
    cfg->define("map_take_over_stock", "1");
    cfg->define("panel_slot", "0x203");
    cfg->define("panel_content_class", "0");
    cfg->define("panel_scene_id", "0x010004f8");
    cfg->define("panel_fallback_scene_id", "0x01000400");
    cfg->define("panel_size", "1920x720");
    cfg->define("panel_linger_ms", "3000");
    cfg->define("panel_take_over_stock", "0");
    cfg->define("hud_linger_ms", "3000");
    cfg->define("hud_take_over_stock", "0");
    /* the stock pop-up slots each target gives way to, and how long after */
    cfg->define("map_yield_slots", "");
    cfg->define("panel_yield_slots", "");
    cfg->define("hud_yield_slots", "0x322");
    cfg->define("map_yield_ms", "1500");
    cfg->define("panel_yield_ms", "1500");
    cfg->define("hud_yield_ms", "1500");
    /* whether clients may have a target's scene loaded from a scene file */
    cfg->define("map_scene_files", "0");
    cfg->define("panel_scene_files", "0");
    cfg->define("hud_scene_files", "0");
    cfg->define("dcsm_hmi_client_handle", "1");
    cfg->define("guide_hold", "1");
    cfg->define("guide_slots", "auto");
    cfg->define("guide_scene_id", "0x010004f1");
    cfg->define("guide_content_class", "0");
    cfg->define("guide_take_over", "0x01000310");
    /* iDrive 8: a target's slot is a DCSM category, its content the content
     * offered there (README.md "iDrive 8"). */
    cfg->define("id8_targets", "map,hud,guide,widget,popup");
    cfg->define("id8_scene_ids_allowed",
                "0x4d4755000108,0x4d4755000110,0x4d4755000118,0x4d4755000001,0x4d4755000002");
    cfg->define("id8_map_category", "43100201");
    cfg->define("id8_map_content", "43204041");
    cfg->define("id8_map_scene_id", "0x4d4755000108");
    cfg->define("id8_map_size", "874x749");
    cfg->define("id8_map_linger_ms", "0");
    cfg->define("id8_hud_category", "43100202");
    cfg->define("id8_hud_content", "43204012");
    cfg->define("id8_hud_scene_id", "0x4d4755000110");
    cfg->define("id8_hud_size", "353x203");
    cfg->define("id8_hud_linger_ms", "3000");
    cfg->define("id8_guide_category", "43100401");
    cfg->define("id8_guide_content", "43204051");
    cfg->define("id8_guide_scene_id", "0x4d4755000118");
    cfg->define("id8_guide_size", "702x213");
    cfg->define("id8_guide_linger_ms", "3000");
    cfg->define("id8_widget_category", "43100301");
    cfg->define("id8_widget_content", "43204001");
    cfg->define("id8_widget_scene_id", "0x4d4755000001");
    cfg->define("id8_widget_size", "2240x896");
    cfg->define("id8_widget_linger_ms", "3000");
    cfg->define("id8_widget_description", "Cluster renderer");
    cfg->define("id8_widget_order", "50");
    cfg->define("id8_widget_preview", "");
    cfg->define("id8_widget_background_id", "");
    cfg->define("id8_widget_layouts", "");
    cfg->define("id8_widget_focus", "0");
    cfg->define("id8_popup_category", "43100301");
    cfg->define("id8_popup_content", "43204003");
    cfg->define("id8_popup_scene_id", "0x4d4755000002");
    cfg->define("id8_popup_size", "2240x896");
    cfg->define("id8_popup_linger_ms", "3000");
    cfg->define("id8_popup_description", "Pop-up");
    cfg->define("id8_popup_order", "51");
    cfg->define("id8_popup_preview", "");
    cfg->define("id8_popup_background_id", "43400005");
    cfg->define("id8_popup_layouts", "");
    cfg->define("id8_popup_focus", "1");
    /* Any other iDrive 8 target's keys (settings.h), and guard_path, which
     * nothing reads: a local_conf that still sets it loads (service_config
     * warns). */
    cfg->extra_keys = [](const std::string &key) {
        std::string name;
        return key == "guard_path" || id8_target_key(key, &name);
    };
    cfg->define("drop_ins", "/var/opt/cluster-render/conf.d/*.conf /var/opt/*/current/cluster-render.d/*.conf");
    cfg->define("fade_ms", "250");
    cfg->define("upload_kb_per_s", "5500");
    cfg->define("upload_floor_kb_per_s", "2000");
    cfg->define("upload_step_kb_per_s", "500");
    cfg->define("upload_burst_ms", "250");
    cfg->define("pixel_mib", "32");
    cfg->define("slow_flush_ms", "150");
    cfg->define("status_path", "/run/cluster-render/status.txt");
    cfg->define("log_file", "/var/opt/cluster-render/log/cluster-render.log");
    cfg->define("log_file_kb", "512");
    /* The shared Config's defaults are cluster-demo's (src/config.cpp). */
    std::string err;
    cfg->set_pair("socket_path=/dev/shm/cluster-render/render.sock", &err);
    cfg->set_pair("local_conf=/var/opt/cluster-render/cluster-render.local.conf", &err);
}

bool load_targets(const cld::Config &cfg, int generation, std::vector<TargetSpec> *out, std::string *err)
{
    const std::string g = generation == 8 ? "id8_" : "";
    const std::string slot = generation == 8 ? "category" : "slot";
    std::vector<uint64_t> allowed;
    {
        std::stringstream ss(cfg.str((g + "scene_ids_allowed").c_str()));
        std::string id;
        while (std::getline(ss, id, ','))
            allowed.push_back(std::strtoull(id.c_str(), nullptr, 0));
    }
    std::stringstream ss(cfg.str((g + "targets").c_str()));
    std::string name;
    while (std::getline(ss, name, ',')) {
        if (name.empty())
            continue;
        const std::string p = g + name + "_";
        TargetSpec t;
        t.name = name;
        if (!cfg.has((p + slot).c_str()) || !cfg.has((p + "scene_id").c_str()) ||
            (generation == 8 && !cfg.has((p + "content").c_str()))) {
            *err = "target " + name + ": " + p + slot + (generation == 8 ? ", " + p + "content" : std::string()) +
                   " and " + p + "scene_id must be set";
            return false;
        }
        t.slot = std::strtoull(cfg.str((p + slot).c_str()).c_str(), nullptr, 0);
        t.scene_id = std::strtoull(cfg.str((p + "scene_id").c_str()).c_str(), nullptr, 0);
        t.linger_ms = (int)cfg.num((p + "linger_ms").c_str());
        if (generation == 8) {
            t.content = std::strtoull(cfg.str((p + "content").c_str()).c_str(), nullptr, 0);
        } else {
            t.content_class = (uint32_t)cfg.num((p + "content_class").c_str());
            t.take_over_stock = cfg.num((p + "take_over_stock").c_str()) != 0;
            t.scene_files = cfg.num((p + "scene_files").c_str()) == 1;
            if (cfg.has((p + "yield_slots").c_str())) {
                std::stringstream ys(cfg.str((p + "yield_slots").c_str()));
                std::string ysl;
                while (std::getline(ys, ysl, ','))
                    if (!ysl.empty())
                        t.yield_slots.push_back(std::strtoull(ysl.c_str(), nullptr, 0));
            }
            if (cfg.has((p + "yield_ms").c_str()))
                t.yield_ms = (int)cfg.num((p + "yield_ms").c_str());
            if (t.yield_ms < 0 || t.yield_ms > 60000) {
                *err = "target " + name + ": " + p + "yield_ms must be from 0 to 60000";
                return false;
            }
        }
        if (!cfg.size((p + "size").c_str(), &t.width, &t.height) || t.width < 16 || t.height < 16 ||
            t.width > 4096 || t.height > 4096) {
            *err = "target " + name + ": " + p + "size must be WxH";
            return false;
        }
        if (t.linger_ms < 0 || t.linger_ms > 600000) {
            *err = "target " + name + ": " + p + "linger_ms must be from 0 to 600000";
            return false;
        }
        std::string why;
        if (generation == 7 && name == "hud" && !hud_rules(&t, &why)) {
            log_warn("target hud left out: %s", why.c_str());
            continue;
        }
        /* Only scene ids the KOMBI is known to take: others (7702, 7703)
         * made it restart the cluster and the HUD. On iDrive 8, the ones
         * the cluster took. */
        if (std::find(allowed.begin(), allowed.end(), t.scene_id) == allowed.end()) {
            *err = "target " + name + ": scene id " + cfg.str((p + "scene_id").c_str()) + " is not in " + g +
                   "scene_ids_allowed";
            return false;
        }
        if (generation == 7 && name == "panel" && !fallback_rules(&t, cfg.str("panel_fallback_scene_id"), allowed, &why)) {
            *err = "target panel: " + why;
            return false;
        }
        out->push_back(t);
    }
    if (out->empty()) {
        *err = "no targets";
        return false;
    }
    if (generation == 8)
        return true;
    std::string warn;
    const bool held = load_holds(HoldSettings{cfg.str("guide_hold"), cfg.str("guide_slots"), cfg.str("guide_scene_id"),
                                              cfg.str("guide_content_class"), cfg.str("guide_take_over")},
                                 allowed, out, &warn);
    if (!warn.empty())
        log_warn("guide hold%s: %s", held ? "" : " off", warn.c_str());
    return true;
}

void load_drop_ins(cld::Config *cfg, uid_t owner, std::vector<std::string> *report)
{
    std::vector<std::string> paths;
    std::stringstream ss(cfg->str("drop_ins"));
    for (std::string pattern; ss >> pattern;) {
        glob_t g;
        if (glob(pattern.c_str(), 0, nullptr, &g) == 0)
            paths.insert(paths.end(), g.gl_pathv, g.gl_pathv + g.gl_pathc);
        globfree(&g);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    const size_t max_targets = entries(cfg->str("id8_targets")).size() + kMaxDropInTargets;
    for (const std::string &path : paths) {
        cld::Config copy = *cfg;
        std::string why;
        if (drop_in(path, owner, max_targets, &copy, &why)) {
            *cfg = copy;
            report->push_back("loaded " + path);
        } else {
            report->push_back("skipped " + path + ": " + why);
        }
    }
}

bool read_preview(const std::string &path, std::vector<char> *png, std::string *why)
{
    /* Checked before the open, so a path that is a device or a pipe is not
     * opened, and again on the open file, a pipe opened without blocking. */
    why->clear();
    png->clear();
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        *why = strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *why = "not a regular file";
        return false;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        *why = strerror(errno);
        return false;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = "not a regular file";
    } else if ((size_t)st.st_size > kMaxPreview) {
        *why = "larger than " + std::to_string(kMaxPreview / 1024) + " KB";
    } else {
        char buf[4096];
        ssize_t n = 0;
        while (png->size() <= kMaxPreview && (n = read(fd, buf, sizeof buf)) > 0)
            png->insert(png->end(), buf, buf + n);
        if (n < 0)
            *why = strerror(errno);
        else if (png->size() > kMaxPreview)
            *why = "larger than " + std::to_string(kMaxPreview / 1024) + " KB";
        else if (png->empty())
            *why = "empty";
    }
    close(fd);
    if (!why->empty())
        png->clear();
    return why->empty();
}

}  // namespace crs
