// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Hardware H.264 decoder: Annex-B byte stream in, 4:2:0 atlases (yuv_atlas.h)
 * out.
 *
 * Runs on the head unit's Intel Media SDK (libmfxhw64.so, API 1.24) over
 * VA-API on a DRM render node:
 *   - Annex-B bytes are framed into access units (nal_reader.h). Each one is
 *     passed as a complete frame, so a picture is decoded as soon as its
 *     access unit is complete.
 *   - MSDK decodes to NV12 in GPU memory that it allocates itself ("opaque"
 *     surfaces) and shares with MSDK VPP.
 *   - VPP scales NV12 to NV12 on the GPU, to the requested size, into one
 *     of two reused system-memory surfaces, by turns, and the CPU packs its
 *     planes into one reused atlas with its levels rounded
 *     (pack_nv12_atlas). Nothing is converted to colour on MGU: the
 *     cluster's GPU does that (the map shader).
 *   - VPP renders into video memory, which MSDK copies into the surface.
 *     With gpu_copy the session asks for that copy on the GPU (GPUCopy on:
 *     MSDK's CM copy). The unit's library copies on the CPU (its FastCopy)
 *     without it, when its CM runtime (igfxcmrt64.so) does not start, and
 *     for a surface whose pitch or planes are not 16-byte aligned; the
 *     surfaces are page-aligned with a pitch a multiple of 64.
 *   - GPU copy failing once ends it for the process. A session that cannot
 *     be made or set up with it (that library fails Init when the CM copy
 *     it sets up there fails) is made again without, and the sequence
 *     starts over in it as it began. A failed VPP run loses its picture
 *     and the session goes on as it is. Only after 3 VPP runs in a row
 *     failed with GPU copy (a successful run starts the count again) does
 *     the session move its output planes off 16-byte alignment so that the
 *     library copies on the CPU, its decoder's references kept, and the
 *     process stops asking for GPU copy (UNVERIFIED that its VPP takes such
 *     a surface; if it does not, every VPP run fails until the next
 *     sequence, which starts without GPU copy).
 *   - A picture that VPP turns out equal, byte for byte, to the one before
 *     (its other surface) is not packed: it comes out with no atlas and
 *     `same` set. A still map whose phone repeats its picture costs no
 *     packing.
 * A frame fed without a picture (feed's picture false, to catch up with a
 * backlog) is decoded as every frame must be, for the frames that refer to
 * it, and waited for (its decode task only), but VPP and the packing are
 * skipped: it comes out with no atlas.
 * Frames come out in decoding order, each as soon as it is decoded. A stream
 * with B frames needs display order: the first B slice switches the decoder
 * to it at the next SPS, and B frames before that come out of order. The
 * first B slice, and each sequence that starts in another order than the
 * last, are logged.
 *
 * Decoding starts at the first access unit that carries an SPS; anything
 * before it is dropped. Damaged or incomplete access units (a lost packet)
 * are decoded as they are: MSDK conceals what is missing and flags the frame
 * (VideoFrame::corrupted). An access unit that fails to decode is skipped;
 * only kFailLimit in a row give the sequence up, and the decoder then waits
 * for the next SPS (decoding() false). A new SPS that needs a larger picture or more
 * surfaces restarts the decoder, in a new MSDK session, from that access
 * unit. With out_w/out_h 0 the output is the size of the sequence the
 * decoder was last started for.
 *
 * Not thread-safe: call every method from one thread. Frames are delivered
 * synchronously from feed() and flush().
 *
 * Checked offline:
 *   - the vendored MSDK structs, constants and prototypes against Intel's
 *     headers;
 *   - every import against the unit's libmfxhw64, libva and libva-drm, by
 *     export list and by loading with RTLD_NOW under the unit's ld.so;
 *   - the whole decode and VPP path against the unit's libmfxsw64 (the same
 *     MSDK release, software implementation): on the car's capture at its
 *     own 1920x900, 40 of 40 atlases equal ffmpeg's decoded planes byte for
 *     byte, and the decoder also scales it to 1088x480;
 *   - the NAL framing by test/nal_reader_test.cpp;
 *   - GPU copy in the unit's libmfxhw64 (its code): on VA-API, MFXInitEx
 *     leaves CM copy allowed only with GPUCopy on (default: off), Init sets
 *     it up, and the video-to-system copy takes it for NV12 with the
 *     alignments above, a width up to 0xff80, a height up to 0xff8 and the
 *     UV plane at most 0x4000 rows after Y.
 * On the car it decodes with the MSDK hardware library through VA-API
 * (render node, VA display, GPU surfaces, opaque decode-to-VPP).
 * UNVERIFIED (CAR-ONLY): that it honours DecodedOrder as the software
 * library does, and whether the phone's stream has B frames. Frames
 * without a picture (decode synced alone, no VPP) are checked against
 * neither library, nor is src_crop: VPP taking each picture's replaced
 * crop, the restart when a picture's rect after the crop differs from the
 * one VPP was set up for, and DecodeHeader reading a later SPS while the
 * decoder runs (Intel's reference allows it at any time). Nor is GPU copy:
 * the CM runtime starting on the unit, its copy's result and cost. */
#ifndef WOC_DECODER_MSDK_H
#define WOC_DECODER_MSDK_H

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "frame_clock.h"
#include "source_crop.h"

namespace woc {

struct VideoFrame {
    uint32_t w, h;        /* the picture's size */
    uint16_t corrupted;   /* MSDK's mfxFrameData::Corrupted flags; 0: decoded whole */
    /* The picture is the last one that came out with a picture (an atlas or
     * same), byte for byte; atlas is null. */
    bool same;
    /* Its atlas, w x atlas_height(h) bytes, the decoder's buffer. The sink
     * may change it in place, or swap it for another buffer (the decoder
     * sizes what it gets back for the next frame). Valid until the next
     * feed/flush/shutdown. Null for a frame decoded without a picture, and
     * for one the same as the last. */
    std::vector<uint8_t> *atlas;
    /* Access units the decoder took in after this frame's own before it
     * output this one: 0 when each comes out as it is decoded; -1 unknown
     * (MSDK did not carry the frame's number to it). */
    int32_t held;
};

class Decoder {
public:
    using FrameSink = std::function<void(const VideoFrame &)>;
    /* An SPS NAL unit from its header byte, without a start code. */
    using SpsSink = std::function<void(const uint8_t *nal, size_t len)>;
    static const int kFailLimit = 30;

    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    /* dev: the render node, "/dev/dri/renderD128". out_w/out_h: output size,
     * even, or both 0 for the stream's own size (the crop's, when it applies).
     * level_bits: low bits of each level rounded away in the atlas
     * (round_levels, yuv_atlas.h); 0: exact. gpu_copy: VPP's output copied
     * to system memory on the GPU.
     * crop: the part of a picture of its size that VPP scales to the output
     * (source_crop.h), decided per picture, after waiting for its decode,
     * while its sequence (the size each SPS gives) is of the crop's size or
     * needs another VPP input rect; the pictures of any other sequence go to
     * VPP whole, without the wait. */
    bool init(const char *dev, uint32_t out_w, uint32_t out_h, unsigned level_bits, bool gpu_copy, std::string *err,
              const SourceCrop &crop = SourceCrop());

    /* Feeds Annex-B bytes, split anywhere; cb gets each decoded frame.
     * end_of_au: the bytes end exactly at the end of an access unit (one
     * whole frame per call), so that frame is decoded now instead of when the
     * next one starts. picture false: the frames that come out of this call
     * have no atlas. Returns false, with *err, when an access unit failed
     * to decode or a sequence could not start, or before init(). */
    bool feed(const uint8_t *data, size_t len, const FrameSink &cb, std::string *err, bool end_of_au = false,
              bool picture = true);

    /* The next picture comes with its atlas even when it is the last one
     * again. */
    void forget_last();

    /* Gives the newest frame that came out, when it came out without a
     * picture (feed's picture false), its picture now, through cb as feed()
     * would: while its surface still holds it (the decoder has not taken
     * that surface since), and unless its picture needs another VPP
     * input rect (a restart starts at an access unit). False when there is
     * no such frame, or with *err set when its VPP run failed. True does not
     * mean cb was called: VPP may hold the input or give no output. */
    bool picture_skipped(const FrameSink &cb, std::string *err);

    /* End of stream: decodes what is buffered and delivers every remaining
     * frame. The next feed() starts a new stream at its first SPS. */
    void flush(const FrameSink &cb);

    void shutdown();

    /* Initialised and holding a sequence: false before the first SPS and
     * after a sequence was given up, until the next SPS. */
    bool decoding() const;

    /* What comparing each picture with the one before and packing it took,
     * in microseconds. */
    crk::Spread pack_us() const;
    /* The wait for each picture's VPP run, its copy to system memory
     * included, and, unless src_crop's check waited for it already, for its
     * decode, in microseconds. */
    crk::Spread vpp_us() const;
    /* Whether the current or last sequence outputs in display order. */
    bool display_order() const;

    /* Called with each SPS of an access unit the decoder takes, from
     * feed() and flush(). */
    void on_sps(SpsSink sink) { sps_sink_ = std::move(sink); }

private:
    struct State;
    std::unique_ptr<State> s_;
    SpsSink sps_sink_;
};

}  // namespace woc

#endif
