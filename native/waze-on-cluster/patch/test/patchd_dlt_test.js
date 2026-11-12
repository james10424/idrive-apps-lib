#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Proves patchd's dlt-receive health ("dlt" in the logs reply) against a fake
 * dlt-receive: a persistent connect failure (counted, bounded stderr, one log
 * line per streak), DLT data arriving (state, counters, the client's own
 * "~DLT~" lines kept apart from DLT data, routing into the rings), a run that
 * ends after data (per-run state reset), and a missing binary. The capture's
 * state is asked each second while a client is connected, or a capture opens
 * or is open, also through a failed status, and not otherwise. Runs the real
 * patchd.js on a temporary socket with a short retry delay. Every patchd it
 * starts is stopped when it exits, also on a failure or exception; it checks
 * that by running itself with an injected fault.
 *
 *   node patchd_dlt_test.js
 */
'use strict';

require('./head_unit_env.js');   /* a HIGH head unit for every patcher and patchd this runs */

var fs = require('fs');
var os = require('os');
var net = require('net');
var path = require('path');
var cp = require('child_process');

var PATCHD = path.join(__dirname, '..', 'patchd.js');
var RETRY_MS = 100;
var FAULT = process.env.PATCHD_DLT_TEST_FAULT;   /* early | late | uncaught: the cleanup check's run */

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-dlt-'));

/* However the test ends (passing, failing, an exception, an uncaught one), every
 * patchd it started is SIGKILLed and the directory removed, which ends any fake
 * dlt-receive a patchd left running. */
var patchds = [];
process.on('exit', function () {
    patchds.forEach(function (c) { try { c.kill('SIGKILL'); } catch (e) {} });
    try { cp.execFileSync('rm', ['-rf', dir]); } catch (e) {}
});

var MODE = path.join(dir, 'mode');
function setMode(m) { fs.writeFileSync(MODE, m); }

/* The fake dlt-receive. What a start does comes from the mode file:
 *   refuse  its logger's connect error on stdout, 3 KB of stderr, exit 1
 *   stream  a "~DLT~" client line, then three DLT lines (a carplay marker, an
 *           AVB stream line, an unrelated one); SIGTERMs itself on a mode change
 *   hold    nothing (connecting, or connected and silent); exit 3 on a change
 * Any start exits once the mode is quit or the file is gone. */
var FAKE = path.join(dir, 'dlt-receive');
fs.writeFileSync(FAKE, '#!' + process.execPath + '\n' + [
    "var fs = require('fs');",
    "var MODE = " + JSON.stringify(MODE) + ";",
    "function mode() { try { return fs.readFileSync(MODE, 'utf8'); } catch (e) { return 'quit'; } }",
    "var m = mode();",
    "if (m === 'refuse') {",
    "    process.stdout.write('[ 1234.567890]~DLT~  321~ERROR    ~dlt_client_connect: ERROR: failed to connect! Connection refused\\n');",
    "    process.stderr.write(new Array(3001).join('x') + '\\nconnect: Connection refused\\n');",
    "    process.exitCode = 1;",
    "} else if (m === 'stream' || m === 'hold') {",
    "    if (m === 'stream') {",
    "        process.stdout.write('[ 1234.600000]~DLT~  321~INFO     ~dlt_client_connect: connected\\n');",
    "        process.stdout.write('2026/10/25 12:00:00.000000   1 000 ECU1 CPAA CPAA log info V 1 [Adding display: 2]\\n');",
    "        process.stdout.write('2026/10/25 12:00:00.000001   2 000 ECU1 AVBH AVBH log info V 1 [stream 0x301a9ee108e TalkerActive]\\n');",
    "        process.stdout.write('2026/10/25 12:00:00.000002   3 000 ECU1 SYS0 SYS0 log info V 1 [unrelated]\\n');",
    "    }",
    "    var until = Date.now() + 30000;",
    "    var t = setInterval(function () {",
    "        var now = mode();",
    "        if (now === m && Date.now() < until) return;",
    "        clearInterval(t);",
    "        if (m === 'stream' && now !== 'quit') process.kill(process.pid, 'SIGTERM');",
    "        else process.exitCode = 3;",
    "    }, 20);",
    "}",
].join('\n') + '\n');
fs.chmodSync(FAKE, 0o755);

/* A journalctl that ends at once, so the journal followers stay quiet. */
var BIN = path.join(dir, 'bin');
fs.mkdirSync(BIN);
fs.writeFileSync(path.join(BIN, 'journalctl'), '#!/bin/sh\nexit 0\n');
fs.chmodSync(path.join(BIN, 'journalctl'), 0o755);

function startPatchd(name, dltReceive, extraEnv) {
    var sock = path.join(dir, name, 'patch.sock');
    var env = Object.assign({}, process.env, {
        PATH: BIN + path.delimiter + process.env.PATH,
        WOC_PATCH_SOCK: sock,
        WOC_CARPLAY: path.join(dir, 'no-carplay'),
        WOC_DLT_RECEIVE: dltReceive,
        WOC_DLT_RETRY_MS: String(RETRY_MS),
        WOC_DLT_RETRY_MAX_MS: String(RETRY_MS),
    }, extraEnv || {});
    var child = cp.spawn(process.execPath, [PATCHD], { env: env, stdio: ['ignore', 'ignore', 'inherit'] });
    patchds.push(child);
    if (FAULT) process.stdout.write('patchd pid ' + child.pid + '\n');
    return { sock: sock, child: child };
}

/* One {"cmd":"logs"} round trip; resolves to the reply, or null if patchd is
 * not listening yet. A line that is not JSON rejects. */
function logs(p) {
    return new Promise(function (resolve, reject) {
        var c = net.connect(p.sock);
        var buf = '';
        var done = false;
        function end(err, v) {
            if (done) return;
            done = true;
            c.destroy();
            if (err) reject(err); else resolve(v);
        }
        c.setEncoding('utf8');
        c.on('connect', function () { c.write('{"cmd":"logs"}\n'); });
        c.on('data', function (d) {
            buf += d;
            var i;
            while ((i = buf.indexOf('\n')) >= 0) {
                var line = buf.slice(0, i);
                buf = buf.slice(i + 1);
                var m;
                try { m = JSON.parse(line); } catch (e) { end(e); return; }
                if (m.ev === 'logs') { end(null, m); return; }
            }
        });
        c.on('error', function () { end(null, null); });
        c.on('close', function () { end(null, null); });
    });
}

function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

/* Poll the logs reply until pred(reply) holds; resolves to the last reply. */
async function waitFor(p, what, pred) {
    var r = null;
    var until = Date.now() + 15000;
    while (Date.now() < until) {
        r = await logs(p);
        if (r && pred(r)) return r;
        await sleep(30);
    }
    ok('reached: ' + what, false, JSON.stringify(r && r.dlt));
    return r;
}

function count(lines, re) { return lines.filter(function (l) { return re.test(l); }).length; }

/* Runs this test with an injected fault. Resolves to its exit code (or signal),
 * its output, and whether its stdout and stderr closed within 5 s of its exit:
 * a patchd that outlives it holds that stderr open, and is then killed here by
 * the pid the run printed. */
function faultRun(fault) {
    return new Promise(function (resolve) {
        var env = Object.assign({}, process.env, { PATCHD_DLT_TEST_FAULT: fault });
        var c = cp.spawn(process.execPath, [__filename], { env: env, stdio: ['ignore', 'pipe', 'pipe'] });
        var out = '';
        var code = null;
        var closeTimer = null;
        var hangTimer = setTimeout(function () { c.kill('SIGKILL'); }, 30000);
        function collect(d) { out += d; }
        c.stdout.on('data', collect);
        c.stderr.on('data', collect);
        c.on('exit', function (exitCode, signal) {
            code = exitCode === null ? signal : exitCode;
            clearTimeout(hangTimer);
            closeTimer = setTimeout(function () {
                var re = /patchd pid (\d+)/g, m;
                while ((m = re.exec(out))) { try { process.kill(+m[1], 'SIGKILL'); } catch (e) {} }
                c.stdout.destroy();
                c.stderr.destroy();
                resolve({ code: code, out: out, closed: false });
            }, 5000);
        });
        c.on('close', function () {
            clearTimeout(closeTimer);
            resolve({ code: code, out: out, closed: true });
        });
    });
}

/* A fake capture service: answers each command with the state set, and
 * counts the status requests; with delay, the next status is answered that
 * many ms late with the state it had when asked. */
function fakeCapture(sock) {
    var f = { state: 'closed', statuses: 0, fail: 0, delay: 0 };
    f.server = net.createServer(function (c) {
        var buf = '';
        c.on('error', function () {});
        c.on('data', function (d) {
            buf += d;
            if (buf.indexOf('\n') < 0) return;
            var cmd = JSON.parse(buf.slice(0, buf.indexOf('\n'))).cmd;
            if (cmd === 'status') f.statuses++;
            if (cmd === 'status' && f.fail > 0) { f.fail--; c.destroy(); return; }
            if (cmd === 'start') f.state = 'opening';
            var reply = JSON.stringify({ state: f.state }) + '\n';
            if (cmd === 'status' && f.delay) {
                var ms = f.delay;
                f.delay = 0;
                f.held = true;
                setTimeout(function () { f.held = false; c.end(reply); }, ms);
                return;
            }
            c.end(reply);
        });
    });
    f.server.listen(sock);
    return f;
}

/* A client on patchd's socket; resolves once connected, keeps its events. */
function client(p) {
    return new Promise(function (resolve) {
        var c = net.connect(p.sock), events = [];
        c.setEncoding('utf8');
        c.on('data', function (d) { d.split('\n').filter(Boolean).forEach(function (l) { events.push(JSON.parse(l)); }); });
        c.on('connect', function () { resolve({ c: c, events: events }); });
        c.on('error', function () { resolve(null); });
    });
}

async function capturePolls() {
    var fake = fakeCapture(path.join(dir, 'capture.sock'));
    var p = startPatchd('e', path.join(dir, 'no-such-dlt-receive'), { WOC_CAPTURE_SOCK: path.join(dir, 'capture.sock') });
    for (var i = 0; i < 200 && !fs.existsSync(p.sock); i++) await sleep(25);
    await sleep(2200);
    ok('capture: asked once at start, then not while no client is connected and it is closed', fake.statuses === 1, fake.statuses);
    var c = await client(p);
    fake.statuses = 0;
    await sleep(2200);
    ok('capture: asked each second while a client is connected', fake.statuses >= 2, fake.statuses);
    ok('capture: and the client told', c.events.filter(function (e) { return e.ev === 'capture'; }).length >= 2);
    c.c.write('{"cmd":"capture-start"}\n');
    await sleep(300);
    c.c.destroy();
    fake.state = 'open';
    fake.statuses = 0;
    await sleep(2200);
    ok('capture: asked each second while it opens and is open, with no client', fake.statuses >= 2, fake.statuses);
    fake.fail = 1;
    await sleep(1500);
    fake.statuses = 0;
    await sleep(2200);
    ok('capture: a status that fails while it is open keeps the asking going', fake.fail === 0 && fake.statuses >= 2, fake.statuses);
    fake.state = 'closed';
    await sleep(1500);
    fake.statuses = 0;
    await sleep(2200);
    ok('capture: closed again, no longer asked', fake.statuses === 0, fake.statuses);
    c = await client(p);
    fake.delay = 800;
    for (i = 0; i < 100 && !fake.held; i++) await sleep(20);
    c.c.write('{"cmd":"capture-start"}\n');
    await sleep(150);
    c.c.destroy();
    for (i = 0; i < 100 && fake.held; i++) await sleep(20);
    fake.state = 'open';
    fake.statuses = 0;
    await sleep(2200);
    ok('capture: a status asked before a start and answered after it does not stop the asking', fake.statuses >= 2, fake.statuses);
    fake.state = 'closed';
    p.child.kill('SIGTERM');
    fake.server.close();
}

async function main() {
    /* The fault runs are separate processes with their own directories:
     * they run alongside the cases below, and end before the backoff case,
     * whose timing runs with nothing else in parallel. */
    var faultRuns = FAULT ? null : ['early', 'late', 'uncaught'].map(faultRun);
    var p = startPatchd('a', FAKE);
    if (FAULT === 'early') throw new Error('injected early fault');
    var r;

    /* A dlt-receive that cannot connect, start after start. */
    setMode('refuse');
    r = await waitFor(p, 'five failed starts', function (r) { return r.dlt.failed_runs >= 5; });
    if (FAULT === 'late') throw new Error('injected late fault');
    if (FAULT === 'uncaught') await new Promise(function () { setTimeout(function () { throw new Error('injected uncaught fault'); }, 0); });
    var x = r.dlt.last_exit;
    ok('reply carries patchd\'s clock', typeof r.now === 'number' && r.now >= x.ended);
    ok('failing: not receiving', r.dlt.state === 'starting' || r.dlt.state === 'retrying', r.dlt.state);
    ok('failing: runs counted', r.dlt.runs >= 5, r.dlt.runs);
    ok('failing: last exit code and no data', x.code === 1 && x.signal === null && x.lines === 0 && x.first_data === null, JSON.stringify(x));
    ok('failing: client messages bounded to 1 KB', x.messages.length <= 1024, x.messages.length);
    ok('failing: client messages keep the tail', /Connection refused\n$/.test(x.messages), JSON.stringify(x.messages.slice(-80)));
    ok('failing: the client\'s "~DLT~" line is not DLT data', r.carplay.length === 0 && r.avb.length === 0);
    ok('failing: one log line for the streak, not one per retry',
        count(r.patchd, /no DLT data from 3 starts in a row \(last: exit code 1: .*Connection refused\)/) === 1, r.patchd.join('\n'));
    ok('failing: no receiving line', count(r.patchd, /receiving DLT/) === 0);

    /* The connection comes up and DLT flows. */
    setMode('stream');
    r = await waitFor(p, 'three DLT lines', function (r) { return r.dlt.run && r.dlt.run.lines === 3; });
    var run = r.dlt.run;
    ok('receiving: state', r.dlt.state === 'receiving', r.dlt.state);
    ok('receiving: failure streak reset', r.dlt.failed_runs === 0, r.dlt.failed_runs);
    ok('receiving: first and last data times', run.first_data >= run.started && run.last_data >= run.first_data, JSON.stringify(run));
    ok('receiving: client line kept as a message', /dlt_client_connect: connected/.test(run.messages), run.messages);
    ok('receiving: carplay marker routed', r.carplay.length === 1 && r.carplay[0].marker && r.carplay[0].msg.indexOf('DLT ') === 0 &&
        r.carplay[0].msg.indexOf('Adding display: 2') > 0, JSON.stringify(r.carplay));
    ok('receiving: marker also in the marker ring', r.markers.length === 1 && r.markers[0].msg === r.carplay[0].msg, JSON.stringify(r.markers));
    ok('receiving: AVB stream line routed', r.avb.length === 1 && r.avb[0].msg.indexOf('301a9ee') > 0, JSON.stringify(r.avb));
    ok('receiving: one recovery log line', count(r.patchd, /dlt-receive: receiving DLT after \d+ failed starts/) === 1, r.patchd.join('\n'));
    var runsAtStream = r.dlt.runs;

    /* The receiving run ends (killed); the next one connects silently. */
    setMode('hold');
    r = await waitFor(p, 'the next run after the stream', function (r) {
        return r.dlt.runs > runsAtStream && r.dlt.state === 'starting';
    });
    x = r.dlt.last_exit;
    ok('ended: last exit is the receiving run', x.signal === 'SIGTERM' && x.code === null && x.lines === 3 && x.first_data !== null, JSON.stringify(x));
    ok('ended: the new run starts empty', r.dlt.run.lines === 0 && r.dlt.run.first_data === null && r.dlt.run.last_data === null &&
        r.dlt.run.messages === '', JSON.stringify(r.dlt.run));
    ok('ended: a run with data is not a failure', r.dlt.failed_runs === 0, r.dlt.failed_runs);
    ok('ended: no log line for it', count(r.patchd, /dlt-receive/) === 2, r.patchd.join('\n'));

    /* It fails again: a new streak gets its own line. */
    setMode('refuse');
    r = await waitFor(p, 'a second streak', function (r) { return r.dlt.failed_runs >= 4; });
    ok('second streak: logged once more', count(r.patchd, /no DLT data from 3 starts in a row/) === 2, r.patchd.join('\n'));
    ok('second streak: every run since the stream counted, the silent one too',
        r.dlt.failed_runs === r.dlt.runs - runsAtStream - (r.dlt.run ? 1 : 0), JSON.stringify(r.dlt));

    setMode('quit');
    await sleep(100);
    p.child.kill('SIGTERM');

    /* No dlt-receive on the unit. */
    var q = startPatchd('b', path.join(dir, 'no-such-dlt-receive'));
    r = await waitFor(q, 'four missing-binary starts', function (r) { return r.dlt.failed_runs >= 4; });
    x = r.dlt.last_exit;
    ok('missing: spawn error recorded', /ENOENT/.test(x.error) && x.lines === 0, JSON.stringify(x));
    ok('missing: not receiving', r.dlt.state !== 'receiving', r.dlt.state);
    ok('missing: the spawn error logged once', count(r.patchd, /DLT markers unavailable/) === 1, r.patchd.join('\n'));
    ok('missing: the streak logged once', count(r.patchd, /no DLT data from 3 starts in a row \(last: spawn .*ENOENT\)/) === 1, r.patchd.join('\n'));
    q.child.kill('SIGTERM');

    /* The respawn delay backs off over consecutive failed starts and stays
     * capped, so a DLT daemon that never accepts a local client is not
     * respawned at a fixed short interval forever. A base well above a start's
     * own duration keeps the delay the dominant term in each run-to-run gap. */
    var faultResults = await Promise.all(faultRuns);
    var BK_BASE = 200, BK_CAP = 800;
    var b = startPatchd('c', FAKE, { WOC_DLT_RETRY_MS: String(BK_BASE), WOC_DLT_RETRY_MAX_MS: String(BK_CAP) });
    setMode('refuse');
    var starts = [];
    var until = Date.now() + 10000;
    while (starts.length < 6 && Date.now() < until) {
        r = await logs(b);
        if (r && r.dlt.last_exit && starts[starts.length - 1] !== r.dlt.last_exit.started) starts.push(r.dlt.last_exit.started);
        await sleep(15);
    }
    ok('backoff: enough failed starts observed', starts.length >= 5, starts.length);
    var gaps = [];
    for (var g = 1; g < starts.length; g++) gaps.push(starts[g] - starts[g - 1]);
    ok('backoff: the delay grows with the streak', gaps.length >= 3 && gaps[1] > gaps[0] + 50 && gaps[2] > gaps[1] + 50, JSON.stringify(gaps));
    ok('backoff: the delay stays capped', Math.max.apply(null, gaps) <= BK_CAP + 400, JSON.stringify(gaps));
    b.child.kill('SIGTERM');

    await capturePolls();

    /* A run that fails before patchd answers, after, or with an uncaught
     * exception still reports the failure and leaves no patchd behind. */
    var faults = ['early', 'late', 'uncaught'];
    for (var i = 0; i < faults.length; i++) {
        var f = faults[i];
        var res = faultResults[i];
        ok('cleanup, ' + f + ' fault: the run fails with it', res.code !== 0 && res.out.indexOf('injected ' + f + ' fault') >= 0, res.code + '\n' + res.out);
        ok('cleanup, ' + f + ' fault: no patchd outlives the run', res.closed, res.out);
    }
}

main().catch(function (e) { ok('no exception', false, e.stack); }).then(function () {
    process.stdout.write(fails ? fails + ' FAILED\n' : 'all passed\n');
    process.exit(fails ? 1 : 0);
});
