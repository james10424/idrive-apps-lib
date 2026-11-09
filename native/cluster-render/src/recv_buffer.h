/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* A client's incoming bytes (PROTOCOL.md): read from its socket straight
 * into the buffer's free tail and taken out as whole messages in place, so
 * a frame's pixels reach the layer from the bytes the kernel wrote.
 *
 * Each read asks for the rest of the message being received, or kChunk
 * when its length is not known yet or the rest is shorter: so what follows
 * a message in the buffer is under kChunk, and that is all a read ever has
 * to move to the front. The buffer grows to the message being received
 * and one read (at most kMaxMessage, its length and kChunk), and once the
 * bytes it holds and the message being received fit in kKeep, it goes back
 * to what they need: a client keeps no more than a video frame's room after
 * one large image. */
#ifndef CRS_RECV_BUFFER_H
#define CRS_RECV_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <memory>

namespace crs {

class RecvBuffer {
public:
    /* A read when the next message's length is not known yet, and the
     * smallest read. */
    static const size_t kChunk = 64 << 10;
    /* The room a client keeps between messages: the largest video frame
     * (a 1920x720 4:2:0 atlas) and a read. */
    static const size_t kKeep = (2 << 20) + kChunk;

    /* One read(2) from fd into the tail; its result (errno set when < 0). */
    ssize_t fill(int fd);
    /* The next whole message, its body inside the buffer until the next
     * fill() or settle(); false when none is whole yet, *bad when the bytes
     * cannot start a message (cr::next_message). */
    bool next(uint8_t *type, const char **body, size_t *body_len, bool *bad);
    /* After the messages taken are handled: the room the buffer keeps. */
    void settle();

    size_t size() const { return end_ - begin_; }
    size_t capacity() const { return cap_; }

private:
    size_t read_size() const;
    void make_room(size_t want);
    void resize(size_t cap);

    std::unique_ptr<char[]> data_;
    size_t cap_ = 0, begin_ = 0, end_ = 0;
};

}  // namespace crs

#endif
