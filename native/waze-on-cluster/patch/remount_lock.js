// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/* A single advisory lock shared by patchd and the boot-recovery service so the
 * two never remount / read-write and rewrite /usr/bin/carplay at the same time.
 * Holding it around a remount-rw -> write -> remount-ro window keeps a
 * concurrent operation from writing while the other has already put / back to
 * read-only, which would fail the write and could leave / writable.
 *
 * The lock is a file created with O_EXCL in the socket directory (tmpfs,
 * cleared each boot). A lock older than STALE_MS is taken as abandoned by a
 * killed holder and broken. Every writer gives up without it: a patchd change
 * or hook waits a bounded time, then writes nothing and reports it; recovery
 * yields: its restore reports a failure and keeps the counter, so the next boot
 * retries.
 *
 * inUse(file) is whether a process runs file (a /proc/<pid>/exe is its
 * inode): such a file, once replaced, has to stay linked for / to go
 * read-only again. holds(link, target) is whether link keeps such a
 * replaced binary: a process runs it and it is not target's own file
 * (which target keeps linked).
 *
 * Node 8 on the unit. No dependencies.
 */
var fs = require('fs');
var path = require('path');

var LOCK = process.env.WOC_REMOUNT_LOCK || '/dev/shm/waze-on-cluster/remount.lock';
var STALE_MS = parseInt(process.env.WOC_REMOUNT_LOCK_STALE_MS, 10) || 120000;

function ensureDir(dir) {
    try { fs.mkdirSync(dir, 0o755); } catch (e) { if (e.code !== 'EEXIST') throw e; }
}

function tryAcquire() {
    var fd;
    try {
        fd = fs.openSync(LOCK, 'wx');
        fs.writeSync(fd, process.pid + ' ' + new Date().toISOString() + '\n');
        fs.closeSync(fd);
        return true;
    } catch (e) {
        if (e.code === 'ENOENT') { try { ensureDir(path.dirname(LOCK)); } catch (e2) { return false; } return tryAcquire(); }
        if (e.code !== 'EEXIST') return false;
        try {
            var st = fs.statSync(LOCK);
            if (Date.now() - st.mtime.getTime() > STALE_MS) { fs.unlinkSync(LOCK); return tryAcquire(); }
        } catch (e3) {}
        return false;
    }
}

/* acquire within timeoutMs, polling; cb(true) on success, cb(false) on timeout. */
function acquire(timeoutMs, cb) {
    var deadline = Date.now() + timeoutMs;
    (function attempt() {
        if (tryAcquire()) { cb(true); return; }
        if (Date.now() >= deadline) { cb(false); return; }
        setTimeout(attempt, 100);
    })();
}

/* Release only a lock this process holds, so a process that gave up waiting
 * cannot delete the holder's lock. */
function release() {
    try {
        if ((fs.readFileSync(LOCK, 'utf8').split(' ')[0] || '') === String(process.pid)) fs.unlinkSync(LOCK);
    } catch (e) {}
}

var PROC = process.env.WOC_PROC || '/proc';
function hex2(n) { return (n < 16 ? '0' : '') + n.toString(16); }
/* A process runs the inode `st` (a stat of it, the file maybe unlinked
 * since): its exe is the inode, or it maps it, as a binary translator
 * runs one, its exe being the translator. */
function runs(st) {
    var mapped = ' ' + hex2((st.dev >> 8) & 0xfff) + ':' + hex2((st.dev & 0xff) | ((st.dev >> 12) & 0xfff00)) + ' ' + st.ino + ' ';
    var pids = [];
    try { pids = fs.readdirSync(PROC).filter(function (n) { return /^\d+$/.test(n); }); } catch (e) {}
    return pids.some(function (pid) {
        try { var x = fs.statSync(path.join(PROC, pid, 'exe')); if (x.ino === st.ino && x.dev === st.dev) return true; } catch (e) {}
        try { return fs.readFileSync(path.join(PROC, pid, 'maps'), 'latin1').indexOf(mapped) >= 0; } catch (e) { return false; }
    });
}

function inUse(file) {
    try { return runs(fs.statSync(file)); } catch (e) { return false; }
}

function holds(link, target) {
    var a, b;
    try { a = fs.statSync(link); } catch (e) { return false; }
    try { b = fs.statSync(target); } catch (e) { b = null; }
    return !(b && a.ino === b.ino && a.dev === b.dev) && inUse(link);
}

module.exports = { acquire, release, tryAcquire, runs, inUse, holds };
