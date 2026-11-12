// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "fake_msdk.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <deque>

#include "mfx/mfxvideo.h"

namespace fake_msdk {

std::vector<std::string> calls;
GpuFault gpu_fault;
std::string picture_size;
int delay = 0;
uint8_t swallow = 0;
bool timestamps = true;
int refs = 0;

namespace {

struct Unit {
    bool sps = false, pps = false, slice = false;
    unsigned w = 0, h = 0;
    uint8_t content = 0;
};

/* The NAL units behind the decoder's 00 00 00 01 start codes. */
Unit parse(const uint8_t *d, size_t n)
{
    Unit u;
    for (size_t i = 0; i + 4 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] || d[i + 3] != 1)
            continue;
        const uint8_t *nal = d + i + 4;
        size_t end = i + 4;
        while (end + 3 < n && !(d[end] == 0 && d[end + 1] == 0 && d[end + 2] == 0 && d[end + 3] == 1))
            end++;
        if (end + 3 >= n)
            end = n;
        const std::string body((const char *)nal + 1, end - (i + 5));
        switch (nal[0] & 0x1f) {
        case 7:
            u.sps = sscanf(body.c_str(), "%ux%u", &u.w, &u.h) == 2;
            break;
        case 8:
            u.pps = true;
            break;
        case 1:
        case 5:
            u.slice = body.size() >= 2;
            u.content = u.slice ? (uint8_t)body[1] : 0;
            break;
        }
        i = end - 1;
    }
    return u;
}

struct Session {
    unsigned surf_w = 0, surf_h = 0;   /* the decoder's surfaces */
    unsigned pic_w = 0, pic_h = 0;     /* the running sequence's pictures */
    int decode_task = 0, vpp_task = 0;
    bool gpu_copy = false;   /* the session was made with GPUCopy on */
    std::deque<mfxFrameSurface1 *> held;   /* decoded, not yet out (delay) */
    std::deque<mfxFrameSurface1 *> refs;   /* the newest decoded pictures (refs) */
} g;

mfxSyncPoint task(int *t) { return reinterpret_cast<mfxSyncPoint>(t); }

std::string rect(const mfxFrameInfo &f)
{
    char b[64];
    snprintf(b, sizeof b, "%u,%u,%u,%u", f.CropX, f.CropY, f.CropW, f.CropH);
    return b;
}

std::string size(unsigned w, unsigned h) { return std::to_string(w) + "x" + std::to_string(h); }

/* gpu_fault's status for call in a session with GPU copy, once due. */
mfxStatus fault(const char *call, bool gpu)
{
    if (!gpu || gpu_fault.call != call)
        return MFX_ERR_NONE;
    if (gpu_fault.after > 0) {
        gpu_fault.after--;
        return MFX_ERR_NONE;
    }
    if (gpu_fault.times > 0 && --gpu_fault.times == 0)
        gpu_fault.call.clear();
    return (mfxStatus)gpu_fault.status;
}

}  // namespace

std::string access_unit(const char *sps, bool idr, uint8_t content)
{
    std::string s;
    if (sps)
        s += std::string("\0\0\0\1\x67", 5) + sps + std::string("\0\0\0\1\x68\x50", 6);
    s += std::string("\0\0\0\1", 4) + (idr ? "\x65\x88" : "\x41\x98") + (char)content + '\x80';
    return s;
}

int count(const std::string &s) { return (int)std::count(calls.begin(), calls.end(), s); }

}  // namespace fake_msdk

using namespace fake_msdk;

extern "C" {

void *vaGetDisplayDRM(int) { return &g; }
int vaInitialize(void *, int *major, int *minor)
{
    *major = 1;
    *minor = 0;
    return 0;
}
int vaTerminate(void *) { return 0; }
const char *vaErrorStr(int) { return "fake"; }

mfxStatus MFXInit(mfxIMPL, mfxVersion *, mfxSession *session)
{
    calls.push_back("session");
    g.gpu_copy = false;
    *session = reinterpret_cast<mfxSession>(&g);
    return MFX_ERR_NONE;
}

mfxStatus MFXInitEx(mfxInitParam par, mfxSession *session)
{
    calls.push_back("session gpucopy " + std::to_string(par.GPUCopy));
    if (const mfxStatus sts = fault("MFXInitEx", true))
        return sts;
    g.gpu_copy = par.GPUCopy == MFX_GPUCOPY_ON;
    *session = reinterpret_cast<mfxSession>(&g);
    return MFX_ERR_NONE;
}
mfxStatus MFXClose(mfxSession) { return MFX_ERR_NONE; }
mfxStatus MFXVideoCORE_SetHandle(mfxSession, mfxHandleType, mfxHDL) { return fault("SetHandle", g.gpu_copy); }

mfxStatus MFXVideoCORE_SyncOperation(mfxSession, mfxSyncPoint s, mfxU32)
{
    calls.push_back(s == task(&g.decode_task) ? "decode-sync" : "vpp-sync");
    return MFX_ERR_NONE;
}

mfxStatus MFXVideoDECODE_DecodeHeader(mfxSession, mfxBitstream *bs, mfxVideoParam *par)
{
    const Unit u = parse(bs->Data + bs->DataOffset, bs->DataLength);
    if (!u.sps || !u.pps)
        return MFX_ERR_MORE_DATA;
    calls.push_back("header " + size(u.w, u.h));
    mfxFrameInfo &f = par->mfx.FrameInfo;
    f.FourCC = MFX_FOURCC_NV12;
    f.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    f.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    f.Width = (mfxU16)((u.w + 15) & ~15u);
    f.Height = (mfxU16)((u.h + 15) & ~15u);
    f.CropW = (mfxU16)u.w;
    f.CropH = (mfxU16)u.h;
    f.FrameRateExtN = 30;
    f.FrameRateExtD = 1;
    return MFX_ERR_NONE;
}

mfxStatus MFXVideoDECODE_QueryIOSurf(mfxSession, mfxVideoParam *, mfxFrameAllocRequest *r)
{
    r->NumFrameSuggested = 4;
    return fault("DECODE_QueryIOSurf", g.gpu_copy);
}

mfxStatus MFXVideoDECODE_Init(mfxSession, mfxVideoParam *par)
{
    const mfxFrameInfo &f = par->mfx.FrameInfo;
    g.surf_w = f.Width;
    g.surf_h = f.Height;
    g.pic_w = f.CropW;
    g.pic_h = f.CropH;
    g.held.clear();
    g.refs.clear();
    calls.push_back("init " + size(f.CropW, f.CropH));
    return MFX_ERR_NONE;
}

mfxStatus MFXVideoDECODE_Close(mfxSession) { return MFX_ERR_NONE; }

mfxStatus MFXVideoDECODE_DecodeFrameAsync(mfxSession, mfxBitstream *bs, mfxFrameSurface1 *work,
                                          mfxFrameSurface1 **out, mfxSyncPoint *sync)
{
    if (!bs || !bs->DataLength)
        return MFX_ERR_MORE_DATA;
    const Unit u = parse(bs->Data + bs->DataOffset, bs->DataLength);
    mfxStatus sts = MFX_ERR_NONE;
    if (u.sps) {
        if (u.w > g.surf_w || u.h > g.surf_h)
            return MFX_ERR_INCOMPATIBLE_VIDEO_PARAM;
        if (u.w != g.pic_w || u.h != g.pic_h)
            sts = MFX_WRN_VIDEO_PARAM_CHANGED;
        g.pic_w = u.w;
        g.pic_h = u.h;
    }
    bs->DataOffset += bs->DataLength;
    bs->DataLength = 0;
    if (!u.slice)
        return MFX_ERR_MORE_DATA;
    work->Info.Width = (mfxU16)g.surf_w;
    work->Info.Height = (mfxU16)g.surf_h;
    work->Info.CropX = work->Info.CropY = 0;
    unsigned pw = g.pic_w, ph = g.pic_h;
    if (!picture_size.empty())
        sscanf(picture_size.c_str(), "%ux%u", &pw, &ph);
    work->Info.CropW = (mfxU16)pw;
    work->Info.CropH = (mfxU16)ph;
    work->Data.Corrupted = 0;
    work->Data.FrameOrder = u.content;
    work->Data.TimeStamp = timestamps ? bs->TimeStamp : 0;
    if (refs > 0) {
        work->Data.Locked++;   /* a reference: not a work surface */
        g.refs.push_back(work);
        if ((int)g.refs.size() > refs) {
            g.refs.front()->Data.Locked--;
            g.refs.pop_front();
        }
    }
    if (swallow && u.content == swallow)
        return MFX_ERR_MORE_DATA;
    if (delay > 0) {
        work->Data.Locked++;   /* held: not a work surface */
        g.held.push_back(work);
        if ((int)g.held.size() <= delay)
            return MFX_ERR_MORE_DATA;
        work = g.held.front();
        g.held.pop_front();
        work->Data.Locked--;
    }
    *out = work;
    *sync = task(&g.decode_task);
    return sts;
}

mfxStatus MFXVideoVPP_QueryIOSurf(mfxSession, mfxVideoParam *, mfxFrameAllocRequest r[2])
{
    r[0].NumFrameSuggested = 2;
    return MFX_ERR_NONE;
}

mfxStatus MFXVideoVPP_Init(mfxSession, mfxVideoParam *par)
{
    calls.push_back("vpp-init " + rect(par->vpp.In));
    return fault("VPP_Init", g.gpu_copy);
}

mfxStatus MFXVideoVPP_Close(mfxSession) { return MFX_ERR_NONE; }

mfxStatus MFXVideoVPP_RunFrameVPPAsync(mfxSession, mfxFrameSurface1 *in, mfxFrameSurface1 *out, mfxExtVppAuxData *,
                                       mfxSyncPoint *sync)
{
    calls.push_back("vpp " + rect(in->Info));
    const bool aligned = !(((uintptr_t)out->Data.Y | (uintptr_t)out->Data.UV | out->Data.Pitch) & 15);
    if (aligned)
        if (const mfxStatus sts = fault("VPP_Run", g.gpu_copy))
            return sts;
    if (!aligned)
        calls.push_back("vpp-out shifted");
    else if ((uintptr_t)out->Data.Y % 4096 || out->Data.Pitch % 64)
        calls.push_back("vpp-out unaligned");
    const mfxFrameInfo &o = out->Info;
    for (unsigned r = 0; r < o.CropH; r++)
        memset(out->Data.Y + (size_t)r * out->Data.Pitch, (int)in->Data.FrameOrder, o.CropW);
    for (unsigned r = 0; r < o.CropH / 2u; r++)
        memset(out->Data.UV + (size_t)r * out->Data.Pitch, (int)in->Data.FrameOrder, o.CropW);
    *sync = task(&g.vpp_task);
    return MFX_ERR_NONE;
}

}  // extern "C"
