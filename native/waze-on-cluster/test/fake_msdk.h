// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* A stand-in for the unit's MSDK and libva, for decoder_msdk.cpp on the host.
 *
 * Its stream is not H.264 past the NAL headers: an SPS (type 7) carries its
 * picture size as text, "1280x720"; a PPS (8) is required next to it; a
 * slice (1 or 5) carries, after its first byte, one byte that is the
 * picture's content. A picture decodes to every luma and chroma byte equal
 * to that content, its surface carrying the bitstream's TimeStamp. An SPS
 * larger than the surfaces the decoder was set up for is incompatible (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM); a smaller one
 * changes the pictures' size in the running sequence.
 *
 * Every call that matters goes to `calls`, in order:
 *   "session"           MFXInit
 *   "session gpucopy N" MFXInitEx, with its GPUCopy
 *   "header WxH"        DecodeHeader read an SPS
 *   "init WxH"          the decoder set up for a sequence
 *   "vpp-init X,Y,W,H"  VPP set up for an input rect
 *   "decode-sync"       SyncOperation on a decode task
 *   "vpp X,Y,W,H"       VPP run on a picture with this input rect
 *   "vpp-sync"          SyncOperation on a VPP task
 *   "vpp-out unaligned" VPP's output surface not page-aligned, or its pitch
 *                       not a multiple of 64, while 16-byte aligned
 *   "vpp-out shifted"   VPP's output planes off 16-byte alignment (the unit's
 *                       library copies into them on the CPU) */
#ifndef WOC_FAKE_MSDK_H
#define WOC_FAKE_MSDK_H

#include <stdint.h>
#include <string>
#include <vector>

namespace fake_msdk {

extern std::vector<std::string> calls;

/* A failure to give in sessions made with GPU copy: call ("MFXInitEx",
 * "SetHandle", "DECODE_QueryIOSurf", "VPP_Init", "VPP_Run") returns status
 * after succeeding `after` times, `times` times (0: every time after). A
 * VPP run fails only into an output its GPU copy would take (planes and
 * pitch 16-byte aligned). */
struct GpuFault {
    std::string call;
    int status = 0, after = 0, times = 0;
};
extern GpuFault gpu_fault;

/* "WxH": every picture decoded at this size, whatever its SPS says. */
extern std::string picture_size;

/* Output order: each picture comes out `delay` pictures after it went in
 * (0: at once), as display order holds pictures. A picture whose content is
 * `swallow` (0: none) is taken and never output, as a damaged one may be.
 * With `timestamps` false no surface carries its bitstream's TimeStamp. */
extern int delay;
extern uint8_t swallow;
extern bool timestamps;

/* The newest `refs` decoded pictures stay locked as references (0: none),
 * so the decoder hands other surfaces as work surfaces. */
extern int refs;

/* An access unit of the fake stream: the SPS and PPS when sps is a size
 * ("1088x480"), then an IDR slice (idr) or a P slice whose picture is
 * content (1-127). */
std::string access_unit(const char *sps, bool idr, uint8_t content);

/* How many entries of calls equal s. */
int count(const std::string &s);

}  // namespace fake_msdk

#endif
