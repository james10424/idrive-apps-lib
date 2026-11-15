// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of AtlasScaler (atlas_scale.h): sizes, flat planes stay flat and
 * U, V and Y never mix, a 2:1 scale is the exact 2x2 average, non-integer
 * ratios keep a gradient's mean, rounding to level bits, refusals, a crop
 * (exact 1:1, scaled as the part cut out first), and the time a 1088x480
 * frame takes to scale to the HUD's sizes. */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "atlas_scale.h"
#include "yuv_atlas.h"

using namespace woc;

static int failures;
static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static std::vector<uint8_t> atlas(uint32_t w, uint32_t h, uint8_t y, uint8_t u, uint8_t v)
{
    std::vector<uint8_t> a((size_t)w * crk::atlas_height(h));
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++) a[(size_t)r * w + c] = y;
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t c = 0; c < w / 2; c++) {
            a[(size_t)(h + r) * w + c] = u;
            a[(size_t)(h + r) * w + w / 2 + c] = v;
        }
    return a;
}

int main()
{
    AtlasScaler s;
    std::vector<uint8_t> out;

    ok("refuses odd, upscaled or empty sizes",
       !s.configure(1088, 480, 453, 200, 0) && !s.configure(1088, 480, 1200, 200, 0) &&
           !s.configure(1088, 480, 452, 482, 0) && !s.configure(0, 480, 452, 200, 0));
    ok("1088x480 -> 452x200", s.configure(1088, 480, 452, 200, 0) && s.dst_bytes() == 452u * 300);

    const std::vector<uint8_t> flat = atlas(1088, 480, 200, 60, 180);
    s.scale(flat.data(), &out);
    bool flat_ok = out.size() == 452u * 300;
    for (uint32_t r = 0; r < 200 && flat_ok; r++)
        for (uint32_t c = 0; c < 452; c++) flat_ok = flat_ok && out[(size_t)r * 452 + c] == 200;
    for (uint32_t r = 0; r < 100 && flat_ok; r++)
        for (uint32_t c = 0; c < 226; c++)
            flat_ok = flat_ok && out[(size_t)(200 + r) * 452 + c] == 60 && out[(size_t)(200 + r) * 452 + 226 + c] == 180;
    ok("flat Y, U and V stay exact: the planes never mix", flat_ok);

    /* 2:1 is the 2x2 average */
    std::vector<uint8_t> g((size_t)8 * crk::atlas_height(8), 128);
    for (uint32_t r = 0; r < 8; r++)
        for (uint32_t c = 0; c < 8; c++) g[r * 8 + c] = (uint8_t)(r * 30 + c * 3);
    AtlasScaler h;
    ok("8x8 -> 4x4", h.configure(8, 8, 4, 4, 0));
    h.scale(g.data(), &out);
    bool avg = true;
    for (uint32_t r = 0; r < 4; r++)
        for (uint32_t c = 0; c < 4; c++) {
            const int want = (int)std::lround((g[(2 * r) * 8 + 2 * c] + g[(2 * r) * 8 + 2 * c + 1] + g[(2 * r + 1) * 8 + 2 * c] +
                                               g[(2 * r + 1) * 8 + 2 * c + 1]) / 4.0);
            avg = avg && std::abs((int)out[r * 4 + c] - want) <= 1;
        }
    ok("2:1 is the 2x2 average (within rounding)", avg);

    /* a horizontal ramp at 1088 -> 452 keeps its mean and stays monotonic */
    std::vector<uint8_t> ramp = atlas(1088, 480, 0, 128, 128);
    for (uint32_t r = 0; r < 480; r++)
        for (uint32_t c = 0; c < 1088; c++) ramp[(size_t)r * 1088 + c] = (uint8_t)(c * 255 / 1087);
    s.scale(ramp.data(), &out);
    double ms = 0, md = 0;
    bool mono = true;
    for (uint32_t c = 0; c < 1088; c++) ms += ramp[c];
    for (uint32_t c = 0; c < 452; c++) {
        md += out[c];
        if (c) mono = mono && out[c] >= out[c - 1];
    }
    ok("a ramp keeps its mean and order at 2.41:1", std::fabs(ms / 1088 - md / 452) < 0.6 && mono);

    AtlasScaler rb;
    rb.configure(1088, 480, 452, 200, 2);
    rb.scale(ramp.data(), &out);
    bool rounded = true;
    for (uint8_t b : out) rounded = rounded && (b % 4 == 0 || b == 255);
    ok("level bits: values rounded to multiples of 4 (255 kept)", rounded);

    ok("same size is allowed (a plain copy)", h.configure(8, 8, 8, 8, 0));
    h.scale(g.data(), &out);
    ok("and copies exactly", out == g);

    {
        /* rounded corners: a white frame, its corners black (Y 16, U and V
         * 128), the edges' middles and the inside untouched */
        AtlasScaler rc;
        rc.configure(1088, 480, 452, 200, 0);
        rc.set_corner_radius(16);
        std::vector<uint8_t> white(1088 * 720, 235);
        for (size_t i = 1088 * 480; i < white.size(); i++) white[i] = 200;
        rc.scale(white.data(), &out);
        const uint8_t *y = out.data(), *u = y + 452 * 200;
        ok("corners: the four corner pixels black", y[0] == 16 && y[451] == 16 && y[199 * 452] == 16 && y[199 * 452 + 451] == 16);
        ok("corners: the chroma corner neutral", u[0] == 128 && u[99 * 452 + 225] == 128);
        ok("corners: the edges' middles and the inside untouched", y[226] == 235 && y[100 * 452] == 235 && y[100 * 452 + 226] == 235 &&
                                                                       y[16 * 452 + 16] == 235);
        bool aa = false;
        for (uint32_t x = 0; x < 16; x++) aa = aa || (y[x] > 16 && y[x] < 235);
        ok("corners: anti-aliased along the curve", aa);
        rc.set_corner_radius(0);
        rc.scale(white.data(), &out);
        ok("radius 0: square again", out[0] == 235);
    }

    {
        /* black background: a night map (navy field Y 60, a road Y 120, a
         * label Y 220): the field goes black with neutral chroma, the road
         * and label stay; a day map (field Y 200) is left alone */
        std::vector<uint8_t> night(1088 * 720, 60);
        for (size_t i = 1088 * 480; i < night.size(); i++) night[i] = (i % 2) ? 150 : 110;   /* bluish U, V */
        for (uint32_t r = 200; r < 240; r++)
            for (uint32_t c = 0; c < 1088; c++) night[r * 1088 + c] = 120;
        for (uint32_t r = 300; r < 320; r++)
            for (uint32_t c = 500; c < 600; c++) night[r * 1088 + c] = 220;
        AtlasScaler kb;
        kb.configure(1088, 480, 452, 200, 0);
        kb.set_black_background(true);
        kb.scale(night.data(), &out);
        const uint8_t *y = out.data(), *u = y + 452 * 200;
        ok("black background: the navy field black", y[20 * 452 + 20] == 16 && kb.background() / 16 == 60);
        ok("black background: its chroma neutral", u[5 * 452 + 5] == 128);
        ok("black background: the road kept", y[91 * 452 + 100] == 120);
        ok("black background: the label kept", y[129 * 452 + 228] == 220);
        /* a park: a dark green block (Y about the field's, U a little below
         * neutral, V well below) on the night field - kept, chroma and all;
         * and a frame mostly park doesn't make the park the background */
        std::vector<uint8_t> park = night;
        for (uint32_t r = 40; r < 120; r++)
            for (uint32_t c = 100; c < 400; c++) park[r * 1088 + c] = 62;
        for (uint32_t r = 20; r < 60; r++)
            for (uint32_t c = 50; c < 200; c++) { park[1088 * 480 + r * 1088 + c] = 124; park[1088 * 480 + r * 1088 + 544 + c] = 116; }
        AtlasScaler kp;
        kp.configure(1088, 480, 452, 200, 0);
        kp.set_black_background(true);
        kp.scale(park.data(), &out);
        const uint8_t *py = out.data(), *pu = py + 452 * 200, *pv = pu + 226;
        ok("black background: a green park kept (luma and chroma)", py[33 * 452 + 100] == 62 && pu[16 * 452 + 50] < 128 && pv[16 * 452 + 50] < 122);
        ok("black background: the navy field still black beside the park", py[150 * 452 + 300] == 16);
        std::vector<uint8_t> parks(1088 * 720, 62);
        for (size_t r = 0; r < 240; r++)
            for (size_t c = 0; c < 544; c++) { parks[1088 * 480 + r * 1088 + c] = 124; parks[1088 * 480 + r * 1088 + 544 + c] = 116; }
        for (uint32_t r = 400; r < 480; r++)
            for (uint32_t c = 0; c < 1088; c++) parks[r * 1088 + c] = 40;
        for (size_t r = 200; r < 240; r++)
            for (size_t c = 0; c < 544; c++) { parks[1088 * 480 + r * 1088 + c] = 140; parks[1088 * 480 + r * 1088 + 544 + c] = 124; }
        AtlasScaler km;
        km.configure(1088, 480, 452, 200, 0);
        km.set_black_background(true);
        km.scale(parks.data(), &out);
        ok("black background: a frame mostly park keeps the park (not taken for the field)", out[50 * 452 + 200] == 62 && km.background() / 16 == 40);
        std::vector<uint8_t> day(1088 * 720, 200);
        for (size_t i = 1088 * 480; i < day.size(); i++) day[i] = 128;
        AtlasScaler kd;
        kd.configure(1088, 480, 452, 200, 0);
        kd.set_black_background(true);
        kd.scale(day.data(), &out);
        ok("black background: a day map left alone", out[20 * 452 + 20] == 200);
    }

    {
        /* a crop (a host's part of the map): 1:1 it is exactly that part,
         * Y and both chroma planes; scaled, it is the scale of that part
         * cut out first */
        std::vector<uint8_t> f(1088 * 720);
        for (size_t i = 0; i < f.size(); i++)
            f[i] = (uint8_t)((i * 7 + i / 1088 * 3) & 0xff);
        AtlasScaler cs;
        ok("crop: refused odd, outside the frame, or smaller than the output",
           !cs.configure(1088, 480, 560, 400, 0, 263, 40, 560, 400) &&
               !cs.configure(1088, 480, 560, 400, 0, 600, 40, 560, 400) &&
               !cs.configure(1088, 480, 564, 400, 0, 264, 40, 560, 400));
        ok("crop: 560x400 at 264,40, 1:1", cs.configure(1088, 480, 560, 400, 0, 264, 40, 560, 400) &&
                                               cs.dst_bytes() == 560u * 600);
        cs.scale(f.data(), &out);
        std::vector<uint8_t> cut(560 * 600);
        for (uint32_t r = 0; r < 400; r++)
            for (uint32_t c = 0; c < 560; c++) cut[r * 560 + c] = f[(r + 40) * 1088 + 264 + c];
        for (uint32_t r = 0; r < 200; r++)
            for (uint32_t c = 0; c < 280; c++) {
                cut[(400 + r) * 560 + c] = f[(480 + 20 + r) * 1088 + 132 + c];
                cut[(400 + r) * 560 + 280 + c] = f[(480 + 20 + r) * 1088 + 544 + 132 + c];
            }
        ok("crop: 1:1 is exactly that part of Y, U and V", out == cut);
        AtlasScaler half, whole;
        half.configure(1088, 480, 280, 200, 0, 264, 40, 560, 400);
        whole.configure(560, 400, 280, 200, 0);
        std::vector<uint8_t> a, b;
        half.scale(f.data(), &a);
        whole.scale(cut.data(), &b);
        ok("crop: scaled is the scale of the part cut out", a == b);
    }

    const struct { uint32_t w, h; } sizes[] = {{452, 200}, {560, 244}, {680, 300}, {800, 352}, {320, 140}};
    for (const auto &z : sizes) {
        AtlasScaler t;
        t.configure(1088, 480, z.w, z.h, 2);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) t.scale(ramp.data(), &out);
        const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        std::printf("     1088x480 -> %ux%u: %.2f ms a frame on this machine, %zu B (%.1fx less than 783,360)\n", z.w, z.h,
                    each, t.dst_bytes(), 783360.0 / t.dst_bytes());
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
