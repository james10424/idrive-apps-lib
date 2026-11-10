/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "stack.h"

#include <algorithm>

namespace crs {

const char *klass_name(Klass k)
{
    switch (k) {
    case Klass::Ambient: return "ambient";
    case Klass::Overlay: return "overlay";
    case Klass::Alert: return "alert";
    default: return "content";
    }
}

bool klass_from_name(const std::string &s, Klass *out)
{
    static const Klass all[] = {Klass::Ambient, Klass::Content, Klass::Overlay, Klass::Alert};
    for (Klass k : all)
        if (s == klass_name(k)) {
            *out = k;
            return true;
        }
    return false;
}

Arrangement arrange(std::vector<StackItem> items)
{
    std::stable_sort(items.begin(), items.end(), [](const StackItem &a, const StackItem &b) {
        if (a.klass != b.klass)
            return a.klass < b.klass;
        if (a.z != b.z)
            return a.z < b.z;
        return a.seq < b.seq;
    });
    Arrangement r;
    /* The topmost drawable exclusive layer; everything below it waits. */
    size_t floor = 0;
    for (size_t i = items.size(); i-- > 0;)
        if (items[i].visible && items[i].ready && items[i].exclusive) {
            floor = i;
            break;
        }
    for (size_t i = 0; i < items.size(); i++) {
        const StackItem &it = items[i];
        r.order.push_back(it.id);
        if (it.visible && it.ready)
            (i < floor ? r.suspended : r.drawn).push_back(it.id);
        if (it.visible && (it.ready || it.prewarm)) {
            r.wanted = true;
            r.contend = r.contend || it.contend;
            r.guide_hold = r.guide_hold && it.guide_hold;
            r.yield = r.yield && it.yield;
        }
    }
    return r;
}

}  // namespace crs
