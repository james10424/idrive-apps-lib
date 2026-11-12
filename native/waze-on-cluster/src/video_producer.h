// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* VideoProducer: the CarPlay cluster stream from capture to decoded frames.
 * It owns the AVB, UDP and FIFO sources, the RTP depacketiser (rtp_h264.h)
 * and the MSDK/VAAPI decoder (decoder_msdk.h), and keeps the newest decoded
 * frame as a 4:2:0 atlas (yuv_atlas.h) with its levels rounded, for the map
 * feed (map_feed.h) to hand to the shared cluster renderer. Only the newest
 * frame waits: each decoded frame replaces one not yet taken. A frame whose
 * picture is the last one again (the decoder's `same`) waits as that: no
 * atlas, and nothing at all while the last one is still to be taken.
 *
 * Decoder lifecycle (warm decoder). The phone sends SPS, PPS and an IDR only
 * when its stream starts (twice in the first second: the opening keyframe and
 * the one requestStream forces), then non-IDR slices only (VERIFIED on the
 * car: none in the following 60 s). A decoder started after the stream began
 * would never sync. So the sources listen from service start (a failed join
 * or open is retried every second, retry_sources), the decoder comes up at
 * the stream's first SPS (a raw Annex-B feed: at its first bytes), decodes
 * from then on whether or not the map is shown, and stays up for the service
 * lifetime; a later stream is picked up at its own SPS.
 *
 * Threads. The AVB bridge's callback and the UDP reader only copy each
 * datagram into a bounded queue (packet_queue.h, kQueuePackets or
 * kQueueBytes, about 4 s of the stream) and return; one decode thread
 * depacketises and decodes them in order. Every thread runs at the
 * process's own priority. The FIFO reader, a byte stream whose writer waits while the pipe is full,
 * decodes on its own thread.
 *
 * Catch-up (backlog.h). Every frame must be decoded, since each refers to
 * the one before, but only the newest needs a picture. The receiver marks
 * each RTP timestamp's first packet; while a later frame's first packet is
 * still queued behind the packet being depacketised, a whole newer frame is
 * waiting, so the frames that packet completes are decoded without a
 * picture: no VPP, packing (with the level rounding), change sampling or
 * hand-off.
 * The newest frame, with nothing complete behind it, gets the full output.
 * A decode slower than the stream thus skips pictures instead of falling
 * behind; the queue's waits, the frames skipped and a decoder that falls
 * behind even so go to the log and probe.txt.
 *
 * Pacing (picture_pace.h). With picture_fps, a frame whose slot has not
 * come is decoded without a picture, as a catch-up frame is. The newest
 * such frame gets its picture late (Decoder::picture_skipped) once its slot
 * has passed with nothing waiting to be decoded: the decode thread waits
 * for that slot, and a frame decoded on the FIFO reader wakes it. Each
 * frame is counted once, a late picture's when it was decoded.
 *
 * Losses (stream_health.h). A lost packet costs only the NAL unit it was in
 * (rtp_h264.h); the decoder decodes through it and the frames it flags
 * corrupted are shown all the same. A gap is a new stream or a loss
 * (gap_settler.h), a loss logged with its source and whether the receive
 * queue dropped it; only a frame of an access unit begun after the latest
 * loss can end a decode-through episode. */
#ifndef WOC_VIDEO_PRODUCER_H
#define WOC_VIDEO_PRODUCER_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "avb_source.h"
#include "backlog.h"
#include "decoder_msdk.h"
#include "freeze_watch.h"
#include "frame_clock.h"
#include "frame_timing.h"
#include "framing_probe.h"
#include "gap_settler.h"
#include "h264_source.h"
#include "keyframe_wait.h"
#include "packet_queue.h"
#include "picture_pace.h"
#include "rtp_h264.h"
#include "sps_info.h"
#include "stream_health.h"
#include "udp_source.h"

namespace woc {

using crk::Spread;

struct VideoConfig {
    std::string fifo_path;          /* the H.264 FIFO the sink patch points at (file:// mode) */
    std::string carplay_group;      /* group that may write the FIFO (carplay's) */
    uint16_t udp_port = 0;          /* local UDP sink port (udp:// mode); 0 disables */
    std::string avb_stream;         /* media-bus stream to join (avb mode); empty disables */
    std::string drm_device;         /* MSDK VAAPI device, e.g. /dev/dri/renderD128 */
    uint32_t tex_width = 1088;      /* decode target: the map area's size, which the patch */
    uint32_t tex_height = 480;      /* advertises to the phone */
    SourceCrop src_crop;            /* the part of a picture of its size that is the map */
    /* Low bits of each Y, U and V level rounded away in the decoder's
     * packing (round_levels, yuv_atlas.h): at 2 a byte is at most 2 levels
     * off and 0, 128 and 255 stay exact. 0: exact. */
    uint32_t tex_level_bits = 2;
    bool gpu_copy = true;           /* VPP's output copied to system memory on the GPU (decoder_msdk.h) */
    /* A frame decoded within this long is live, while the alternate
     * screen's connection cannot tell (alt_screen.h). */
    int stall_ms = 6000;
    /* Pictures at most this many a second (picture_pace.h); the frames in
     * between are decoded without one. 0: every frame gets a picture. */
    unsigned picture_fps = 0;
    FreezeConfig freeze;
};

class VideoProducer {
public:
    static const size_t kQueuePackets = 4096;
    static const size_t kQueueBytes = 6 << 20;

    explicit VideoProducer(const VideoConfig &cfg);
    ~VideoProducer();

    /* The newest decoded frame not yet taken, swapped into *frame (its
     * buffer comes back for the next one), or with *same the picture last
     * taken again (*frame untouched); false when there is none. */
    bool take(std::vector<uint8_t> *frame, uint64_t *id, bool *same);
    /* Every decoded frame the same blank picture while they keep coming,
     * after the stream showed a picture (freeze_watch.h). */
    bool frozen(int64_t now_ms) const;
    /* What the stream has to show (FreezeWatch::picture). */
    Picture picture(int64_t now_ms) const;
    /* Whether CarPlay's cluster stream is live (FreezeWatch::live). Main
     * thread. */
    void set_live(bool on, int64_t now_ms);
    /* Whether anyone takes the frames (the map on the cluster or the HUD).
     * Without, every frame is still decoded, since the phone sends its
     * keyframes only when its stream starts and the next ones refer to it,
     * but none gets a picture: no VPP, readback, rounding or hand-off. The
     * first picture after it is wanted again comes whole, even when it is
     * the last one again, and counts as a change. */
    void set_pictures(bool on);
    /* Frames decoded without a picture for picture_fps since the last
     * call (main thread). */
    uint64_t take_paced()
    {
        const uint64_t t = paced_total_.load(), d = t - paced_seen_;
        paced_seen_ = t;
        return d;
    }
    /* When the last frame of the decode size was decoded; -1 for none. */
    int64_t last_frame_ms() const { return last_frame_ms_.load(); }
    /* The last minute's pictures of the decode size, how many of them
     * changed and whether the last was blank (FrameTally), once a minute
     * while pictures come, with the stream's losses and corrupted frames,
     * the queue's waits and the frames decoded without a picture, how the
     * frames reached the decoder and left it (frame_timing.h), and the
     * receive queue's overflows since the last time. Main thread. */
    bool minute(int64_t now_ms, FrameTally::Minute *m, StreamHealth::Minute *s, Backlog::Minute *b,
                FrameTiming::Minute *t, uint64_t *overflows);
    /* The stream's status reason and its detail for the log and probe.txt
     * (stream_health.h), and whether it is lost. */
    std::string stream_reason() const;
    std::string stream_detail() const;
    bool stream_lost() const;
    /* Decode errors so far, and the last one's text in *last. */
    uint32_t decode_errors(std::string *last) const;
    /* Readable once a frame waits to be taken. */
    int ready_fd() const { return ready_[0]; }
    size_t frame_bytes() const { return atlas_bytes_; }

    /* Joins the media bus and opens the FIFO again if either failed, at most
     * once a second: at boot the AVB bridge can come up after this service,
     * and the stream's keyframes are only at its start. Main thread. */
    void retry_sources(int64_t now_ms);

    /* The sources' states, the depacketiser and decoder, and what the 2nd
     * stream looks like on the wire, for the diagnostics. */
    std::string probe_report() const;
    /* The capture and decode part of the status line. */
    std::string decode_status() const;

    /* The keyframe wait (keyframe_wait.h), sampled by track_keyframe_wait
     * about once a second from the main loop, which also tells the stream's
     * health. */
    int64_t keyframe_wait_ms(int64_t now_ms) const { return keyframe_wait_.wait_ms(now_ms); }
    void track_keyframe_wait(int64_t now_ms);

private:
    /* Each sets its source's status line; a failure is logged when its
     * reason changes. Main thread. */
    void join_avb();
    void open_fifo();
    /* A receive thread's datagram, into the queue. */
    void receive(const uint8_t *data, size_t len, int source);
    void decode_loop();
    /* One datagram (a UDP packet or a media-bus packet, the same RTP on the
     * wire): an RTP packet goes through the depacketiser, anything else is
     * taken as raw Annex-B. Recorded for the framing probe either way. On the
     * decode thread. */
    void feed_datagram(const PacketQueue::Packet &p);
    /* Raw Annex-B: a FIFO read, on the FIFO thread, or a datagram that is
     * not RTP; source names it for the SPS line. */
    void feed_fifo(const uint8_t *data, size_t len, const char *source);
    /* Depacketiser output: one NAL unit, or an access unit end. Brings the
     * decoder up at an SPS. Under decode_mutex_. */
    void on_nal(const uint8_t *data, size_t len, bool end_of_au);
    /* Under decode_mutex_. */
    bool start_decoder(std::string *err);
    void decode(const uint8_t *data, size_t len, bool end_of_au);
    /* Decoder callback, each decoded frame: the pace's record of it, then
     * on_frame. Under decode_mutex_. */
    void on_decoded(const VideoFrame &f);
    /* How long the decode thread waits for a packet: until the late picture
     * of a frame the pace left without one is due (its slot and the lead),
     * at most 500 ms. */
    int late_wait_ms() const;
    /* That frame's late picture, once its slot has come (decode thread,
     * nothing waiting to be decoded). */
    void picture_paced();
    /* Swaps the decoder's atlas with back_ under lock, or marks the last
     * picture as due again; a frame without a picture is only counted. A
     * late picture's frame was counted when it was decoded. Under
     * decode_mutex_. */
    void on_frame(const VideoFrame &f, bool late);

    VideoConfig cfg_;
    size_t atlas_bytes_;

    Decoder decoder_;
    H264Source source_;
    UdpSource udp_source_;
    AvbSource avb_source_;
    FramingProbe probe_;
    RtpH264 rtp_;
    RtpH264::Sink rtp_sink_;   /* built once, so a packet allocates nothing */
    /* Serialises the decode thread and the FIFO reader: guards decoder_,
     * decoder_up_, decoding_, rtp_, packet_, gaps_, raw_retry_ms_,
     * feed_source_, au_last_us_, sps_log_ and the pace's state below. */
    mutable std::mutex decode_mutex_;
    bool decoder_up_ = false, fifo_up_ = false, udp_up_ = false, avb_up_ = false;
    bool decoding_ = false;          /* the decoder held a sequence after the last feed */
    const PacketQueue::Packet *packet_ = nullptr;   /* the datagram being depacketised */
    const char *feed_source_ = "fifo";   /* the source of the bytes being fed */
    int64_t au_last_us_ = -1;        /* the open RTP access unit's last packet's arrival */
    SpsLog sps_log_;
    GapSettler gaps_;
    int64_t raw_retry_ms_ = 0;       /* a raw feed's next decoder start attempt */
    int64_t source_retry_ms_ = 0;    /* retry_sources' next attempt */

    /* The newest decoded frame, produced on the source thread, taken on the
     * main thread (frame_mutex_). */
    mutable std::mutex frame_mutex_;
    std::vector<uint8_t> back_;
    bool back_dirty_ = false;
    bool repeat_ = false;            /* the picture last taken is due again (back_dirty_ false) */
    int ready_[2] = {-1, -1};        /* a byte written when a frame waits to be taken */
    uint32_t back_w_ = 0, back_h_ = 0;
    uint64_t back_id_ = 0;           /* its number in the decoder's lifetime */
    uint64_t same_total_ = 0;        /* pictures the same as the last (frame_mutex_) */
    ChangeSampler change_;           /* source thread */
    FreezeWatch freeze_;             /* frame_mutex_ */
    FrameTally tally_;               /* frame_mutex_ */
    std::string last_err_;           /* last decode error, for the status line and the log (frame_mutex_) */
    uint32_t decode_errs_ = 0;
    StreamHealth health_;            /* frame_mutex_ */
    uint64_t overflows_seen_ = 0;    /* the queue's overflows at the last minute (frame_mutex_) */
    Backlog backlog_;                /* frame_mutex_ */
    FrameTiming timing_;             /* frame_mutex_ */
    int64_t rtp_ts_[3] = {-1, -1, -1};   /* each source's last RTP timestamp, its receive thread's */

    std::atomic<uint64_t> decoded_total_{0};
    std::atomic<bool> pictures_{true};
    std::atomic<bool> resumed_{false};   /* the next picture is the first since pictures were off */
    bool catching_up_ = false;           /* the last feed decoded without a picture to catch up (decode_mutex_) */
    PicturePace pace_;                   /* decode_mutex_ */
    /* The newest frame came out without a picture for the pace, and may
     * still get it late (decode_mutex_). */
    bool paced_ = false;
    bool pace_skip_ = false;             /* this feed's frames go without a picture for the pace (decode_mutex_) */
    /* Frames the pace left without a picture for good: a newer frame came,
     * or no late picture was made. */
    std::atomic<uint64_t> paced_total_{0};
    uint64_t paced_seen_ = 0;            /* paced_total_ at the last take_paced (main thread) */
    std::atomic<int64_t> last_frame_ms_{-1};
    KeyframeWait keyframe_wait_;            /* main thread */
    std::string udp_status_, avb_status_, fifo_status_;   /* each source's state, for the probe report */

    PacketQueue queue_{kQueuePackets, kQueueBytes};
    std::thread decode_thread_;
};

}  // namespace woc

#endif
