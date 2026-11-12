// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* PacketQueue: datagrams from the receive threads (the AVB bridge's callback,
 * the UDP reader) to the decode thread, so a slow decode never holds up a
 * receive. A push copies the datagram and returns; a pop hands the oldest one
 * over by swapping buffers, so with datagrams of the stream's size (RTP at
 * most an MTU) nothing is allocated once every slot has a buffer. A buffer
 * larger than kKeepBytes that comes back, or whose packet is dropped, is
 * freed, so the slots hold at most kKeepBytes each besides the packets
 * waiting.
 *
 * Bounded by a packet count and a byte total. A push that would exceed
 * either drops the oldest packets until it fits (a datagram larger than the
 * byte bound is dropped itself); the drops are counted, and the next packet
 * delivered carries how many were dropped right before it, so the decode
 * side can tell its own losses from the sender's.
 *
 * Each packet is stamped when it arrives, and a pop says how long it waited.
 * The pusher marks the first packet of each frame; a pop also says whether a
 * later frame's first packet was still waiting, so the decode side knows a
 * whole newer frame is queued behind the one it is finishing. Thread-safe. */
#ifndef WOC_PACKET_QUEUE_H
#define WOC_PACKET_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace woc {

class PacketQueue {
public:
    struct Packet {
        std::vector<uint8_t> data;
        int source = 0;          /* the pusher's tag */
        uint32_t dropped = 0;    /* packets dropped by this queue right before this one */
        bool frame_start = false;   /* the pusher's mark: a frame's first packet */
        int64_t arrived_us = 0;  /* steady clock */
        int64_t waited_us = 0;   /* from its arrival until the pop */
        bool later_frame = false;   /* another frame start still waited when it was popped */
    };

    static const size_t kKeepBytes = 2048;

    PacketQueue(size_t max_packets, size_t max_bytes) : slots_(max_packets), max_bytes_(max_bytes) {}

    static int64_t now_us()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void push(const uint8_t *data, size_t len, int source, bool frame_start = false)
    {
        const int64_t now = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (count_ > 0 && (count_ == slots_.size() || bytes_ + len > max_bytes_))
                drop_oldest();
            if (len > max_bytes_ || slots_.empty()) {
                carry_++;
                overflows_++;
                return;
            }
            Packet &p = slots_[(head_ + count_) % slots_.size()];
            p.data.assign(data, data + len);
            p.source = source;
            p.dropped = carry_;
            p.frame_start = frame_start;
            p.arrived_us = now;
            carry_ = 0;
            count_++;
            bytes_ += len;
            starts_ += frame_start;
            if (count_ > high_)
                high_ = count_;
        }
        cv_.notify_one();
    }

    /* The oldest packet into *out (its buffer goes back to the queue),
     * waiting up to timeout_ms; false when there is none or the queue is
     * closed. */
    bool pop(Packet *out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return count_ > 0 || closed_ || woken_; });
        woken_ = false;
        if (closed_ || count_ == 0)
            return false;
        Packet &p = slots_[head_];
        bytes_ -= p.data.size();
        starts_ -= p.frame_start;
        out->data.swap(p.data);
        if (p.data.capacity() > kKeepBytes)
            std::vector<uint8_t>().swap(p.data);
        out->source = p.source;
        out->dropped = p.dropped;
        out->frame_start = p.frame_start;
        out->arrived_us = p.arrived_us;
        out->waited_us = now_us() - p.arrived_us;
        out->later_frame = starts_ > 0;
        head_ = (head_ + 1) % slots_.size();
        count_--;
        return true;
    }

    /* Ends the wait of a pop, or of the next one, at once: it returns the
     * oldest packet, or false when there is none. */
    void wake()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
        }
        cv_.notify_all();
    }

    /* Wakes a pop; every later pop returns false. */
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }
    uint64_t overflows() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return overflows_;
    }
    /* Bytes the slots' buffers hold, waiting or kept. */
    size_t held_bytes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        for (const Packet &p : slots_)
            n += p.data.capacity();
        return n;
    }
    /* How long the oldest packet waiting has waited; 0 for none. */
    int64_t oldest_wait_us() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ ? now_us() - slots_[head_].arrived_us : 0;
    }
    /* The most packets waiting at once. */
    size_t high_water() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_;
    }

private:
    void drop_oldest()
    {
        Packet &p = slots_[head_];
        bytes_ -= p.data.size();
        starts_ -= p.frame_start;
        if (p.data.capacity() > kKeepBytes)
            std::vector<uint8_t>().swap(p.data);
        carry_ += p.dropped + 1;
        overflows_++;
        head_ = (head_ + 1) % slots_.size();
        count_--;
        if (count_ > 0) {
            slots_[head_].dropped += carry_;
            carry_ = 0;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Packet> slots_;
    const size_t max_bytes_;
    size_t head_ = 0, count_ = 0, bytes_ = 0, high_ = 0;
    size_t starts_ = 0;          /* frame starts waiting */
    uint32_t carry_ = 0;         /* drops not yet attributed to a packet still queued */
    uint64_t overflows_ = 0;
    bool closed_ = false;
    bool woken_ = false;
};

}  // namespace woc

#endif
