#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Proves the install-time activation (`patchd.js activate` / `deactivate`, the
 * deploy tool's hooks) against the unit's carplay binary (CARPLAY_BIN): a fresh
 * install writes CarPlay's Patch button's patch and leaves it pending for the next
 * head-unit start; carplay is never stopped, killed or restarted, even where
 * recmgr would allow it; the running binary stays linked and / goes read-only
 * again; a second run changes nothing; after that start the patch is left as
 * it is, a user's revert is kept, an older patch of ours is brought up to
 * date, a stock build outside the proven list is left as a conflict until
 * CarPlay's Patch button patched it, then patched like the proven one. Removal (deactivate) reverts and drops our backup: before the pending
 * start it moves carplay's running file back and leaves nothing pending;
 * while carplay runs the patch it keeps that file linked and hands it to the
 * deploy tool (removeWhenUnused); with carplay not running it keeps nothing;
 * changes another tool made elsewhere in the binary stay; an install after a
 * removal patches again. patchd removes the link once nothing runs it and
 * keeps one carplay runs (boot recovery's). A binary with an edit site not
 * found, one modified by something else and a carplay.service that starts
 * another program are left untouched (a conflict, exit 0); a remount failure before
 * any write exits non-zero. On a head unit that refuses our parts the
 * install writes nothing, patchd refuses CarPlay's Patch button and the
 * removal reverts; MID and HIGH F patch as HIGH. The first install on HIGH F
 * applies the patch again to an original 0.8.2's boot recovery restored. The pending marker is the world-readable JSON
 * patchd and the app read. Runs the real patchd.js and carplay_patch.js on a
 * temporary copy, with a fake /proc (carplay's process holds the inode it
 * runs), a fake mount that, like the kernel, refuses read-only while a file a
 * process runs is unlinked, and a recording systemctl, kill and reboot.
 * Without CARPLAY_BIN it skips.
 *
 *   node patchd_activate_test.js
 */
'use strict';

var headUnit = require('./head_unit_env.js');   /* a HIGH head unit, coded KI_KOMBI_VARIANT 7, for every patcher and patchd this runs */

var fs = require('fs');
var os = require('os');
var net = require('net');
var path = require('path');
var cp = require('child_process');
var crypto = require('crypto');

var PATCHD = path.join(__dirname, '..', 'patchd.js');
var SRC = process.env.CARPLAY_BIN || '';
var SLOT = 0x2ba4f0, SINK = 0x2a20b0;

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}

if (!fs.existsSync(SRC)) { process.stdout.write('SKIP: set CARPLAY_BIN to a copy of the unit\'s /usr/bin/carplay (carplay 22.28.3)\n'); process.exit(0); }
var SRCBUF = fs.readFileSync(SRC);

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-activate-'));
var children = [];
process.on('exit', function () {
    children.forEach(function (c) { try { c.kill('SIGKILL'); } catch (e) {} });
    try { cp.execFileSync('rm', ['-rf', dir]); } catch (e) {}
});

var TGT = path.join(dir, 'carplay');
var BAK = TGT + '.woc-orig';
var RUNNING = TGT + '.woc-running';
var SOCKDIR = path.join(dir, 'shm');
var PENDING = path.join(SOCKDIR, 'activation-pending');
var CALLS = path.join(dir, 'systemctl.calls');
var MOUNTS = path.join(dir, 'mounts');
var RW_FAILS = path.join(dir, 'rw-fails');
var RECMGR = path.join(dir, 'recmgr-restartlist.conf');
var PROC_STAT = path.join(dir, 'stat');
var PROC = path.join(dir, 'proc');
var UNITS = path.join(dir, 'etc-systemd');
var BOOT_S = Math.floor(Date.now() / 1000) - 60;
fs.writeFileSync(PROC_STAT, 'cpu  1 2 3\nbtime ' + BOOT_S + '\n');
/* carplay in both restart lists as loaded at boot: patchd itself would
 * restart carplay; the install must not. */
fs.writeFileSync(RECMGR, '[Whitelist]\ncarplay\n[WhitelistExitSuccess]\ncarplay\n');
fs.utimesSync(RECMGR, BOOT_S - 3600, BOOT_S - 3600);
fs.writeFileSync(CALLS, '');
fs.writeFileSync(MOUNTS, '/dev/root / ext4 ro 0 0\n');
fs.mkdirSync(UNITS);
fs.mkdirSync(PROC);

var BIN = path.join(dir, 'bin');
fs.mkdirSync(BIN);
/* Every systemctl, kill or reboot is recorded; the test asserts none. */
['systemctl', 'kill', 'reboot'].forEach(function (n) {
    fs.writeFileSync(path.join(BIN, n), '#!/bin/sh\necho "' + n + ' $*" >> "' + CALLS + '"\n');
    fs.chmodSync(path.join(BIN, n), 0o755);
});
/* remount,ro fails while a process runs a file no name links any more (its
 * fake /proc exe is the inode's only link), as the kernel refuses it. */
var MOUNT = path.join(dir, 'mount');
fs.writeFileSync(MOUNT, '#!' + process.execPath + '\n' + [
    "var fs = require('fs'), path = require('path');",
    "var a = process.argv.slice(2).join(' '), P = " + JSON.stringify(PROC) + ", M = " + JSON.stringify(MOUNTS) + ";",
    "if (/remount,rw/.test(a)) {",
    "    if (fs.existsSync(" + JSON.stringify(RW_FAILS) + ")) { process.stderr.write('mount: / is busy\\n'); process.exit(32); }",
    "    fs.writeFileSync(M, '/dev/root / ext4 rw 0 0\\n');",
    "} else if (/remount,ro/.test(a)) {",
    "    var open = fs.readdirSync(P).some(function (p) { try { return fs.statSync(path.join(P, p, 'exe')).nlink <= 1; } catch (e) { return false; } });",
    "    if (open) { process.stderr.write('mount: / is busy\\n'); process.exit(32); }",
    "    fs.writeFileSync(M, '/dev/root / ext4 ro 0 0\\n');",
    "}",
].join('\n') + '\n');
fs.chmodSync(MOUNT, 0o755);

var ENV = Object.assign({}, process.env, {
    PATH: BIN + path.delimiter + process.env.PATH,
    WOC_PATCH_SOCK: path.join(SOCKDIR, 'patch.sock'),
    WOC_CARPLAY: TGT,
    WOC_MOUNT: MOUNT,
    WOC_PROC_MOUNTS: MOUNTS,
    WOC_PROC: PROC,
    WOC_RECMGR_CONF: RECMGR,
    WOC_RECMGR_AT_BOOT: path.join(dir, 'no-recmgr-at-boot.json'),
    WOC_PROC_STAT: PROC_STAT,
    WOC_BOOT_ID: path.join(dir, 'no-boot-id'),
    WOC_UNIT_DIRS: UNITS,
    WOC_CAPTURE_SOCK: path.join(dir, 'no-capture.sock'),
    WOC_DLT_RECEIVE: path.join(dir, 'no-dlt-receive'),
    WOC_DLT_RETRY_MS: '600000',
    WOC_REBOOT_CMD: path.join(BIN, 'reboot'),
    WOC_STATE_DIR: path.join(dir, 'var'),
    WOC_REMOUNT_LOCK: path.join(SOCKDIR, 'remount.lock'),
});

/* One hook run: { code, result (the last stdout line as JSON), out }. */
function hook(which) {
    var r = cp.spawnSync(process.execPath, [PATCHD, which], { env: ENV, encoding: 'utf8', timeout: 60000 });
    var lines = (r.stdout || '').split('\n').filter(function (l) { return l.trim(); });
    var result = null;
    try { result = JSON.parse(lines[lines.length - 1]); } catch (e) {}
    return { code: r.status, result: result || {}, out: (r.stdout || '') + (r.stderr || '') };
}
function sha(buf) { return crypto.createHash('sha256').update(buf).digest('hex'); }
function disk() { return sha(fs.readFileSync(TGT)); }
function ino(f) { try { return fs.statSync(f).ino; } catch (e) { return null; } }
function slot() { return fs.readFileSync(TGT).slice(SLOT, SLOT + 4).toString('hex'); }
function sinkText() { var b = fs.readFileSync(TGT).slice(SINK, SINK + 59); var n = b.indexOf(0); return b.slice(0, n < 0 ? 59 : n).toString('latin1'); }
function marker() { try { return JSON.parse(fs.readFileSync(PENDING, 'utf8')); } catch (e) { return null; } }
function ro() { return /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8')); }
function calls() { return fs.readFileSync(CALLS, 'utf8').split('\n').filter(Boolean); }
function verify() {
    return cp.spawnSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'verify', '--target', TGT], { encoding: 'utf8' }).stdout;
}
/* carplay's process runs file: its /proc/<pid>/exe holds that inode. */
function runs(file) {
    stops();
    fs.mkdirSync(path.join(PROC, '4242'));
    fs.linkSync(file, path.join(PROC, '4242', 'exe'));
}
function stops() { cp.execFileSync('rm', ['-rf', path.join(PROC, '4242')]); }
/* The next head-unit start: /dev/shm is empty again and carplay runs what is
 * on disk. */
function headUnitStart() {
    try { fs.unlinkSync(PENDING); } catch (e) {}
    runs(TGT);
}
function fresh(buf) {
    stops();
    [PENDING, RUNNING, BAK, TGT + '.woc-orig.json', TGT + '.woc-patch.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    fs.writeFileSync(TGT, buf, { mode: 0o755 });
    runs(TGT);
}

/* patchd started (as at a head-unit start), and its first status. */
function daemonStatus() { return daemonCommand(null, 'status'); }

/* patchd started, msg sent once its first status is in (none when null),
 * and the first event ev after that. */
function daemonCommand(msg, ev) {
    return new Promise(function (resolve, reject) {
        var d = cp.spawn(process.execPath, [PATCHD], { env: ENV, stdio: ['ignore', 'ignore', 'inherit'] });
        children.push(d);
        var sock = path.join(SOCKDIR, 'patch.sock');
        var tries = 0;
        (function connect() {
            var c = net.connect(sock), buf = '';
            c.setEncoding('utf8');
            c.on('error', function () { if (++tries > 200) { d.kill(); reject(new Error('no patchd socket')); } else setTimeout(connect, 25); });
            c.on('data', function (chunk) {
                buf += chunk;
                var i;
                while ((i = buf.indexOf('\n')) >= 0) {
                    var m = JSON.parse(buf.slice(0, i));
                    buf = buf.slice(i + 1);
                    if (msg && m.ev === 'status') { c.write(JSON.stringify(msg) + '\n'); msg = null; continue; }
                    /* Resolved once it is gone, so the next patchd's socket is the only one. */
                    if (m.ev === ev) { c.destroy(); d.on('exit', function () { resolve(m); }); d.kill('SIGKILL'); return; }
                }
            });
        })();
    });
}

async function main() {
    var r, before, m, st, runningIno, patchedIno;

    /* A fresh install: the pristine build, no backup of ours, carplay running it. */
    fresh(SRCBUF);
    runningIno = ino(TGT);
    r = hook('activate');
    m = marker();
    ok('fresh: activate exits 0', r.code === 0, r.out);
    ok('fresh: CarPlay\'s Patch button\'s patch is on disk (IC_HIGH, stock AVB sink)', slot() === 'bcf2f7ff' && /^avb:\/\//.test(sinkText()), slot() + ' ' + sinkText());
    ok('fresh: the patcher verifies it, 1088x480 and the default safe area',
        /OK: variant IC_HIGH, sink original \(AVB 15\), self-start on, res 1088x480/.test(verify()));
    ok('fresh: the result asks for a head-unit start', /next head-unit start/.test(r.result.reboot || '') && /written/.test(r.result.status || ''), JSON.stringify(r.result));
    ok('fresh: the marker says apply', m && m.action === 'apply' && !m.by && /next head-unit start/.test(m.reason), JSON.stringify(m));
    ok('fresh: the marker is world-readable, its folder traversable',
        (fs.statSync(PENDING).mode & 0o777) === 0o644 && (fs.statSync(SOCKDIR).mode & 0o755) === 0o755);
    ok('fresh: carplay\'s running file stays linked, / read-only again', ino(RUNNING) === runningIno && ro());
    ok('fresh: no systemctl, kill or reboot', calls().length === 0, calls().join('; '));
    st = await daemonStatus();
    ok('fresh: patchd reports the pending apply and keeps the link carplay runs', st.state === 'unknown' && st.pending === 'apply' &&
        st.carplay === 'deferred' && !st.conflict && ino(RUNNING) === runningIno && ro(), JSON.stringify(st));

    /* The install run again (a reinstall or a rollback's activate): no write. */
    before = disk();
    r = hook('activate');
    ok('pending: a second activate changes nothing', r.code === 0 && disk() === before && marker().action === 'apply', r.out);
    ok('pending: it still reports the head-unit start', /next head-unit start/.test(r.result.reboot || ''), JSON.stringify(r.result));

    /* The head-unit start: carplay runs the patch; patchd removes the link
     * nothing runs any more as it starts. */
    headUnitStart();
    st = await daemonStatus();
    ok('patched: patchd reports patched, avb, high, nothing pending', st.state === 'patched' && st.mode === 'avb' && st.panel === 'high' && !st.pending, JSON.stringify(st));
    ok('patched: patchd removed the unused link as it started, / read-only', !fs.existsSync(RUNNING) && ro());
    before = disk();
    r = hook('activate');
    ok('patched: activate changes nothing, asks for nothing', r.code === 0 && disk() === before && !r.result.reboot && marker() === null, JSON.stringify(r.result));

    /* Removal while carplay runs the patch: reverted, pending, our backup gone,
     * carplay's file kept linked and handed to the deploy tool. */
    patchedIno = ino(TGT);
    r = hook('deactivate');
    m = marker();
    ok('removal while running the patch: reverts to the original, exit 0', r.code === 0 && disk() === sha(SRCBUF), r.out);
    ok('removal while running the patch: the marker is the removal\'s revert, a start is asked for',
        m && m.action === 'revert' && m.by === 'removal' && /next head-unit start/.test(r.result.reboot || ''), JSON.stringify([m, r.result]));
    ok('removal while running the patch: our backup is dropped', !fs.existsSync(BAK) && !fs.existsSync(TGT + '.woc-orig.json'));
    ok('removal while running the patch: carplay\'s file stays linked and goes to the deploy tool, / read-only',
        ino(RUNNING) === patchedIno && JSON.stringify(r.result.removeWhenUnused) === JSON.stringify([RUNNING]) && ro(), JSON.stringify(r.result));
    st = await daemonStatus();
    ok('removal while running the patch: patchd reports the pending revert, keeps the link', st.pending === 'revert' && st.carplay === 'deferred' &&
        ino(RUNNING) === patchedIno, JSON.stringify(st));

    /* Installed again before that start: the removal is undone; carplay's own
     * file is back in place and nothing is pending. */
    r = hook('activate');
    ok('reinstall before the start: the running patch is back in place, nothing pending, / read-only',
        r.code === 0 && ino(TGT) === patchedIno && !fs.existsSync(RUNNING) && marker() === null && !r.result.reboot && ro(), JSON.stringify(r.result));

    /* Removed, then the start, then installed again: a fresh install. */
    r = hook('deactivate');
    ok('removal again: pending, the link handed over', r.code === 0 && marker().by === 'removal' && r.result.removeWhenUnused, JSON.stringify(r.result));
    headUnitStart();
    ok('after the start nothing runs the link', ino(RUNNING) === patchedIno);
    r = hook('activate');
    ok('reinstall after the start: patched again as a fresh install, pending', r.code === 0 && marker() && marker().action === 'apply' &&
        slot() === 'bcf2f7ff' && fs.existsSync(BAK), JSON.stringify(r.result));
    ok('reinstall after the start: the old unused link went in the write window, carplay\'s current file is kept, / read-only',
        ino(RUNNING) !== patchedIno && ino(RUNNING) !== null && ro());

    /* Installed and removed before the head-unit start: carplay still runs
     * what it ran, so its own file goes back in place and nothing is pending. */
    fresh(SRCBUF);
    runningIno = ino(TGT);
    r = hook('activate');
    ok('undo: activate leaves an apply pending', r.code === 0 && marker().action === 'apply');
    r = hook('deactivate');
    ok('undo: deactivate puts carplay\'s own file back, nothing pending, nothing handed over', r.code === 0 && disk() === sha(SRCBUF) &&
        marker() === null && ino(TGT) === runningIno && !fs.existsSync(RUNNING) && !r.result.reboot && !r.result.removeWhenUnused &&
        !fs.existsSync(BAK), JSON.stringify(r.result));
    ok('undo: / is read-only again', ro());

    /* Removal with carplay not running: nothing needs keeping. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    stops();
    r = hook('deactivate');
    ok('removal, carplay not running: reverted, no link kept or handed over, / read-only', r.code === 0 && disk() === sha(SRCBUF) &&
        !fs.existsSync(RUNNING) && !r.result.removeWhenUnused && ro(), JSON.stringify(r.result));

    /* CarPlay's Remove patch button pressed (patchd's revert keeps the backup), taken
     * effect: an update leaves the original, a removal drops the backup. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'revert', '--target', TGT, '--yes']);
    headUnitStart();
    r = hook('activate');
    ok('reverted: activate leaves the original', r.code === 0 && disk() === sha(SRCBUF) && !r.result.reboot && /turned off/.test(r.result.status), JSON.stringify(r.result));
    r = hook('deactivate');
    ok('reverted: deactivate drops the backup, nothing to restart', r.code === 0 && disk() === sha(SRCBUF) && !r.result.reboot &&
        !fs.existsSync(BAK) && marker() === null && ro(), JSON.stringify(r.result));
    r = hook('deactivate');
    ok('original: deactivate has nothing to do', r.code === 0 && disk() === sha(SRCBUF) && !r.result.reboot, JSON.stringify(r.result));

    /* CarPlay's Remove patch button's revert still pending when the tool is removed: the
     * revert becomes the removal's, so an install before the start patches
     * again; our backup goes. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    patchedIno = ino(TGT);
    fs.unlinkSync(RUNNING);   /* unused since the start: patchd's write window removes it */
    fs.linkSync(TGT, RUNNING);
    cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'revert', '--target', TGT, '--yes']);
    fs.writeFileSync(PENDING, JSON.stringify({ action: 'revert' }) + '\n');
    r = hook('deactivate');
    ok('pending CarPlay\'s Remove patch button, then removal: the backup goes, the revert is the removal\'s, the running file stays linked',
        r.code === 0 && !fs.existsSync(BAK) && marker().action === 'revert' && marker().by === 'removal' &&
        /next head-unit start/.test(r.result.reboot || '') && ino(RUNNING) === patchedIno && disk() === sha(SRCBUF) && ro(), JSON.stringify(r.result));
    r = hook('activate');
    ok('pending CarPlay\'s Remove patch button, then removal: an install before the start puts the running patch back',
        r.code === 0 && ino(TGT) === patchedIno && marker() === null && !fs.existsSync(RUNNING), JSON.stringify(r.result));

    /* Another tool's edit elsewhere in the binary, made after our patch:
     * removal reverts our regions only and keeps it. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    var theirs = Buffer.from(fs.readFileSync(TGT));
    Buffer.from('e900000000', 'hex').copy(theirs, 0x9f9a0);
    fs.writeFileSync(TGT + '.theirs', theirs, { mode: 0o755 });
    fs.renameSync(TGT + '.theirs', TGT);
    runs(TGT);
    r = hook('deactivate');
    var after = fs.readFileSync(TGT);
    ok('another tool\'s edit: removal reverts our edits only, keeps theirs, drops our backup, / read-only',
        r.code === 0 && after.slice(0x9f9a0, 0x9f9a5).toString('hex') === 'e900000000' && /variant original, sink original \(AVB 15\), self-start off, res stock/.test(verify()) &&
        !fs.existsSync(BAK) && ro(), JSON.stringify(r.result) + ' ' + verify());
    headUnitStart();
    before = disk();
    r = hook('activate');
    ok('another tool\'s edit: a later install leaves it alone (modified by something else)', r.code === 0 && disk() === before &&
        /modified by something else/.test(r.result.conflict || ''), JSON.stringify(r.result));

    /* An older patch of ours (before the resolution edit): brought up to date. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    var older = Buffer.from(SRCBUF);
    older.writeUInt32LE(0xfff7f2bc >>> 0, SLOT);
    Buffer.from('e89f000000', 'hex').copy(older, 0x18176c);
    fs.writeFileSync(TGT, older);
    r = hook('activate');
    ok('older patch: activate adds the missing edit, pending', r.code === 0 && marker().action === 'apply' &&
        /resolution -> 1088x480/.test(r.out), r.out);

    /* The patcher fails after replacing the binary (here: the backup's record
     * cannot be dropped): carplay's file stays linked and the revert waits
     * for the next start. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    patchedIno = ino(TGT);
    fs.unlinkSync(TGT + '.woc-orig.json');
    fs.mkdirSync(TGT + '.woc-orig.json');
    r = hook('deactivate');
    ok('failure after the write: exit 0, the revert pending, carplay\'s file still linked, / read-only',
        r.code === 0 && /dropping .*woc-orig.json/.test(r.out) && disk() === sha(SRCBUF) && marker() && marker().action === 'revert' &&
        ino(RUNNING) === patchedIno && ro(), JSON.stringify(r.result));
    fs.rmdirSync(TGT + '.woc-orig.json');

    /* Boot recovery restored the original while carplay runs the patch (it
     * keeps the running file linked and clears the marker): patchd starting
     * keeps that link while carplay runs it and removes it after the start. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    patchedIno = ino(TGT);
    try { fs.unlinkSync(RUNNING); } catch (e) {}
    fs.linkSync(TGT, RUNNING);
    cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'revert', '--target', TGT, '--yes']);
    try { fs.unlinkSync(PENDING); } catch (e) {}
    await daemonStatus();
    ok('recovery\'s link: kept by patchd while carplay runs it', ino(RUNNING) === patchedIno && ro());
    headUnitStart();
    await daemonStatus();
    ok('recovery\'s link: removed by patchd once nothing runs it, / read-only', !fs.existsSync(RUNNING) && ro());

    /* Boot recovery's own restore at its threshold (the boot step's, nothing
     * running): patchd reports it, and an install leaves the patch off. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    stops();
    r = cp.spawnSync(process.execPath, ['-e', 'var r = require(process.argv[1]); r.restoreTarget(r.TARGETS.carplay, true, 2, function (e, o) { process.stdout.write(String(e || o)); });',
        path.join(__dirname, '..', 'recovery.js')], { env: ENV, encoding: 'utf8' });
    ok('boot recovery\'s restore: the original, / read-only', r.stdout === 'restored' && disk() === sha(SRCBUF) && ro(), r.stdout + r.stderr);
    st = await daemonStatus();
    ok('boot recovery\'s restore: patchd reports the original and the failed starts it was made at', st.state === 'original' && st.recovered === 2 && !st.pending, JSON.stringify(st));
    before = disk();
    r = hook('activate');
    ok('boot recovery\'s restore: an install leaves the patch off, and says why', r.code === 0 && disk() === before && marker() === null &&
        /boot recovery removed the patch after 2 failed head-unit starts/.test(r.result.status || ''), JSON.stringify(r.result));

    /* A write window cut short between keeping the link and the patcher's
     * rename leaves a link that is carplay's own file: patchd removes it as
     * it starts, carplay running or not. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    try { fs.unlinkSync(RUNNING); } catch (e) {}
    fs.linkSync(TGT, RUNNING);
    patchedIno = ino(TGT);
    await daemonStatus();
    ok('a link that is carplay\'s own file: removed by patchd\'s start, carplay\'s file intact, / read-only',
        !fs.existsSync(RUNNING) && ino(TGT) === patchedIno && ro());

    /* The same leftover when the tool is removed with nothing to revert: the
     * removal drops it itself and hands nothing over. */
    fresh(SRCBUF);
    fs.linkSync(TGT, RUNNING);
    r = hook('deactivate');
    ok('a link that is carplay\'s own file at removal: dropped, nothing handed over, / read-only',
        r.code === 0 && !fs.existsSync(RUNNING) && !r.result.removeWhenUnused && disk() === sha(SRCBUF) && ro(), JSON.stringify(r.result));

    /* An unused link the removal cannot drop itself (/ will not remount
     * read-write) goes to the deploy tool rather than staying behind. */
    fresh(SRCBUF);
    fs.writeFileSync(RUNNING, 'an older binary\n');
    fs.writeFileSync(RW_FAILS, '');
    r = hook('deactivate');
    fs.unlinkSync(RW_FAILS);
    ok('an unused link the removal cannot drop: handed to the deploy tool', r.code === 0 && fs.existsSync(RUNNING) &&
        JSON.stringify(r.result.removeWhenUnused) === JSON.stringify([RUNNING]), JSON.stringify(r.result));

    /* A binary the patcher cannot patch (an edit site not found): left
     * alone, a conflict naming the site, exit 0. */
    var unknown = Buffer.from(SRCBUF);
    unknown.writeUInt32LE(0x11223344, 0x2ba4d4);   /* the converter table's slot 0 */
    fresh(unknown);
    r = hook('activate');
    ok('unpatchable binary: activate leaves it, conflict names the site, exit 0', r.code === 0 && disk() === sha(unknown) &&
        /^not patchable: variant: slot 0 handler not a store$/.test(r.result.conflict || '') && !r.result.reboot, JSON.stringify(r.result));
    r = hook('deactivate');
    ok('unpatchable binary: deactivate leaves it, exit 0', r.code === 0 && disk() === sha(unknown), JSON.stringify(r.result));

    /* What an older release patched where the locator now refuses a site
     * (with its backup and record): activate leaves it, a conflict; the
     * removal restores the original whole and drops the backup. */
    if (sha(SRCBUF) === 'ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50') {
        var locate = require('../carplay_locate.js');
        require('./carplay_mutations.js')(SRCBUF).oldRelease().forEach(function (f) {
            fresh(f.patched);
            fs.writeFileSync(BAK, f.original, { mode: 0o600 });
            fs.writeFileSync(TGT + '.woc-orig.json', JSON.stringify({ buildId: locate.buildId(f.original), sha256_original: sha(f.original) }));
            r = hook('activate');
            ok(f.name + ': activate leaves it, conflict, exit 0', r.code === 0 && fs.readFileSync(TGT).equals(f.patched) &&
                /^not patchable: /.test(r.result.conflict || '') && !r.result.reboot && ro(), JSON.stringify(r.result));
            r = hook('deactivate');
            ok(f.name + ': deactivate restores the original, drops the backup, pending', r.code === 0 && fs.readFileSync(TGT).equals(f.original) &&
                !fs.existsSync(BAK) && !fs.existsSync(TGT + '.woc-orig.json') && marker() && marker().action === 'revert' && ro(), JSON.stringify(r.result));
        });
    }

    /* A build outside the proven list with every site stock: a fresh install
     * leaves it, a conflict; once CarPlay's Patch button patched it (our backup), an
     * install brings the patch up to date. */
    var other = Buffer.from(SRCBUF);
    other[other.indexOf(Buffer.from('5de62156575d737391ea42254e778a97dbbb51f8', 'hex'))] ^= 0xff;
    Buffer.from('e900000000', 'hex').copy(other, 0x9f9a0);
    fresh(other);
    r = hook('activate');
    ok('another build: activate leaves it, conflict, exit 0', r.code === 0 && disk() === sha(other) && !marker() &&
        r.result.conflict === 'untested carplay build' && !r.result.reboot && ro(), JSON.stringify(r.result));
    cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'apply', '--target', TGT, '--panel', 'mid', '--unproven', '--yes']);
    headUnitStart();
    r = hook('activate');
    ok('another build CarPlay\'s Patch button patched: activate writes CarPlay\'s Patch button\'s patch, pending', r.code === 0 && !r.result.conflict && marker() && marker().action === 'apply' &&
        slot() === 'bcf2f7ff' && /OK: variant IC_HIGH, sink original \(AVB 15\), self-start on, res 1088x480/.test(verify()) && ro(), JSON.stringify(r.result));
    r = hook('deactivate');
    ok('another build: deactivate restores it byte-exact', r.code === 0 && disk() === sha(other) && !fs.existsSync(BAK), JSON.stringify(r.result));

    /* No backup of ours left: the proven build is rebuilt from its own bytes
     * and the removal passes; another build cannot be, and the removal is
     * refused while our edits read in it. */
    fresh(SRCBUF);
    hook('activate');
    headUnitStart();
    [BAK, TGT + '.woc-orig.json'].forEach(function (f) { fs.unlinkSync(f); });
    r = hook('deactivate');
    ok('the proven build patched, no backup: deactivate restores it byte-exact, exit 0', r.code === 0 && disk() === sha(SRCBUF), JSON.stringify(r.result));
    fresh(other);
    cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'apply', '--target', TGT, '--panel', 'high', '--unproven', '--yes']);
    headUnitStart();
    [BAK, TGT + '.woc-orig.json'].forEach(function (f) { fs.unlinkSync(f); });
    var stillPatched = disk();
    r = hook('deactivate');
    ok('another build patched, no backup: deactivate refused, exit 1, our edits named, nothing written', r.code === 1 && disk() === stillPatched &&
        /CarPlay not undone \(our edits read in carplay\)/.test(r.result.status || ''), JSON.stringify(r.result));

    /* The pristine build with another tool's in-place edit and no backup of
     * ours: modified by something else. */
    var other = Buffer.from(SRCBUF);
    Buffer.from('e900000000', 'hex').copy(other, 0x9f9a0);
    fresh(other);
    r = hook('activate');
    ok('modified elsewhere: left alone, conflict, exit 0', r.code === 0 && disk() === sha(other) &&
        /modified by something else/.test(r.result.conflict || ''), JSON.stringify(r.result));

    /* carplay.service started through another program (another tool's
     * drop-in): the install does not write the patch; an environment-only
     * drop-in is fine. */
    fresh(SRCBUF);
    fs.mkdirSync(path.join(UNITS, 'carplay.service.d'));
    var dropin = path.join(UNITS, 'carplay.service.d', '60-other.conf');
    fs.writeFileSync(dropin, '[Service]\nExecStart=\nExecStart=/bin/sh -c \'exec /var/opt/other/carplay "$$@"\' -- -c /etc/cinemo/cinemo_options.xml\n');
    r = hook('activate');
    ok('foreign start: activate leaves the original, conflict names the drop-in', r.code === 0 && disk() === sha(SRCBUF) &&
        /carplay\.service starts \/bin\/sh/.test(r.result.conflict || '') && r.result.conflict.indexOf('60-other.conf') >= 0 && !r.result.reboot,
        JSON.stringify(r.result));
    st = await daemonStatus();
    ok('foreign start: patchd reports the conflict', st.state === 'original' && /60-other\.conf/.test(st.conflict || ''), JSON.stringify(st));
    fs.writeFileSync(dropin, '[Service]\nEnvironment=LD_PRELOAD=/usr/lib/libschedconfig.so\nExecStart=\nExecStart=' + TGT + ' -c /etc/cinemo/cinemo_options.xml\n');
    r = hook('activate');
    ok('own start: an environment drop-in that still starts carplay is fine', r.code === 0 && marker().action === 'apply' && !r.result.conflict, JSON.stringify(r.result));
    fs.unlinkSync(dropin);

    /* / cannot be remounted read-write: nothing written, exit non-zero. */
    fresh(SRCBUF);
    fs.writeFileSync(RW_FAILS, '');
    r = hook('activate');
    ok('remount fails: activate exits non-zero, nothing written, no marker', r.code !== 0 && disk() === sha(SRCBUF) && marker() === null &&
        /remount rw/.test(r.result.status || ''), JSON.stringify(r.result));
    fs.unlinkSync(RW_FAILS);

    /* A head unit that refuses our parts; a patch of ours from before is
     * left to boot recovery. */
    var NOT = 'not supported on this car (' + headUnit.REFUSED + ')';
    [['no kernel command line', null, 'not supported on this car (head unit: unknown variant is not supported (kernel command line unreadable))'],
     ['no hwvariant=', headUnit.NONE, NOT]].forEach(function (c) {
        fresh(SRCBUF);
        headUnit.set(c[1]);
        r = hook('activate');
        ok(c[0] + ': activate writes nothing, no backup, no marker, conflict', r.code === 0 && disk() === sha(SRCBUF) && !fs.existsSync(BAK) &&
            marker() === null && !r.result.reboot && r.result.conflict === c[2] && ro(), JSON.stringify(r.result));
        headUnit.set(headUnit.HIGH);
    });
    [['MID', headUnit.MID], ['HIGH F', headUnit.HIGHF]].forEach(function (v) {
        fresh(SRCBUF);
        headUnit.set(v[1]);
        r = hook('activate');
        ok(v[0] + ': activate patches as on HIGH, pending', r.code === 0 && !r.result.conflict && marker() && marker().action === 'apply' && slot() === 'bcf2f7ff',
            JSON.stringify(r.result));
        headUnit.set(headUnit.HIGH);
    });
    fresh(SRCBUF);
    headUnit.set(headUnit.NONE);
    st = await daemonStatus();
    ok('no hwvariant=: patchd reports the original and the conflict', st.state === 'original' && st.conflict === NOT, JSON.stringify(st));
    m = await daemonCommand({ cmd: 'apply', panel: 'high', mode: 'avb' }, 'refused');
    ok('no hwvariant=: patchd refuses CarPlay\'s Patch button, nothing written', m.reason === headUnit.REFUSED &&
        disk() === sha(SRCBUF) && !fs.existsSync(BAK) && marker() === null && ro(), JSON.stringify(m));
    headUnit.set(headUnit.HIGH);
    hook('activate');
    headUnitStart();
    patchedIno = ino(TGT);
    headUnit.set(headUnit.NONE);
    r = hook('activate');
    ok('no hwvariant=, patched before: activate leaves it, conflict, nothing pending', r.code === 0 && ino(TGT) === patchedIno && marker() === null &&
        r.result.conflict === NOT, JSON.stringify(r.result));
    r = hook('deactivate');
    ok('no hwvariant=, patched before: deactivate reverts it, drops the backup', r.code === 0 && disk() === sha(SRCBUF) && !fs.existsSync(BAK) &&
        marker() && marker().action === 'revert' && ro(), JSON.stringify(r.result));
    headUnit.set(headUnit.HIGH);

    /* A car 0.8.2's boot recovery restored on HIGH F: the original with our
     * trusted backup. The first install on HIGH F applies the patch again,
     * pending, and marks CarPlay done; none after it, nor on HIGH, nor over
     * an untrusted backup, which stays unmarked for the next install. */
    var MARK = path.join(ENV.WOC_STATE_DIR, 'highf-repatched');
    function restored(unit) {
        try { fs.unlinkSync(MARK); } catch (e) {}
        headUnit.set(headUnit.HIGH);
        fresh(SRCBUF);
        hook('activate');
        headUnitStart();
        cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'revert', '--target', TGT, '--yes']);
        headUnitStart();
        headUnit.set(unit);
    }
    restored(headUnit.HIGHF);
    r = hook('activate');
    m = marker();
    var mark = null;
    try { mark = JSON.parse(fs.readFileSync(MARK, 'utf8')); } catch (e) {}
    ok('HIGH F restored by 0.8.2: the install applies the patch again, pending, and marks it done', r.code === 0 && m && m.action === 'apply' &&
        slot() === 'bcf2f7ff' && r.result.reset === true && mark && mark.carplay === 'applied', JSON.stringify([r.result, mark]));
    st = await daemonStatus();
    ok('HIGH F restored by 0.8.2: patchd reports the pending apply', st.pending === 'apply' && !st.conflict, JSON.stringify(st));
    headUnitStart();
    cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'revert', '--target', TGT, '--yes']);
    headUnitStart();
    r = hook('activate');
    ok('HIGH F, marker present: a Remove patch since stays', r.code === 0 && disk() === sha(SRCBUF) && marker() === null && /turned off/.test(r.result.status),
        JSON.stringify(r.result));
    restored(headUnit.HIGH);
    r = hook('activate');
    ok('HIGH: an original with our backup stays off, no marker', r.code === 0 && disk() === sha(SRCBUF) && marker() === null &&
        /turned off/.test(r.result.status) && !fs.existsSync(MARK), JSON.stringify(r.result));
    restored(headUnit.HIGHF);
    fs.writeFileSync(TGT + '.woc-orig.json', JSON.stringify({ sha256_original: '0'.repeat(64) }));
    r = hook('activate');
    ok('HIGH F, untrusted backup: nothing written, CarPlay left for the next install', r.code === 0 && disk() === sha(SRCBUF) && marker() === null &&
        !fs.existsSync(MARK), JSON.stringify(r.result));
    restored(headUnit.HIGHF);
    fs.unlinkSync(TGT + '.woc-orig.json');
    r = hook('activate');
    ok('HIGH F, a backup without its record: nothing written', r.code === 0 && disk() === sha(SRCBUF) && marker() === null, JSON.stringify(r.result));
    try { fs.unlinkSync(MARK); } catch (e) {}
    headUnit.set(headUnit.HIGH);

    /* The cluster coding. An install patches only a car coded
     * KI_KOMBI_VARIANT 7, and its first patch there is on trial (unproven
     * until boot recovery's watcher sees it run healthy). On another coding,
     * or one that cannot be read, it writes nothing and reports the conflict
     * the app shows, also on HIGH F over 0.8.2's restore; a patch CarPlay's
     * Patch button wrote there is left as it is. */
    var record = require('../patch_record.js');
    function rec() { var x = record.current(TGT); return x ? { patched: x.patched, proven: x.proven, trial: x.trial } : null; }
    fresh(SRCBUF);
    r = hook('activate');
    ok('coding 7: the install patches, pending, the record on trial', r.code === 0 && !r.result.conflict && marker() && marker().action === 'apply' &&
        JSON.stringify(rec()) === JSON.stringify({ patched: true, proven: false, trial: true }), JSON.stringify([r.result, rec()]));
    headUnitStart();
    r = hook('activate');
    ok('coding 7: the next install leaves the trial as it is', r.code === 0 && !r.result.reboot &&
        JSON.stringify(rec()) === JSON.stringify({ patched: true, proven: false, trial: true }), JSON.stringify([r.result, rec()]));
    /* A car an earlier release patched and recorded proven: an update keeps
     * it proven, also when it brings the patch up to date. */
    record.write(TGT, { patched: true, proven: true });
    r = hook('activate');
    ok('migration: a patch recorded proven by an earlier release stays proven', r.code === 0 && !r.result.reboot &&
        JSON.stringify(rec()) === JSON.stringify({ patched: true, proven: true }), JSON.stringify([r.result, rec()]));
    var olderPatch = Buffer.from(SRCBUF);
    olderPatch.writeUInt32LE(0xfff7f2bc >>> 0, SLOT);
    Buffer.from('e89f000000', 'hex').copy(olderPatch, 0x18176c);
    fs.writeFileSync(TGT + '.older', olderPatch, { mode: 0o755 });
    fs.renameSync(TGT + '.older', TGT);
    record.write(TGT, { patched: true, proven: true });
    headUnitStart();
    r = hook('activate');
    ok('migration: an older patch recorded proven, brought up to date, stays proven', r.code === 0 && marker() && marker().action === 'apply' &&
        /resolution -> 1088x480/.test(r.out) && JSON.stringify(rec()) === JSON.stringify({ patched: true, proven: true }), JSON.stringify([r.result, rec()]));

    var CODINGS = [['coding 9', 9, undefined, 'not supported on this car (KI_KOMBI_VARIANT 9)'],
        ['invalid coding flags', 7, 0x0b, 'cluster coding unreadable'], ['no coding', null, undefined, 'cluster coding unreadable']];
    for (var k = 0; k < CODINGS.length; k++) {
        var c = CODINGS[k];
        fresh(SRCBUF);
        headUnit.coding(c[1], c[2]);
        r = hook('activate');
        ok(c[0] + ': the install writes nothing, no backup, no marker, the conflict', r.code === 0 && disk() === sha(SRCBUF) && !fs.existsSync(BAK) &&
            marker() === null && !r.result.reboot && r.result.conflict === c[3] && rec() === null && ro(), JSON.stringify(r.result));
        st = await daemonStatus();
        ok(c[0] + ': patchd reports the original and the conflict', st.state === 'original' && st.conflict === c[3], JSON.stringify(st));
        /* CarPlay's Patch button (patchd runs the patcher with --unproven). */
        cp.execFileSync(process.execPath, [path.join(__dirname, '..', 'carplay_patch.js'), 'apply', '--target', TGT, '--panel', 'high', '--unproven', '--yes']);
        headUnitStart();
        before = disk();
        r = hook('activate');
        var buttonRec = c[1] === 9 ? { patched: true, proven: false } : { patched: true, proven: false, trial: true };
        ok(c[0] + ': a patch CarPlay\'s Patch button wrote: the install leaves it and its record (' + (buttonRec.trial ? 'on trial' : 'unproven') + ')', r.code === 0 &&
            disk() === before && marker() === null && r.result.conflict === c[3] && JSON.stringify(rec()) === JSON.stringify(buttonRec), JSON.stringify([r.result, rec()]));
        r = hook('deactivate');
        ok(c[0] + ': the removal reverts it as ever', r.code === 0 && disk() === sha(SRCBUF) && !fs.existsSync(BAK) && marker() && marker().action === 'revert' && ro(),
            JSON.stringify(r.result));
        headUnit.coding(7);
        restored(headUnit.HIGHF);
        ok(c[0] + ', HIGH F restored by 0.8.2: the original with our trusted backup', disk() === sha(SRCBUF) && fs.existsSync(BAK) && marker() === null);
        headUnit.coding(c[1], c[2]);
        r = hook('activate');
        ok(c[0] + ', HIGH F restored by 0.8.2: the re-apply writes nothing, the conflict, left for the next install', r.code === 0 && disk() === sha(SRCBUF) &&
            marker() === null && !r.result.reset && r.result.conflict === c[3] && !fs.existsSync(MARK), JSON.stringify(r.result));
        headUnit.coding(7);
        r = hook('activate');
        ok(c[0] + ', HIGH F restored by 0.8.2: coded 7 again, the next install applies it, on trial', r.code === 0 && r.result.reset === true &&
            marker() && marker().action === 'apply' && JSON.stringify(rec()) === JSON.stringify({ patched: true, proven: false, trial: true }), JSON.stringify([r.result, rec()]));
        try { fs.unlinkSync(MARK); } catch (e) {}
        headUnit.set(headUnit.HIGH);
    }

    /* A car coded otherwise on which an earlier release's install wrote the
     * patch and recorded it proven: while that patch has not started (its
     * apply pending, or carplay running the file it replaced), an install
     * puts its record on trial, carplay's bytes as they are. A patch that
     * has started, and a variant-7 car's pending one, keep their record; a
     * write that fails leaves it. */
    var RECORD = TGT + '.woc-patch.json';
    function earlierRelease(pending) {
        headUnit.coding(7);
        fresh(SRCBUF);
        hook('activate');
        if (!pending) fs.unlinkSync(PENDING);
        record.write(TGT, { patched: true, proven: true });
    }
    var TRIAL = JSON.stringify({ patched: true, proven: false, trial: true }), PROVEN = JSON.stringify({ patched: true, proven: true, trial: undefined });
    [['its apply pending', true], ['carplay running the file it replaced, no marker', false]].forEach(function (c) {
        earlierRelease(c[1]);
        before = disk();
        headUnit.coding(9);
        r = hook('activate');
        ok('coding 9, an earlier release\'s patch not started (' + c[0] + '): its record on trial, carplay as it was, / read-only', r.code === 0 && disk() === before &&
            JSON.stringify(rec()) === TRIAL && (marker() !== null) === c[1] && ro() && /recorded on trial/.test(r.out), JSON.stringify([r.result, rec()]));
    });
    earlierRelease(true);
    var recBefore = fs.readFileSync(RECORD, 'utf8');
    headUnit.coding(9);
    fs.writeFileSync(RW_FAILS, '');
    r = hook('activate');
    fs.unlinkSync(RW_FAILS);
    ok('coding 9, the record\'s write fails: the record as it was, exit 0', r.code === 0 && fs.readFileSync(RECORD, 'utf8') === recBefore && JSON.stringify(rec()) === PROVEN,
        JSON.stringify([r.result, rec()]));
    earlierRelease(true);
    headUnitStart();
    recBefore = fs.readFileSync(RECORD, 'utf8');
    headUnit.coding(9);
    r = hook('activate');
    ok('coding 9, an earlier release\'s patch that started: its proven record untouched, the conflict', r.code === 0 && fs.readFileSync(RECORD, 'utf8') === recBefore &&
        r.result.conflict === 'not supported on this car (KI_KOMBI_VARIANT 9)', JSON.stringify([r.result, rec()]));
    earlierRelease(true);
    recBefore = fs.readFileSync(RECORD, 'utf8');
    r = hook('activate');
    ok('coding 7, an earlier release\'s patch waiting for its start: its proven record untouched', r.code === 0 && fs.readFileSync(RECORD, 'utf8') === recBefore &&
        marker() && marker().action === 'apply', JSON.stringify([r.result, rec()]));
    headUnit.coding(7);

    ok('never a systemctl, kill or reboot', calls().length === 0, calls().join('; '));
}

main().catch(function (e) { ok('no exception', false, e.stack); }).then(function () {
    process.stdout.write(fails ? fails + ' FAILED\n' : 'all passed\n');
    process.exit(fails ? 1 : 0);
});
