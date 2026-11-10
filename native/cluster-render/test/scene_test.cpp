/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 IterioDev */
/* Host test of scene files (scene_file.h) and their edits (scene_edit.h),
 * on files the test writes in a folder of its own: SHA-256 against the
 * standard's vectors, a file of a million bytes read in chunks; the checks
 * of a scene file and its resource files: accepted from under the root,
 * through a symlink that stays there, refused through one that leaves it,
 * with "..", relative, missing, a folder, writable by the group or others
 * (the file or a folder below the root), another owner's, a hash not its
 * file's or one too few, too many resource files, no root; every edit kind
 * parsed, and malformed lines, back-references and overlong ones refused;
 * the keys that
 * make a line replace a kept one; the edits applied to a synthetic scene of
 * a gauge (a needle turning, a bar, a plain quad): hidden subtrees, poses,
 * a viewport, passes, uniforms (one a mesh lacks), a data object; a pattern
 * resolved once for many stages; a line that changes nothing reported once;
 * and a new scene given every kept line, ending exactly as the first. */
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "../src/scene_edit.h"
#include "../src/scene_file.h"
#include "cluster_render/protocol.h"
#include "fake_graph.h"

using namespace crs;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static void write_file(const std::string &path, const std::string &text, mode_t mode = 0644)
{
    std::ofstream(path) << text;
    chmod(path.c_str(), mode);
}

static void sha256()
{
    const std::string abc = "abc", two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ok("sha256 of nothing", sha256_hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ok("sha256 of abc", sha256_hex((const uint8_t *)abc.data(), abc.size()) ==
                            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ok("sha256 of two blocks", sha256_hex((const uint8_t *)two.data(), two.size()) ==
                                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string path = "/tmp/crs-scene-test-" + std::to_string(getpid()) + ".bin";
    write_file(path, std::string(1000000, 'a'));
    ok("sha256 of a million bytes, read in chunks",
       sha256_file(path) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    unlink(path.c_str());
    ok("sha256 of no file: empty", sha256_file(path).empty());
}

/* The synthetic gauge: a needle turning on a pivot, a bar, a plain quad. */
static const char *const kGauge = "scene 0x01000400\n"
                                  "node gauge -\n"
                                  "node needle_pivot gauge\n"
                                  "mesh needle needle_pivot u_color\n"
                                  "mesh bar gauge u_level u_color\n"
                                  "mesh quad - u_color\n"
                                  "camera main_cam -\n"
                                  "pass main_pass\n"
                                  "pass glow_pass\n"
                                  "data level 1\n";

static void checks(const std::string &dir)
{
    SceneFiles w;
    w.root = dir + "/root";
    w.owner = getuid();
    mkdir(w.root.c_str(), 0755);
    mkdir((w.root + "/scenes").c_str(), 0755);
    const std::string file = w.root + "/scenes/gauge.scene", res = w.root + "/scenes/gauge.res";
    write_file(file, kGauge);
    write_file(res, "resources");
    const std::string sha = sha256_file(file), rsha = sha256_file(res);
    SceneSource s;
    std::string err;
    ok("a scene file and its resource file under the root, each with its hash",
       check_scene_source(w, file, res, sha + " " + rsha, &s, &err) &&
           s.file.find("/scenes/gauge.scene") != std::string::npos && s.res.size() == 1 &&
           s.bytes == std::string(kGauge).size() + 9);
    std::string upper = sha;
    for (char &c : upper)
        c = (char)std::toupper((unsigned char)c);
    ok("a hash in upper case too", check_scene_source(w, file, "", upper, &s, &err) && s.res.empty());
    ok("the scene file's hash not its: refused, named",
       !check_scene_source(w, file, res, std::string(64, '0') + " " + rsha, &s, &err) &&
           err.find("gauge.scene's SHA-256 is " + sha) != std::string::npos);
    ok("the resource file's not its: refused, named",
       !check_scene_source(w, file, res, sha + " " + sha, &s, &err) &&
           err.find("gauge.res's SHA-256 is " + rsha) != std::string::npos);
    ok("not a hash, or one too few: refused", !check_scene_source(w, file, res, sha + " abc", &s, &err) &&
                                                  !check_scene_source(w, file, res, sha, &s, &err) &&
                                                  err.find("2 SHA-256s") != std::string::npos);

    SceneSource direct;
    check_scene_source(w, file, "", sha, &direct, &err);
    ok("through a symlink that stays under the root: its real path",
       symlink(file.c_str(), (w.root + "/inside").c_str()) == 0 &&
           check_scene_source(w, w.root + "/inside", "", sha, &s, &err) && s.file == direct.file);
    write_file(dir + "/outside.scene", kGauge);
    ok("through a symlink that leaves the root: refused",
       symlink((dir + "/outside.scene").c_str(), (w.root + "/scenes/out.scene").c_str()) == 0 &&
           !check_scene_source(w, w.root + "/scenes/out.scene", "", sha, &s, &err) &&
           err.find("not under") != std::string::npos);
    ok("with .. out of the root: refused",
       !check_scene_source(w, w.root + "/scenes/../../outside.scene", "", sha, &s, &err));
    ok("a resource file out of the root: refused",
       !check_scene_source(w, file, res + " " + dir + "/outside.scene", sha + " " + rsha + " " + sha, &s, &err) &&
           err.find("not under") != std::string::npos);
    ok("relative: refused", !check_scene_source(w, "scenes/gauge.scene", "", sha, &s, &err));
    ok("missing: refused", !check_scene_source(w, w.root + "/scenes/none", "", sha, &s, &err));
    ok("a folder: refused", !check_scene_source(w, w.root + "/scenes", "", sha, &s, &err));
    chmod(file.c_str(), 0664);
    ok("the file writable by the group: refused",
       !check_scene_source(w, file, "", sha, &s, &err) && err.find("writable") != std::string::npos);
    chmod(file.c_str(), 0646);
    ok("by others: refused", !check_scene_source(w, file, "", sha, &s, &err));
    chmod(file.c_str(), 0644);
    chmod((w.root + "/scenes").c_str(), 0775);
    ok("a folder below the root writable by the group: refused",
       !check_scene_source(w, file, "", sha, &s, &err) && err.find("/scenes is not owned") != std::string::npos);
    chmod((w.root + "/scenes").c_str(), 0755);
    SceneFiles other = w;
    other.owner = getuid() + 1;
    ok("another owner's: refused", !check_scene_source(other, file, "", sha, &s, &err));
    std::string many, hashes = sha;
    for (int i = 0; i < 17; i++) {
        many += res + " ";
        hashes += " " + rsha;
    }
    ok("17 resource files: refused", !check_scene_source(w, file, many, hashes, &s, &err) &&
                                         err.find("at most 16") != std::string::npos);
    SceneFiles none = w;
    none.root = dir + "/nothing";
    ok("no root: refused", !check_scene_source(none, file, "", sha, &s, &err));
}

static void parsing()
{
    SceneEdit e;
    std::string err;
    ok("--hide", parse_edit("--hide ^quad$", &e, &err) && e.op == SceneEdit::Hide && e.pattern == "^quad$");
    ok("--uniform: the pattern up to the last colon, the name, the values",
       parse_edit("--uniform a:b|bar:u_color=1,0.5,0,1", &e, &err) && e.op == SceneEdit::Uniform &&
           e.pattern == "a:b|bar" && e.name == "u_color" && e.v == std::vector<float>({1, 0.5f, 0, 1}));
    ok("--rotate x,y,z", parse_edit("--rotate needle=0,0,-45.5", &e, &err) && e.op == SceneEdit::Rotate &&
                             e.v == std::vector<float>({0, 0, -45.5f}));
    ok("--viewport x,y,w,h", parse_edit("--viewport cam=0,0,1920,720", &e, &err) && e.v.size() == 4);
    ok("--set", parse_edit("--set level=0.25", &e, &err) && e.op == SceneEdit::Set && e.pattern == "level");
    ok("--pass-on", parse_edit("--pass-on glow", &e, &err) && e.op == SceneEdit::PassOn);
    const char *const bad[] = {"--hide",          "--spin x=1,2,3",  "--move x=1,2",       "--rotate x",
                               "--uniform x=1",   "--uniform x:=1",  "--viewport c=0,0,0,1", "--viewport c=1.5,0,1,1",
                               "--set =1",        "--move x=1,2,nan", "hide x",             "--hide (a)\\1"};
    bool refused = true;
    for (const char *b : bad)
        refused = parse_edit(b, &e, &err) ? (std::printf("     accepted: %s\n", b), false) : refused;
    ok("malformed lines and back-references refused", refused);
    ok("a line too long refused", !parse_edit("--hide " + std::string(2000, 'a'), &e, &err));
    ok("keys: --show is --hide's, --pass-on --pass-off's",
       cr::edit_key("--show a") == cr::edit_key("--hide a") && cr::edit_key("--pass-on a") == cr::edit_key("--pass-off a") &&
           cr::edit_key("--hide a") != cr::edit_key("--hide b"));
    ok("keys: the value left out", cr::edit_key("--uniform m:u=1") == cr::edit_key("--uniform m:u=2,3") &&
                                       cr::edit_key("--uniform m:u=1") != cr::edit_key("--uniform m:v=1") &&
                                       cr::edit_key("--move a=b=1,2,3") == cr::edit_key("--move a=b=4,5,6"));
}

static void edits(const std::string &dir)
{
    const std::string file = dir + "/gauge.scene";
    write_file(file, kGauge);
    FakeGpu gpu;
    TargetSpec t;
    t.name = "panel";
    t.scene_id = 0x01000400;
    std::string err;
    std::unique_ptr<FileScene> owned = gpu.load(t, file, {}, &err);
    FakeFileScene *s = gpu.file("panel");
    ok("the synthetic scene loads", s && s->names[FileScene::Node].size() == 6);
    if (!s)
        return;
    SceneEdits ed;
    const std::vector<EditProblem> refused = ed.add("--hide ^gauge$\n"
                                                    "--show ^needle_pivot$\r\n"
                                                    "\n"
                                                    "--rotate ^needle_pivot$=0,0,30\n"
                                                    "--uniform ^(needle|quad)$:u_color=1,0,0,1\n"
                                                    "--uniform ^bar$:u_level=0.5\n"
                                                    "--viewport ^main_cam$=0,0,1920,720\n"
                                                    "--pass-off glow\n"
                                                    "--set level=0.75\n"
                                                    "--bogus x\n");
    ok("one refused line, named", refused.size() == 1 && refused[0].line == "--bogus x" && refused[0].reason.find("'--bogus x'") == 0);
    ok("staged once", ed.stage(s) && !ed.stage(s) && s->flush(1));
    const FakeFileState &v = s->shown;
    ok("the gauge hidden but for the pivot shown again under it, with the needle",
       !v.visible.at("gauge") && !v.visible.at("bar") && v.visible.at("needle_pivot") && v.visible.at("needle") &&
           !v.visible.count("quad"));
    ok("the needle's pivot turned", v.pose.at("needle_pivot r") == std::vector<float>({0, 0, 30}));
    ok("uniforms of every mesh matched; a viewport; a pass; a data object",
       v.values.at("needle:u_color")[0] == 1 && v.values.at("quad:u_color")[0] == 1 &&
           v.values.at("bar:u_level") == std::vector<float>({0.5f}) &&
           v.viewport.at("main_cam") == std::vector<uint32_t>({0, 0, 1920, 720}) && !v.enabled.at("glow_pass") &&
           !v.enabled.count("main_pass") && v.values.at("level") == std::vector<float>({0.75f}));
    ok("nothing to report", ed.take_problems().empty());

    const int listed = s->listed;
    for (int i = 0; i < 30; i++)
        ed.add("--rotate ^needle_pivot$=0,0," + std::to_string(i) + "\n--uniform ^bar$:u_level=" + std::to_string(i));
    ok("30 updates of two lines: kept as two, their patterns not matched again",
       ed.kept() == 8 && ed.stage(s) && s->listed == listed);
    ed.add("--uniform ^bar$:u_nothing=1\n--hide ^nothing$\n--set nowhere=1\n--hide (\n");
    ok("lines that all change nothing: nothing to flush", !ed.stage(s));
    const std::vector<EditProblem> p = ed.take_problems();
    ok("lines that change nothing reported, each named with why",
       p.size() == 4 && p[0].line == "--uniform ^bar$:u_nothing=1" &&
           p[0].reason.find("'--uniform ^bar$:u_nothing=1' changed nothing: no uniform u_nothing") == 0 &&
           p[1].reason.find("no node matches") != std::string::npos &&
           p[3].reason.find("not a regular expression") != std::string::npos);
    ed.add("--hide ^nothing$");
    ed.stage(s);
    ok("and once per scene", ed.take_problems().empty());
    ed.add("--show ^gauge$\n--hide ^nothing$\n--hide ^needle_pivot$");
    ok("with one that changes something: a flush", ed.stage(s));
    s->flush(2);
    const FakeFileState first = s->shown;

    /* The scene published again: a new one, every kept line in order. */
    owned.reset();
    owned = gpu.load(t, file, {}, &err);
    s = gpu.file("panel");
    ed.reset();
    ed.stage(s);
    s->flush(1);
    ok("a new scene given every kept line ends as the first",
       s->shown.visible == first.visible && s->shown.pose == first.pose && s->shown.values == first.values &&
           s->shown.viewport == first.viewport && s->shown.enabled == first.enabled && s->listed == 4);
    ok("its problems reported again", ed.take_problems().size() == 4);
}

int main()
{
    char tmpl[] = "/tmp/crs-scene-test-XXXXXX";
    const std::string dir = mkdtemp(tmpl) ? tmpl : "";
    sha256();
    checks(dir);
    parsing();
    edits(dir);
    const int removed = std::system(("rm -rf " + dir).c_str());
    (void)removed;
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
