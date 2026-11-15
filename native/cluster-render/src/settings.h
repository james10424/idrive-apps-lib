/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* The service's settings beyond the shared Config's own (cluster-render.conf
 * documents each), the targets they declare, and the drop-ins: files a
 * tool installs beside the settings to add iDrive 8 targets of its own, a
 * widget in the cluster's widget menu (README.md "iDrive 8").
 *
 * On iDrive 8 a target needs no key of its own here: id8_<name>_<field> is
 * taken for any name of lowercase letters and digits and the generation-8
 * per-target fields (category, content, scene_id, size, linger_ms,
 * description, order, preview, background_id, layouts, focus), unset ones
 * empty. guard_path, which nothing reads, is taken and ignored. Every other
 * unknown key is still refused, so a misspelt field is an error, not a
 * setting nobody reads.
 *
 * A drop-in is read only when it is a regular file (no symlink) owned by
 * the owner (root on the unit) and writable by it alone, and it only adds:
 * id8_targets and id8_scene_ids_allowed keep every entry they had (+=), and
 * every other key it sets is an id8_<name>_ key of a target it adds. A
 * target it adds is a new name and a widget: in the widget category
 * (kWidgetCategory), its content a dynamic widget's and never the head
 * unit's own (kWidgetContent*), its scene id ours (kOurScenes), as is every
 * scene id it allows; the drop-ins together add at most kMaxDropInTargets.
 * Each goes onto a copy of the settings, which is kept only when the
 * targets still load (load_targets, as on iDrive 8) and each target it
 * adds has a content and a scene id no other target has; anything else
 * skips the file whole, with why, and the settings are as before it. A
 * third party's broken file never stops the service: the main file and
 * local_conf stay strict (an error there is a configuration fault, exit
 * 2). */
#ifndef CRS_SETTINGS_H
#define CRS_SETTINGS_H

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "slots.h"

namespace crs {

/* What a drop-in's target may be (above, README.md "iDrive 8"): the
 * cluster's widget category Ca_Widget_IC; a dynamic widget content, the
 * head unit's own left out; a scene id 0x4d4755xxxxxx ("MGU"). */
const uint64_t kWidgetCategory = 43100301;
const uint64_t kWidgetContentFirst = 43202001, kWidgetContentLast = 43205000;
const uint64_t kHeadUnitContentFirst = 43202020, kHeadUnitContentLast = 43202120;
const uint64_t kOurScenes = 0x4d4755000000ull;
const size_t kMaxDropInTargets = 8;
/* A widget's menu picture (id8_<name>_preview) is at most this. */
const size_t kMaxPreview = 1024 * 1024;

/* Every key this service reads beyond the shared Config's, with its
 * built-in default, and the id8_<name>_<field> family (above). */
void define_keys(cld::Config *cfg);

/* Generation 7's targets, or 8's from the id8_ keys: a category in place
 * of the slot, a content, no content class and no take-over. False, with
 * err, when one is missing or out of range. */
bool load_targets(const cld::Config &cfg, int generation, std::vector<TargetSpec> *out, std::string *err);

/* Reads the files matching drop_ins (glob patterns separated by spaces)
 * into cfg in the order of their paths, a file matched twice once, each as
 * above; one line per file into *report: "loaded PATH" or "skipped PATH:
 * why". */
void load_drop_ins(cld::Config *cfg, uid_t owner, std::vector<std::string> *report);

/* The picture at path into *png: a regular file (a symlink to one too) of
 * at most kMaxPreview, not a device or a pipe. False with why, *png
 * empty. */
bool read_preview(const std::string &path, std::vector<char> *png, std::string *why);

}  // namespace crs

#endif
