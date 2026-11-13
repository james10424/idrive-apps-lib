/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "guard.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "log.h"

namespace crs {

/* The file:
 *   version <cluster-render's version>
 *   boot <boot id>
 *   <target> <slot> <scene id> <class> <disabled 0|1> <clean boots> <strikes>
 * with slot "auto" for a hold whose slot comes from the slot table.
 */

std::string Guard::key(const TargetSpec &t)
{
    char slot[24] = "auto", b[160];
    if (t.table_slots.empty())
        std::snprintf(slot, sizeof slot, "0x%llx", (unsigned long long)t.slot);
    std::snprintf(b, sizeof b, "%s %s 0x%llx %u", t.name.c_str(), slot, (unsigned long long)t.scene_id,
                  t.content_class);
    return b;
}

bool Guard::never(const TargetSpec &t)
{
    return t.strike_limit <= 0 || (t.slot == 0x100 && t.scene_id == 0x010004f0);
}

void Guard::load(const std::string &path, const std::string &version, const std::string &boot_id)
{
    path_ = path;
    version_ = version;
    boot_ = boot_id;
    m_.clear();
    std::ifstream f(path);
    if (!f)
        return;
    std::string line, word, file_version, file_boot;
    bool bad = false;
    std::map<std::string, Mapping> m;
    while (!bad && std::getline(f, line)) {
        std::istringstream in(line);
        if (!(in >> word))
            continue;
        if (word == "version") {
            in >> file_version;
        } else if (word == "boot") {
            in >> file_boot;
        } else {
            std::string slot, scene, klass;
            Mapping g;
            bad = !(in >> slot >> scene >> klass >> g.disabled >> g.clean_boots >> g.strikes);
            g.disabled = g.disabled && !(slot == "0x100" && scene == "0x10004f0");
            m[word + " " + slot + " " + scene + " " + klass] = g;
        }
    }
    if (bad) {
        log_warn("guard: %s does not read; starting over", path.c_str());
    } else if (file_version != version) {
        if (!m.empty())
            log_info("guard: %s is cluster-render %s's; starting over for %s", path.c_str(), file_version.c_str(),
                     version.c_str());
    } else {
        m_ = m;
    }
    for (auto it = m_.begin(); it != m_.end();) {
        Mapping &g = it->second;
        if (file_boot != boot_id && !g.disabled && ++g.clean_boots >= kCleanBoots)
            it = m_.erase(it);
        else
            ++it;
    }
    save();
    for (const auto &kv : m_)
        if (kv.second.disabled)
            log_error("guard: %s stays disabled after %d strikes (cluster-render guard-reset)", kv.first.c_str(),
                      kv.second.strikes);
}

bool Guard::disabled(const TargetSpec &t) const
{
    auto it = m_.find(key(t));
    return !never(t) && it != m_.end() && it->second.disabled;
}

bool Guard::strike(const TargetSpec &t, const std::string &why)
{
    Mapping &g = m_[key(t)];
    g.strikes++;
    g.clean_boots = 0;
    g.disabled = !never(t) && g.strikes >= t.strike_limit;
    if (g.disabled)
        log_error("guard: %s disabled at strike %d: %s; not asked for again until cluster-render's version "
                  "changes or `cluster-render guard-reset`",
                  key(t).c_str(), g.strikes, why.c_str());
    else
        log_warn("guard: %s: strike %d%s: %s", key(t).c_str(), g.strikes,
                 never(t) ? " (never disabled)" : (" of " + std::to_string(t.strike_limit)).c_str(), why.c_str());
    save();
    return g.disabled;
}

void Guard::excuse(const TargetSpec &t, const std::string &why) const
{
    log_info("guard: %s: no strike: %s", key(t).c_str(), why.c_str());
}

std::string Guard::status_text() const
{
    std::string s;
    for (const auto &kv : m_)
        s += "guard: " + kv.first + ": " + (kv.second.disabled ? "disabled, " : "") +
             std::to_string(kv.second.strikes) + " strike" + (kv.second.strikes == 1 ? "" : "s") + "\n";
    return s;
}

void Guard::save() const
{
    std::string text = "version " + version_ + "\nboot " + boot_ + "\n";
    for (const auto &kv : m_)
        text += kv.first + " " + (kv.second.disabled ? "1 " : "0 ") + std::to_string(kv.second.clean_boots) + " " +
                std::to_string(kv.second.strikes) + "\n";
    const std::string tmp = path_ + ".tmp", dir = path_.substr(0, path_.rfind('/') + 1);
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const bool whole = fd >= 0 && write(fd, text.data(), text.size()) == (ssize_t)text.size() && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    if (!whole || rename(tmp.c_str(), path_.c_str()) != 0) {
        unlink(tmp.c_str());
        log_warn("guard: cannot write %s; the strikes are kept until cluster-render stops", path_.c_str());
        return;
    }
    const int dfd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

}  // namespace crs
