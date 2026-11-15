/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cld {

namespace {

/* Every key and its built-in default (cluster-demo's). */
const char *const DEFAULTS[][2] = {
    {"local_conf", "/var/opt/cluster-demo/cluster-demo.local.conf"},
    {"socket_path", "/dev/shm/cluster-demo/ctl.sock"},
    {"socket_mode", "0660"},
    {"socket_group", ""},
    {"start_timeout_ms", "5000"},
    {"ramses_user", "801"},
    {"hud_scene_id", "0x01000400"},
    {"sail_scene_id", "0x010004f0"},
    {"showcase_step", "5"},
    {"fps", "30"},
    {"page_ms", "8000"},
    {"video_size", "320x160"},
    {"dcsm_client_handle", ""},
    {"dcsm_heartbeat_replies", "1"},
    {"hud_slot", "0x323"},
    {"hud_content_class", "0"},
    {"hud_size", "800x480"},
    {"sail_slot", "0x203"},
    {"sail_content_class", "0"},
    {"sail_size", "1920x720"},
    {"verbose", "0"},
};

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r");
    size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

Config::Config()
{
    for (auto &kv : DEFAULTS)
        values_[kv[0]] = kv[1];
}

void Config::define(const char *key, const char *def)
{
    values_.emplace(key, def);
}

bool Config::set(const std::string &key, const std::string &value, bool append, std::string *err)
{
    auto it = values_.find(key);
    if (it == values_.end()) {
        if (!extra_keys || !extra_keys(key)) {
            *err = "unknown setting " + key;
            return false;
        }
        it = values_.emplace(key, std::string()).first;
    }
    if (!append) {
        it->second = value;
        return true;
    }
    /* Each entry once: the list is short. */
    std::stringstream add(value);
    for (std::string e; std::getline(add, e, ',');) {
        e = trim(e);
        bool have = e.empty();
        std::stringstream cur(it->second);
        for (std::string c; !have && std::getline(cur, c, ',');)
            have = trim(c) == e;
        if (!have)
            it->second += (trim(it->second).empty() ? "" : ",") + e;
    }
    return true;
}

bool Config::set_pair(const std::string &pair, std::string *err)
{
    size_t eq = pair.find('=');
    if (eq == std::string::npos) {
        *err = "expected key=value: " + pair;
        return false;
    }
    std::string key = trim(pair.substr(0, eq));
    const bool append = !key.empty() && key.back() == '+';
    if (append)
        key = trim(key.substr(0, key.size() - 1));
    return set(key, trim(pair.substr(eq + 1)), append, err);
}

bool Config::load(const std::string &path, std::string *err)
{
    std::ifstream f(path);
    if (!f) {
        *err = "cannot read " + path;
        return false;
    }
    return load(f, path, err);
}

bool Config::load(std::istream &f, const std::string &path, std::string *err)
{
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        n++;
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        line = trim(line);
        if (line.empty())
            continue;
        if (!set_pair(line, err)) {
            *err = path + ":" + std::to_string(n) + ": " + *err;
            return false;
        }
    }
    return true;
}

std::string Config::str(const char *key) const
{
    auto it = values_.find(key);
    return it == values_.end() ? std::string() : it->second;
}

long Config::num(const char *key) const
{
    return std::strtol(str(key).c_str(), nullptr, 0);
}

bool Config::has(const char *key) const
{
    return !str(key).empty();
}

bool Config::size(const char *key, unsigned *w, unsigned *h) const
{
    return std::sscanf(str(key).c_str(), "%u x %u", w, h) == 2 || std::sscanf(str(key).c_str(), "%ux%u", w, h) == 2;
}

}  // namespace cld
