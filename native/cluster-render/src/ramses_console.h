/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* What the display's renderer does with our scenes, read from the RAMSES
 * client's own console log.
 *
 * RAMSES tells a client nothing about who subscribes to its scenes, but the
 * unit's libramses logs it on stdout at console level info, its default
 * (the service sets it through the framework's arguments all the same):
 *
 *   SceneGraphComponent::handleSceneSubscription: received scene subscription for scene 16778480 from
 *     00000000-0000-0000-0000-000000000001
 *   SceneGraphComponent::handleSceneUnsubscription:  received scene unsubscription for scene 16778480 from
 *     00000000-0000-0000-0000-000000000001
 *   SceneGraphComponent::participantHasDisconnected: unsubscribing all scenes for particpant:
 *     00000000-0000-0000-0000-000000000001
 *   ResourceComponent::handleRequestResources(from 00000000-0000-0000-0000-000000000001): send locally
 *     available 4 resources: 0x166fdde1b42326269dd47178577c086f ... ;
 *
 * each one line after "<unix time> [ Info  ] ramses.Client        | ", the
 * time, level and context wrapped in colour codes; the (un)subscription of a
 * scene the client does not know says "for unknown scene" (VERIFIED on a
 * test unit with the car's libramses build, against its stand-in KOMBI
 * renderer). Scene ids print in decimal; 0x hex is read too.
 *
 * RamsesConsole moves fd 1 onto a pipe before any RAMSES object exists and
 * makes stdout line buffered, so each line reaches the pipe as it is
 * written; a thread reads it. RAMSES's own lines (a "ramses." context, or
 * one of the above) are dropped: at info they are several a second. Any
 * other line (another library's console log) goes on to the original stdout
 * unchanged. Of RAMSES's lines it keeps the parsed events, with the time
 * each was read, and, as raw text without colour codes, the first
 * kFirstLines that are not the 2-second statistics (ramses.Periodic), then
 * every line that mentions one of our scene ids or contains "ubscri" (not
 * the statistics), at most kRawPerMinute a minute, so field logs show the
 * exact formats even where the parser misses one. Every line read is counted
 * by its context ("other" for another library's), with the time of the
 * last, so the status shows whether the console stays alive while no
 * subscription line comes.
 * fd() is readable while something waits to be taken. If the pipe cannot be
 * set up, stdout is left as it was and nothing is ever read; if reading it
 * fails, the original stdout goes back onto fd 1 (so RAMSES never blocks on
 * a full pipe) and nothing more is read. It lives for the process: made
 * once, never destroyed. */
#ifndef CRS_RAMSES_CONSOLE_H
#define CRS_RAMSES_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace crs {

struct ConsoleEvent {
    enum Kind { Subscribed, Unsubscribed, RendererGone, ResourceRequest };
    Kind kind = Subscribed;
    uint64_t scene = 0;      /* Subscribed, Unsubscribed */
    bool unknown = false;    /* the client did not know the scene */
    std::string guid;        /* the renderer's; empty when the line names none */
    unsigned resources = 0;  /* ResourceRequest */
    int64_t ms = 0;          /* when the line was read */
};

/* The line without colour codes and line ends. */
std::string strip_colours(const char *s, size_t n);
/* One line, colour codes stripped, as an event; false: none of the above. */
bool parse_console_line(const std::string &line, ConsoleEvent *e);
/* The line comes from RAMSES's logger. */
bool ramses_line(const std::string &line);
/* The line mentions one of scenes (decimal or 0x hex), contains "ubscri" or
 * is the levels the logger starts with. */
bool console_interesting(const std::string &line, const std::vector<uint64_t> &scenes);

class RamsesConsole {
public:
    static const unsigned kRawPerMinute = 30;
    static const unsigned kFirstLines = 20;
    static const size_t kMaxContexts = 16;

    explicit RamsesConsole(const std::vector<uint64_t> &scenes) : scenes_(scenes) {}
    /* Call once, before any RAMSES object exists. */
    bool start(std::string *err);
    int fd() const { return wake_[0]; }
    /* What was read since the last call, oldest first, and how many raw
     * lines went over kRawPerMinute since. Any thread. */
    void take(std::vector<ConsoleEvent> *events, std::vector<std::string> *raw, uint64_t *raw_dropped);
    /* "N lines (context n, ...), the last X s ago", most frequent context
     * first. Any thread. */
    std::string counts_text(int64_t now_ms) const;
    /* One line as read at now_ms; forwarded or kept as above (the reader
     * thread's, and tests'). */
    void feed(const char *s, size_t n, int64_t now_ms);

private:
    void run();

    std::vector<uint64_t> scenes_;
    int in_ = -1;    /* the pipe's read end */
    int out_ = -1;   /* the original stdout; -1 in tests: other lines are dropped */
    int wake_[2] = {-1, -1};
    mutable std::mutex mutex_;
    std::vector<ConsoleEvent> events_;
    std::vector<std::string> raw_;
    uint64_t raw_dropped_ = 0;
    unsigned raw_minute_ = 0;    /* raw lines kept in the minute from raw_since_ms_ */
    int64_t raw_since_ms_ = -1;
    unsigned first_ = 0;         /* RAMSES lines other than the statistics read so far, up to kFirstLines */
    std::map<std::string, uint64_t> contexts_;   /* lines read, by context */
    uint64_t lines_ = 0;
    int64_t last_ms_ = -1;
};

}  // namespace crs

#endif
