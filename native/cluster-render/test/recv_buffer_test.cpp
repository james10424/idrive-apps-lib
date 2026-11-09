/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* RecvBuffer (recv_buffer.h) over a socket pair: a message split across
 * reads, its length too; several in one read; a message of the largest
 * length; one longer and one empty refused; nothing to read and the peer
 * gone; a stream cut at random byte by byte the same as sent; the room a
 * read takes bounded while a large message arrives, given back after it,
 * and kept across video frames; and a video frame read in at most two reads
 * once it is all in the socket. */
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/recv_buffer.h"
#include "cluster_render/protocol.h"

using crs::RecvBuffer;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

struct Pair {
    int w = -1, r = -1;
    Pair()
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
            std::perror("socketpair");
        w = sv[0];
        r = sv[1];
        for (int s : sv)
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    }
    ~Pair()
    {
        close(w);
        close(r);
    }
};

struct Msg {
    uint8_t type;
    std::string body;
};

/* Every whole message the buffer holds now. */
static std::vector<Msg> take(RecvBuffer &b, bool *bad = nullptr)
{
    std::vector<Msg> out;
    uint8_t type = 0;
    const char *body = nullptr;
    size_t n = 0;
    bool was_bad = false;
    while (b.next(&type, &body, &n, &was_bad))
        out.push_back(Msg{type, std::string(body, n)});
    if (bad)
        *bad = was_bad;
    return out;
}

static std::string header(uint32_t len)
{
    std::string h(4, '\0');
    for (int i = 0; i < 4; i++)
        h[i] = (char)(len >> (8 * i));
    return h;
}

static std::string pattern(size_t n, unsigned seed)
{
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        s[i] = (char)(x >> 24);
    }
    return s;
}

/* Writes all of m through p, reading into b whenever the socket is full;
 * the messages taken on the way are appended to got. The largest capacity
 * seen is kept in *peak. */
static void push(Pair &p, RecvBuffer &b, const std::string &m, std::vector<Msg> *got, size_t *peak = nullptr)
{
    size_t off = 0;
    while (off < m.size()) {
        const ssize_t k = write(p.w, m.data() + off, m.size() - off);
        if (k > 0) {
            off += (size_t)k;
            continue;
        }
        while (b.fill(p.r) > 0) {
            for (Msg &x : take(b))
                got->push_back(std::move(x));
            b.settle();
            if (peak && b.capacity() > *peak)
                *peak = b.capacity();
        }
    }
}

/* Reads until the socket is empty, taking and settling after each read. */
static void drain(Pair &p, RecvBuffer &b, std::vector<Msg> *got, size_t *peak = nullptr)
{
    while (b.fill(p.r) > 0) {
        if (peak && b.capacity() > *peak)
            *peak = b.capacity();
        for (Msg &x : take(b))
            got->push_back(std::move(x));
        b.settle();
    }
}

static void split()
{
    Pair p;
    RecvBuffer b;
    const std::string m = cr::encode(cr::kJson, "{\"op\":\"hello\"}");
    std::vector<Msg> got;
    for (size_t i = 0; i < m.size(); i++) {
        if (write(p.w, &m[i], 1) != 1)
            std::printf("write failed\n");
        drain(p, b, &got);
        if (i + 1 < m.size() && !got.empty())
            break;
    }
    ok("a message written a byte at a time, its length too, is taken once whole",
       got.size() == 1 && got[0].type == cr::kJson && got[0].body == "{\"op\":\"hello\"}" && b.size() == 0);

    /* A frame split inside its header, its body and at the end. */
    const std::string f = cr::encode(cr::kFrame, pattern(200000, 1));
    got.clear();
    const size_t cuts[] = {2, 3, 70000, 199000, f.size()};
    size_t from = 0;
    bool early = false;
    for (size_t cut : cuts) {
        push(p, b, f.substr(from, cut - from), &got);
        drain(p, b, &got);
        from = cut;
        early = early || (cut < f.size() && !got.empty());
    }
    ok("a frame split across reads is taken whole, byte for byte",
       !early && got.size() == 1 && got[0].type == cr::kFrame && got[0].body == f.substr(5));
}

static void several()
{
    Pair p;
    RecvBuffer b;
    const std::string m = cr::encode(cr::kJson, "{\"a\":1}") + cr::encode(cr::kFrame, pattern(1000, 2)) +
                          cr::encode(cr::kMesh, "xyz") + cr::encode(cr::kJson, "{\"b\":2}") +
                          cr::encode(cr::kJson, "{\"c\":3}").substr(0, 7);
    if (write(p.w, m.data(), m.size()) != (ssize_t)m.size())
        std::printf("write failed\n");
    const ssize_t r = b.fill(p.r);
    bool bad = true;
    std::vector<Msg> got = take(b, &bad);
    ok("several messages in one read are taken in order",
       r == (ssize_t)m.size() && got.size() == 4 && got[0].body == "{\"a\":1}" && got[1].type == cr::kFrame &&
           got[1].body == pattern(1000, 2) && got[2].type == cr::kMesh && got[2].body == "xyz" &&
           got[3].body == "{\"b\":2}" && !bad);
    b.settle();
    ok("and the start of the next one waits for the rest", b.size() == 7);
    const std::string rest = cr::encode(cr::kJson, "{\"c\":3}").substr(7);
    if (write(p.w, rest.data(), rest.size()) != (ssize_t)rest.size())
        std::printf("write failed\n");
    b.fill(p.r);
    got = take(b);
    ok("which is then taken", got.size() == 1 && got[0].body == "{\"c\":3}" && b.size() == 0);
}

static void largest_and_refused()
{
    {
        Pair p;
        RecvBuffer b;
        const std::string m = header((uint32_t)cr::kMaxMessage) + std::string(1, (char)cr::kFrame) +
                              pattern(cr::kMaxMessage - 1, 3) + cr::encode(cr::kJson, "{}");
        std::vector<Msg> got;
        size_t peak = 0;
        push(p, b, m, &got, &peak);
        drain(p, b, &got, &peak);
        ok("a message of the largest length is taken, and the one after it",
           got.size() == 2 && got[0].body.size() == cr::kMaxMessage - 1 &&
               got[0].body == pattern(cr::kMaxMessage - 1, 3) && got[1].body == "{}");
        ok("the buffer never holds more than that message and one read",
           peak >= 4 + cr::kMaxMessage && peak <= 4 + cr::kMaxMessage + RecvBuffer::kChunk);
        ok("and gives its room back once it is taken", b.capacity() <= RecvBuffer::kKeep);
    }
    {
        Pair p;
        RecvBuffer b;
        const std::string m = header((uint32_t)cr::kMaxMessage + 1) + "F" + std::string(100, 'x');
        if (write(p.w, m.data(), m.size()) != (ssize_t)m.size())
            std::printf("write failed\n");
        b.fill(p.r);
        bool bad = false;
        const std::vector<Msg> got = take(b, &bad);
        ok("a message longer than the largest is refused from its length", got.empty() && bad);
    }
    {
        Pair p;
        RecvBuffer b;
        const std::string m = cr::encode(cr::kJson, "{}") + header(0) + "J{}";
        if (write(p.w, m.data(), m.size()) != (ssize_t)m.size())
            std::printf("write failed\n");
        b.fill(p.r);
        bool bad = false;
        const std::vector<Msg> got = take(b, &bad);
        ok("an empty message is refused, after the whole one before it", got.size() == 1 && bad);
    }
}

static void nothing_and_gone()
{
    Pair p;
    RecvBuffer b;
    errno = 0;
    const ssize_t r = b.fill(p.r);
    ok("nothing to read: EAGAIN, nothing held", r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && b.size() == 0);
    const std::string half = cr::encode(cr::kJson, "{\"x\":1}").substr(0, 6);
    if (write(p.w, half.data(), half.size()) != (ssize_t)half.size())
        std::printf("write failed\n");
    b.fill(p.r);
    errno = 0;
    const ssize_t again = b.fill(p.r);
    ok("a part message stays through EAGAIN",
       again < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && b.size() == 6 && take(b).empty());
    close(p.w);
    p.w = -1;
    ok("the peer gone: 0", b.fill(p.r) == 0);
}

static void random_cuts()
{
    Pair p;
    RecvBuffer b;
    std::string stream;
    std::vector<Msg> sent;
    uint32_t x = 7;
    auto rnd = [&x](uint32_t n) {
        x = x * 1664525u + 1013904223u;
        return (x >> 8) % n;
    };
    for (int i = 0; i < 300; i++) {
        const uint32_t pick = rnd(10);
        const size_t n = pick < 6 ? rnd(200) : pick < 9 ? rnd(80000) : 783360 + 19;
        const uint8_t type = pick < 6 ? cr::kJson : cr::kFrame;
        sent.push_back(Msg{type, pattern(n, (unsigned)i)});
        stream += cr::encode(type, sent.back().body);
    }
    std::vector<Msg> got;
    size_t peak = 0;
    for (size_t off = 0; off < stream.size();) {
        const size_t n = 1 + rnd(rnd(4) ? 3000 : 400000);
        const std::string piece = stream.substr(off, n);
        push(p, b, piece, &got, &peak);
        drain(p, b, &got, &peak);
        off += piece.size();
    }
    bool same = got.size() == sent.size();
    for (size_t i = 0; same && i < got.size(); i++)
        same = got[i].type == sent[i].type && got[i].body == sent[i].body;
    ok("a stream cut at random is taken message for message, byte for byte", same && b.size() == 0);
    ok("its largest message and a read bound the buffer", peak <= 783360 + 24 + RecvBuffer::kChunk);
}

static void room()
{
    Pair p;
    int big = 4 << 20;
    setsockopt(p.w, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(p.r, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    RecvBuffer b;
    const std::string frame = cr::encode(cr::kFrame, pattern(783360 + 14, 9));
    std::vector<Msg> got;
    push(p, b, frame, &got);
    drain(p, b, &got);
    const size_t cap = b.capacity();
    bool kept = true;
    for (int i = 0; i < 5; i++) {
        push(p, b, frame, &got);
        drain(p, b, &got);
        kept = kept && b.capacity() == cap;
    }
    ok("video frames keep the buffer's room: no allocation per frame",
       got.size() == 6 && cap >= frame.size() && cap <= frame.size() + RecvBuffer::kChunk && kept);

    /* A frame all in the socket: one read of kChunk for its length, then
     * one for the rest (when the socket holds a whole frame). */
    int sndbuf = 0;
    socklen_t sl = sizeof sndbuf;
    getsockopt(p.w, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl);
    got.clear();
    const ssize_t k = write(p.w, frame.data(), frame.size());
    if (k == (ssize_t)frame.size()) {
        int reads = 0;
        while (got.empty() && b.fill(p.r) > 0) {
            reads++;
            got = take(b);
            b.settle();
        }
        ok("a frame all in the socket is read in two reads", reads == 2 && got.size() == 1);
    } else {
        std::printf("skip a frame all in the socket: the socket holds %d bytes\n", sndbuf);
        push(p, b, frame.substr(k > 0 ? (size_t)k : 0), &got);
        drain(p, b, &got);
    }

    /* After a large image, small messages: the room goes back. */
    const std::string image = cr::encode(cr::kFrame, pattern(cr::kMaxMessage - 1, 4));
    got.clear();
    push(p, b, image + cr::encode(cr::kJson, "{\"after\":1}").substr(0, 9), &got);
    drain(p, b, &got);
    ok("after a large image with the next message part read: room for what it needs",
       got.size() == 1 && b.size() == 9 && b.capacity() <= RecvBuffer::kKeep);
    const std::string rest = cr::encode(cr::kJson, "{\"after\":1}").substr(9);
    push(p, b, rest, &got);
    drain(p, b, &got);
    for (int i = 0; i < 50; i++) {
        push(p, b, cr::encode(cr::kJson, "{\"n\":" + std::to_string(i) + "}"), &got);
        drain(p, b, &got);
    }
    ok("and small messages after it keep one read's room", got.size() == 52 && b.capacity() <= RecvBuffer::kChunk + 64);
    push(p, b, image, &got);
    drain(p, b, &got);
    ok("a large image taken with nothing after it: one read's room held",
       got.size() == 53 && b.capacity() == RecvBuffer::kChunk);
}

int main()
{
    split();
    several();
    largest_and_refused();
    nothing_and_gone();
    random_cuts();
    room();
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
