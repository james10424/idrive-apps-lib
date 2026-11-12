// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test for the picture pace (picture_pace.h). Off, every frame gets a
 * picture. At picture_fps 10, 15 and 30, a phone at any rate from 1 to 60
 * frames a second, on time or jittered by up to 8 ms, gives picture_fps
 * pictures a second when it is faster (within two a minute), the late
 * pictures included, and a picture for every frame when it is not. A frame
 * a quarter of an interval early takes its slot; a skipped frame's late
 * picture is due at its slot and the lead after it, and takes that slot;
 * slots that passed are not made up; reset starts the grid again. */
#include <algorithm>
#include <cstdio>
#include <vector>

#include "../src/picture_pace.h"

using namespace woc;

static int failures;
static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

/* Frames at times t (ascending), each given its picture when due, or late,
 * as the producer gives it, when no newer frame came by its late time.
 * Returns how many pictures were made. */
static int simulate(unsigned fps, const std::vector<int64_t> &t)
{
    PicturePace p(fps);
    int pictures = 0;
    int64_t late_at = -1;   /* the skipped newest frame's late time; -1 for none */
    for (int64_t now : t) {
        if (late_at >= 0 && now > late_at) {
            p.took(late_at);
            pictures++;
        }
        late_at = -1;
        if (p.due(now)) {
            p.took(now);
            pictures++;
        } else {
            late_at = now + p.late_in_ms(now);
        }
    }
    return pictures + (late_at >= 0);
}

/* A phone at fps for secs seconds, each frame up to jitter ms early or late. */
static std::vector<int64_t> phone(unsigned fps, int secs, int jitter)
{
    std::vector<int64_t> t;
    uint32_t seed = 12345;
    for (uint64_t k = 0; k < (uint64_t)fps * secs; k++) {
        seed = seed * 1103515245u + 12345u;
        const int j = jitter ? (int)((seed >> 16) % (2 * jitter + 1)) - jitter : 0;
        t.push_back(1000 + (int64_t)(k * 1000 / fps) + j);
    }
    std::sort(t.begin(), t.end());
    return t;
}

int main()
{
    {
        ok("off: every frame", simulate(0, phone(30, 10, 0)) == 300);
    }
    {
        /* picture_fps a second from a faster phone, fewer from a slower one,
         * at every rate and with jitter. */
        const int secs = 60;
        bool exact = true, jittered = true, slower = true;
        for (unsigned target : {10u, 15u, 30u}) {
            for (unsigned fps = 1; fps <= 60; fps++) {
                const int want = (int)std::min(fps, target) * secs;
                const int on_time = simulate(target, phone(fps, secs, 0));
                const int jitter = simulate(target, phone(fps, secs, 8));
                if (fps > target) {
                    if (on_time < want - 1 || on_time > want + 1) {
                        std::printf("     %u from %u fps on time: %d, not %d\n", target, fps, on_time, want);
                        exact = false;
                    }
                    if (jitter < want - 2 || jitter > want + 2) {
                        std::printf("     %u from %u fps jittered: %d, not %d\n", target, fps, jitter, want);
                        jittered = false;
                    }
                } else if (on_time != want || jitter != want) {
                    std::printf("     %u from %u fps: %d and %d, not %d\n", target, fps, on_time, jitter, want);
                    slower = false;
                }
            }
        }
        ok("picture_fps a second from any faster phone", exact);
        ok("picture_fps a second from a jittered phone", jittered);
        ok("a picture for every frame of a slower phone", slower);
        /* A slot every 66.7 ms over 60 s, and the first picture. */
        ok("15 a second from 25 fps", simulate(15, phone(25, secs, 0)) == 15 * secs + 1);
        ok("15 a second from 22 fps", simulate(15, phone(22, secs, 0)) == 15 * secs + 1);
        ok("15 a second from 20 fps", simulate(15, phone(20, secs, 0)) == 15 * secs + 1);
    }
    {
        /* 15 a second: slots every 66.7 ms, a lead of 16 ms. */
        PicturePace p(15);
        ok("the first frame", p.due(0));
        p.took(0);
        ok("33 ms after: skipped", !p.due(33));
        ok("its late picture at the slot and the lead after it", p.late_in_ms(33) == 66 + 16 - 33);
        ok("then due", p.late_in_ms(82) == 0);
        ok("a frame a quarter interval early takes its slot", p.due(50));
        ok("a frame earlier does not", !p.due(49));
        p.took(82);   /* the late picture takes the slot at 66 */
        ok("after a late picture, the next slot's frame", !p.due(116) && p.due(117));
        p.took(140);
        ok("slots that passed are not made up", !p.due(150) && !p.due(183) && p.due(184));
        p.took(1000);
        ok("after a long pause, no burst", !p.due(1010) && !p.due(1033));
        p.reset();
        ok("reset: the next frame at once", p.due(1010));
        p.took(1010);
        ok("reset: the grid starts from it", !p.due(1059) && p.due(1060));
    }
    return failures ? 1 : 0;
}
