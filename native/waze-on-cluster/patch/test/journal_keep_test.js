// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/* journal_keep.js: the files' caps, the rate limit that keeps 10 minutes in
 * them under a flood, the lines it writes, and a restart resuming after the
 * newest line with a fake journalctl. */
var assert = require('assert'), fs = require('fs'), os = require('os'), path = require('path'), cp = require('child_process');
var jk = require('../journal_keep.js');

var tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wjk-'));
function fresh(name) { var d = path.join(tmp, name); fs.mkdirSync(d); return d; }
function files(dir) { return fs.readdirSync(dir).filter(function (n) { return /^journal\.log/.test(n); }).sort(); }
function line(n, len) { var s = 'line ' + n + ' '; return Buffer.from(s + 'x'.repeat(len - s.length - 1) + '\n'); }
function delay(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
async function until(what, fn) {
    for (var i = 0; i < 300; i++) { if (fn()) return; await delay(20); }
    throw new Error('timed out waiting for ' + what);
}

/* rotation: never more than `files` files, none past fileMax, oldest dropped */
(function () {
    var dir = fresh('caps'), ring = new jk.Ring(dir, { files: 4, fileMax: 1000 });
    for (var i = 0; i < 200; i++) ring.append([line(i, 90)]);
    assert.deepStrictEqual(files(dir), ['journal.log', 'journal.log.1', 'journal.log.2', 'journal.log.3']);
    files(dir).forEach(function (n) { assert(fs.statSync(path.join(dir, n)).size <= 1000, n); });
    var all = [3, 2, 1, 0].map(function (i) { return fs.readFileSync(ring.name(i), 'utf8'); }).join('');
    var nums = all.split('\n').filter(Boolean).map(function (l) { return Number(l.split(' ')[1]); });
    assert.strictEqual(nums[nums.length - 1], 199, 'the newest line is last in journal.log');
    nums.forEach(function (n, k) { if (k) assert.strictEqual(n, nums[k - 1] + 1, 'contiguous, oldest first'); });
    /* a batch rotates between its lines */
    ring.append([200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211].map(function (n) { return line(n, 90); }));
    files(dir).forEach(function (n) { assert(fs.statSync(path.join(dir, n)).size <= 1000, n); });
    /* a stray rotation past the cap is dropped and an oversize file rotated */
    fs.writeFileSync(path.join(dir, 'journal.log.7'), 'old\n');
    fs.writeFileSync(path.join(dir, 'journal.log'), 'y'.repeat(1500));
    var again = new jk.Ring(dir, { files: 4, fileMax: 1000 });
    again.append([line(300, 90)]);
    assert.deepStrictEqual(files(dir), ['journal.log', 'journal.log.1', 'journal.log.2', 'journal.log.3']);
    assert.strictEqual(fs.statSync(again.name(1)).size, 1500, 'the oversize file moved to .1, not written to');
    console.log('ok   rotation: at most FILES files, none past FILE_MAX, oldest dropped, stray rotations removed');
})();

/* sync (the boot's first minutes): every write is followed by an
 * fdatasync of a read-only descriptor, the write's own closed by then; the
 * files only grow by appends, and the caps and rotation hold. */
(function () {
    var dir = fresh('sync'), ring = new jk.Ring(dir, { files: 4, fileMax: 1000 });
    var realOpen = fs.openSync, realSync = fs.fdatasyncSync, realClose = fs.closeSync, open = {}, events = [];
    fs.openSync = function (p, flags) {
        var fd = realOpen.apply(fs, arguments);
        open[fd] = flags;
        if (flags === 'a') events.push('write');
        return fd;
    };
    fs.closeSync = function (fd) { delete open[fd]; return realClose.apply(fs, arguments); };
    fs.fdatasyncSync = function (fd) {
        assert.strictEqual(open[fd], 'r', 'synced through a read-only descriptor');
        assert(!Object.keys(open).some(function (k) { return open[k] !== 'r'; }), 'no descriptor open for writing while it syncs');
        events.push('sync');
        return realSync.apply(fs, arguments);
    };
    try {
        ring.append([line(0, 90), line(1, 90)], true);
        var ino = fs.statSync(ring.name(0)).ino, before = fs.readFileSync(ring.name(0));
        ring.append([line(2, 90)], true);
        var after = fs.readFileSync(ring.name(0));
        assert.strictEqual(fs.statSync(ring.name(0)).ino, ino, 'the same file');
        assert(after.slice(0, before.length).equals(before) && after.length === before.length + 90, 'appended, nothing rewritten');
        for (var i = 3; i < 200; i += 3) ring.append([line(i, 90), line(i + 1, 90), line(i + 2, 90)], true);
        var plain = events.length;
        ring.append([line(300, 90)]);
    } finally {
        fs.openSync = realOpen; fs.fdatasyncSync = realSync; fs.closeSync = realClose;
    }
    var writes = events.slice(0, plain).filter(function (e) { return e === 'write'; }).length;
    assert(writes > 67, 'a batch that rotates is written in two');
    assert.strictEqual(events.slice(0, plain).join(' '), new Array(writes + 1).join('write sync ').trim(), 'each write synced before the next');
    assert.deepStrictEqual(events.slice(plain), ['write'], 'without sync: the write alone');
    assert.deepStrictEqual(files(dir), ['journal.log', 'journal.log.1', 'journal.log.2', 'journal.log.3']);
    files(dir).forEach(function (n) { assert(fs.statSync(path.join(dir, n)).size <= 1000, n); });
    console.log('ok   sync: each write fdatasynced through a read-only descriptor, appended only, the caps hold');
})();

/* The shipped caps under a flood: lines of LINE_MAX from both kinds, each
 * ten times its rate, for half an hour. The files stay within 10 files and
 * 4 MB and keep every line let through in the last 10 minutes. */
(function () {
    var dir = fresh('flood'), t = 0, now = function () { return t; };
    var ring = new jk.Ring(dir), limits = [new jk.Limiter(jk.RATE / 2, now), new jk.Limiter(jk.RATE / 2, now)];
    assert(jk.FILES <= 10 && jk.FILES * jk.FILE_MAX <= 4e6, 'caps: 10 files, 4 MB');
    var kept = [], perStep = Math.ceil(10 * jk.RATE / 2 / jk.LINE_MAX / 60), n = 0;
    for (t = 0; t <= 30 * 60000; t += 2000) {
        var batch = [];
        limits.forEach(function (limiter) {
            for (var k = 0; k < 2 * perStep; k++, n++) {
                var b = line(n, jk.LINE_MAX);
                if (limiter.take(b.length)) { batch.push(b); kept.push({ n: n, t: t }); }
            }
        });
        limits.forEach(function (limiter) {
            var s = limiter.summary(t * 1000, 'some');
            if (s) batch.push(s);
        });
        ring.append(batch);
    }
    assert(files(dir).length <= jk.FILES);
    var total = files(dir).reduce(function (a, f) { return a + fs.statSync(path.join(dir, f)).size; }, 0);
    assert(total <= jk.FILES * jk.FILE_MAX, 'total ' + total);
    var held = {};
    files(dir).forEach(function (f) {
        fs.readFileSync(path.join(dir, f), 'utf8').split('\n').forEach(function (l) {
            var m = /^line (\d+) /.exec(l);
            if (m) held[m[1]] = true;
        });
    });
    var recent = kept.filter(function (x) { return x.t >= 20 * 60000; });
    assert(recent.length > 0 && kept.length < n, 'the flood was limited');
    recent.forEach(function (x) { assert(held[x.n], 'line ' + x.n + ' of the last 10 minutes is held'); });
    assert(/dropped \d+ some lines \(\d+ KiB\) over the rate limit/.test(fs.readFileSync(path.join(dir, 'journal.log'), 'utf8')));
    console.log('ok   a 30-minute flood at 10x the rate: ' + files(dir).length + ' files, ' + total + ' B, the last 10 minutes held');
})();

/* the line format and journalctl's arguments */
(function () {
    var us = Date.UTC(2026, 10, 3, 6, 58, 54) * 1000 + 365447;
    assert.strictEqual(jk.format({ __REALTIME_TIMESTAMP: String(us), _SYSTEMD_UNIT: 'cluster-render.service',
        SYSLOG_IDENTIFIER: 'cluster-render', _PID: '1152', MESSAGE: 'two\nlines' }).line.toString(),
        '2026-11-03T06:58:54.365447Z cluster-render.service cluster-render[1152]: two lines\n');
    var k = jk.format({ __REALTIME_TIMESTAMP: String(us), _TRANSPORT: 'kernel', SYSLOG_IDENTIFIER: 'kernel', MESSAGE: [104, 105] });
    assert.strictEqual(k.line.toString(), '2026-11-03T06:58:54.365447Z kernel: hi\n');
    assert.strictEqual(k.tool, false);
    ['waze-on-cluster.service', 'waze-on-cluster-logs.service', 'cluster-render.service'].forEach(function (u) {
        assert.strictEqual(jk.format({ __REALTIME_TIMESTAMP: String(us), _SYSTEMD_UNIT: u, MESSAGE: 'x' }).tool, true, u);
        assert.strictEqual(jk.format({ __REALTIME_TIMESTAMP: String(us), _SYSTEMD_UNIT: 'init.scope', UNIT: u, MESSAGE: 'x' }).tool, true, u);
    });
    ['carplay.service', 'recmgr.service', 'crashhandler.service'].forEach(function (u) {
        assert.strictEqual(jk.format({ __REALTIME_TIMESTAMP: String(us), _SYSTEMD_UNIT: u, MESSAGE: 'x' }).tool, false, u);
    });
    assert.strictEqual(jk.format({ __REALTIME_TIMESTAMP: String(us), _SYSTEMD_UNIT: 'init.scope', UNIT: 'carplay.service',
        SYSLOG_IDENTIFIER: 'systemd', _PID: '1', MESSAGE: 'Started carplay.' }).line.toString(),
        '2026-11-03T06:58:54.365447Z carplay.service systemd[1]: Started carplay.\n');
    var long = jk.format({ __REALTIME_TIMESTAMP: String(us), MESSAGE: 'z'.repeat(5000) }).line;
    assert.strictEqual(long.length, jk.LINE_MAX);
    assert.strictEqual(jk.format({ MESSAGE: 'no time' }), null);

    var args = jk.journalArgs('s=1;i=2');
    assert(args.indexOf('-f') >= 0 && args.indexOf('--after-cursor=s=1;i=2') >= 0, args.join(' '));
    assert(jk.journalArgs(null).indexOf('-f') < 0, 'the read of this boot so far ends');
    assert(jk.journalArgs('').indexOf('--no-tail') >= 0, 'with no entry yet, the follow starts at the boot');
    assert(jk.journalArgs(null).concat(args).every(function (a) { return !/^--since|^--until/.test(a); }), 'no clock');
    var groups = args.slice(args.indexOf('_SYSTEMD_UNIT=' + jk.UNITS[0])).join(' ').split(' + ');
    assert.strictEqual(groups.length, 3, 'three alternatives: the units, systemd about them, the kernel');
    assert(/^(_SYSTEMD_UNIT=\S+ ?)+$/.test(groups[0]));
    assert(/^_PID=1( UNIT=\S+)+$/.test(groups[1]) && groups[1].indexOf('UNIT=waze-on-cluster-logs.socket') > 0 &&
        groups[1].indexOf('UNIT=waze-on-cluster-journal.timer') > 0 && groups[1].indexOf('UNIT=waze-on-cluster-logs-fw.service') > 0);
    assert(groups[0].indexOf('_SYSTEMD_UNIT=waze-on-cluster-logs-fw.service') >= 0);
    assert.strictEqual(groups[2], '_TRANSPORT=kernel');
    ['waze-on-cluster.service', 'waze-on-cluster-patchd.service', 'waze-on-cluster-boot.service',
        'waze-on-cluster-recovery.service', 'cluster-render.service', 'carplay.service', 'recmgr.service',
        'crashhandler.service'].forEach(function (u) { assert(jk.UNITS.indexOf(u) >= 0, u); });
    console.log('ok   lines: time, unit, identifier[pid]; kernel; systemd\'s about a unit; LINE_MAX; three match groups');
})();

/* The service with a stand-in journalctl: without -f it prints `backlog`
 * and ends, with -f it prints `live` and stays. */
(async function () {
    var dir = fresh('run'), argsLog = path.join(tmp, 'args'), fake = path.join(tmp, 'journalctl');
    var backlogFile = path.join(tmp, 'backlog'), liveFile = path.join(tmp, 'live'), codeFile = path.join(tmp, 'code');
    var t0 = Date.UTC(2026, 10, 3, 7) * 1000;
    function entry(i, extra) {
        return JSON.stringify(Object.assign({ __CURSOR: 'c' + i, __REALTIME_TIMESTAMP: String(t0 + i),
            _SYSTEMD_UNIT: 'waze-on-cluster.service', SYSLOG_IDENTIFIER: 'waze-on-cluster', _PID: '7', MESSAGE: 'm' + i }, extra || {}));
    }
    function put(file, list) { fs.writeFileSync(file, list.join('\n') + (list.length ? '\n' : '')); }
    fs.writeFileSync(fake, '#!/bin/sh\necho "$@" >> ' + argsLog + '\ncase " $* " in *" -f "*) cat ' + liveFile +
        '; exec sleep 30;; *) cat ' + backlogFile + '; exit $(cat ' + codeFile + ' 2>/dev/null || echo 0);; esac\n');
    fs.chmodSync(fake, 0o755);
    var env = Object.assign({}, process.env, { WOC_JOURNAL_DIR: dir, WOC_JOURNALCTL: fake, WOC_JOURNAL_FLUSH_MS: '50',
        WOC_JOURNAL_RESPAWN_MS: '100' });
    function run() { return cp.spawn(process.execPath, [path.join(__dirname, '..', 'journal_keep.js')], { env: env, stdio: 'ignore' }); }
    function all(d) {
        return fs.readdirSync(d || dir).filter(function (n) { return /^journal\.log/.test(n); }).sort().reverse()
            .map(function (f) {
                try { return fs.readFileSync(path.join(d || dir, f), 'utf8'); } catch (e) { return ''; } // rotated meanwhile
            }).join('').split('\n').filter(Boolean);
    }
    function msgs() { return all().map(function (l) { return l.replace(/^.*: /, ''); }); }
    function calls() { return fs.readFileSync(argsLog, 'utf8').trim().split('\n'); }
    async function stop(c) { var done = new Promise(function (r) { c.on('exit', r); }); c.kill('SIGTERM'); return done; }

    put(backlogFile, [1, 2, 3].map(function (i) { return entry(i); }));
    put(liveFile, [entry(4)]);
    var c = run();
    await until('four lines', function () { return msgs().length === 4; });
    assert(/^2026-11-03T07:00:00\.000001Z waze-on-cluster\.service waze-on-cluster\[7\]: m1$/.test(all()[0]), all()[0]);
    assert.strictEqual(await stop(c), 0, 'SIGTERM: a clean exit');
    assert.deepStrictEqual(calls().map(function (l) { return / -f /.test(' ' + l + ' ') ? l.split(' ').filter(function (a) { return /^--after-cursor/.test(a); })[0] : 'backlog'; }),
        ['backlog', '--after-cursor=c3'], 'this boot so far, then followed from its last cursor');
    console.log('ok   the service writes this boot so far, then follows from the last entry\'s cursor');

    /* At the shipped flush interval: this boot so far is on disk as soon as
     * it is read, a line logged later within FLUSH_MS of it, each appended
     * after what the file holds (the same file, its bytes unchanged). */
    var edir = fresh('early'), early = path.join(tmp, 'journalctl-early'), live2 = path.join(tmp, 'live2');
    put(backlogFile, [1, 2, 3].map(function (i) { return entry(i); }));
    put(liveFile, [entry(4)]);
    put(live2, [entry(5)]);
    fs.writeFileSync(early, '#!/bin/sh\ncase " $* " in *" -f "*) cat ' + liveFile + '; sleep 1; cat ' + live2 +
        '; exec sleep 30;; *) cat ' + backlogFile + ';; esac\n');
    fs.chmodSync(early, 0o755);
    var t = Date.now();
    c = cp.spawn(process.execPath, [path.join(__dirname, '..', 'journal_keep.js')], { stdio: 'ignore',
        env: Object.assign({}, process.env, { WOC_JOURNAL_DIR: edir, WOC_JOURNALCTL: early }) });
    await until('this boot so far', function () { return all(edir).length >= 3; });
    var backlogMs = Date.now() - t, file = path.join(edir, 'journal.log'), ino = fs.statSync(file).ino, first = fs.readFileSync(file);
    assert(backlogMs < 3000, 'this boot so far written ' + backlogMs + ' ms after the start');
    await until('the line logged a second later', function () { return all(edir).length === 5; });
    var liveMs = Date.now() - t, now = fs.readFileSync(file);
    await stop(c);
    assert(liveMs < 1000 + 2000 + 2000, 'the later line written ' + liveMs + ' ms after the start');
    assert.strictEqual(fs.statSync(file).ino, ino, 'the same file');
    assert(now.length > first.length && now.slice(0, first.length).equals(first), 'appended after what the file held');
    console.log('ok   at the shipped interval: this boot so far written ' + backlogMs + ' ms after the start, a later line ' +
        liveMs + ' ms, appended');

    /* a restart: the backlog holds what the files have and more; only the newer is written */
    put(backlogFile, [1, 2, 3, 4, 5, 6].map(function (i) { return entry(i); }));
    put(liveFile, []);
    c = run();
    await until('six lines', function () { return msgs().length === 6; });
    await delay(200);
    assert.deepStrictEqual(msgs(), ['m1', 'm2', 'm3', 'm4', 'm5', 'm6'], 'no line twice, none missed');
    await stop(c);

    /* a new boot whose clock is behind the files' newest line: all of it written */
    put(backlogFile, [entry(-3600000000 + 1, { MESSAGE: 'n1' }), entry(-3600000000 + 2, { MESSAGE: 'n2' })]);
    c = run();
    await until('the new boot', function () { return msgs().length === 8; });
    assert.deepStrictEqual(msgs().slice(-2), ['n1', 'n2']);
    await stop(c);
    console.log('ok   a restart writes only what follows the newest kept line; a boot behind the clock is written whole');

    /* a read of this boot that fails partway: what it read past the files'
     * newest line is written and followed from; before that line, nothing */
    var pdir = fresh('partial');
    env.WOC_JOURNAL_DIR = pdir;
    fs.writeFileSync(argsLog, '');
    fs.writeFileSync(codeFile, '1');
    put(backlogFile, [1, 2, 3].map(function (i) { return entry(i); }));
    put(liveFile, [entry(4)]);
    c = run();
    await until('partial read kept', function () { return all(pdir).length === 4; });
    await stop(c);
    assert.deepStrictEqual(all(pdir).map(function (l) { return l.replace(/^.*: /, ''); }), ['m1', 'm2', 'm3', 'm4']);
    assert(calls().some(function (l) { return / --after-cursor=c3/.test(' ' + l); }), 'followed from the last line read');
    fs.writeFileSync(argsLog, '');
    put(backlogFile, [1, 2].map(function (i) { return entry(i); }));
    c = run();
    await until('retried', function () { return calls().length >= 3; });
    await stop(c);
    assert.strictEqual(all(pdir).length, 4, 'a failed read short of the newest line writes nothing');
    assert(!calls().some(function (l) { return / -f /.test(' ' + l + ' '); }), 'and is read again, not followed');
    fs.writeFileSync(codeFile, '0');
    put(liveFile, []);
    console.log('ok   a read that fails partway: kept and followed from past the newest line, read again short of it');

    /* the backfill is not limited: a flood before the start is kept (its newest, within the caps) */
    var flood = [], fdir = fresh('backfill');
    for (var i = 1; i <= 2000; i++) flood.push(entry(1000 + i, { _SYSTEMD_UNIT: 'recmgr.service', SYSLOG_IDENTIFIER: 'recmgr', MESSAGE: 'r' + i + ' ' + 'x'.repeat(800) }));
    put(backlogFile, flood);
    env.WOC_JOURNAL_DIR = fdir;
    c = run();
    await until('backfill written', function () { return all(fdir).some(function (l) { return / r2000 /.test(l); }); });
    await stop(c);
    var kept = all(fdir);
    assert(!kept.some(function (l) { return /dropped/.test(l); }), 'nothing dropped from the backfill');
    assert(kept.length > 1500 && / r2000 /.test(kept[kept.length - 1]), kept.length + ' lines, the newest last');
    console.log('ok   the backfill of this boot is written whole, newest kept, nothing rate-dropped');

    /* a live kernel flood past its share: the tools' lines all kept, the kernel's cut and counted */
    var live = [], kdir = fresh('fair');
    for (i = 1; i <= 2000; i++) {
        live.push(entry(5000 + 2 * i, { _TRANSPORT: 'kernel', MESSAGE: 'k'.repeat(900) }));
        if (i % 100 === 0) live.push(entry(5001 + 2 * i, { MESSAGE: 'tool ' + i }));
    }
    put(backlogFile, []);
    put(liveFile, live);
    env.WOC_JOURNAL_DIR = kdir;
    c = run();
    await until('the flood summarised', function () { return all(kdir).some(function (l) { return /dropped \d+ other lines/.test(l); }); });
    await stop(c);
    var k = all(kdir);
    assert.strictEqual(k.filter(function (l) { return / tool \d+$/.test(l); }).length, 20, 'every tool line kept');
    assert(!k.some(function (l) { return /dropped \d+ tool lines/.test(l); }));
    var kernel = k.filter(function (l) { return / kernel: k+$/.test(l); }).length;
    assert(kernel > 0 && kernel < 2000, kernel + ' kernel lines kept');
    console.log('ok   a live kernel flood past its share is cut and counted; every tool line is kept');

    /* prepare: the folder, the group's, 2770 (as root only) */
    if (process.getuid && process.getuid() === 0) {
        var pdir = path.join(tmp, 'prepared');
        var r = cp.spawnSync(process.execPath, [path.join(__dirname, '..', 'journal_keep.js'), 'prepare'],
            { env: Object.assign({}, env, { WOC_JOURNAL_DIR: pdir }), encoding: 'utf8' });
        if (/no group systemd-journal/.test(r.stderr)) {
            console.log('ok   prepare: (no systemd-journal group here)');
        } else {
            assert.strictEqual(r.status, 0, r.stderr);
            assert.strictEqual(fs.statSync(pdir).mode & 0o7777, 0o2770);
            console.log('ok   prepare: the folder made, mode 2770');
        }
    }
    cp.execFileSync('rm', ['-rf', tmp]);
    console.log('journal_keep: all passed');
})().catch(function (e) { console.error('FAIL', e && e.stack); process.exit(1); });
