/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Host test for reading the renderer's subscriptions from RAMSES's console
 * (ramses_console.h), on lines the car's libramses printed on a test unit,
 * colour codes included: a subscription, an unsubscription of a scene the
 * client no longer knows, a participant gone (with the library's own
 * spelling) and a resource request parse with their scene id and guid, a
 * scene id in 0x hex too; a line cut short and other lines are no event;
 * RAMSES's lines are told from other libraries'; a line is kept as raw text
 * when it names one of our scene ids (decimal or hex, not inside a longer
 * number) or a subscription, not the 2-second statistics; and the reader
 * keeps events, the first 20 RAMSES lines other than the statistics and
 * those kept raw, at most 30 a minute, counting the rest, drops the other
 * lines, and counts every line by its context with the time of the last. */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/ramses_console.h"

using namespace crs;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static const char *const kPrefix =
    "\x1b[1;37m1793493758.563 \x1b[0m\x1b[1;32m[ Info  ] \x1b[0m\x1b[1;36mramses.Client        \x1b[0m | ";
static const char *const kWarnPrefix =
    "\x1b[1;37m1793493770.416 \x1b[0m\x1b[1;33m[ Warn  ] \x1b[0m\x1b[1;36mramses.Client        \x1b[0m | ";
static const char *const kGuid = "00000000-0000-0000-0000-000000000001";
static const std::vector<uint64_t> kScenes = {0x010004f0, 0x01000400};

static std::string line(const char *prefix, const char *text)
{
    return std::string(prefix) + text + "\n";
}

static bool parse(const std::string &raw, ConsoleEvent *e)
{
    *e = ConsoleEvent();
    return parse_console_line(strip_colours(raw.data(), raw.size()), e);
}

int main()
{
    ConsoleEvent e;
    const std::string sub = line(kPrefix, "SceneGraphComponent::handleSceneSubscription: received scene subscription "
                                          "for scene 16778480 from 00000000-0000-0000-0000-000000000001");
    const std::string s = strip_colours(sub.data(), sub.size());
    ok("colour codes and the line end stripped", s.find("1793493758.563 [ Info  ] ramses.Client ") == 0 &&
                                                     s.find('\x1b') == std::string::npos && s.back() == '1');
    ok("a subscription: its scene and the renderer's guid",
       parse(sub, &e) && e.kind == ConsoleEvent::Subscribed && e.scene == 0x010004f0 && !e.unknown && e.guid == kGuid);
    ok("an unsubscription of a scene the client no longer knows",
       parse(line(kWarnPrefix, "SceneGraphComponent::handleSceneUnsubscription:  received scene unsubscription for "
                               "unknown scene 16778480 from 00000000-0000-0000-0000-000000000001"),
             &e) &&
           e.kind == ConsoleEvent::Unsubscribed && e.scene == 0x010004f0 && e.unknown && e.guid == kGuid);
    ok("one of a scene it knows",
       parse(line(kPrefix, "SceneGraphComponent::handleSceneUnsubscription:  received scene unsubscription for "
                           "scene 16778240 from 00000000-0000-0000-0000-000000000001"),
             &e) &&
           e.kind == ConsoleEvent::Unsubscribed && e.scene == 0x01000400 && !e.unknown);
    ok("a scene id in 0x hex",
       parse(line(kPrefix, "SceneGraphComponent::handleSceneSubscription: received scene subscription for scene "
                           "0x10004f0 from G"),
             &e) &&
           e.scene == 0x010004f0 && e.guid == "G");
    ok("a subscription line cut before its id is no event",
       !parse(line(kPrefix, "SceneGraphComponent::handleSceneSubscription: received scene subscription for scene "),
              &e));
    ok("a renderer gone, as the library spells it",
       parse("\x1b[1;37m1793493790.100 \x1b[0m\x1b[1;32m[ Info  ] \x1b[0m\x1b[1;36mramses.Framework     \x1b[0m | "
             "SceneGraphComponent::participantHasDisconnected: unsubscribing all scenes for particpant: "
             "00000000-0000-0000-0000-000000000002\n",
             &e) &&
           e.kind == ConsoleEvent::RendererGone && e.guid == "00000000-0000-0000-0000-000000000002");
    const std::string req =
        line(kPrefix, "ResourceComponent::handleRequestResources(from 00000000-0000-0000-0000-000000000001): send "
                      "locally available 4 resources: 0x166fdde1b42326269dd47178577c086f "
                      "0x60a2c68b34244bb36c885d94d3a3e91d 0xc56754a0f81551e205b458ead8065613 "
                      "0x6cdaec950853575053af9dae02e68cd3 ; ");
    ok("a resource request: the renderer and how many",
       parse(req, &e) && e.kind == ConsoleEvent::ResourceRequest && e.guid == kGuid && e.resources == 4);
    const std::string add = line(kPrefix, "ClientSceneLogic::addSubscriber: add 00000000-0000-0000-0000-000000000001 "
                                          "for scene 16778480, flushCounter 2");
    ok("other lines are no event", !parse(add, &e) &&
                                       !parse(line(kPrefix, "Scene::Scene: sceneId 16778480, publicationMode "
                                                            "LocalAndRemote"),
                                              &e));

    ok("RAMSES's lines are told from others'",
       ramses_line(s) && !ramses_line("2026-11-01 00:42:38.459 [info] vsomeip: routing ready") &&
           !ramses_line("[warn] vsomeip | a line naming ramses.Client"));
    auto interesting = [](const std::string &raw) {
        return console_interesting(strip_colours(raw.data(), raw.size()), kScenes);
    };
    ok("kept raw: a subscription line, and one naming our scene in decimal",
       interesting(sub) && interesting(add) &&
           interesting(line(kPrefix, "SceneGraphComponent::handleCreateScene: creating scene 16778480 (shadow copy)")));
    ok("and in hex, and the levels line",
       interesting(line(kPrefix, "something about 0x010004f0 here")) &&
           interesting(line(kPrefix, "Ramses log levels: Contexts INFO, Console INFO, DLT INFO")));
    ok("not an id inside a longer number, nor a resource request",
       !interesting(line(kPrefix, "flush 167784801 took 2 ms")) && !interesting(req));
    ok("not the 2-second statistics",
       !interesting("\x1b[1;37m1793493759.824 \x1b[0m\x1b[1;32m[ Info  ] \x1b[0m\x1b[1;36mramses.Periodic      "
                    "\x1b[0m | Client: 1 scene(s): 16778480 Subscribed\n"));

    RamsesConsole c(kScenes);
    const int64_t t0 = 1000;
    c.feed(sub.data(), sub.size(), t0);
    c.feed(req.data(), req.size(), t0 + 5);
    const std::string other = "2026-11-01 00:42:38.459 [info] vsomeip: routing ready\n";
    c.feed(other.data(), other.size(), t0 + 6);
    std::vector<ConsoleEvent> events;
    std::vector<std::string> raw;
    uint64_t dropped = 0;
    c.take(&events, &raw, &dropped);
    ok("the reader: events with the time read, both RAMSES lines raw (among the first), another library's line not kept",
       events.size() == 2 && events[0].kind == ConsoleEvent::Subscribed && events[0].ms == t0 &&
           events[1].kind == ConsoleEvent::ResourceRequest && events[1].ms == t0 + 5 && raw.size() == 2 &&
           raw[0] == s && raw[1] == strip_colours(req.data(), req.size()) && dropped == 0);
    for (int k = 0; k < 40; k++)
        c.feed(add.data(), add.size(), t0 + 100 + k);
    events.clear();
    raw.clear();
    c.take(&events, &raw, &dropped);
    ok("at most 30 raw lines in a minute, the rest counted", raw.size() == 28 && dropped == 12 && events.empty());
    c.feed(add.data(), add.size(), t0 + 60000);
    raw.clear();
    c.take(&events, &raw, &dropped);
    ok("the next minute, kept again", raw.size() == 1 && dropped == 0);
    ok("every line counted by context, the most frequent first, with the last one's age",
       c.counts_text(t0 + 62000) == "44 lines (ramses.Client 43, other 1), the last 2 s ago");

    RamsesConsole q(kScenes);
    ok("before any line", q.counts_text(0) == "no line read since start");
    const std::string periodic = "\x1b[1;37m1793493759.824 \x1b[0m\x1b[1;32m[ Info  ] \x1b[0m\x1b[1;36mramses.Periodic"
                                 "      \x1b[0m | Client: 1 scene(s): 16778480 Subscribed\n";
    const std::string levels = "1793536095.922 [ Info  ] ramses.Framework      | Ramses log levels: Contexts INFO, "
                               "Console INFO, DLT INFO\n";
    q.feed(levels.data(), levels.size(), 0);
    for (int k = 0; k < 50; k++)
        q.feed(periodic.data(), periodic.size(), 2000 * k);
    const std::string plain = line(kPrefix, "ClientApplicationLogic::flush: flushing scene");
    for (int k = 0; k < 25; k++)
        q.feed(plain.data(), plain.size(), 100000 + k);
    raw.clear();
    q.take(&events, &raw, &dropped);
    ok("the statistics counted, never kept; the first 20 other RAMSES lines kept raw",
       raw.size() == 20 && raw[0].find("Ramses log levels") != std::string::npos &&
           raw[19].find("flushing scene") != std::string::npos && dropped == 0);
    ok("and the counts show the console alive with no subscription line",
       q.counts_text(104024) == "76 lines (ramses.Periodic 50, ramses.Client 25, ramses.Framework 1), the last 4 s ago");

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
