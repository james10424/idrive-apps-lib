// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The producer's picture pace (picture_fps, picture_pace.h) against the fake
 * MSDK (fake_msdk.h), fed over its UDP source and its FIFO, as the phone's
 * stream reaches it.
 *
 * Over UDP (the decode thread): a frame the pace skips while a newer one
 * comes stays without a picture and is counted once; the newest skipped
 * frame gets its picture late, not before its slot, and is not counted
 * again. A skipped frame whose late picture comes after the map was shown
 * again gets it whole, not as the same picture again. Over the FIFO (its
 * reader thread): a skipped frame's late picture comes at its slot, well
 * before the decode thread's 500 ms wait for a packet. picture_fps 0: every
 * frame gets a picture.
 *
 * Linux only: the producer's pipes are pipe2's. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "fake_msdk.h"
#include "video_producer.h"

/* The IAS AVB bridge, which the unit's library provides: never joined here
 * (avb_stream empty). */
extern "C" {
void *ias_avbbridge_create_receiver(const char *, const char *) { return nullptr; }
int ias_avbbridge_register_H264_cb(void *, void (*)(void *, const void *, void *), void *) { return -1; }
void ias_avbbridge_destroy_receiver(void *) {}
}

using fake_msdk::access_unit;
using woc::VideoConfig;
using woc::VideoProducer;

static int failures;
static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static VideoConfig config(unsigned picture_fps)
{
    VideoConfig c;
    c.drm_device = "/dev/null";
    c.tex_level_bits = 0;
    c.gpu_copy = false;
    c.picture_fps = picture_fps;
    return c;
}

/* A picture the producer hands out: its first byte (the fake's content) and
 * whether it is the same as the last again. */
struct Picture {
    bool got = false;
    int content = -1;
    bool same = false;
    int64_t at = 0;
};

static Picture next_picture(VideoProducer &p, int timeout_ms)
{
    Picture r;
    std::vector<uint8_t> frame;
    const int64_t end = now_ms() + timeout_ms;
    for (int64_t now = now_ms(); now < end; now = now_ms()) {
        struct pollfd pfd = {p.ready_fd(), POLLIN, 0};
        poll(&pfd, 1, (int)(end - now));
        uint64_t id = 0;
        if (p.take(&frame, &id, &r.same)) {
            r.got = true;
            r.at = now_ms();
            r.content = r.same || frame.empty() ? -1 : frame[0];
            return r;
        }
    }
    return r;
}

static unsigned long long decoded(const VideoProducer &p)
{
    unsigned long long n = 0;
    std::sscanf(p.decode_status().c_str(), "%*[^,], %llu frames", &n);
    return n;
}

/* The fake stream over RTP, one NAL unit a packet, the marker on the slice. */
struct Rtp {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{};
    uint16_t seq = 0;
    uint32_t ts = 0;

    explicit Rtp(uint16_t port)
    {
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    ~Rtp() { close(fd); }

    void send(const char *sps, bool idr, uint8_t content)
    {
        const std::string au = access_unit(sps, idr, content);
        ts += 3000;
        size_t at = 0;
        while (at < au.size()) {
            const size_t start = at + 4;
            size_t end = au.find(std::string("\0\0\0\1", 4), start);
            if (end == std::string::npos)
                end = au.size();
            const bool slice = (au[start] & 0x1f) == 1 || (au[start] & 0x1f) == 5;
            std::string pkt(12, '\0');
            pkt[0] = (char)0x80;
            pkt[1] = (char)(96 | (slice ? 0x80 : 0));
            pkt[2] = (char)(seq >> 8);
            pkt[3] = (char)seq;
            seq++;
            for (int i = 0; i < 4; i++)
                pkt[4 + i] = (char)(ts >> (24 - 8 * i));
            pkt[8] = 1;
            pkt += au.substr(start, end - start);
            sendto(fd, pkt.data(), pkt.size(), 0, (const sockaddr *)&to, sizeof to);
            at = end;
        }
    }
};

static uint16_t free_udp_port()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t n = sizeof a;
    bind(fd, (const sockaddr *)&a, sizeof a);
    getsockname(fd, (sockaddr *)&a, &n);
    close(fd);
    return ntohs(a.sin_port);
}

static void over_udp()
{
    /* 2 a second: a slot every 500 ms, a lead of 125 ms. */
    VideoConfig c = config(2);
    c.udp_port = free_udp_port();
    VideoProducer p(c);
    Rtp rtp(c.udp_port);

    rtp.send("1088x480", true, 1);
    const Picture first = next_picture(p, 2000);
    ok("udp: the first frame's picture", first.got && first.content == 1);
    rtp.send(nullptr, false, 2);
    sleep_ms(50);   /* decoded on its own, not to catch up */
    rtp.send(nullptr, false, 3);
    const Picture late = next_picture(p, 2000);
    ok("udp: the newest skipped frame's late picture", late.got && late.content == 3);
    ok("udp: not before its slot", late.at - first.at >= 450);
    ok("udp: soon after its slot", late.at - first.at < 1200);
    ok("udp: every frame counted once", decoded(p) == 3);
    ok("udp: the frame a newer one passed counted for picture_fps", p.take_paced() == 1);

    /* The next slot is 1000 ms after the first picture: this frame is
     * skipped, and its late picture comes after the map was shown again. */
    rtp.send(nullptr, false, 3);
    sleep_ms(50);
    p.set_pictures(false);
    p.set_pictures(true);
    const Picture again = next_picture(p, 2000);
    ok("udp: after the map was shown again, the late picture whole", again.got && !again.same && again.content == 3);
    ok("udp: no frame counted for picture_fps", p.take_paced() == 0);
    ok("udp: still every frame counted once", decoded(p) == 4);
}

static void over_fifo()
{
    char dir[] = "/tmp/woc-producer-XXXXXX";
    if (!mkdtemp(dir)) {
        ok("fifo: a temporary directory", false);
        return;
    }
    VideoConfig c = config(15);
    c.fifo_path = std::string(dir) + "/video.h264";
    {
        VideoProducer p(c);
        const int fd = open(c.fifo_path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        ok("fifo: open for writing", fd >= 0);
        /* An access unit delimiter ends each access unit in the byte stream. */
        const std::string aud("\0\0\0\1\x09\xf0", 6);
        auto put = [&](const char *sps, bool idr, uint8_t content) {
            const std::string b = access_unit(sps, idr, content) + aud;
            ok("fifo: written", write(fd, b.data(), b.size()) == (ssize_t)b.size());
        };
        put("1088x480", true, 10);
        const Picture first = next_picture(p, 2000);
        ok("fifo: the first frame's picture", first.got && first.content == 10);
        bool soon = true, right = true;
        for (uint8_t content = 11; content < 14; content++) {
            /* Skipped (its slot is 66 ms after the last picture), then given
             * its picture at the slot and the lead (16 ms) after it. */
            const int64_t sent = now_ms();
            put(nullptr, false, content);
            const Picture late = next_picture(p, 2000);
            right = right && late.got && late.content == content;
            soon = soon && late.got && late.at - sent < 300;
            if (late.got && late.at - sent >= 300)
                std::printf("     fifo: frame %u's picture after %lld ms\n", content, (long long)(late.at - sent));
        }
        ok("fifo: each frame's picture", right);
        ok("fifo: each within its slot, not the decode thread's 500 ms wait", soon);
        ok("fifo: every frame counted once", decoded(p) == 4);
        close(fd);
    }
    unlink(c.fifo_path.c_str());
    rmdir(dir);
}

static void off()
{
    VideoConfig c = config(0);
    c.udp_port = free_udp_port();
    VideoProducer p(c);
    Rtp rtp(c.udp_port);
    rtp.send("1088x480", true, 20);
    bool every = next_picture(p, 2000).content == 20;
    for (uint8_t content = 21; content < 25; content++) {
        rtp.send(nullptr, false, content);
        every = every && next_picture(p, 2000).content == content;
    }
    ok("off: every frame's picture at once", every);
    ok("off: none counted for picture_fps", p.take_paced() == 0);
}

int main()
{
    off();
    over_udp();
    over_fifo();
    return failures ? 1 : 0;
}
