/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
#include "recv_buffer.h"

#include <cstring>
#include <unistd.h>

#include "cluster_render/protocol.h"

namespace crs {

const size_t RecvBuffer::kChunk;
const size_t RecvBuffer::kKeep;

size_t RecvBuffer::read_size() const
{
    const size_t held = end_ - begin_;
    if (held < 4)
        return kChunk;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data_.get() + begin_);
    const size_t whole = 4 + (size_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    if (whole < 5 || whole > 4 + cr::kMaxMessage || whole <= held)
        return kChunk;
    return whole - held > kChunk ? whole - held : kChunk;
}

void RecvBuffer::resize(size_t cap)
{
    const size_t held = end_ - begin_;
    std::unique_ptr<char[]> d(cap ? new char[cap] : nullptr);
    if (held)
        std::memcpy(d.get(), data_.get() + begin_, held);
    data_ = std::move(d);
    cap_ = cap;
    begin_ = 0;
    end_ = held;
}

void RecvBuffer::make_room(size_t want)
{
    if (cap_ - end_ >= want)
        return;
    const size_t held = end_ - begin_;
    if (cap_ - held < want) {
        resize(held + want);
        return;
    }
    std::memmove(data_.get(), data_.get() + begin_, held);
    begin_ = 0;
    end_ = held;
}

ssize_t RecvBuffer::fill(int fd)
{
    if (begin_ == end_)
        begin_ = end_ = 0;
    const size_t want = read_size();
    make_room(want);
    const ssize_t r = read(fd, data_.get() + end_, want);
    if (r > 0)
        end_ += (size_t)r;
    return r;
}

bool RecvBuffer::next(uint8_t *type, const char **body, size_t *body_len, bool *bad)
{
    size_t used = 0;
    if (!cr::next_message(data_.get() + begin_, end_ - begin_, &used, type, body, body_len, bad))
        return false;
    begin_ += used;
    return true;
}

void RecvBuffer::settle()
{
    const size_t held = end_ - begin_;
    if (!held)
        begin_ = end_ = 0;
    /* Only a few bytes are moved: a large message still arriving keeps its
     * room until it is taken. */
    if (cap_ <= kKeep || held > kChunk)
        return;
    const size_t need = held ? held + read_size() : 0;
    if (need <= kKeep)
        resize(need);
}

}  // namespace crs
