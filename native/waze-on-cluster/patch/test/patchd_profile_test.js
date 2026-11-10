#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* patchd's driver profiles ({"cmd":"profile"}, {"cmd":"profile-own"}): the
 * map settings are shared until a profile takes its own; a profile switch
 * puts that profile's own settings, or the shared ones, in map.state.conf
 * through the map settings' save, in the format a patchd without profiles
 * reads; a change of a setting goes to the set in effect and map.state.conf.
 * profiles.json: never written while no profile has its own settings, else
 * written whole by rename, 0644 whatever the umask, before map.state.conf in
 * the same delayed write (a burst is one write; a stop writes what waits). A
 * write that fails or is cut short loses no profile's settings, nor the
 * shared ones at the last untick: tried again, and at the start
 * map.state.conf is written again from profiles.json. A
 * tick for another profile than the one in effect is refused. A profile id
 * is bounded in length and characters; a missing, corrupt or unreadable
 * profiles.json is no profile with its own settings (a warning, patchd keeps
 * running); at most MAX_OWN profiles have their own. Off (cluster and hud 0)
 * as a profile's own or the shared settings is switched to and from and kept
 * at the next start. The HUD map kept shown over BMW's pop-ups is a
 * profile's own setting like the others; a profiles.json saved before it is
 * taken, giving way. */
'use strict';
var cp = require('child_process');
var fs = require('fs');
var net = require('net');
var os = require('os');
var path = require('path');

var PATCHD = path.join(__dirname, '..', 'patchd.js');
var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-prof-'));
var SOCK = path.join(dir, 'sock', 'patch.sock');
var CONF = path.join(dir, 'waze-on-cluster.local.conf');
var STATE = path.join(dir, 'map.state.conf');
var PROFILES = path.join(dir, 'profiles.json');
var SAVE_MS = 200;
var LOCAL = 'upload_kb_per_s = 3000\n';
fs.writeFileSync(CONF, LOCAL);
fs.writeFileSync(STATE, 'hud = 1\nhud_width_pct = 70\n');

var ENV = Object.assign({}, process.env, {
    WOC_PATCH_SOCK: SOCK,
    WOC_PATCH_GROUP: '',
    WOC_LOCAL_CONF: CONF,
    WOC_STATE_DIR: dir,
    WOC_CARPLAY: path.join(dir, 'no-carplay'),
    WOC_STREAM_STATE: path.join(dir, 'no-stream.json'),
    WOC_RESYNC_FILE: path.join(dir, 'resync.json'),
    WOC_RECMGR_CONF: path.join(dir, 'no-recmgr'),
    WOC_RECMGR_AT_BOOT: path.join(dir, 'no-recmgr-at-boot.json'),
    WOC_PROC_STAT: path.join(dir, 'no-stat'),
    WOC_BOOT_ID: path.join(dir, 'no-boot-id'),
    WOC_MOUNT: path.join(dir, 'no-mount'),
    WOC_PROC_MOUNTS: path.join(dir, 'no-mounts'),
    WOC_CAPTURE_SOCK: path.join(dir, 'no-capture.sock'),
    WOC_DLT_RECEIVE: path.join(dir, 'no-dlt-receive'),
    WOC_DLT_RETRY_MS: '600000',
    WOC_REMOUNT_LOCK: path.join(dir, 'remount.lock'),
    WOC_MAP_SAVE_MS: String(SAVE_MS),
    WOC_SAVE_RETRY_MS: '300',
});
var out = '';
var p = null;
function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
async function start() {
    var n = out.split('patchd listening').length;
    p = cp.spawn(process.execPath, [PATCHD], { env: ENV, stdio: ['ignore', 'pipe', 'pipe'] });
    p.stdout.on('data', function (d) { out += d; });
    p.stderr.on('data', function (d) { out += d; });
    for (var t = 0; t < 5000 && out.split('patchd listening').length <= n; t += 50) await sleep(50);
}
async function stop(signal) {
    var c = p;
    c.kill(signal);
    await new Promise(function (resolve) { c.on('exit', resolve); });
}

/* Several requests on one connection, written at once; resolves to their replies. */
function askAll(list) {
    return new Promise(function (resolve, reject) {
        var s = net.connect(SOCK), buf = '', replies = [];
        s.setEncoding('utf8');
        s.on('connect', function () { s.write(list.map(function (o) { return JSON.stringify(o) + '\n'; }).join('')); });
        s.on('data', function (d) {
            buf += d;
            var i;
            while ((i = buf.indexOf('\n')) >= 0) {
                var m = null;
                try { m = JSON.parse(buf.slice(0, i)); } catch (e) {}
                buf = buf.slice(i + 1);
                if (m && /-reply$/.test(m.ev)) replies.push(m);
            }
            if (replies.length === list.length) { s.end(); resolve(replies); }
        });
        s.on('error', reject);
        setTimeout(function () { reject(new Error('no replies to ' + JSON.stringify(list))); }, 5000);
    });
}
function ask(obj) { return askAll([obj]).then(function (r) { return r[0]; }); }
/* The settings the render reads: map.state.conf as key -> number, and its keys in order. */
function state() {
    var s = {};
    fs.readFileSync(STATE, 'utf8').split('\n').forEach(function (l) {
        var m = /^(\w+) = (-?\d+)$/.exec(l);
        if (m) s[m[1]] = Number(m[2]);
    });
    return s;
}
function profiles() { var t = fs.readFileSync(PROFILES, 'utf8'); try { return JSON.parse(t); } catch (e) { throw new Error(e.message + ': ' + t); } }
function settings(cluster, hud, pct, x, y, keep) {
    return { cluster: cluster, hud: hud, hud_width_pct: pct, hud_offset_x: x, hud_offset_y: y, hud_keep_shown: keep || 0 };
}
function same(a, b) { return JSON.stringify(a) === JSON.stringify(b); }
/* For a check that nothing is written: past the save's delay. */
function saved() { return sleep(SAVE_MS + 150); }
/* For a check that something is written: until it holds, at most 10 s
 * (a save syncs the disk, which can take long on a busy machine). */
async function when(cond) {
    for (var end = Date.now() + 10000; ;) {
        var v = false;
        try { v = !!cond(); } catch (e) {}
        if (v || Date.now() > end) return v;
        await sleep(20);
    }
}
/* A folder where f's temporary file goes, so f's write fails; a temporary
 * file a killed patchd left goes first. */
function block(f) {
    try { fs.unlinkSync(f + '.tmp'); } catch (e) {}
    fs.mkdirSync(f + '.tmp');
}
function ino(f) { try { return fs.statSync(f).ino; } catch (e) { return null; } }

var passed = 0, total = 0;
function ok(name, cond, detail) {
    total++;
    if (cond) passed++;
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (cond || detail === undefined ? '' : ' (' + detail + ')') + '\n');
}

var A = 'a1b2c3d4-0000-4000-8000-000000000001', B = 'a1b2c3d4-0000-4000-8000-000000000002';

(async function () {
    await start();
    var first = fs.readFileSync(STATE, 'utf8');
    var r = await ask({ cmd: 'profile', id: A });
    await saved();
    ok('a profile without its own settings: the shared ones, nothing written', r.ok && r.id === A && r.own === false && r.changed === false &&
        same(r.settings, settings(1, 1, 70, 0, 0)) && fs.readFileSync(STATE, 'utf8') === first && !fs.existsSync(PROFILES));
    r = await ask({ cmd: 'profile', id: null });
    ok('no profile detected: the shared settings, nothing written', r.ok && r.id === null && r.own === false && r.changed === false &&
        !fs.existsSync(PROFILES));
    r = await ask({ cmd: 'profile-own', id: A, on: 1 });
    ok('a tick for a profile not in effect is refused', !r.ok && /not the driver profile in effect/.test(r.reason));
    await ask({ cmd: 'profile', id: A });
    r = await ask({ cmd: 'profile-own', id: A, on: 0 });
    await saved();
    ok('unticking a profile without its own settings changes nothing', r.ok && r.own === false && !r.changed && !fs.existsSync(PROFILES) &&
        fs.readFileSync(STATE, 'utf8') === first);

    /* Bounded ids, refused before anything is read or written. */
    var bad = ['x'.repeat(129), 'a/b', '../etc', '__proto__', 'a b', 'a\nb', 'ä', 42, {}, ['a']];
    var refused = [];
    for (var i = 0; i < bad.length; i++) refused.push(await ask({ cmd: 'profile-own', id: bad[i], on: 1 }));
    refused.push(await ask({ cmd: 'profile', id: '-a' }));
    refused.push(await ask({ cmd: 'profile-own', id: null, on: 1 }));
    refused.push(await ask({ cmd: 'profile-own', id: A, on: 2 }));
    await saved();
    ok('an id too long, with a character outside the set, not a string, or none: refused, nothing written',
        refused.every(function (x) { return x.ok === false && !!x.reason; }) && !fs.existsSync(PROFILES) && fs.readFileSync(STATE, 'utf8') === first,
        refused.map(function (x) { return x.ok; }).join());
    r = await ask({ cmd: 'profile', id: 'x'.repeat(128) });
    ok('an id of 128 characters is taken', r.ok);

    /* Tick: a copy of the settings in effect, map.state.conf as it was. */
    r = await ask({ cmd: 'profile', id: A });
    var t0 = Date.now();
    r = await ask({ cmd: 'profile-own', id: A, on: 1 });
    await sleep(SAVE_MS / 2);
    var early = fs.existsSync(PROFILES);
    ok('ticked: not written at once (the map settings\' delay)', !early || Date.now() - t0 >= SAVE_MS);
    ok('ticked: own, the settings in effect unchanged', r.ok && r.own === true && r.changed === false && same(r.settings, settings(1, 1, 70, 0, 0)));
    var pf;
    ok('ticked: profiles.json holds the shared settings, the profile\'s copy and the profile in effect',
        await when(function () { pf = profiles(); return same(pf, { shared: settings(1, 1, 70, 0, 0), own: { [A]: settings(1, 1, 70, 0, 0) }, active: A }); }),
        JSON.stringify(pf));
    ok('profiles.json readable by all (0644) under umask 077, no temporary file left',
        (fs.statSync(PROFILES).mode & 0o777) === 0o644 && !fs.existsSync(PROFILES + '.tmp'), (fs.statSync(PROFILES).mode & 0o777).toString(8));
    await saved();
    ok('ticked: map.state.conf as it was', fs.readFileSync(STATE, 'utf8') === first);

    /* A change while the profile has its own: its copy and map.state.conf. */
    r = await ask({ cmd: 'hud-size', hud_width_pct: 50 });
    ok('a change with own settings: in the profile\'s copy and in map.state.conf', r.ok && await when(function () {
        return state().hud_width_pct === 50 && profiles().own[A].hud_width_pct === 50 && profiles().shared.hud_width_pct === 70;
    }));
    r = await ask({ cmd: 'profile-own', id: B, on: 1 });
    ok('a tick for another profile than the one in effect: refused, nothing changed', !r.ok && /not the driver profile in effect/.test(r.reason) &&
        !(B in profiles().own));

    /* Switch to a profile without its own: the shared settings back, A's kept. */
    r = await ask({ cmd: 'profile', id: B });
    ok('another profile, without its own: the shared settings, changed', r.ok && r.own === false && r.changed === true &&
        same(r.settings, settings(1, 1, 70, 0, 0)));
    ok('map.state.conf: the shared settings, every key', await when(function () { return same(state(), settings(1, 1, 70, 0, 0)); }), JSON.stringify(state()));
    ok('profiles.json: A\'s own settings with its change, the shared ones in effect', await when(function () {
        pf = profiles();
        return same(pf.own[A], settings(1, 1, 50, 0, 0)) && pf.active === null;
    }), JSON.stringify(pf));
    ok('the local conf untouched', fs.readFileSync(CONF, 'utf8') === LOCAL);

    /* B changes the shared settings; A's stay A's. */
    await ask({ cmd: 'hud-pos', hud_x: 40, hud_y: -20 });
    await ask({ cmd: 'cluster', on: 0 });
    await when(function () { return state().cluster === 0 && profiles().shared.cluster === 0; });
    r = await ask({ cmd: 'profile', id: A });
    ok('back to A: its own settings, changed', r.ok && r.own === true && r.changed === true && same(r.settings, settings(1, 1, 50, 0, 0)));
    ok('map.state.conf: A\'s; profiles.json: the shared settings with B\'s changes', await when(function () {
        pf = profiles();
        return same(state(), settings(1, 1, 50, 0, 0)) && same(pf.shared, settings(0, 1, 70, 40, -20)) && pf.active === A;
    }), JSON.stringify(pf));
    r = await ask({ cmd: 'profile', id: A });
    ok('the same profile again: nothing changes', r.ok && r.own === true && r.changed === false);

    /* The format a patchd and render without profiles read. */
    var lines = fs.readFileSync(STATE, 'utf8').split('\n').filter(Boolean);
    ok('map.state.conf: only "key = whole number" lines of the six map keys (0.9.x reads them, leaving out hud_keep_shown)',
        lines.length === 6 && lines.every(function (l) {
            return /^(cluster|hud|hud_width_pct|hud_offset_x|hud_offset_y|hud_keep_shown) = -?\d+$/.test(l);
        }), lines.join('|'));

    /* Untick: the shared settings back, the copy gone. */
    r = await ask({ cmd: 'profile-own', id: A, on: 0 });
    ok('unticked: the shared settings, changed', r.ok && r.own === false && r.changed === true && same(r.settings, settings(0, 1, 70, 40, -20)));
    ok('unticked: map.state.conf the shared settings; profiles.json without profiles', await when(function () {
        return same(state(), settings(0, 1, 70, 40, -20)) && same(profiles(), { shared: null, own: {}, active: null });
    }), JSON.stringify(profiles()));

    /* A burst of switches, sent at once: one write, after the last. */
    await ask({ cmd: 'profile-own', id: A, on: 1 });
    await ask({ cmd: 'hud-size', hud_width_pct: 40 });
    await when(function () { return state().hud_width_pct === 40 && profiles().own[A].hud_width_pct === 40; });
    await saved();
    var pIno = ino(PROFILES);
    var sIno = ino(STATE), writes = out.split('(saved for the next start)').length;
    t0 = Date.now();
    var burst = await askAll([{ cmd: 'profile', id: B }, { cmd: 'profile', id: A }, { cmd: 'profile', id: B }]);
    await sleep(SAVE_MS / 2);
    early = ino(PROFILES) !== pIno || ino(STATE) !== sIno;
    ok('a burst of switches: not written at once (the map settings\' delay)', !early || Date.now() - t0 >= SAVE_MS);
    ok('a burst of switches: each taken', burst.every(function (x) { return x.ok; }) && burst[1].own && !burst[2].own);
    ok('a burst of switches: written after the last, as it ends', await when(function () {
        pf = profiles();
        return ino(PROFILES) !== pIno && ino(STATE) !== sIno && pf.active === null && same(pf.own[A], settings(0, 1, 40, 40, -20)) &&
            same(state(), settings(0, 1, 70, 40, -20)) && out.split('(saved for the next start)').length - writes >= 1;
    }), JSON.stringify(pf));
    await saved();
    ok('a burst of switches: map.state.conf written once', out.split('(saved for the next start)').length - writes === 1);

    /* A stop writes what waits. */
    r = await ask({ cmd: 'profile', id: A });
    await stop();
    ok('stopped before the write was due: both written at the stop', same(state(), settings(0, 1, 40, 40, -20)) && profiles().active === A);

    /* Corrupt: the shared settings, a warning, patchd keeps running. */
    fs.writeFileSync(PROFILES, '{"own": {"' + A + '": {"cluster": 1');
    var before = fs.readFileSync(STATE, 'utf8');
    await start();
    r = await ask({ cmd: 'profile', id: A });
    await saved();
    ok('a corrupt profiles.json: a warning, the settings in effect are the shared ones, nothing written', r.ok && r.own === false &&
        r.changed === false && /WARNING: driver profiles: .* unreadable/.test(out) && fs.readFileSync(STATE, 'utf8') === before &&
        fs.readFileSync(PROFILES, 'utf8').indexOf('{"own"') === 0);
    r = await ask({ cmd: 'status' }).catch(function () { return null; });
    ok('patchd still answers', p.exitCode === null);
    r = await ask({ cmd: 'profile-own', id: A, on: 1 });
    ok('ticked again: a new profiles.json in place of the corrupt one', r.ok && r.own && await when(function () { return profiles().active === A; }));
    await stop();

    /* A profiles file of another shape, or with bad entries: those left out. */
    fs.writeFileSync(PROFILES, JSON.stringify({ shared: settings(1, 0, 0, 0, 0), active: 'bad/id',
        own: { 'bad/id': settings(1, 1, 60, 0, 0), [B]: { cluster: 1, hud: 1 }, [A]: settings(1, 1, 999, 0, 0) } }));
    await start();
    r = await ask({ cmd: 'profile', id: A });
    ok('entries with a bad id or bad settings left out with a warning: A has no own settings, the shared ones kept in effect',
        r.ok && r.own === false && /WARNING: driver profiles: 3 unreadable profile\(s\)/.test(out) && same(state(), settings(1, 0, 0, 0, 0)) &&
        profiles().shared === null);
    await stop();
    fs.writeFileSync(PROFILES, '[1,2]');
    await start();
    r = await ask({ cmd: 'profile', id: A });
    ok('a profiles.json of another shape: a warning, the shared settings', r.ok && r.own === false && /is not a profiles file/.test(out));
    await stop();

    /* Unreadable (a folder in its place): a warning, saving fails with a log line, patchd keeps running. */
    fs.unlinkSync(PROFILES);
    fs.mkdirSync(PROFILES);
    await start();
    r = await ask({ cmd: 'profile', id: B });
    ok('an unreadable profiles.json: a warning, the shared settings', r.ok && r.own === false && /unreadable \(EISDIR/.test(out));
    r = await ask({ cmd: 'profile-own', id: B, on: 1 });
    ok('saving into it fails with a log line; patchd keeps running', r.ok && await when(function () { return /saving the driver profiles failed/.test(out); }) &&
        p.exitCode === null);
    await stop();
    fs.rmdirSync(PROFILES);

    /* A write cut short: no profile's own settings lost or swapped. A and B
     * each with their own (60 % and 40 %). */
    fs.writeFileSync(STATE, 'hud = 1\n');
    await start();
    await ask({ cmd: 'profile', id: A });
    await ask({ cmd: 'profile-own', id: A, on: 1 });
    await ask({ cmd: 'hud-size', hud_width_pct: 60 });
    await ask({ cmd: 'profile', id: B });
    await ask({ cmd: 'profile-own', id: B, on: 1 });
    await ask({ cmd: 'hud-size', hud_width_pct: 40 });
    var both = function () { var f = profiles(); return f.own[A].hud_width_pct === 60 && f.own[B].hud_width_pct === 40; };
    ok('A and B with their own settings', await when(function () { return both() && state().hud_width_pct === 40; }));
    /* map.state.conf's write fails: profiles.json already right, map.state.conf written when tried again */
    block(STATE);
    r = await ask({ cmd: 'profile', id: A });
    ok('map.state.conf\'s write fails: profiles.json has A in effect and both sets, map.state.conf as it was',
        r.ok && r.changed && await when(function () { return /saving driver profile .* failed/.test(out); }) && profiles().active === A && both() &&
        state().hud_width_pct === 40);
    fs.rmdirSync(STATE + '.tmp');
    ok('tried again: map.state.conf A\'s', await when(function () { return state().hud_width_pct === 60 && both(); }));
    /* killed between the two writes */
    block(STATE);
    var failures = out.split('saving driver profile').length;
    await ask({ cmd: 'profile', id: B });
    await when(function () { return out.split('saving driver profile').length > failures; });
    await stop('SIGKILL');
    fs.rmdirSync(STATE + '.tmp');
    ok('killed between the writes: profiles.json has B in effect, map.state.conf still A\'s', profiles().active === B && state().hud_width_pct === 60 && both());
    await start();
    ok('at the start: map.state.conf written again from profiles.json (B\'s), both sets kept', state().hud_width_pct === 40 && both() &&
        /written again from/.test(out));
    r = await ask({ cmd: 'profile', id: B });
    ok('the first profile command after it: changed, so the render is moved', r.ok && r.changed && r.own && r.settings.hud_width_pct === 40);
    /* profiles.json's write fails: map.state.conf left too; killed then: the two still agree */
    block(PROFILES);
    failures = out.split('saving the driver profiles failed').length;
    r = await ask({ cmd: 'profile', id: A });
    var failed = await when(function () { return out.split('saving the driver profiles failed').length > failures; });
    await saved();
    ok('profiles.json\'s write fails: map.state.conf not written either', failed && state().hud_width_pct === 40 && profiles().active === B);
    await stop('SIGKILL');
    fs.rmdirSync(PROFILES + '.tmp');
    var n = out.split('written again from').length;
    await start();
    ok('killed then: started with B in effect, nothing to write again, both sets kept', out.split('written again from').length === n &&
        state().hud_width_pct === 40 && both());
    r = await ask({ cmd: 'profile', id: A });
    ok('A told again: its own settings', r.changed && await when(function () { return state().hud_width_pct === 60 && profiles().active === A && both(); }));
    await stop();
    fs.unlinkSync(PROFILES);

    /* The last own settings unticked, the write cut short: the shared
     * settings (the HUD alone, standard size) survive; A's were 60 %. */
    var SHARED = settings(0, 1, 0, 0, 0);
    async function aOwn() {
        fs.writeFileSync(STATE, 'cluster = 0\nhud = 1\n');
        try { fs.unlinkSync(PROFILES); } catch (e) {}
        await start();
        await ask({ cmd: 'profile', id: A });
        await ask({ cmd: 'profile-own', id: A, on: 1 });
        await ask({ cmd: 'hud-size', hud_width_pct: 60 });
        await when(function () { return state().hud_width_pct === 60 && profiles().own[A].hud_width_pct === 60; });
    }
    await aOwn();
    block(STATE);
    failures = out.split('saving driver profile').length;
    r = await ask({ cmd: 'profile-own', id: A, on: 0 });
    ok('the last untick, map.state.conf\'s write fails: profiles.json keeps the shared settings, none own, A\'s still in map.state.conf',
        r.ok && r.changed && await when(function () { return out.split('saving driver profile').length > failures; }) &&
        same(profiles(), { shared: SHARED, own: {}, active: null }) && state().hud_width_pct === 60);
    await stop('SIGKILL');
    fs.rmdirSync(STATE + '.tmp');
    await start();
    ok('restarted before it was tried again: map.state.conf the shared settings, then profiles.json without them',
        same(state(), SHARED) && same(profiles(), { shared: null, own: {}, active: null }));
    r = await ask({ cmd: 'profile', id: A });
    ok('the next profile command: the shared settings, changed (the render moved)', r.ok && !r.own && r.changed && same(r.settings, SHARED));
    await stop();

    await aOwn();
    block(STATE);
    failures = out.split('saving driver profile').length;
    await ask({ cmd: 'profile-own', id: A, on: 0 });
    await when(function () { return out.split('saving driver profile').length > failures; });
    fs.rmdirSync(STATE + '.tmp');
    ok('the last untick, map.state.conf\'s write fails and is tried again: the shared settings in it, profiles.json without them',
        await when(function () { return same(state(), SHARED) && same(profiles(), { shared: null, own: {}, active: null }); }));
    await stop();

    /* killed at each point of the last untick's writes */
    fs.writeFileSync(PROFILES, JSON.stringify({ shared: SHARED, own: {}, active: null }));
    fs.writeFileSync(STATE, 'cluster = 0\nhud = 1\nhud_width_pct = 60\n');
    await start();
    ok('killed after profiles.json, before map.state.conf: at the start the shared settings in map.state.conf',
        same(state(), SHARED) && same(profiles(), { shared: null, own: {}, active: null }));
    await stop();
    fs.writeFileSync(PROFILES, JSON.stringify({ shared: SHARED, own: {}, active: null }));
    var sharedText = 'cluster = 0\nhud = 1\nhud_width_pct = 0\nhud_offset_x = 0\nhud_offset_y = 0\n';
    fs.writeFileSync(STATE, sharedText);
    await start();
    ok('killed after map.state.conf, before profiles.json dropped them: map.state.conf as it was, profiles.json without them',
        fs.readFileSync(STATE, 'utf8') === sharedText && same(profiles(), { shared: null, own: {}, active: null }));
    await ask({ cmd: 'hud-size', hud_width_pct: 45 });
    await saved();
    await stop();
    await start();
    ok('a change after it is map.state.conf\'s alone, kept at the next start', state().hud_width_pct === 45);
    await stop();
    fs.unlinkSync(PROFILES);

    /* Off (the map on neither) as a profile's own and as the shared
     * settings: saved, switched to and from, kept at the next start, in the
     * lines an earlier patchd and render read. */
    var OFF = settings(0, 0, 0, 0, 0), ON = settings(1, 0, 0, 0, 0);
    /* map.state.conf's settings, in whatever order its lines are */
    var sameKeys = function (a, b) { return Object.keys(b).length === Object.keys(a).length && Object.keys(b).every(function (k) { return a[k] === b[k]; }); };
    fs.writeFileSync(STATE, 'cluster = 1\nhud = 0\nhud_width_pct = 0\nhud_offset_x = 0\nhud_offset_y = 0\nhud_keep_shown = 0\n');
    await start();
    await ask({ cmd: 'profile', id: A });
    await ask({ cmd: 'profile-own', id: A, on: 1 });
    r = await ask({ cmd: 'cluster', on: 0 });
    ok('Off as A\'s own: map.state.conf on neither, A\'s copy too, the shared settings on the cluster', r.ok && await when(function () {
        pf = profiles();
        return sameKeys(state(), OFF) && same(pf.own[A], OFF) && same(pf.shared, ON) && pf.active === A;
    }), JSON.stringify(pf));
    r = await ask({ cmd: 'profile', id: B });
    ok('B without its own: the shared settings, the map on the cluster, changed', r.ok && !r.own && r.changed && same(r.settings, ON) &&
        await when(function () { return sameKeys(state(), ON); }), JSON.stringify(r));
    r = await ask({ cmd: 'profile', id: A });
    ok('back to A: Off, changed (the render moved to neither)', r.ok && r.own && r.changed && same(r.settings, OFF) &&
        await when(function () { return sameKeys(state(), OFF); }), JSON.stringify(r));
    await stop();
    await start();
    r = await ask({ cmd: 'profile', id: A });
    ok('started again: A\'s Off kept in map.state.conf and profiles.json, nothing to move', r.ok && r.own && !r.changed && same(r.settings, OFF) &&
        sameKeys(state(), OFF) && same(profiles().own[A], OFF), JSON.stringify(r));
    r = await ask({ cmd: 'profile', id: B });
    await ask({ cmd: 'cluster', on: 0 });
    ok('Off as the shared settings: B on neither, A\'s own still Off', r.ok && !r.own && await when(function () {
        pf = profiles();
        return sameKeys(state(), OFF) && same(pf.shared, OFF) && same(pf.own[A], OFF) && pf.active === null;
    }), JSON.stringify(pf));
    await ask({ cmd: 'profile', id: A });
    r = await ask({ cmd: 'profile-own', id: A, on: 0 });
    ok('A unticked: the shared Off, profiles.json without profiles', r.ok && !r.own && !r.changed && await when(function () {
        return sameKeys(state(), OFF) && same(profiles(), { shared: null, own: {}, active: null });
    }), JSON.stringify(profiles()));
    await stop();
    await start();
    r = await ask({ cmd: 'profile', id: B });
    lines = fs.readFileSync(STATE, 'utf8').split('\n').filter(Boolean);
    ok('started again: the shared Off kept, map.state.conf in the lines an earlier version reads', r.ok && !r.changed && same(r.settings, OFF) &&
        lines.indexOf('cluster = 0') >= 0 && lines.indexOf('hud = 0') >= 0 &&
        lines.every(function (l) { return /^(cluster|hud|hud_width_pct|hud_offset_x|hud_offset_y|hud_keep_shown) = -?\d+$/.test(l); }),
        lines.join('|'));
    r = await ask({ cmd: 'cluster', on: 1 });
    ok('the cluster again from Off: saved', r.ok && await when(function () { return sameKeys(state(), ON); }), JSON.stringify(state()));
    await stop();
    fs.unlinkSync(PROFILES);

    /* At most 16 profiles with their own settings. */
    await start();
    var many = [];
    for (i = 0; i < 17; i++) {
        await ask({ cmd: 'profile', id: 'p' + i });
        many.push(await ask({ cmd: 'profile-own', id: 'p' + i, on: 1 }));
    }
    ok('16 profiles take their own settings, the 17th is refused', many.slice(0, 16).every(function (x) { return x.ok && x.own; }) &&
        !many[16].ok && await when(function () { return Object.keys(profiles().own).length === 16; }));
    await stop();
    fs.unlinkSync(PROFILES);

    /* The HUD map kept shown over BMW's pop-ups, per profile; a profiles.json
     * saved before the setting: its sets taken, giving way. */
    var OLD = { cluster: 1, hud: 1, hud_width_pct: 50, hud_offset_x: 0, hud_offset_y: 0 };
    var OLD_SHARED = { cluster: 1, hud: 1, hud_width_pct: 0, hud_offset_x: 0, hud_offset_y: 0 };
    fs.writeFileSync(PROFILES, JSON.stringify({ shared: OLD_SHARED, own: { [A]: OLD }, active: A }));
    fs.writeFileSync(STATE, 'cluster = 1\nhud = 1\nhud_width_pct = 50\nhud_offset_x = 0\nhud_offset_y = 0\n');
    var mark = out.length;
    await start();
    r = await ask({ cmd: 'profile', id: A });
    ok('a profiles.json from before hud_keep_shown: A keeps its own settings, giving way', r.ok && r.own &&
        same(r.settings, settings(1, 1, 50, 0, 0, 0)) && !/WARNING: driver profiles/.test(out.slice(mark)), JSON.stringify(r));
    r = await ask({ cmd: 'hud-keep-shown', on: 1 });
    ok('kept shown as A\'s own: in A\'s copy and map.state.conf', r.ok && r.hud_keep_shown === 1 && await when(function () {
        return state().hud_keep_shown === 1 && profiles().own[A].hud_keep_shown === 1;
    }), JSON.stringify(profiles()));
    r = await ask({ cmd: 'profile', id: B });
    ok('B without its own: the shared settings, giving way, changed', r.ok && !r.own && r.changed && same(r.settings, settings(1, 1, 0, 0, 0, 0)) &&
        await when(function () { return state().hud_keep_shown === 0; }), JSON.stringify(r));
    r = await ask({ cmd: 'profile', id: A });
    ok('back to A: kept shown, changed', r.ok && r.own && r.changed && r.settings.hud_keep_shown === 1 &&
        await when(function () { return state().hud_keep_shown === 1; }), JSON.stringify(r));
})().catch(function (e) {
    ok('no error: ' + e.message, false);
}).then(function () {
    if (p) p.kill();
    try { fs.rmSync ? fs.rmSync(dir, { recursive: true, force: true }) : null; } catch (e) {}
    process.stdout.write(passed + '/' + total + ' passed\n');
    if (passed !== total) { process.stdout.write(out); process.exitCode = 1; }
});
