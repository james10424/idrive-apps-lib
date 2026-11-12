// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test for the receive queue (packet_queue.h): packets come out in
 * order; an overflow drops the oldest, is counted, and the next packet out
 * says how many went before it; a consumer that stalls for a while within
 * the queue's headroom loses nothing; large datagrams leave no large buffer
 * behind; each packet says how long it waited and whether a later frame
 * waits behind it; wake ends a pop's wait. With that, a decoder slower than
 * the stream that skips the pictures of frames with a later one waiting
 * (video_producer.h) keeps the wait bounded and decodes every frame, and a
 * fast one shows every frame. */
#include <algorithm>
#include <cstdio>
#include <future>
#include <thread>

#include "../src/packet_queue.h"

using namespace woc;

static int failures;
static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static void push_n(PacketQueue &q, uint32_t from, uint32_t n, size_t len, int source)
{
    std::vector<uint8_t> b(len);
    for (uint32_t i = from; i < from + n; i++) {
        for (size_t k = 0; k < len && k < 4; k++) b[k] = (uint8_t)(i >> (8 * k));
        q.push(b.data(), len, source);
    }
}

static uint32_t id(const PacketQueue::Packet &p)
{
    return (uint32_t)p.data[0] | (uint32_t)p.data[1] << 8 | (uint32_t)p.data[2] << 16 | (uint32_t)p.data[3] << 24;
}

struct Run {
    uint32_t decoded = 0, pictures = 0, last_picture = 0;
    bool in_order = true;
    int64_t max_wait_us = 0;
    uint64_t overflows = 0;
};

/* `frames` frames of 4 packets, `fps` a second, through a queue of the
 * stream's bounds into a stub decoder. A frame completes at the next one's
 * first packet, as the depacketiser frames it; each costs decode_ms, and
 * output_ms more for its picture, which it gets unless catch_up and a later
 * frame waits. */
static Run simulate(uint32_t frames, uint32_t fps, int decode_ms, int output_ms, bool catch_up)
{
    PacketQueue q(4096, 6 << 20);
    std::thread producer([&] {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> b(1400);
        for (uint32_t f = 0; f <= frames; f++) {
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(1000000ull * f / fps));
            for (size_t k = 0; k < 4; k++) b[k] = (uint8_t)(f >> (8 * k));
            for (int k = 0; k < 4; k++)
                q.push(b.data(), b.size(), 1, k == 0);
        }
    });
    Run r;
    PacketQueue::Packet p;
    bool first = true;
    while (r.decoded < frames && q.pop(&p, 1000)) {
        r.max_wait_us = std::max(r.max_wait_us, p.waited_us);
        if (!p.frame_start)
            continue;
        if (!first) {
            const uint32_t done = id(p) - 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(decode_ms));
            if (!(catch_up && p.later_frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(output_ms));
                r.pictures++;
                r.last_picture = done;
            }
            r.in_order = r.in_order && done == r.decoded;
            r.decoded++;
        }
        first = false;
    }
    producer.join();
    r.overflows = q.overflows();
    return r;
}

int main()
{
    {
        PacketQueue q(8, 1 << 20);
        push_n(q, 0, 5, 100, 1);
        push_n(q, 5, 1, 1400, 2);
        PacketQueue::Packet p;
        bool order = true;
        for (uint32_t i = 0; i < 6; i++)
            order = order && q.pop(&p, 0) && id(p) == i && p.dropped == 0 && p.source == (i < 5 ? 1 : 2);
        ok("packets come out in order, with their sizes and sources", order && p.data.size() == 1400);
        ok("an empty queue times out", !q.pop(&p, 10));
    }
    {
        /* 8 slots, 12 pushed: the 4 oldest go, and packet 4 says so. */
        PacketQueue q(8, 1 << 20);
        push_n(q, 0, 12, 100, 1);
        PacketQueue::Packet p;
        ok("a full queue drops its oldest packets and counts them", q.overflows() == 4 && q.high_water() == 8);
        ok("the oldest left carries the drops before it", q.pop(&p, 0) && id(p) == 4 && p.dropped == 4);
        bool rest = true;
        for (uint32_t i = 5; i < 12; i++)
            rest = rest && q.pop(&p, 0) && id(p) == i && p.dropped == 0;
        ok("the rest follow with none", rest);
    }
    {
        /* The byte bound: 3 packets of 400 B fit in 1000 B. */
        PacketQueue q(64, 1000);
        push_n(q, 0, 4, 400, 1);
        PacketQueue::Packet p;
        ok("the byte bound drops the oldest too", q.overflows() == 2 && q.pop(&p, 0) && id(p) == 2 && p.dropped == 2);
        push_n(q, 10, 1, 2000, 1);
        push_n(q, 11, 1, 100, 1);
        ok("a datagram over the byte bound empties the queue and is dropped itself; the next says so",
           q.overflows() == 4 && q.pop(&p, 0) && id(p) == 11 && p.dropped == 2 && !q.pop(&p, 0));
    }
    {
        /* A consumer that stalls 200 ms against 1,100 packets a second of
         * 1,400 B: the queue holds what arrives meanwhile. */
        PacketQueue q(4096, 6 << 20);
        const uint32_t total = 1100;
        std::thread producer([&] {
            for (uint32_t i = 0; i < total; i += 11) {
                push_n(q, i, 11, 1400, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        PacketQueue::Packet p;
        uint32_t next = 0;
        bool in_order = true;
        while (next < total && q.pop(&p, 1000)) {
            in_order = in_order && id(p) == next && p.dropped == 0;
            next++;
        }
        producer.join();
        ok("a slow consumer loses nothing within the headroom, in order", next == total && in_order && q.overflows() == 0);
    }
    {
        /* Datagrams of 60 KB through a queue of the stream's bounds: the
         * slots keep no buffer that large once the packets are out. */
        PacketQueue q(4096, 6 << 20);
        PacketQueue::Packet p;
        for (int round = 0; round < 50; round++) {
            push_n(q, 0, 100, 60000, 1);
            while (q.pop(&p, 0)) {
            }
        }
        ok("large datagrams leave at most 2 KB a slot behind", q.held_bytes() <= 4096 * PacketQueue::kKeepBytes &&
                                                                    q.held_bytes() < 1000000 && q.overflows() == 0);
        bool popped = true;
        for (int k = 0; k < 2; k++) {
            push_n(q, 0, 1, 1400, 1);
            popped = popped && q.pop(&p, 0);
        }
        ok("stream-sized ones keep their buffers", popped && q.held_bytes() >= 1400);
    }
    {
        /* Three 60 KB datagrams, then one of 150 KB that only fits once all
         * three are dropped: the dropped slots keep nothing large. */
        PacketQueue q(64, 200000);
        push_n(q, 0, 3, 60000, 1);
        push_n(q, 3, 1, 150000, 1);
        PacketQueue::Packet p;
        ok("an overflow of large datagrams frees the dropped slots' buffers",
           q.overflows() == 3 && q.held_bytes() < 150000 + 4 * PacketQueue::kKeepBytes && q.pop(&p, 0) && id(p) == 3 &&
               p.dropped == 3);
    }
    {
        PacketQueue q(8, 1 << 20);
        bool popped = true;
        std::thread waiter([&] {
            PacketQueue::Packet p;
            popped = q.pop(&p, 5000);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.close();
        waiter.join();
        ok("close wakes a waiting pop, which returns nothing", !popped);
    }
    {
        PacketQueue q(8, 1 << 20);
        push_n(q, 0, 1, 100, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int64_t oldest = q.oldest_wait_us();
        PacketQueue::Packet p;
        ok("a packet says how long it waited", oldest >= 20000 && q.pop(&p, 0) && p.waited_us >= oldest &&
                                                   p.waited_us < 1000000 && q.oldest_wait_us() == 0);
    }
    {
        /* Two frames of two packets: while the second frame's first packet
         * waits, every packet before it says so. */
        PacketQueue q(8, 1 << 20);
        const uint8_t b[4] = {0, 0, 0, 0};
        const bool starts[4] = {true, false, true, false};
        for (bool st : starts)
            q.push(b, sizeof b, 1, st);
        PacketQueue::Packet p;
        bool later[4] = {false, false, false, false}, marked = true;
        for (int i = 0; i < 4; i++) {
            q.pop(&p, 0);
            later[i] = p.later_frame;
            marked = marked && p.frame_start == starts[i];
        }
        ok("a later frame's start waiting is told until it is taken",
           marked && later[0] && later[1] && !later[2] && !later[3]);
        /* 4 slots, 5 packets: the dropped one was a frame start. */
        PacketQueue d(4, 1 << 20);
        const bool five[5] = {true, false, true, false, true};
        for (bool st : five)
            d.push(b, sizeof b, 1, st);
        bool laters = true;
        for (int i = 0; i < 4; i++) {
            d.pop(&p, 0);
            laters = laters && p.later_frame == (i < 3);
        }
        ok("a dropped frame start no longer counts as waiting", d.overflows() == 1 && laters);
    }
    {
        const uint32_t frames = 60;
        /* Independent runs, each with its own queue and threads, at once. */
        auto slow_run = std::async(std::launch::async, simulate, frames, 60u, 4, 30, true);
        auto behind_run = std::async(std::launch::async, simulate, frames, 60u, 4, 30, false);
        auto fast_run = std::async(std::launch::async, simulate, 20u, 20u, 2, 3, true);
        const Run slow = slow_run.get();
        std::printf("     slow decoder with catch-up: %u of %u frames shown, packets waited at most %lld ms\n",
                    slow.pictures, slow.decoded, (long long)(slow.max_wait_us / 1000));
        ok("a decoder slower than the stream: every frame decoded in order, none lost to the queue",
           slow.decoded == frames && slow.in_order && slow.overflows == 0);
        ok("it skips pictures, and the newest frame gets one",
           slow.pictures > 0 && slow.pictures < slow.decoded && slow.last_picture == frames - 1);
        ok("the wait stays bounded", slow.max_wait_us < 250000);
        const Run behind = behind_run.get();
        std::printf("     the same without catch-up: packets waited at most %lld ms\n",
                    (long long)(behind.max_wait_us / 1000));
        ok("without catch-up the same decoder falls ever further behind", behind.max_wait_us > 500000);
        const Run fast = fast_run.get();
        std::printf("     a fast decoder at 20 frames a second: %u of %u frames shown, packets waited at most %lld ms\n",
                    fast.pictures, fast.decoded, (long long)(fast.max_wait_us / 1000));
        ok("a decoder faster than the stream shows every frame",
           fast.decoded == 20 && fast.in_order && fast.pictures == 20);
    }
    {
        /* wake ends a pop's wait at once, or the next pop's; a packet
         * waiting still comes out. */
        PacketQueue q(8, 1 << 20);
        PacketQueue::Packet p;
        auto waker = std::async(std::launch::async, [&q] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            q.wake();
        });
        const int64_t t0 = PacketQueue::now_us();
        const bool got = q.pop(&p, 5000);
        const int64_t waited = PacketQueue::now_us() - t0;
        waker.get();
        ok("wake: a waiting pop returns false at once", !got && waited < 2000000);
        q.wake();
        const int64_t t2 = PacketQueue::now_us();
        ok("wake: before a pop, that pop returns at once", !q.pop(&p, 5000) && PacketQueue::now_us() - t2 < 2000000);
        const int64_t t1 = PacketQueue::now_us();
        ok("wake: once", !q.pop(&p, 100) && PacketQueue::now_us() - t1 >= 90000);
        push_n(q, 7, 1, 32, 1);
        q.wake();
        ok("wake: a packet waiting still comes out", q.pop(&p, 0) && p.data.size() == 32);
    }
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
