// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The decoder against the fake MSDK (fake_msdk.h). Its src_crop path: a
 * CarPlay sequence goes from decode to VPP without waiting for the decode
 * task, an Android Auto one is waited for and cropped, and a session that
 * goes from one to the other and back sets VPP up for each. Its pictures: a
 * picture equal to the one before comes out as the same, unpacked, and the
 * others packed with their levels rounded; after forget_last, the same
 * picture once more packed. GPU copy: asked for with
 * MFXInitEx on page-aligned surfaces. A session that cannot be made or set
 * up with it is made again without, the sequence starting over as it began
 * (a rect restart's exemption kept), and no session in the process asks
 * for it again. A VPP run failing with it loses that picture and the
 * session goes on as it is; three in a row move its output planes off
 * 16-byte alignment (the CPU copy), and every picture after them comes
 * out. Each failure runs in a child process, since the fallback is the
 * process's. Each SPS goes to the SPS sink. Each frame says how many frames
 * the decoder took after it before it came out: none when each comes out as
 * it is decoded, none for a frame taken and never output, the lag of
 * pictures held for display order, and unknown when the library does not
 * carry the bitstream's TimeStamp. A frame decoded without a picture gets it
 * late while its surface still holds it: once, the newest one's, cropped
 * as any other, and none once a later frame came out, once the decoder may
 * have taken its surface, or when its picture needs another VPP rect. */
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <stdio.h>

#include <string>
#include <vector>

#include "decoder_msdk.h"
#include "fake_msdk.h"

using fake_msdk::access_unit;
using fake_msdk::calls;
using fake_msdk::count;
using woc::Decoder;
using woc::SourceCrop;
using woc::VideoFrame;

static int failures = 0;

/* body in a child process, so the process-wide GPU copy fallback starts
 * afresh; a failure in it is one here. */
static void isolated(const char *what, const std::function<void()> &body)
{
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        failures = 0;
        body();
        fflush(stderr);
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void fault(const char *call, int status, int after = 0, int times = 0)
{
    fake_msdk::gpu_fault.call = call;
    fake_msdk::gpu_fault.status = status;
    fake_msdk::gpu_fault.after = after;
    fake_msdk::gpu_fault.times = times;
}

static void expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        for (const std::string &c : calls)
            fprintf(stderr, "    %s\n", c.c_str());
        failures++;
    }
}

namespace {

const char *const kCarPlay = "1088x480", *const kAndroidAuto = "1280x720";

struct Run {
    Decoder dec;
    /* Each frame: its atlas's first byte, 0 for the same picture again, -1
     * for none. */
    std::vector<int> frames;
    std::vector<uint8_t> pictures;   /* the atlases' first bytes */
    bool whole = true;               /* every atlas one level throughout */
    std::vector<int> held;           /* each frame's VideoFrame::held */

    explicit Run(const char *crop, unsigned level_bits = 0, bool gpu_copy = false)
    {
        calls.clear();
        SourceCrop c;
        parse_source_crop(crop, &c);
        std::string err;
        if (!dec.init("/dev/null", 1088, 480, level_bits, gpu_copy, &err, c))
            fprintf(stderr, "init: %s\n", err.c_str());
    }
    Decoder::FrameSink sink()
    {
        return [this](const VideoFrame &f) {
            expect(f.w == 1088 && f.h == 480, "every frame of the output size");
            expect(!(f.same && f.atlas), "the same picture has no atlas");
            held.push_back(f.held);
            frames.push_back(f.atlas ? (*f.atlas)[0] : f.same ? 0 : -1);
            if (!f.atlas)
                return;
            pictures.push_back((*f.atlas)[0]);
            for (uint8_t b : *f.atlas)
                whole = whole && b == (*f.atlas)[0];
        };
    }
    void feed(const char *sps, bool idr, uint8_t content, bool picture = true)
    {
        const std::string au = access_unit(sps, idr, content);
        std::string err;
        dec.feed((const uint8_t *)au.data(), au.size(), sink(), &err, true, picture);
    }
    /* picture_skipped through this run's sink. */
    bool late()
    {
        std::string err;
        const bool ok = dec.picture_skipped(sink(), &err);
        expect(err.empty(), "a late picture without an error");
        return ok;
    }
    /* A sequence: its SPS with an IDR, then n - 1 P frames. */
    void sequence(const char *size, int n, uint8_t first)
    {
        feed(size, true, first);
        for (int i = 1; i < n; i++)
            feed(nullptr, false, (uint8_t)(first + i));
    }
};

const char *const kAndroidAutoCrop = "1280x720:0,0,1280,564";

}  // namespace

int main()
{
    {
        /* Each SPS to the SPS sink, from its header byte to the next start
         * code, once whether its sequence goes on or restarts the decoder. */
        Run r("");
        std::vector<std::string> sps;
        r.dec.on_sps([&](const uint8_t *nal, size_t len) { sps.emplace_back((const char *)nal, len); });
        r.sequence(kCarPlay, 3, 10);
        r.sequence(kAndroidAuto, 2, 20);
        expect(sps == std::vector<std::string>({"\x67" "1088x480", "\x67" "1280x720"}), "each SPS to the SPS sink");
        expect(r.pictures.size() == 5, "each SPS's sequence decoded");
    }
    {
        /* held: a frame out as it is decoded holds none, and a frame the
         * decoder takes without output (a damaged one) is not held. */
        Run r("");
        fake_msdk::swallow = 12;
        r.sequence(kCarPlay, 5, 10);
        fake_msdk::swallow = 0;
        expect(r.pictures == std::vector<uint8_t>({10, 11, 13, 14}), "a frame taken without output: the others out");
        expect(r.held == std::vector<int>({0, 0, 0, 0}), "a frame taken without output: none held after it");
    }
    {
        /* Pictures held for display order: each out two frames late. */
        Run r("");
        fake_msdk::delay = 2;
        r.sequence(kCarPlay, 6, 20);
        fake_msdk::delay = 0;
        expect(r.pictures == std::vector<uint8_t>({20, 21, 22, 23}), "held pictures: out in order");
        expect(r.held == std::vector<int>({2, 2, 2, 2}), "held pictures: two frames taken after each");
    }
    {
        /* A library that does not carry the TimeStamp: held unknown. */
        Run r("");
        fake_msdk::timestamps = false;
        r.sequence(kCarPlay, 3, 30);
        fake_msdk::timestamps = true;
        expect(r.held == std::vector<int>({-1, -1, -1}), "no TimeStamp carried: held unknown");
    }
    {
        Run r(kAndroidAutoCrop);
        r.sequence(kCarPlay, 5, 10);
        expect(r.pictures == std::vector<uint8_t>({10, 11, 12, 13, 14}), "carplay with a crop: every picture");
        expect(r.held == std::vector<int>(5, 0), "carplay: each frame out as it is decoded");
        expect(count("decode-sync") == 0, "carplay with a crop: no wait for the decode task");
        expect(count("vpp 0,0,1088,480") == 5 && count("vpp-sync") == 5, "carplay with a crop: VPP on the whole picture");
        expect(count("init 1088x480") == 1, "carplay with a crop: one sequence");
    }
    {
        Run r(kAndroidAutoCrop);
        r.sequence(kAndroidAuto, 5, 20);
        expect(r.pictures == std::vector<uint8_t>({20, 21, 22, 23, 24}), "android auto: every picture");
        expect(count("vpp-init 0,0,1280,564") == 1, "android auto: VPP set up for the crop");
        expect(count("decode-sync") == 5 && count("vpp 0,0,1280,564") == 5, "android auto: each picture waited for and cropped");
        bool ordered = true;
        for (size_t i = 0; i < calls.size(); i++)
            if (calls[i] == "vpp 0,0,1280,564")
                ordered = ordered && i > 0 && calls[i - 1] == "decode-sync";
        expect(ordered, "android auto: the wait comes before VPP");
    }
    {
        /* One session: Android Auto, then CarPlay's smaller (compatible)
         * sequence, then Android Auto's larger (incompatible) one. */
        Run r(kAndroidAutoCrop);
        r.sequence(kAndroidAuto, 3, 30);
        r.sequence(kCarPlay, 3, 40);
        const size_t carplay_from = calls.size();
        r.feed(nullptr, false, 50);
        r.feed(nullptr, false, 51);
        const std::vector<std::string> carplay(calls.begin() + (long)carplay_from, calls.end());
        r.sequence(kAndroidAuto, 3, 60);
        expect(r.pictures == std::vector<uint8_t>({30, 31, 32, 40, 41, 42, 50, 51, 60, 61, 62}),
               "android auto, carplay, android auto: every picture");
        expect(count("init 1280x720") == 2 && count("init 1088x480") == 1, "a sequence set up for each change");
        expect(count("vpp-init 0,0,1280,564") == 2 && count("vpp-init 0,0,1088,480") == 1, "VPP set up for each rect");
        expect(count("vpp 0,0,1280,564") == 6 && count("vpp 0,0,1088,480") == 5, "each picture in its own rect");
        expect(carplay == std::vector<std::string>({"vpp 0,0,1088,480", "vpp-sync", "vpp 0,0,1088,480", "vpp-sync"}),
               "carplay after android auto: no wait for the decode task");
        /* The compatible SPS's picture is waited for once, to find it needs
         * another rect; Android Auto's six are waited for. */
        expect(count("decode-sync") == 7, "the waits: android auto's pictures and the change");
    }
    {
        /* A compatible SPS that makes a sequence croppable: a larger stream,
         * then Android Auto's within its surfaces. */
        Run r(kAndroidAutoCrop);
        r.sequence("1920x1080", 2, 70);
        r.sequence(kAndroidAuto, 2, 80);
        expect(r.pictures == std::vector<uint8_t>({70, 71, 80, 81}), "1920x1080 then android auto: every picture");
        expect(count("vpp 0,0,1920,1080") == 2 && count("vpp 0,0,1280,564") == 2, "the larger whole, android auto cropped");
        expect(count("init 1280x720") == 1 && count("vpp-init 0,0,1280,564") == 1, "android auto set up for its rect");
    }
    {
        /* Pictures equal to the one before: VPP's last output is what they
         * are compared with (a frame without a picture has none), an SPS that
         * keeps the sequence keeps it, and a new sequence starts afresh. */
        Run r("", 2);
        r.feed(kCarPlay, true, 5);
        r.feed(nullptr, false, 5);
        r.feed(nullptr, false, 5);
        r.feed(nullptr, false, 6);
        r.feed(nullptr, false, 6);
        r.feed(nullptr, false, 9, false);
        r.feed(nullptr, false, 6);
        r.feed(nullptr, false, 5);
        r.feed(kCarPlay, true, 5);
        r.feed(kAndroidAuto, true, 5);
        expect(r.frames == std::vector<int>({4, 0, 0, 8, 0, -1, 0, 4, 0, 4}),
               "the same pictures, the others rounded to 2 bits");
        expect(r.whole, "each atlas packed whole");
        expect(count("vpp-sync") == 9, "VPP on every frame with a picture");
        expect(r.dec.pack_us().n == 9, "each compared or packed");
        /* forgotten, the same picture again comes with its atlas, once */
        r.dec.forget_last();
        r.feed(nullptr, false, 5);
        r.feed(nullptr, false, 5);
        expect(r.frames.size() == 12 && r.frames[10] == 4 && r.frames[11] == 0,
               "after forget_last the last picture again is packed, then the same");
    }
    {
        Run r("", 0, true);
        r.sequence(kCarPlay, 3, 1);
        expect(r.pictures == std::vector<uint8_t>({1, 2, 3}), "gpu copy: every picture");
        expect(count("session gpucopy 1") == 1 && count("session") == 0, "gpu copy: asked for with MFXInitEx");
        expect(count("vpp-out unaligned") == 0, "gpu copy: VPP's output page-aligned, its pitch a multiple of 64");
        expect(r.dec.vpp_us().n == 3, "the wait for each VPP run timed");
    }
    {
        Run r("");
        r.sequence(kCarPlay, 2, 1);
        expect(count("session") == 1 && count("session gpucopy 1") == 0, "no gpu copy: MFXInit");
    }
    /* Each step of a session's setup failing with GPU copy. */
    const struct {
        const char *call, *what;
        int status;
    } steps[] = {{"MFXInitEx", "MFXInitEx refused", -3},
                 {"SetHandle", "SetHandle failing", -6},
                 {"DECODE_QueryIOSurf", "QueryIOSurf failing", -3},
                 {"VPP_Init", "VPP's Init failing", -4}};
    for (const auto &step : steps)
        isolated(step.what, [&step] {
            fault(step.call, step.status);
            Run r("", 0, true);
            r.sequence(kCarPlay, 2, 1);
            expect(r.pictures == std::vector<uint8_t>({1, 2}), step.what);
            expect(count("session gpucopy 1") == 1, "GPU copy asked for once");
            Run again("", 0, true);
            again.sequence(kCarPlay, 2, 3);
            expect(count("session gpucopy 1") == 0 && again.pictures == std::vector<uint8_t>({3, 4}),
                   "a decoder set up again with it: no GPU copy in this process");
        });
    isolated("a VPP run failing alone with GPU copy", [] {
        fault("VPP_Run", -17, 1, 1);
        Run r("", 0, true);
        r.sequence(kCarPlay, 4, 1);
        expect(r.pictures == std::vector<uint8_t>({1, 3, 4}), "that picture lost, every other one out");
        expect(count("session gpucopy 1") == 1 && count("session") == 0 && count("init 1088x480") == 1,
               "in the same session");
        expect(count("vpp-out shifted") == 0, "its output still copied on the GPU");
        Run again("", 0, true);
        again.sequence(kCarPlay, 2, 5);
        expect(count("session gpucopy 1") == 1 && again.pictures == std::vector<uint8_t>({5, 6}),
               "a decoder set up again still asks for GPU copy");
    });
    isolated("VPP runs failing with GPU copy, a successful one between", [] {
        fault("VPP_Run", -17, 0, 2);
        Run r("", 0, true);
        r.sequence(kCarPlay, 3, 1);
        fault("VPP_Run", -17, 0, 2);
        for (uint8_t i = 4; i <= 6; i++)
            r.feed(nullptr, false, i);
        expect(r.pictures == std::vector<uint8_t>({3, 6}), "the failed pictures lost");
        expect(count("vpp-out shifted") == 0, "the success starts the count again: still copied on the GPU");
    });
    isolated("three VPP runs in a row failing with GPU copy", [] {
        fault("VPP_Run", -17);
        Run r("", 0, true);
        r.sequence(kCarPlay, 6, 1);
        expect(r.pictures == std::vector<uint8_t>({4, 5, 6}),
               "those pictures lost, the session going on with every picture after them");
        expect(count("session gpucopy 1") == 1 && count("session") == 0 && count("init 1088x480") == 1,
               "in the same session");
        expect(count("vpp-out shifted") == 3, "its output copied on the CPU");
        r.sequence(kAndroidAuto, 2, 9);
        expect(count("session") == 1, "the next sequence in a session without GPU copy");
    });
    isolated("a rect restart's exemption kept through the fallback", [] {
        /* Android Auto's sequence whose pictures are CarPlay's size: a rect
         * restart, then converted as they are. VPP's Init fails with GPU
         * copy at that restart. */
        fault("VPP_Init", -4, 1);
        fake_msdk::picture_size = "1088x480";
        Run r(kAndroidAutoCrop, 0, true);
        r.sequence(kAndroidAuto, 3, 1);
        expect(r.pictures == std::vector<uint8_t>({1, 2, 3}), "every picture");
        expect(count("init 1280x720") == 3, "the first start, the restart with GPU copy, and without");
    });
    {
        /* A frame decoded without a picture gets it late (picture_skipped)
         * while nothing was decoded since: once, never after a later decode,
         * and nothing when the last frame had its picture. */
        Run r("");
        r.feed(kCarPlay, true, 10);
        expect(!r.late(), "late: nothing when the last frame had its picture");
        r.feed(nullptr, false, 11, false);
        expect(r.late(), "late: the skipped frame's picture");
        expect(!r.late(), "late: only once");
        r.feed(nullptr, false, 12, false);
        r.feed(nullptr, false, 13);
        expect(!r.late(), "late: nothing once a later frame was decoded");
        r.feed(nullptr, false, 14, false);
        r.feed(nullptr, false, 15, false);
        expect(r.late(), "late: the newest of two skipped frames");
        expect(r.pictures == std::vector<uint8_t>({10, 11, 13, 15}), "late: each picture the newest frame's");
        expect(r.frames == std::vector<int>({10, -1, 11, -1, 13, -1, -1, 15}), "late: frames in order");
    }
    {
        /* A sequence src_crop checks: the late picture is cropped as any
         * other, without waiting for its decode again. */
        Run r(kAndroidAutoCrop);
        r.feed(kAndroidAuto, true, 20);
        r.feed(nullptr, false, 21, false);
        const int syncs = count("decode-sync");
        expect(r.late(), "late: a sequence src_crop checks");
        expect(count("decode-sync") == syncs, "late: its decode not waited for again");
        expect(count("vpp 0,0,1280,564") == 2, "late: cropped");
        expect(r.pictures == std::vector<uint8_t>({20, 21}), "late: the cropped sequence's pictures");
    }
    {
        /* A picture that needs another VPP input rect gets no late picture
         * (a restart starts at an access unit); the next frame's picture
         * restarts the decoder. */
        Run r(kAndroidAutoCrop);
        r.feed(kAndroidAuto, true, 22);
        fake_msdk::picture_size = "1088x480";
        r.feed(nullptr, false, 23, false);
        expect(!r.late(), "late: none for a picture that needs another rect");
        expect(count("init 1280x720") == 1, "late: no restart outside an access unit");
        fake_msdk::picture_size = "";
    }
    {
        /* An access unit that outputs no frame leaves the skipped frame's
         * surface alone when the decoder keeps it as a reference: its late
         * picture still comes. Without that, the decoder may be handed the
         * surface, and no late picture comes. */
        fake_msdk::refs = 1;
        Run r("");
        r.feed(kCarPlay, true, 30);
        r.feed(nullptr, false, 31, false);
        fake_msdk::swallow = 32;
        r.feed(nullptr, false, 32);
        expect(r.late(), "late: after an access unit that output nothing");
        expect(r.pictures == std::vector<uint8_t>({30, 31}), "late: the skipped frame's picture");
        fake_msdk::refs = 0;
        Run u("");
        u.feed(kCarPlay, true, 33);
        u.feed(nullptr, false, 34, false);
        fake_msdk::swallow = 35;
        u.feed(nullptr, false, 35);
        expect(!u.late(), "late: none once its surface was handed to the decoder");
        fake_msdk::swallow = 0;
    }
    {
        Run r("");
        r.sequence(kAndroidAuto, 3, 90);
        r.sequence(kCarPlay, 3, 95);
        expect(count("decode-sync") == 0, "no crop: no wait for the decode task");
        expect(count("header 1280x720") == 1 && count("header 1088x480") == 0, "no crop: no header read while running");
    }
    if (failures) {
        fprintf(stderr, "decoder_test: %d failures\n", failures);
        return 1;
    }
    printf("decoder_test: ok\n");
    return 0;
}
