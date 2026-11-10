// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/* <target>.woc-patch.json: what this tool last wrote to a binary it patches
 * (carplay, aapd) and which file that is, so boot recovery knows from a
 * stat and this small file, without reading the binary, whether the file
 * about to start is our patch and whether its build is proven:
 *
 *   {"patched": true|false, "proven": true|false, "ino", "size", "mtime"
 *    (ms), for carplay's patch "trial": true while a patch of a proven build
 *    has not yet run healthy on this car, and after a revert
 *    of one that ran healthy "healthy": true, and, after boot recovery's
 *    restore, "by": "recovery" or "refused", "at" (ISO time), and with
 *    "recovery" "fails"}
 *
 * Written by the patchers after each write to the target (and by an apply
 * or revert that finds nothing to do when the file has changed since the
 * record was written, or there is none), by boot recovery's watcher, which
 * records a patch on trial proven once the service has run it healthy, and
 * by boot recovery's restore,
 * with "by": "recovery" after failed starts or "refused" on a head unit
 * that refuses our parts; removed with the backup. It
 * describes the target only while its inode, size and modification time are
 * the ones recorded (beside the target, on its file system): anything else
 * that writes the file changes one of them. Every write is a temporary file, fsynced, renamed over it, then the
 * folder fsynced.
 *
 * Node 8 on the unit. No dependencies.
 */
var fs = require('fs');
var path = require('path');

function recordPath(target) { return target + '.woc-patch.json'; }

function same(rec, st) {
    return rec.ino === st.ino && rec.size === st.size && rec.mtime === st.mtime.getTime();
}

/* The record, while it describes target as it is now; else null. */
function current(target) {
    var rec, st;
    try { rec = JSON.parse(fs.readFileSync(recordPath(target), 'utf8')); st = fs.statSync(target); } catch (e) { return null; }
    return rec && typeof rec === 'object' && typeof rec.patched === 'boolean' && same(rec, st) ? rec : null;
}

function fsyncPath(p) {
    var fd = fs.openSync(p, 'r');
    try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
}

/* Records target as it is now with fields ({patched, proven, ...}). Throws
 * when it cannot. */
function write(target, fields) {
    var st = fs.statSync(target), file = recordPath(target), tmp = file + '.tmp';
    var rec = Object.assign({}, fields, { ino: st.ino, size: st.size, mtime: st.mtime.getTime() });
    try {
        fs.writeFileSync(tmp, JSON.stringify(rec) + '\n', { mode: 0o644 });
        fsyncPath(tmp);
        fs.renameSync(tmp, file);
    } catch (e) {
        try { fs.unlinkSync(tmp); } catch (e2) {}
        throw e;
    }
    try { fsyncPath(path.dirname(file)); } catch (e) {}
}

/* write() unless the record already describes target with these fields. A
 * failure is a warning on stderr, not an error: boot recovery then treats
 * the binary as one it cannot vouch for. */
function ensure(target, fields) {
    var rec = current(target);
    if (rec && Object.keys(fields).every(function (k) { return rec[k] === fields[k]; })) return;
    try { write(target, fields); } catch (e) { process.stderr.write('warning: patch record not written: ' + e.message + '\n'); }
}

function remove(target) {
    try { fs.unlinkSync(recordPath(target)); } catch (e) { if (e.code !== 'ENOENT') throw e; }
}

module.exports = { recordPath: recordPath, current: current, write: write, ensure: ensure, remove: remove, fsyncPath: fsyncPath };
