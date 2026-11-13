/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The mapping guard (README.md "Mapping guard"): a mapping the cluster
 * cannot take can make the KOMBI restart the cluster and the HUD, and the
 * service would map it again after every restart and every boot.
 *
 * A mapping is a target's (slot, scene id, content class); a hold whose
 * slot comes from the slot table (TargetSpec::table_slots) has one mapping
 * whichever of its slots it is in, so a move never escapes a strike and its
 * slots share one strike_limit. The slot manager
 * calls strike() against a mapping it holds to blame (slots.h). At its
 * target's strike_limit strikes the mapping is disabled: never asked for
 * again until cluster-render's version changes or the file is removed
 * (`cluster-render guard-reset`). A target with a strike_limit of 0, and the
 * map's mapping in slot 0x100 with scene 0x010004f0 whatever its limit,
 * only has its strikes counted and logged, also when the file says
 * otherwise. A strike stops counting once kCleanBoots boots have passed
 * without another against the same mapping; a disabled mapping keeps its
 * strikes.
 *
 * The strikes and the disabled mappings are kept in a file, written whole,
 * renamed into place and its folder synced at each change, so a boot never
 * asks for a disabled mapping. A file of another version is started over,
 * and so is one that cannot be read, with a warning; a file that cannot be
 * written leaves the guard working from memory, with a warning. */
#ifndef CRS_GUARD_H
#define CRS_GUARD_H

#include <stdint.h>
#include <map>
#include <string>

#include "slots.h"

namespace crs {

class Guard {
public:
    static const int64_t kWindowMs = 10000;
    static const int kCleanBoots = 3;

    /* boot_id: this boot's (a new one counts a boot for kCleanBoots). */
    void load(const std::string &path, const std::string &version, const std::string &boot_id);
    /* False for a mapping that is never disabled. */
    bool disabled(const TargetSpec &t) const;
    /* One strike against t's mapping, why = what it is blamed for; true
     * when it disables the mapping. */
    bool strike(const TargetSpec &t, const std::string &why);
    /* A loss not struck against t's mapping (slots.h on_availability),
     * logged with why. */
    void excuse(const TargetSpec &t, const std::string &why) const;
    /* "guard: <mapping>: disabled, 2 strikes" lines; empty without strikes. */
    std::string status_text() const;

private:
    struct Mapping {
        int strikes = 0;
        int clean_boots = 0;
        bool disabled = false;
    };
    static std::string key(const TargetSpec &t);
    static bool never(const TargetSpec &t);
    void save() const;

    std::string path_, version_, boot_;
    std::map<std::string, Mapping> m_;
};

}  // namespace crs

#endif
