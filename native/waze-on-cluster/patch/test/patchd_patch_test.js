#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Proves patchd's patch contract against the unit's carplay binary (CARPLAY_BIN): the
 * status it reports (patched only with every edit present; a binary missing
 * the self-start or the resolution edit is unknown with a reason; the
 * advertised size and safe area) and how a
 * changed binary takes effect. With
 * carplay in both recmgr restart lists as loaded at boot and the tool's
 * carplay.service drop-in loaded by systemd: SIGKILL the main PID,
 * wait for it to go, systemctl start, confirm is-active (no main PID, a failed
 * start or a unit that stays inactive is an error). Without that (the stock
 * list, a list written after boot, or no drop-in): carplay is left running, the change waits
 * for the next head-unit start and status says so. Never systemctl
 * restart/stop. Runs the real patchd.js and carplay_patch.js on a temporary
 * copy, with a fake systemctl, mount, recmgr list and /proc/stat, and a dummy
 * process standing in for carplay. Without CARPLAY_BIN it
 * skips.
 *
 *   node patchd_patch_test.js
 */
'use strict';

var car = require('./head_unit_env.js');   /* a HIGH head unit, coded KI_KOMBI_VARIANT 7, for every patcher and patchd this runs */
var record = require('../patch_record.js');

var fs = require('fs');
var os = require('os');
var net = require('net');
var path = require('path');
var cp = require('child_process');

var PATCHD = path.join(__dirname, '..', 'patchd.js');
var SRC = process.env.CARPLAY_BIN || '';
var SLOT = 0x2ba4f0, SINK = 0x2a20b0, SINK_LEN = 59;
var UDP_SINK = 'udp://127.0.0.1:20557?mime_type=video/H264';

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}

if (!fs.existsSync(SRC)) { process.stdout.write('SKIP: set CARPLAY_BIN to a copy of the unit\'s /usr/bin/carplay (carplay 22.28.3)\n'); process.exit(0); }
var SRCBUF = fs.readFileSync(SRC);

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-patchd-'));
var children = [];
process.on('exit', function () {
    children.forEach(function (c) { try { c.kill('SIGKILL'); } catch (e) {} });
    try { cp.execFileSync('rm', ['-rf', dir]); } catch (e) {}
});

var TGT = path.join(dir, 'carplay');
var DROPIN = path.join(dir, 'carplay.service.d', '50-waze-on-cluster.conf');
var SYSTEMD = path.join(dir, 'systemd.json');   /* the fake unit: main pid, state, behaviour */
var CALLS = path.join(dir, 'systemctl.calls');
var MOUNTS = path.join(dir, 'mounts');
var RECMGR = path.join(dir, 'recmgr-restartlist.conf');
var PROC_STAT = path.join(dir, 'stat');
var BOOT_S = Math.floor(Date.now() / 1000) - 60;   /* this "boot" was a minute ago */
fs.writeFileSync(PROC_STAT, 'cpu  1 2 3\nbtime ' + BOOT_S + '\n');
/* The restart list; loadedAtBoot dates it before the boot (recmgr read it). */
function setRecmgr(names, loadedAtBoot) {
    fs.writeFileSync(RECMGR, '[Whitelist]\n' + names.join('\n') + '\n[WhitelistExitSuccess]\n' + names.join('\n') + '\n');
    if (loadedAtBoot) fs.utimesSync(RECMGR, BOOT_S - 3600, BOOT_S - 3600);
}
var BIN = path.join(dir, 'bin');
fs.mkdirSync(BIN);
fs.writeFileSync(CALLS, '');
fs.writeFileSync(MOUNTS, '/dev/root / ext4 ro 0 0\n');

/* The fake systemctl records each call (start also records whether the killed
 * PID is still alive). mode: ok | nopid (MainPID=0) | startfail | inactive
 * (start succeeds, the unit then reads failed); dropin: whether DropInPaths
 * names the tool's drop-in. restart/stop are recorded and
 * otherwise do nothing; the test asserts they never occur. */
/* carplay's process in a fake /proc: its exe holds the inode it runs. */
var PROC = path.join(dir, 'proc');
fs.mkdirSync(PROC);
function runs(file) {
    cp.execFileSync('rm', ['-rf', path.join(PROC, '4242')]);
    fs.mkdirSync(path.join(PROC, '4242'));
    fs.linkSync(file, path.join(PROC, '4242', 'exe'));
}
fs.writeFileSync(path.join(BIN, 'systemctl'), '#!' + process.execPath + '\n' + [
    "var cp = require('child_process'), path = require('path');",
    "var P = " + JSON.stringify(PROC) + ", T = " + JSON.stringify(TGT) + ";",
    "var fs = require('fs');",
    "var ST = " + JSON.stringify(SYSTEMD) + ";",
    "var st = JSON.parse(fs.readFileSync(ST, 'utf8'));",
    "var a = process.argv.slice(2);",
    "function alive(p) { try { process.kill(p, 0); return true; } catch (e) { return e.code !== 'ESRCH'; } }",
    "var line = a.join(' ');",
    "if (a[0] === 'start') line += ' killed-pid-alive=' + alive(st.pid);",
    "fs.appendFileSync(" + JSON.stringify(CALLS) + ", line + '\\n');",
    "function gone() { cp.execFileSync('rm', ['-rf', path.join(P, '4242')]); }",
    "if (a[0] === 'show' && a.indexOf('DropInPaths') >= 0) process.stdout.write('DropInPaths=/run/other.conf' + (st.dropin === false ? '' : ' ' + " + JSON.stringify(DROPIN) + ") + '\\n');",
    "else if (a[0] === 'show') { if (st.mode === 'nopid') gone(); var p = st.mode === 'nopid' ? 0 : st.pid; process.stdout.write('MainPID=' + p + '\\nInvocationID=inv-' + p + '\\nExecMainStartTimestampMonotonic=5\\n'); }",
    "else if (a[0] === 'start') {",
    "    if (st.mode === 'startfail') { gone(); process.stderr.write('Job for carplay.service failed.\\n'); process.exit(1); }",
    "    st.state = st.mode === 'inactive' ? 'failed' : 'active';",
    "    fs.writeFileSync(ST, JSON.stringify(st));",
    "    cp.execFileSync('rm', ['-rf', path.join(P, '4242')]); fs.mkdirSync(path.join(P, '4242')); fs.linkSync(T, path.join(P, '4242', 'exe'));",
    "} else if (a[0] === 'is-active') { process.stdout.write(st.state + '\\n'); process.exit(st.state === 'active' ? 0 : 3); }",
].join('\n') + '\n');
fs.chmodSync(path.join(BIN, 'systemctl'), 0o755);
fs.writeFileSync(path.join(BIN, 'journalctl'), '#!/bin/sh\nexit 0\n');
fs.chmodSync(path.join(BIN, 'journalctl'), 0o755);
/* A fake reboot that records the call instead of rebooting the test host. */
var REBOOTS = path.join(dir, 'reboot.calls');
fs.writeFileSync(REBOOTS, '');
var REBOOT = path.join(BIN, 'woc-reboot');
fs.writeFileSync(REBOOT, '#!/bin/sh\necho reboot >> "' + REBOOTS + '"\n');
fs.chmodSync(REBOOT, 0o755);
/* remount,ro fails while carplay runs a file no name links any more, as the
 * kernel refuses it. */
var MOUNT = path.join(dir, 'mount');
fs.writeFileSync(MOUNT, '#!' + process.execPath + '\n' + [
    "var fs = require('fs'), path = require('path');",
    "var a = process.argv.slice(2).join(' '), P = " + JSON.stringify(PROC) + ", M = " + JSON.stringify(MOUNTS) + ";",
    "if (/remount,rw/.test(a)) fs.writeFileSync(M, '/dev/root / ext4 rw 0 0\\n');",
    "else if (/remount,ro/.test(a)) {",
    "    var open = fs.readdirSync(P).some(function (p) { try { return fs.statSync(path.join(P, p, 'exe')).nlink <= 1; } catch (e) { return false; } });",
    "    if (open) { process.stderr.write('mount: / is busy\\n'); process.exit(32); }",
    "    fs.writeFileSync(M, '/dev/root / ext4 ro 0 0\\n');",
    "}",
].join('\n') + '\n');
fs.chmodSync(MOUNT, 0o755);

/* A dummy carplay main process; resolves .died to the signal that ended it. */
function dummyCarplay() {
    var c = cp.spawn(process.execPath, ['-e', 'setInterval(function () {}, 1e6)'], { stdio: 'ignore' });
    children.push(c);
    c.died = new Promise(function (r) { c.on('exit', function (code, signal) { r(signal || code); }); });
    return c;
}
/* What the next head-unit start leaves: carplay runs the binary on disk,
 * /dev/shm is empty and nothing runs the kept link. */
function nextStart() {
    [path.join(dir, 'sock', 'activation-pending'), TGT + '.woc-running'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    runs(TGT);
}
function diedWithin(c) { return Promise.race([c.died, sleep(5000).then(function () { return 'still running'; })]); }
function setUnit(mode, pid, dropin) { fs.writeFileSync(SYSTEMD, JSON.stringify({ mode: mode, pid: pid, state: 'active', dropin: dropin !== false })); }
var callsSeen = 0;
function newCalls() {
    var all = fs.readFileSync(CALLS, 'utf8').split('\n').filter(Boolean);
    var fresh = all.slice(callsSeen);
    callsSeen = all.length;
    return fresh;
}

function edit12Only() {
    var b = Buffer.from(SRCBUF);
    b.writeUInt32LE(0xfff7f2bc >>> 0, SLOT);   /* IC_HIGH */
    var s = Buffer.alloc(SINK_LEN, 0); s.write(UDP_SINK, 'latin1'); s.copy(b, SINK);
    return b;   /* self-start call left stock */
}

function edit123Only() {
    var b = edit12Only();
    Buffer.from('e89f000000', 'hex').copy(b, 0x18176c);   /* self-start retarget */
    return b;   /* resolution immediates left stock */
}

/* A crash restart cut across a revert: with CRASH_REPLACED, carplay starts
 * from the file the revert replaces, as the patcher starts; with
 * CRASH_WRITTEN, from the file it wrote, as the patcher ends (before patchd
 * puts its running file back). */
var CRASH_REPLACED = path.join(dir, 'crash-replaced'), CRASH_WRITTEN = path.join(dir, 'crash-written');
var CRASH_HOOK = path.join(dir, 'crash-hook.js');
fs.writeFileSync(CRASH_HOOK, [
    "var fs = require('fs'), cp = require('child_process'), path = require('path');",
    "var P = " + JSON.stringify(PROC) + ", T = " + JSON.stringify(TGT) + ";",
    "function start() { cp.execFileSync('rm', ['-rf', path.join(P, '4242')]); fs.mkdirSync(path.join(P, '4242')); fs.linkSync(T, path.join(P, '4242', 'exe')); }",
    "function once(f) { if (!fs.existsSync(f)) return false; fs.unlinkSync(f); return true; }",
    "if (/carplay_patch\\.js$/.test(process.argv[1] || '') && process.argv[2] === 'revert') {",
    "    if (once(" + JSON.stringify(CRASH_REPLACED) + ")) start();",
    "    process.on('exit', function () { if (once(" + JSON.stringify(CRASH_WRITTEN) + ")) start(); });",
    "}",
].join('\n') + '\n');

var SOCK = path.join(dir, 'sock', 'patch.sock');
var patchd = cp.spawn(process.execPath, [PATCHD], {
    env: Object.assign({}, process.env, {
        PATH: BIN + path.delimiter + process.env.PATH,
        WOC_PATCH_SOCK: SOCK,
        WOC_CARPLAY: TGT,
        WOC_MOUNT: MOUNT,
        WOC_PROC_MOUNTS: MOUNTS,
        WOC_RESTART_POLL_MS: '20',
        WOC_RECMGR_CONF: RECMGR,
        WOC_CARPLAY_DROPIN: DROPIN,
        WOC_RECMGR_AT_BOOT: path.join(dir, 'no-recmgr-at-boot.json'),
        WOC_PROC_STAT: PROC_STAT,
        WOC_BOOT_ID: path.join(dir, 'no-boot-id'),
        WOC_CAPTURE_SOCK: path.join(dir, 'no-capture.sock'),
        WOC_DLT_RECEIVE: path.join(dir, 'no-dlt-receive'),
        WOC_DLT_RETRY_MS: '600000',
        WOC_REBOOT_CMD: REBOOT,
        WOC_STATE_DIR: path.join(dir, 'var'),
        WOC_REMOUNT_LOCK: path.join(dir, 'sock', 'remount.lock'),
        WOC_PROC: PROC,
        NODE_OPTIONS: '--require=' + CRASH_HOOK,
    }),
    stdio: ['ignore', 'ignore', 'inherit'],
});
children.push(patchd);

function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

/* Connect, wait for the status patchd sends on connect, optionally send cmd,
 * and resolve to the first status message done(msg) accepts. */
function session(cmd, done) {
    return new Promise(function (resolve, reject) {
        var c = net.connect(SOCK), buf = '', sent = !cmd;
        var timer = setTimeout(function () { c.destroy(); reject(new Error('timed out waiting on ' + JSON.stringify(cmd))); }, 30000);
        c.setEncoding('utf8');
        c.on('error', reject);
        c.on('data', function (d) {
            buf += d;
            var i;
            while ((i = buf.indexOf('\n')) >= 0) {
                var m = JSON.parse(buf.slice(0, i));
                buf = buf.slice(i + 1);
                if (m.ev !== 'status' && m.ev !== 'refused') continue;
                if (!sent) { sent = true; c.write(JSON.stringify(cmd) + '\n'); continue; }
                if (done(m)) { clearTimeout(timer); c.destroy(); resolve(m); return; }
            }
        });
    });
}
function status() { return session(null, function (m) { return m.state !== 'busy'; }); }
function op(cmd) { return session(cmd, function (m) { return m.state === 'error' || m.carplay === 'restarted' || m.carplay === 'deferred'; }); }
/* Send a command and resolve to the first reboot-reply it gets. */
function rebootCmd(cmd) {
    return new Promise(function (resolve, reject) {
        var c = net.connect(SOCK), buf = '', sent = false;
        var timer = setTimeout(function () { c.destroy(); reject(new Error('timed out on ' + JSON.stringify(cmd))); }, 30000);
        c.setEncoding('utf8');
        c.on('error', reject);
        c.on('data', function (d) {
            buf += d;
            var i;
            while ((i = buf.indexOf('\n')) >= 0) {
                var m = JSON.parse(buf.slice(0, i));
                buf = buf.slice(i + 1);
                if (!sent && m.ev === 'status') { sent = true; c.write(JSON.stringify(cmd) + '\n'); continue; }
                if (m.ev === 'reboot-reply') { clearTimeout(timer); c.destroy(); resolve(m); return; }
            }
        });
    });
}
function rebootCount() { return fs.readFileSync(REBOOTS, 'utf8').split('\n').filter(Boolean).length; }
function slot() { return fs.readFileSync(TGT).slice(SLOT, SLOT + 4).toString('hex'); }

async function main() {
    for (var i = 0; i < 200 && !fs.existsSync(SOCK); i++) await sleep(25);
    var r, calls, dummy;

    /* Status: patched needs every edit. */
    fs.writeFileSync(TGT, edit12Only());
    r = await status();
    ok('status: Edit-1/2-only binary is not patched', r.state === 'unknown' && r.mode === null, JSON.stringify(r));
    ok('status: Edit-1/2-only binary carries the missing self-start reason', /self-start edit missing/.test(r.reason || ''), JSON.stringify(r));
    fs.writeFileSync(TGT, edit123Only());
    r = await status();
    ok('status: pre-resolution binary is not patched', r.state === 'unknown' && r.mode === null && r.res === null && r.safe === null, JSON.stringify(r));
    ok('status: pre-resolution binary carries the missing resolution reason', /resolution edit missing/.test(r.reason || ''), JSON.stringify(r));
    fs.writeFileSync(TGT, SRCBUF);
    r = await status();
    ok('status: pristine binary is original', r.state === 'original', JSON.stringify(r));

    /* reboot with nothing pending is refused and reboots nothing. */
    var rb = await rebootCmd({ cmd: 'reboot' });
    ok('reboot: refused when no activation is pending', rb.ok === false && /no restart is pending/.test(rb.reason || ''), JSON.stringify(rb));
    ok('reboot: nothing rebooted', rebootCount() === 0);

    /* carplay in both restart lists, loaded at boot: an apply restarts carplay. */
    setRecmgr(['HBAdasMgu', 'carplay'], true);
    dummy = dummyCarplay();
    setUnit('ok', dummy.pid);
    r = await op({ cmd: 'apply', panel: 'high', mode: 'udp' });
    calls = newCalls();
    ok('apply: patched, udp, carplay restarted', r.state === 'patched' && r.mode === 'udp' && r.panel === 'high', JSON.stringify(r));
    ok('apply: the carplay main process got SIGKILL', await diedWithin(dummy) === 'SIGKILL');
    ok('apply: the killed run is recorded for boot recovery',
        fs.readFileSync(path.join(path.dirname(SOCK), 'carplay-killed'), 'utf8') === 'inv-' + dummy.pid + '\n');
    ok('apply: the drop-in checked, MainPID read, then start once it was gone, then is-active',
        calls.length === 4 && calls[0] === 'show carplay.service -p DropInPaths' &&
        calls[1] === 'show carplay.service -p MainPID -p InvocationID -p ExecMainStartTimestampMonotonic' &&
        calls[2] === 'start carplay.service killed-pid-alive=false' && calls[3] === 'is-active carplay.service', JSON.stringify(calls));
    ok('apply: / is read-only again', /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8')));
    r = await status();
    ok('status: fully patched binary is patched, res 1088x480', r.state === 'patched' && r.mode === 'udp' && r.res === '1088x480' && !r.reason, JSON.stringify(r));
    ok('status: fully patched binary carries the default safe area', r.safe === '504x368+286+72', JSON.stringify(r));
    /* The full-frame safe area (the geometry before the safe-area default):
     * still patched at 1088x480, with its safe area reported for the app. */
    var patchedBuf = fs.readFileSync(TGT);
    var fullSafe = Buffer.from(patchedBuf);
    Buffer.from('0000000040040000', 'hex').copy(fullSafe, 0x20ca66);
    Buffer.from('e001000000000000', 'hex').copy(fullSafe, 0x20ca74);
    fs.writeFileSync(TGT, fullSafe);
    r = await status();
    ok('status: full-frame safe area is patched, res 1088x480, safe 1088x480+0+0',
        r.state === 'patched' && r.res === '1088x480' && r.safe === '1088x480+0+0' && !r.reason, JSON.stringify(r));
    fs.writeFileSync(TGT, patchedBuf);

    /* No main PID: an error, and carplay is neither started nor restarted. */
    setUnit('nopid', 0);
    r = await op({ cmd: 'revert' });
    calls = newCalls();
    ok('no main PID: revert reports the error', r.state === 'error' && /no main PID \(MainPID=0\)/.test(r.reason), JSON.stringify(r));
    ok('no main PID: only the drop-in and MainPID read', calls.length === 2 && /^show .*DropInPaths/.test(calls[0]) && /^show .*MainPID/.test(calls[1]), JSON.stringify(calls));

    /* The start fails: an error, never a restart. */
    dummy = dummyCarplay();
    setUnit('startfail', dummy.pid);
    r = await op({ cmd: 'apply', panel: 'high', mode: 'udp' });
    calls = newCalls();
    ok('start fails: apply reports the error', r.state === 'error' && /systemctl start failed/.test(r.reason), JSON.stringify(r));
    ok('start fails: killed, start tried once, no is-active', calls.length === 3 && /^start /.test(calls[2]) && await diedWithin(dummy) === 'SIGKILL', JSON.stringify(calls));

    /* The unit never becomes active after start: an error after the bounded wait. */
    dummy = dummyCarplay();
    setUnit('inactive', dummy.pid);
    r = await op({ cmd: 'revert' });
    calls = newCalls();
    ok('stays inactive: revert reports the error', r.state === 'error' && /carplay\.service is failed after start/.test(r.reason), JSON.stringify(r));
    ok('stays inactive: is-active polled a bounded number of times', calls.filter(function (l) { return /^is-active/.test(l); }).length === 41, JSON.stringify(calls));

    /* carplay in the list, but systemd has not loaded the drop-in: a SIGKILL
     * would fail carplay.service and run its OnFailure=, so the change waits. */
    dummy = dummyCarplay();
    setUnit('ok', dummy.pid, false);
    r = await op({ cmd: 'apply', panel: 'high', mode: 'udp' });
    calls = newCalls();
    ok('no drop-in: apply defers activation', r.carplay === 'deferred' && r.state === 'unknown', JSON.stringify(r));
    ok('no drop-in: only DropInPaths read, carplay not killed', calls.length === 1 && /DropInPaths/.test(calls[0]) && dummy.exitCode === null && dummy.signalCode === null, JSON.stringify(calls));
    fs.unlinkSync(TGT);
    fs.writeFileSync(TGT, SRCBUF);
    nextStart();

    /* The stock list (no carplay): the binary is written, carplay keeps
     * running, and the change waits for the next head-unit start. */
    setRecmgr(['HBAdasMgu'], true);
    dummy = dummyCarplay();
    setUnit('ok', dummy.pid);
    runs(TGT);
    var runningIno = fs.statSync(TGT).ino;
    r = await op({ cmd: 'apply', panel: 'high', mode: 'udp' });
    calls = newCalls();
    ok('stock list: apply defers activation', r.carplay === 'deferred' && r.state === 'unknown' && /takes effect at the next head-unit start/.test(r.reason || ''), JSON.stringify(r));
    ok('stock list: the binary is patched on disk', slot() === 'bcf2f7ff', slot());
    ok('stock list: carplay neither killed nor started (no systemctl call)', calls.length === 0 && dummy.exitCode === null && dummy.signalCode === null, JSON.stringify(calls));
    ok('stock list: the running binary stays linked, so / is read-only again',
        fs.statSync(TGT + '.woc-running').ino === runningIno && /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8')));
    r = await status();
    ok('stock list: status reports the pending start, not patched', r.state === 'unknown' && r.carplay === 'deferred' && /next head-unit start/.test(r.reason || ''), JSON.stringify(r));

    ok('stock list: the marker says apply', JSON.parse(fs.readFileSync(path.join(dir, 'sock', 'activation-pending'), 'utf8')).action === 'apply');
    ok('stock list: status says the apply is pending', r.pending === 'apply', JSON.stringify(r));

    /* carplay in the list, but written after boot (recmgr has not read it): deferred. */
    setRecmgr(['HBAdasMgu', 'carplay'], false);
    r = await op({ cmd: 'apply', panel: 'high', mode: 'avb' });
    calls = newCalls();
    ok('list written after boot: apply defers activation', r.carplay === 'deferred' && calls.length === 0 && dummy.exitCode === null, JSON.stringify([r, calls]));
    ok('list written after boot: the first running binary stays linked', fs.statSync(TGT + '.woc-running').ino === runningIno);

    /* Loaded again (a later boot in effect): the next change restarts carplay,
     * clears the pending start and drops the kept binary. */
    setRecmgr(['HBAdasMgu', 'carplay'], true);
    setUnit('ok', dummy.pid);
    r = await op({ cmd: 'apply', panel: 'mid', mode: 'udp' });
    calls = newCalls();
    ok('loaded list: apply restarts carplay', r.carplay === 'restarted' && r.state === 'patched' && r.panel === 'mid' && await diedWithin(dummy) === 'SIGKILL' && calls.length === 4, JSON.stringify([r, calls]));
    ok('loaded list: the kept binary is gone', !fs.existsSync(TGT + '.woc-running'));

    /* A pending change undone before the head-unit start: the kept link,
     * carplay's own file, moves back into place and nothing is pending. */
    setRecmgr(['HBAdasMgu'], true);
    var undoIno = fs.statSync(TGT).ino;
    r = await op({ cmd: 'apply', panel: 'high', mode: 'avb' });
    ok('undo: an apply on the stock list is pending', r.carplay === 'deferred' && r.pending === 'apply', JSON.stringify(r));
    r = await session({ cmd: 'apply', panel: 'mid', mode: 'udp' }, function (m) { return m.state !== 'busy'; });
    ok('undo: the running binary\'s own file back in place leaves nothing pending', r.state === 'patched' && r.panel === 'mid' && !r.carplay && !r.pending &&
        fs.statSync(TGT).ino === undoIno && !fs.existsSync(TGT + '.woc-running') &&
        !fs.existsSync(path.join(dir, 'sock', 'activation-pending')), JSON.stringify(r));
    ok('undo: no systemctl call', newCalls().length === 0);

    /* Remove patch of a waiting patch, cut across by a crash restart. Every
     * file carplay runs keeps a name, so / goes read-only again, and the
     * state is the true one. */
    function held() { return ['', '.1', '.2'].filter(function (s) { return fs.existsSync(TGT + '.woc-running' + s); }); }
    function exeIno() { return fs.statSync(path.join(PROC, '4242', 'exe')).ino; }
    function freshStart() { fs.writeFileSync(TGT, SRCBUF); ['', '.1', '.2'].forEach(function (s) { try { fs.unlinkSync(TGT + '.woc-running' + s); } catch (e) {} }); nextStart(); }
    setRecmgr(['HBAdasMgu'], true);
    [TGT + '.woc-orig', TGT + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    freshStart();
    r = await op({ cmd: 'apply', panel: 'high', mode: 'avb' });
    var patchedIno = fs.statSync(TGT).ino;
    fs.writeFileSync(CRASH_REPLACED, '');
    r = await session({ cmd: 'revert' }, function (m) { return m.state !== 'busy' && m.reason !== 'reverting'; });
    ok('crash restart from the waiting patch during its removal: the removal waits, the patch kept linked, / read-only', r.pending === 'revert' && r.carplay === 'deferred' &&
        r.state !== 'error' && fs.readFileSync(TGT).equals(SRCBUF) && exeIno() === patchedIno && fs.statSync(TGT + '.woc-running').ino === patchedIno &&
        held().length === 1 && /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8')), JSON.stringify(r) + ' ' + held().join(','));
    freshStart();
    r = await op({ cmd: 'apply', panel: 'high', mode: 'avb' });
    fs.writeFileSync(CRASH_WRITTEN, '');
    r = await session({ cmd: 'revert' }, function (m) { return m.state !== 'busy' && m.reason !== 'reverting'; });
    ok('crash restart from the reverted file before the put-back: it stays linked, nothing waits, / read-only', r.state === 'original' && !r.pending &&
        !r.carplay && fs.readFileSync(TGT).equals(SRCBUF) && fs.readFileSync(path.join(PROC, '4242', 'exe')).equals(SRCBUF) &&
        fs.statSync(TGT + '.woc-running').ino === exeIno() && fs.statSync(TGT).ino !== exeIno() && held().length === 1 &&
        !fs.existsSync(path.join(dir, 'sock', 'activation-pending')) && /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8')), JSON.stringify(r) + ' ' + held().join(','));
    ok('crash restarts during a removal: no systemctl call', newCalls().length === 0);
    freshStart();

    /* CarPlay's Remove patch button on what an older release patched where the locator now
     * refuses a site (with its backup and record): the original back, carplay
     * restarted. */
    if (require('crypto').createHash('sha256').update(SRCBUF).digest('hex') === 'ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50') {
        setRecmgr(['HBAdasMgu', 'carplay'], true);
        var locate = require('../carplay_locate.js'), fx = require('./carplay_mutations.js')(SRCBUF).oldRelease();
        for (var k = 0; k < fx.length; k++) {
            fs.writeFileSync(TGT, fx[k].patched);
            fs.writeFileSync(TGT + '.woc-orig', fx[k].original);
            fs.writeFileSync(TGT + '.woc-orig.json', JSON.stringify({ buildId: locate.buildId(fx[k].original),
                sha256_original: require('crypto').createHash('sha256').update(fx[k].original).digest('hex') }));
            dummy = dummyCarplay();
            setUnit('ok', dummy.pid);
            r = await op({ cmd: 'revert' });
            ok(fx[k].name + ': CarPlay\'s Remove patch button restores the original, carplay restarted', r.carplay === 'restarted' && fs.readFileSync(TGT).equals(fx[k].original) &&
                /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8')), JSON.stringify(r));
            newCalls();
        }
        fs.writeFileSync(TGT, SRCBUF);
        [TGT + '.woc-orig', TGT + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    }

    /* CarPlay's Patch button on a car coded otherwise than KI_KOMBI_VARIANT
     * 7, or whose coding cannot be read: patchd reports the conflict as
     * Android Auto does, the button still patches, and the patch is recorded
     * unproven (boot recovery restores it after one failed start). On a
     * variant-7 car the first patch is on trial. */
    setRecmgr(['HBAdasMgu'], true);
    var cases = [[9, undefined, 'not supported on this car (KI_KOMBI_VARIANT 9)'], [7, 0x0b, 'cluster coding unreadable'],
        [null, undefined, 'cluster coding unreadable'], [7, undefined, null]];
    for (var q = 0; q < cases.length; q++) {
        var cq = cases[q], what = cq[0] === null ? 'no coding' : 'coding ' + cq[0] + (cq[1] !== undefined ? ' flags 0x' + cq[1].toString(16) : '');
        [TGT + '.woc-orig', TGT + '.woc-orig.json', record.recordPath(TGT)].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
        freshStart();
        car.coding(cq[0], cq[1]);
        r = await status();
        ok(what + ': patchd reports ' + (cq[2] || 'no conflict'), r.state === 'original' && (r.conflict || null) === cq[2], JSON.stringify(r));
        r = await op({ cmd: 'apply', panel: 'high', mode: 'avb' });
        var rc = record.current(TGT);
        /* on trial unless the coding reads another variant */
        var onTrial = cq[0] !== 9;
        ok(what + ': CarPlay\'s Patch button patches it, recorded ' + (onTrial ? 'on trial' : 'unproven'), r.carplay === 'deferred' && slot() === 'bcf2f7ff' && !!rc &&
            rc.patched === true && rc.proven === false && (rc.trial === true) === onTrial, JSON.stringify([r, rc]));
        nextStart();
        r = await status();
        ok(what + ': after the start it reads patched, the conflict still named', r.state === 'patched' && (r.conflict || null) === cq[2], JSON.stringify(r));
        car.coding(7);
    }
    [TGT + '.woc-orig', TGT + '.woc-orig.json', record.recordPath(TGT)].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    freshStart();
    newCalls();

    /* reboot with a pending activation: one head-unit restart. A second
     * request is refused while that one is in progress; carplay is never killed
     * or restarted (no systemctl call), and apply/revert are refused meanwhile.
     * Set up a fresh deferred patch on the stock list first. */
    setRecmgr(['HBAdasMgu'], true);
    dummy = dummyCarplay();
    setUnit('ok', dummy.pid);
    r = await op({ cmd: 'apply', panel: 'high', mode: 'avb' });
    ok('reboot setup: a deferred patch is pending', r.carplay === 'deferred', JSON.stringify(r));
    newCalls();   /* drain; the reboot must add no systemctl call */
    rb = await rebootCmd({ cmd: 'reboot' });
    ok('reboot: accepted while an activation is pending', rb.ok === true, JSON.stringify(rb));
    for (var w = 0; w < 100 && rebootCount() < 1; w++) await sleep(20);
    ok('reboot: one head-unit restart was asked for', rebootCount() === 1);
    rb = await rebootCmd({ cmd: 'reboot' });
    ok('reboot: a second request is refused (already in progress)', rb.ok === false && /already in progress/.test(rb.reason || ''), JSON.stringify(rb));
    ok('reboot: still only one restart asked for', rebootCount() === 1);
    r = await session({ cmd: 'apply', panel: 'high', mode: 'avb' }, function (m) { return m.ev === 'refused' || m.state === 'error'; });
    ok('reboot: apply refused while a restart is in progress', r.ev === 'refused' && /restart is in progress/.test(r.reason || ''), JSON.stringify(r));
    ok('reboot: no systemctl call (carplay not stopped or restarted)', newCalls().length === 0, 'unexpected systemctl calls');

    var all = fs.readFileSync(CALLS, 'utf8');
    ok('never systemctl restart or stop', !/^(restart|stop|try-restart|reload-or-restart)\b/m.test(all), all);
}

main().catch(function (e) { ok('no exception', false, e.stack); }).then(function () {
    process.stdout.write(fails ? fails + ' FAILED\n' : 'all passed\n');
    process.exit(fails ? 1 : 0);
});
