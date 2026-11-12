// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "decoder_msdk.h"
#include "nal_reader.h"
#include "log.h"
#include "yuv_atlas.h"

#include "mfx/mfxvideo.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <vector>

/* libva, from va/va.h and va/va_drm.h; the unit has libva.so.1 and
 * libva-drm.so.1 (1.9900.0). */
extern "C" {
typedef void *VADisplay;
typedef int VAStatus;
#define VA_STATUS_SUCCESS 0x00000000
VADisplay vaGetDisplayDRM(int fd);
VAStatus vaInitialize(VADisplay dpy, int *major_version, int *minor_version);
VAStatus vaTerminate(VADisplay dpy);
const char *vaErrorStr(VAStatus error_status);
}

namespace woc {

using crk::atlas_height;
using crk::pack_nv12_atlas;

namespace {

const mfxU32 kSyncTimeoutMs = 1000;   /* longest wait for one frame */
const int kBusyRetries = 1000;        /* MFX_WRN_DEVICE_BUSY retries, 1 ms apart */
const int kStallLimit = 64;           /* decode calls in a row that neither output nor consume */
const mfxIMPL kImpl = MFX_IMPL_HARDWARE | MFX_IMPL_VIA_VAAPI;
static_assert(sizeof(mfxInitParam) == 80 && offsetof(mfxInitParam, GPUCopy) == 32,
              "mfxInitParam as the unit's MFXInitEx takes it");

/* Whether the process has the CM runtime mapped: MSDK loads it to copy on
 * the GPU, and only then. */
bool cm_runtime_loaded()
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line))
        if (line.find("igfxcmrt") != std::string::npos)
            return true;
    return false;
}

struct Free {
    void operator()(uint8_t *p) const { free(p); }
};

/* GPU copy failed once in this process: no session asks for it again,
 * whatever a decoder is set up with. */
std::atomic<bool> g_gpu_copy_failed{false};

/* How far VPP's output planes are moved off 16-byte alignment, so that the
 * unit's library copies into them on the CPU. */
const size_t kCpuCopyShift = 8;

/* VPP runs in a row that fail in a session with GPU copy before its output
 * is moved for the CPU copy; one that fails alone only loses its picture. */
const int kVppFailLimit = 3;

const char *status_name(mfxStatus s)
{
    switch (s) {
    case MFX_ERR_NONE: return "MFX_ERR_NONE";
    case MFX_ERR_UNKNOWN: return "MFX_ERR_UNKNOWN";
    case MFX_ERR_NULL_PTR: return "MFX_ERR_NULL_PTR";
    case MFX_ERR_UNSUPPORTED: return "MFX_ERR_UNSUPPORTED";
    case MFX_ERR_MEMORY_ALLOC: return "MFX_ERR_MEMORY_ALLOC";
    case MFX_ERR_NOT_ENOUGH_BUFFER: return "MFX_ERR_NOT_ENOUGH_BUFFER";
    case MFX_ERR_INVALID_HANDLE: return "MFX_ERR_INVALID_HANDLE";
    case MFX_ERR_LOCK_MEMORY: return "MFX_ERR_LOCK_MEMORY";
    case MFX_ERR_NOT_INITIALIZED: return "MFX_ERR_NOT_INITIALIZED";
    case MFX_ERR_NOT_FOUND: return "MFX_ERR_NOT_FOUND";
    case MFX_ERR_MORE_DATA: return "MFX_ERR_MORE_DATA";
    case MFX_ERR_MORE_SURFACE: return "MFX_ERR_MORE_SURFACE";
    case MFX_ERR_ABORTED: return "MFX_ERR_ABORTED";
    case MFX_ERR_DEVICE_LOST: return "MFX_ERR_DEVICE_LOST";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_ERR_INVALID_VIDEO_PARAM: return "MFX_ERR_INVALID_VIDEO_PARAM";
    case MFX_ERR_UNDEFINED_BEHAVIOR: return "MFX_ERR_UNDEFINED_BEHAVIOR";
    case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED";
    case MFX_ERR_MORE_BITSTREAM: return "MFX_ERR_MORE_BITSTREAM";
    case MFX_ERR_INCOMPATIBLE_AUDIO_PARAM: return "MFX_ERR_INCOMPATIBLE_AUDIO_PARAM";
    case MFX_ERR_INVALID_AUDIO_PARAM: return "MFX_ERR_INVALID_AUDIO_PARAM";
    case MFX_ERR_GPU_HANG: return "MFX_ERR_GPU_HANG";
    case MFX_ERR_REALLOC_SURFACE: return "MFX_ERR_REALLOC_SURFACE";
    case MFX_WRN_IN_EXECUTION: return "MFX_WRN_IN_EXECUTION";
    case MFX_WRN_DEVICE_BUSY: return "MFX_WRN_DEVICE_BUSY";
    case MFX_WRN_VIDEO_PARAM_CHANGED: return "MFX_WRN_VIDEO_PARAM_CHANGED";
    case MFX_WRN_PARTIAL_ACCELERATION: return "MFX_WRN_PARTIAL_ACCELERATION";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: return "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_WRN_VALUE_NOT_CHANGED: return "MFX_WRN_VALUE_NOT_CHANGED";
    case MFX_WRN_OUT_OF_RANGE: return "MFX_WRN_OUT_OF_RANGE";
    case MFX_WRN_FILTER_SKIPPED: return "MFX_WRN_FILTER_SKIPPED";
    case MFX_WRN_INCOMPATIBLE_AUDIO_PARAM: return "MFX_WRN_INCOMPATIBLE_AUDIO_PARAM";
    }
    return "unknown status";
}

std::string mfx_error(const char *call, mfxStatus s)
{
    return std::string(call) + ": " + status_name(s) + " (" + std::to_string(s) + ")";
}

mfxU16 align16(uint32_t v) { return (mfxU16)((v + 15) & ~15u); }

int64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Whether two NV12 surfaces of the same layout hold the same w x h picture. */
bool same_picture(const mfxFrameSurface1 &a, const mfxFrameSurface1 &b, uint32_t w, uint32_t h)
{
    const size_t pitch = a.Data.Pitch;
    for (uint32_t r = 0; r < h; r++)
        if (memcmp(a.Data.Y + r * pitch, b.Data.Y + r * pitch, w) != 0)
            return false;
    for (uint32_t r = 0; r < h / 2; r++)
        if (memcmp(a.Data.UV + r * pitch, b.Data.UV + r * pitch, w) != 0)
            return false;
    return true;
}

void wrap(mfxBitstream *bs, const AccessUnit &au)
{
    memset(bs, 0, sizeof *bs);
    bs->Data = const_cast<mfxU8 *>(au.data);
    bs->DataLength = bs->MaxLength = (mfxU32)au.size;
    /* The reader only delivers whole access units, so the decoder need not
     * wait for the next start code to know the picture is complete. */
    bs->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;
}

}  // namespace

struct Decoder::State {
    int fd = -1;
    VADisplay va = nullptr;
    mfxSession session = nullptr;
    uint32_t out_w = 0, out_h = 0;   /* requested; 0 = the stream's size */
    SourceCrop crop;
    uint16_t seen_w = 0, seen_h = 0; /* the picture size last logged */
    /* The size of the sequence the last SPS started, as DecodeHeader reads
     * it; 0 x 0 when it could not. */
    uint32_t seq_w = 0, seq_h = 0;
    bool rect_restart = false;       /* a picture needs VPP set up for another rect */
    bool start_by_rect = false;      /* the next start() is a rect restart's */
    RectRestarts rects;

    NalReader nal;
    const SpsSink *sps_sink = nullptr;
    std::string error;               /* first hard error of the current call */
    bool reorder = false;            /* the stream has B slices: output in display order */
    bool display_order = false;      /* the order the last sequence started in */
    /* Access units given to the decoder in this sequence. Each goes in with
     * its number as its TimeStamp, which MSDK carries to the surface it
     * decodes into, as a presentation time stamp. */
    uint32_t given = 0;
    int failed = 0;                  /* access units in a row that failed to decode */
    bool picture = true;             /* this call's frames get an atlas */
    /* The newest frame that came out, when it came out without a picture
     * and its surface still holds it: the decoder writes only into the
     * work surfaces decode() hands it, and has not taken this one since.
     * Null once a later frame comes out, once the decoder may have taken
     * its surface, and after stop(). */
    mfxFrameSurface1 *skipped_out = nullptr;
    unsigned level_bits = 0;
    crk::Spread pack_us, vpp_us;
    bool gpu_copy = false;           /* asked for (g_gpu_copy_failed decides) */
    bool session_gpu_copy = false;   /* the open session was made with it */
    /* VPP's output planes moved off 16-byte alignment (kCpuCopyShift) after
     * kVppFailLimit VPP runs in a row failed with GPU copy. */
    bool cpu_layout = false;
    int vpp_fails = 0;               /* VPP runs in a row that failed */
    bool copy_logged = false;

    /* The current sequence; valid while `running`. */
    bool running = false;
    mfxVideoParam dec, vpp;
    mfxExtOpaqueSurfaceAlloc dec_opaque, vpp_opaque;
    mfxExtBuffer *dec_ext[1], *vpp_ext[1];
    std::vector<mfxFrameSurface1> pool;          /* decoder output = VPP input; MSDK owns the memory */
    std::vector<mfxFrameSurface1 *> pool_ptrs;
    /* VPP output, system memory, by turns: vout[last] holds the last
     * picture that came out, when have_last. Page-aligned with a pitch a
     * multiple of 64, as MSDK's GPU copy takes them. */
    mfxFrameSurface1 vout[2];
    std::unique_ptr<uint8_t, Free> vout_mem[2];
    int last = 0;
    bool have_last = false;
    std::vector<uint8_t> pixels;                 /* the atlas handed out */
    uint32_t w = 0, h = 0;

    enum Result { Ok, Failed, Restart };

    ~State();
    bool open_session();
    bool session_up(bool gpu, std::string *why);
    bool setup_failed(const AccessUnit &au, bool by_rect, const std::string &why);
    void place_vout();
    void access_unit(const AccessUnit &au, const FrameSink &cb);
    bool start(const AccessUnit &au);
    mfxStatus read_header(const AccessUnit &au, mfxVideoParam *par);
    void stop();
    Result decode(mfxBitstream *bs, const FrameSink &cb);
    bool convert(mfxFrameSurface1 *in, mfxSyncPoint decoded, const FrameSink &cb);
    bool run_vpp(mfxFrameSurface1 *in, const FrameSink &cb);
    bool skip(mfxFrameSurface1 *out, mfxSyncPoint sync, const FrameSink &cb);
    bool fail(const std::string &msg)
    {
        if (error.empty()) error = msg;
        return false;
    }
    /* The access units given after the one a decoded surface came from:
     * how far behind its input the decoder output it. -1 when the surface
     * carries no number of this sequence. A unit taken without output (a
     * damaged one) counts only for frames whose own unit came before it. */
    int32_t held(const mfxFrameSurface1 *s) const
    {
        const mfxU64 n = s->Data.TimeStamp;
        return n >= 1 && n <= given ? (int32_t)(given - n) : -1;
    }
    void cpu_copy(const std::string &why)
    {
        log_warn("decoder: %s with GPU copy; VPP's output is copied on the CPU from now on", why.c_str());
        g_gpu_copy_failed = true;
    }
    /* A failed VPP run loses its picture. In a session with GPU copy, the
     * kVppFailLimit-th in a row moves the output for the CPU copy: that
     * session goes on with its decoder's references. */
    bool vpp_failed(const std::string &msg)
    {
        if (session_gpu_copy && !cpu_layout && ++vpp_fails >= kVppFailLimit) {
            cpu_copy(std::to_string(kVppFailLimit) + " VPP runs in a row failed (the last: " + msg + ")");
            cpu_layout = true;
            place_vout();
            have_last = false;
        }
        return fail(msg);
    }
};

Decoder::State::~State()
{
    /* The session uses the VA display, so it closes first. */
    stop();
    if (va) vaTerminate(va);
    if (fd >= 0) close(fd);
}

bool Decoder::State::open_session()
{
    std::string why;
    if (gpu_copy && !g_gpu_copy_failed) {
        if (session_up(true, &why))
            return true;
        cpu_copy(why);
    }
    return session_up(false, &why) || fail(why);
}

/* A session, with GPU copy or not, on our VA display. */
bool Decoder::State::session_up(bool gpu, std::string *why)
{
    mfxVersion ver;
    ver.Major = MFX_VERSION_MAJOR;
    ver.Minor = MFX_VERSION_MINOR;
    mfxStatus sts;
    if (gpu) {
        /* On VA-API the unit's library copies video memory to system memory
         * on the CPU unless GPUCopy is on. */
        mfxInitParam par;
        memset(&par, 0, sizeof par);
        par.Implementation = kImpl;
        par.Version = ver;
        par.GPUCopy = MFX_GPUCOPY_ON;
        sts = MFXInitEx(par, &session);
    } else {
        sts = MFXInit(kImpl, &ver, &session);
    }
    if (sts < MFX_ERR_NONE) {
        session = nullptr;
        *why = mfx_error(gpu ? "MFXInitEx" : "MFXInit", sts);
        return false;
    }
    /* On Linux the hardware library takes its device from the application
     * as a VA display (the unit's libNmeVideoMSDK imports vaGetDisplayDRM,
     * vaInitialize and MFXVideoCORE_SetHandle for the same purpose). */
    sts = MFXVideoCORE_SetHandle(session, MFX_HANDLE_VA_DISPLAY, va);
    if (sts < MFX_ERR_NONE) {
        MFXClose(session);
        session = nullptr;
        *why = mfx_error("MFXVideoCORE_SetHandle", sts);
        return false;
    }
    session_gpu_copy = gpu;
    return true;
}

/* A step of start() after the header failed. In a session with GPU copy the
 * sequence starts again, as it began, in a session without: the unit's
 * library fails Init when the GPU copy it sets up there cannot start. */
bool Decoder::State::setup_failed(const AccessUnit &au, bool by_rect, const std::string &why)
{
    if (!session_gpu_copy)
        return fail(why);
    cpu_copy(why);
    MFXClose(session);
    session = nullptr;
    start_by_rect = by_rect;
    return start(au);
}

/* The SPS NAL units of an access unit, each behind a 00 00 00 01 start
 * code, to the SPS sink. */
static void each_sps(const AccessUnit &au, const Decoder::SpsSink &sink)
{
    const uint8_t *end = au.data + au.size;
    const uint8_t *p = au.data;
    while (end - p > 4) {
        const uint8_t *nal = p + 4;
        const uint8_t *next = nal;
        while (end - next >= 3 && !(next[0] == 0 && next[1] == 0 && next[2] == 1))
            next++;
        if (end - next < 3)
            next = end;
        else if (next > nal && next[-1] == 0)
            next--;   /* the start code's leading zero */
        if ((nal[0] & 0x1f) == 7)
            sink(nal, (size_t)(next - nal));
        p = next;
    }
}

void Decoder::State::access_unit(const AccessUnit &au, const FrameSink &cb)
{
    if (au.sps && sps_sink && *sps_sink)
        each_sps(au, *sps_sink);
    /* Once a B slice shows that pictures are reordered, the decoder restarts
     * in display order at the next SPS. Until then B frames come out in
     * decoding order. */
    if (au.b && !reorder)
        log_info("decoder: B slices in the stream: output in display order from the next SPS");
    reorder = reorder || au.b;
    if (running && au.sps && reorder && dec.mfx.DecodedOrder) {
        decode(nullptr, cb);
        stop();
    }
    /* A compatible SPS keeps the sequence going at its own size. */
    if (running && au.sps && crop.enabled()) {
        mfxVideoParam par;
        const bool read = read_header(au, &par) >= MFX_ERR_NONE;
        seq_w = read ? par.mfx.FrameInfo.CropW : 0;
        seq_h = read ? par.mfx.FrameInfo.CropH : 0;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!running) {
            if (!au.sps)
                return;   /* nothing decodes before a sequence header */
            if (!start(au)) {
                stop();
                return;
            }
            if (!running)
                return;
        }
        mfxBitstream bs;
        wrap(&bs, au);
        bs.TimeStamp = ++given;
        Result r = decode(&bs, cb);
        if (r == Ok) {
            failed = 0;
            return;
        }
        if (r == Failed) {
            if (++failed >= kFailLimit) {
                failed = 0;
                stop();
                error += "; " + std::to_string(kFailLimit) + " access units in a row failed: waiting for an SPS";
            }
            return;
        }
        /* The access unit starts a sequence that needs a larger picture or
         * more surfaces, or, with src_crop, another VPP input rect. Deliver
         * what the old sequence still holds, then reinitialise from this
         * access unit. */
        start_by_rect = rect_restart;
        decode(nullptr, cb);
        stop();
    }
    fail("decoder rejected the new sequence after reinitialising");
}

/* Every sequence gets a fresh session. Reinitialising decode and VPP with a
 * new opaque pool in the same session makes MSDK hand out output surfaces
 * from the closed pool (seen with the unit's libmfxsw64, the same MSDK
 * release as its libmfxhw64). */
bool Decoder::State::start(const AccessUnit &au)
{
    const bool by_rect = start_by_rect;
    if (!session && !open_session())
        return false;

    mfxStatus sts = read_header(au, &dec);
    if (sts == MFX_ERR_MORE_DATA)
        return true;   /* SPS without its PPS: wait for the next SPS */
    if (sts < MFX_ERR_NONE)
        return fail(mfx_error("MFXVideoDECODE_DecodeHeader", sts));

    /* One frame in flight: lowest latency. */
    dec.AsyncDepth = 1;
    dec.IOPattern = MFX_IOPATTERN_OUT_OPAQUE_MEMORY;
    /* Decoding order outputs each frame as soon as it is decoded. In display
     * order the unit's libmfxsw64 held frames until its DPB was full for an
     * SPS without VUI, as Apple's real-time encoder writes it: 11 frames for
     * a 320x160 level 1.2 stream. */
    dec.mfx.DecodedOrder = reorder ? 0 : 1;
    if (reorder != display_order) {
        display_order = reorder;
        log_info(display_order ? "decoder: output in display order from this sequence"
                               : "decoder: output in decoding order from this sequence");
    }
    given = 0;

    const mfxFrameInfo &in = dec.mfx.FrameInfo;
    seq_w = in.CropW;
    seq_h = in.CropH;
    memset(&vpp, 0, sizeof vpp);
    vpp.AsyncDepth = 1;
    vpp.IOPattern = MFX_IOPATTERN_IN_OPAQUE_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    vpp.vpp.In = in;
    const bool cropped = crop_picture(crop, &vpp.vpp.In.CropX, &vpp.vpp.In.CropY, &vpp.vpp.In.CropW, &vpp.vpp.In.CropH);
    w = out_w ? out_w : cropped ? crop.w : in.CropW;
    h = out_h ? out_h : cropped ? crop.h : in.CropH;
    seen_w = seen_h = 0;
    rect_restart = false;
    rects.started(start_by_rect);
    start_by_rect = false;
    if (crop.enabled())
        log_info("decoder: sequence %ux%u, to %ux%u", in.CropW, in.CropH, w, h);
    mfxFrameInfo &out = vpp.vpp.Out;
    out = in;
    out.FourCC = MFX_FOURCC_NV12;
    out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    out.Width = align16(w);
    out.Height = align16(h);
    out.CropX = out.CropY = 0;
    out.CropW = (mfxU16)w;
    out.CropH = (mfxU16)h;
    /* VPP needs a frame rate; the same one in and out means no frame rate
     * conversion. A stream without VUI timing gives none. */
    if (!in.FrameRateExtN || !in.FrameRateExtD) {
        vpp.vpp.In.FrameRateExtN = out.FrameRateExtN = 30;
        vpp.vpp.In.FrameRateExtD = out.FrameRateExtD = 1;
    }

    /* Decoder output surfaces are VPP input surfaces, so one pool serves both
     * and has to cover both components' suggested counts. */
    mfxFrameAllocRequest dec_req, vpp_req[2];
    memset(&dec_req, 0, sizeof dec_req);
    memset(vpp_req, 0, sizeof vpp_req);
    sts = MFXVideoDECODE_QueryIOSurf(session, &dec, &dec_req);
    if (sts < MFX_ERR_NONE)
        return setup_failed(au, by_rect, mfx_error("MFXVideoDECODE_QueryIOSurf", sts));
    sts = MFXVideoVPP_QueryIOSurf(session, &vpp, vpp_req);
    if (sts < MFX_ERR_NONE)
        return setup_failed(au, by_rect, mfx_error("MFXVideoVPP_QueryIOSurf", sts));
    mfxU16 n = (mfxU16)(dec_req.NumFrameSuggested + vpp_req[0].NumFrameSuggested);

    /* Opaque surfaces: the application passes surface headers only, and MSDK
     * allocates the memory behind them when the decoder is initialised. VPP
     * is given the same headers as its input. */
    pool.assign(n, mfxFrameSurface1());
    pool_ptrs.resize(n);
    for (mfxU16 i = 0; i < n; i++) {
        pool[i].Info = in;
        pool_ptrs[i] = &pool[i];
    }
    memset(&dec_opaque, 0, sizeof dec_opaque);
    dec_opaque.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
    dec_opaque.Header.BufferSz = sizeof dec_opaque;
    dec_opaque.Out.Surfaces = pool_ptrs.data();
    dec_opaque.Out.NumSurface = n;
    dec_opaque.Out.Type = dec_req.Type;
    memset(&vpp_opaque, 0, sizeof vpp_opaque);
    vpp_opaque.Header = dec_opaque.Header;
    vpp_opaque.In = dec_opaque.Out;
    dec_ext[0] = &dec_opaque.Header;
    vpp_ext[0] = &vpp_opaque.Header;
    dec.ExtParam = dec_ext;
    dec.NumExtParam = 1;
    vpp.ExtParam = vpp_ext;
    vpp.NumExtParam = 1;

    const char *call = "MFXVideoDECODE_Init";
    sts = MFXVideoDECODE_Init(session, &dec);
    if (sts >= MFX_ERR_NONE) {
        call = "MFXVideoVPP_Init";
        sts = MFXVideoVPP_Init(session, &vpp);
        if (sts < MFX_ERR_NONE)
            MFXVideoDECODE_Close(session);
    }
    if (sts < MFX_ERR_NONE)
        return setup_failed(au, by_rect, mfx_error(call, sts));

    /* Each VPP run is synchronised before the next; the second surface
     * keeps the picture before for the comparison. NV12 is the Y plane, then
     * U,V interleaved at half size. */
    const mfxU32 pitch = (out.Width + 63u) & ~63u;
    const size_t bytes = (size_t)pitch * out.Height * 3 / 2 + kCpuCopyShift;
    for (int k = 0; k < 2; k++) {
        void *mem = nullptr;
        if (posix_memalign(&mem, 4096, bytes) != 0) {
            MFXVideoVPP_Close(session);
            MFXVideoDECODE_Close(session);
            return fail("no memory for VPP's output");
        }
        memset(mem, 0, bytes);
        vout_mem[k].reset(static_cast<uint8_t *>(mem));
        memset(&vout[k], 0, sizeof vout[k]);
        vout[k].Info = out;
        vout[k].Data.Pitch = (mfxU16)pitch;
    }
    cpu_layout = false;
    vpp_fails = 0;
    place_vout();
    have_last = false;
    pixels.assign((size_t)w * atlas_height(h), 0);
    running = true;
    return true;
}

void Decoder::State::place_vout()
{
    for (int k = 0; k < 2; k++) {
        vout[k].Data.Y = vout_mem[k].get() + (cpu_layout ? kCpuCopyShift : 0);
        vout[k].Data.UV = vout[k].Data.Y + (size_t)vout[k].Data.Pitch * vout[k].Info.Height;
    }
}

/* DecodeHeader parses the SPS and PPS into par->mfx: picture and crop size,
 * frame rate, profile. It moves only this local bitstream's offset, and
 * leaves a running decoder as it is. */
mfxStatus Decoder::State::read_header(const AccessUnit &au, mfxVideoParam *par)
{
    mfxBitstream bs;
    wrap(&bs, au);
    memset(par, 0, sizeof *par);
    par->mfx.CodecId = MFX_CODEC_AVC;
    return MFXVideoDECODE_DecodeHeader(session, &bs, par);
}

void Decoder::State::stop()
{
    if (running) {
        MFXVideoDECODE_Close(session);
        MFXVideoVPP_Close(session);
        running = false;
    }
    if (session) {
        MFXClose(session);
        session = nullptr;
    }
    skipped_out = nullptr;
    pool.clear();
    pool_ptrs.clear();
}

/* Decodes everything in bs, or with bs null drains the frames the decoder
 * holds, until MSDK asks for more data. */
Decoder::State::Result Decoder::State::decode(mfxBitstream *bs, const FrameSink &cb)
{
    int busy = 0, stalls = 0;
    for (;;) {
        mfxFrameSurface1 *work = nullptr;
        for (auto &s : pool) {
            if (!s.Data.Locked) {
                work = &s;
                break;
            }
        }
        if (!work) {
            fail("no free decode surface");
            return Failed;
        }
        mfxFrameSurface1 *out = nullptr;
        mfxSyncPoint sync = nullptr;
        const bool to_decode = !bs || bs->DataLength > 0;
        mfxStatus sts = MFXVideoDECODE_DecodeFrameAsync(session, bs, work, &out, &sync);
        /* The decoder writes into a work surface only when it takes it: with
         * bytes to decode, or keeping it (locked) or answering other than
         * MORE_DATA. A surface it had nothing to write into still holds its
         * picture. */
        if (work == skipped_out && (to_decode || sts != MFX_ERR_MORE_DATA || work->Data.Locked))
            skipped_out = nullptr;
        if (sts == MFX_WRN_DEVICE_BUSY) {
            if (++busy > kBusyRetries) {
                fail(mfx_error("MFXVideoDECODE_DecodeFrameAsync", sts));
                return Failed;
            }
            usleep(1000);
            continue;
        }
        busy = 0;
        if (sts == MFX_ERR_MORE_DATA)
            return Ok;
        if (sts == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM && bs)
            return Restart;
        if (sts < MFX_ERR_NONE && sts != MFX_ERR_MORE_SURFACE) {
            fail(mfx_error("MFXVideoDECODE_DecodeFrameAsync", sts));
            return Failed;
        }
        /* MFX_ERR_MORE_SURFACE: the decoder kept `work` as a reference and
         * wants another. MFX_ERR_NONE or a warning such as
         * MFX_WRN_VIDEO_PARAM_CHANGED (a repeated or compatible SPS) may come
         * with a frame; with no frame the call is repeated. */
        if (sync) {
            stalls = 0;
            skipped_out = nullptr;   /* skip() sets it again for this frame */
            if (!(picture ? convert(out, sync, cb) : skip(out, sync, cb)))
                return Failed;
            if (rect_restart && bs)
                return Restart;
        } else if (++stalls > kStallLimit) {
            fail(mfx_error("MFXVideoDECODE_DecodeFrameAsync makes no progress", sts));
            return Failed;
        }
    }
}

/* Runs VPP on a decoded surface and hands the packed atlas to cb. The VPP
 * task depends on the decode task in the same session, so waiting for VPP
 * waits for both. */
bool Decoder::State::convert(mfxFrameSurface1 *in, mfxSyncPoint decoded, const FrameSink &cb)
{
    /* A sequence src_crop cannot touch, in the rect VPP is set up for, goes
     * straight to VPP: its pictures fit that rect (which also ends a rect
     * restart's exemption, as the check below would). */
    if (!sequence_needs_check(crop, vpp.vpp.In.CropW, vpp.vpp.In.CropH, seq_w, seq_h)) {
        rects.restart(false);
        return run_vpp(in, cb);
    }
    /* VPP reads the input crop from the surface, which the decoder sets to
     * its picture's own. The decode task is waited for first, so the
     * surface's info is final and nothing of the decoder writes over
     * src_crop's rect, which replaces it for a picture of its size; the
     * decoder's crop goes back on every way out. */
    if (decoded) {   /* null: waited for already (a late picture, picture_skipped) */
        const mfxStatus sts = MFXVideoCORE_SyncOperation(session, decoded, kSyncTimeoutMs);
        if (sts != MFX_ERR_NONE)
            return fail(mfx_error("MFXVideoCORE_SyncOperation", sts));
    }
    struct Restore {
        mfxFrameSurface1 *s;
        mfxFrameInfo own;
        ~Restore()
        {
            s->Info.CropX = own.CropX;
            s->Info.CropY = own.CropY;
            s->Info.CropW = own.CropW;
            s->Info.CropH = own.CropH;
        }
    } restore{in, in->Info};
    const mfxFrameInfo &own = restore.own;
    const bool cropped = crop_picture(crop, &in->Info.CropX, &in->Info.CropY, &in->Info.CropW, &in->Info.CropH);
    if (own.CropW != seen_w || own.CropH != seen_h) {
        seen_w = own.CropW;
        seen_h = own.CropH;
        if (cropped)
            log_info("decoder: picture %ux%u, map from its %ux%u at %u,%u, to %ux%u", own.CropW, own.CropH, crop.w, crop.h,
                     crop.x, crop.y, w, h);
        else
            log_info("decoder: picture %ux%u, to %ux%u", own.CropW, own.CropH, w, h);
    }
    /* VPP was set up for one input rect. A picture of another (a compatible
     * sequence after a cropped one, or the reverse) is not converted: the
     * decoder restarts at this access unit, the path an incompatible
     * sequence takes, with a fresh session set up for its rect; never twice
     * in a row (RectRestarts). */
    const bool other = rect_needs_reset(crop, vpp.vpp.In.CropW, vpp.vpp.In.CropH, own.CropW, own.CropH);
    if (rects.restart(other)) {
        if (!rect_restart)
            log_info("decoder: picture %ux%u needs another VPP input rect: restarting at this sequence", own.CropW, own.CropH);
        rect_restart = true;
        return true;
    }
    if (other) {
        if (rects.warn())
            log_warn("decoder: picture %ux%u still differs from its sequence's VPP input rect after a restart: "
                     "converted as VPP is set up", own.CropW, own.CropH);
    }
    return run_vpp(in, cb);
}

bool Decoder::State::run_vpp(mfxFrameSurface1 *in, const FrameSink &cb)
{
    mfxFrameSurface1 &out = vout[1 - last];
    mfxSyncPoint sync = nullptr;
    mfxStatus sts;
    int busy = 0;
    while ((sts = MFXVideoVPP_RunFrameVPPAsync(session, in, &out, nullptr, &sync)) == MFX_WRN_DEVICE_BUSY) {
        if (++busy > kBusyRetries)
            return fail(mfx_error("MFXVideoVPP_RunFrameVPPAsync", sts));
        usleep(1000);
    }
    if (sts == MFX_ERR_MORE_DATA)
        return true;   /* VPP holds the input for a later output (deinterlacing) */
    if (sts < MFX_ERR_NONE)
        return vpp_failed(mfx_error("MFXVideoVPP_RunFrameVPPAsync", sts));
    if (!sync)
        return true;
    const int64_t synced = now_us();
    sts = MFXVideoCORE_SyncOperation(session, sync, kSyncTimeoutMs);
    vpp_us.add(now_us() - synced);
    if (sts != MFX_ERR_NONE)
        return vpp_failed(mfx_error("MFXVideoCORE_SyncOperation", sts));
    vpp_fails = 0;
    if (!copy_logged) {
        copy_logged = true;
        log_info(!session_gpu_copy || cpu_layout ? "decoder: VPP output copied on the CPU"
                 : cm_runtime_loaded()           ? "decoder: GPU copy asked for; MSDK loaded its CM runtime"
                                                 : "decoder: GPU copy asked for, but MSDK did not load its CM "
                                                   "runtime: VPP output copied on the CPU");
    }

    const int64_t t0 = now_us();
    const bool same = have_last && same_picture(out, vout[last], w, h);
    if (!same) {
        const size_t n = (size_t)w * atlas_height(h);
        if (pixels.size() != n)
            pixels.assign(n, 0);
        pack_nv12_atlas(out.Data.Y, out.Data.Pitch, out.Data.UV, out.Data.Pitch, w, h, pixels.data(), level_bits);
    }
    pack_us.add(now_us() - t0);
    last = 1 - last;
    have_last = true;
    /* The decode task finished before the VPP task that read its surface,
     * so its flags are set. */
    VideoFrame f = {w, h, in->Data.Corrupted, same, same ? nullptr : &pixels, held(in)};
    cb(f);
    return true;
}

/* A frame without a picture: no VPP. The wait costs nothing the next frame
 * would not wait for anyway (it refers to this one, and AsyncDepth 1 holds
 * its decode until this one's is done), and it sets the Corrupted flags.
 * Nothing holds the surface for VPP, so the decoder frees it once it is no
 * longer a reference. */
bool Decoder::State::skip(mfxFrameSurface1 *out, mfxSyncPoint sync, const FrameSink &cb)
{
    const mfxStatus sts = MFXVideoCORE_SyncOperation(session, sync, kSyncTimeoutMs);
    if (sts != MFX_ERR_NONE)
        return fail(mfx_error("MFXVideoCORE_SyncOperation", sts));
    VideoFrame f = {w, h, out->Data.Corrupted, false, nullptr, held(out)};
    skipped_out = out;
    cb(f);
    return true;
}

Decoder::Decoder() {}

Decoder::~Decoder() { shutdown(); }

bool Decoder::init(const char *dev, uint32_t out_w, uint32_t out_h, unsigned level_bits, bool gpu_copy,
                   std::string *err, const SourceCrop &crop)
{
    shutdown();
    if ((out_w == 0) != (out_h == 0)) {
        *err = "out_w and out_h must both be 0 or both be set";
        return false;
    }
    if (out_w % 2 || out_h % 2) {
        *err = "the 4:2:0 atlas needs an even out_w and out_h";
        return false;
    }
    std::unique_ptr<State> s(new State);
    s->out_w = out_w;
    s->out_h = out_h;
    s->level_bits = level_bits;
    s->gpu_copy = gpu_copy;
    s->crop = crop;
    s->sps_sink = &sps_sink_;

    s->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (s->fd < 0) {
        *err = std::string("open ") + dev + ": " + strerror(errno);
        return false;
    }
    s->va = vaGetDisplayDRM(s->fd);
    if (!s->va) {
        *err = "vaGetDisplayDRM failed";
        return false;
    }
    int major = 0, minor = 0;
    VAStatus vs = vaInitialize(s->va, &major, &minor);
    if (vs != VA_STATUS_SUCCESS) {
        *err = std::string("vaInitialize: ") + vaErrorStr(vs);
        return false;
    }

    /* Opened now to report a missing or refusing MSDK early; the first
     * sequence uses it. */
    if (!s->open_session()) {
        *err = s->error;
        return false;
    }
    s_ = std::move(s);
    return true;
}

bool Decoder::feed(const uint8_t *data, size_t len, const FrameSink &cb, std::string *err, bool end_of_au,
                   bool picture)
{
    if (!s_) {
        *err = "decoder not initialised";
        return false;
    }
    State &s = *s_;
    s.error.clear();
    s.picture = picture;
    s.nal.push(data, len, end_of_au, [&](const AccessUnit &au) { s.access_unit(au, cb); });
    if (!s.error.empty()) {
        *err = s.error;
        return false;
    }
    return true;
}

void Decoder::flush(const FrameSink &cb)
{
    if (!s_)
        return;
    State &s = *s_;
    s.picture = true;
    s.nal.flush([&](const AccessUnit &au) { s.access_unit(au, cb); });
    if (s.running)
        s.decode(nullptr, cb);
    s.stop();
    s.error.clear();
}

void Decoder::forget_last()
{
    if (s_)
        s_->have_last = false;
}

bool Decoder::picture_skipped(const FrameSink &cb, std::string *err)
{
    if (!s_ || !s_->running || !s_->skipped_out)
        return false;
    State &s = *s_;
    /* A rect restart starts at an access unit: a picture that would ask for
     * one gets none here, and the next frame's picture asks for it. */
    const mfxFrameInfo &own = s.skipped_out->Info;
    if (sequence_needs_check(s.crop, s.vpp.vpp.In.CropW, s.vpp.vpp.In.CropH, s.seq_w, s.seq_h) &&
        s.rects.would_restart(rect_needs_reset(s.crop, s.vpp.vpp.In.CropW, s.vpp.vpp.In.CropH, own.CropW, own.CropH)))
        return false;
    mfxFrameSurface1 *in = s.skipped_out;
    s.skipped_out = nullptr;
    s.error.clear();
    const bool ok = s.convert(in, nullptr, cb);
    if (!ok || !s.error.empty()) {
        *err = s.error;
        return false;
    }
    return true;
}

void Decoder::shutdown() { s_.reset(); }

bool Decoder::decoding() const { return s_ && s_->running; }

crk::Spread Decoder::pack_us() const { return s_ ? s_->pack_us : crk::Spread(); }

crk::Spread Decoder::vpp_us() const { return s_ ? s_->vpp_us : crk::Spread(); }

bool Decoder::display_order() const { return s_ && s_->display_order; }

}  // namespace woc
