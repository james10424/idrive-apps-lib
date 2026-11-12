#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Every released carplay patcher against this one (PATCH.md "Rollback"): each
 * release's own carplay_patch.js and boot recovery, taken from its release
 * tag, acting on a carplay this version patched, and this version acting on
 * a carplay each release patched. Boot recovery is each release's own
 * restore code (what its boot step and watcher run once carplay failed
 * enough), called directly; the crash count that leads there is
 * recovery_test.js's.
 *
 * The releases, grouped by what their patcher and boot recovery do:
 *   A 0.5.0-0.6.3  fixed 22.28.3 offsets; revert restores the edits' bytes
 *                  of a file it recognises, refuses one it does not; boot
 *                  recovery runs that revert
 *   B 0.7.0-0.7.6  located sites, else as A
 *   C 0.7.7-0.8.4  revert writes the whole backup over a file it does not
 *                  recognise; boot recovery runs that revert
 *   D 0.8.5-0.9.10 boot recovery writes the whole trusted backup
 *   E 0.9.11       D, and knows the map URL edit at carplay 22.28.3's
 *                  offsets
 *   F 0.9.12-      E, and writes the map URL edit wherever its sites are
 *                  found
 * Groups A-D know four edits, E and F five.
 *
 * With CARPLAY_BIN every release runs on a copy of carplay 22.28.3; without
 * it B-F run on a synthetic build (synthetic_carplay.js), A not at all.
 * The release tags come from the repository's git, fetched when missing.
 *
 *   [CARPLAY_BIN=<carplay 22.28.3>] node rollback_test.js
 */
'use strict';

var car = require('./head_unit_env.js');   /* a HIGH head unit coded KI_KOMBI_VARIANT 7 */
var fs = require('fs');
var os = require('os');
var path = require('path');
var cp = require('child_process');
var synthetic = require('./synthetic_carplay.js');
var locator = require('../carplay_locate.js');

var NEW = path.join(__dirname, '..');
var REPO = path.join(__dirname, '..', '..', '..', '..');
var BIN = process.env.CARPLAY_BIN || '';

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}
function git(args) { return cp.execFileSync('git', args, { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }); }

/* ---- the releases ------------------------------------------------------- */

function semver(v) { return v.split('.').map(Number); }
function cmp(a, b) { a = semver(a); b = semver(b); for (var i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] - b[i]; return 0; }
function group(v) {
    return cmp(v, '0.6.3') <= 0 ? 'A' : cmp(v, '0.7.6') <= 0 ? 'B' : cmp(v, '0.8.4') <= 0 ? 'C' : cmp(v, '0.9.10') <= 0 ? 'D' :
        cmp(v, '0.9.11') <= 0 ? 'E' : 'F';
}

function tags() {
    try { return git(['tag', '-l', 'waze-on-cluster@*']).split('\n').filter(Boolean); } catch (e) { return null; }
}
var list = tags();
if (list === null) { process.stdout.write('SKIP: not in a git checkout\n'); process.exit(0); }
if (!list.length) {
    /* a shallow checkout (CI) fetches them shallow; a full clone stays full */
    var shallow = false;
    try { shallow = git(['rev-parse', '--is-shallow-repository']).trim() === 'true'; } catch (e) {}
    try { git(['fetch', '--no-tags'].concat(shallow ? ['--depth=1'] : [], ['origin', '+refs/tags/waze-on-cluster@*:refs/tags/waze-on-cluster@*'])); } catch (e) {}
    list = tags() || [];
}
ok('the release tags are at hand', list.length > 0, 'no waze-on-cluster@* tag, and fetching them failed');
if (!list.length) process.exit(1);

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-rollback-'));
process.on('exit', function () { try { cp.execFileSync('rm', ['-rf', dir]); } catch (e) {} });
var RELEASES = list.map(function (t) { return t.slice('waze-on-cluster@'.length); }).filter(function (v) { return /^\d+\.\d+\.\d+$/.test(v); })
    .sort(cmp).filter(function (v) { return BIN || group(v) !== 'A'; });
RELEASES.forEach(function (v) {
    var d = path.join(dir, 'rel', v);
    fs.mkdirSync(d, { recursive: true });
    cp.execSync('git archive "waze-on-cluster@' + v + '" native/waze-on-cluster/patch | tar -x -C "' + d + '" --strip-components=3', { cwd: REPO });
});

/* ---- a world: carplay, its backup and the fakes the patchers and boot
 * recovery run against ---------------------------------------------------- */

var STOCK = BIN ? fs.readFileSync(BIN) : synthetic.build({}).buf;
var W = path.join(dir, 'world'), TGT = path.join(W, 'carplay');
fs.mkdirSync(path.join(W, 'proc'), { recursive: true });
var MOUNTS = path.join(W, 'mounts');
fs.writeFileSync(MOUNTS, '/dev/root / ext4 ro 0 0\n');
fs.writeFileSync(path.join(W, 'mount'), '#!/bin/sh\ncase "$*" in *remount,rw*) echo "/dev/root / ext4 rw 0 0" > "' + MOUNTS + '";; ' +
    '*remount,ro*) echo "/dev/root / ext4 ro 0 0" > "' + MOUNTS + '";; esac\n', { mode: 0o755 });
var ENV = Object.assign({}, process.env, {
    WOC_CARPLAY: TGT, WOC_MOUNT: path.join(W, 'mount'), WOC_PROC_MOUNTS: MOUNTS, WOC_PROC: path.join(W, 'proc'),
    WOC_REMOUNT_LOCK: path.join(W, 'remount.lock'), WOC_PROC_STAT: path.join(W, 'no-stat'), WOC_SYSTEMCTL: '/usr/bin/false',
    WOC_RECOVERY_STATE: path.join(W, 'recovery.json'), WOC_PATCH_SOCK: path.join(W, 'patch.sock'),
});
var FILES = ['', '.woc-orig', '.woc-orig.json', '.woc-patch.json', '.woc-running'];
function clear() { FILES.forEach(function (s) { try { fs.unlinkSync(TGT + s); } catch (e) {} }); }
function snapshot() { var o = {}; FILES.forEach(function (s) { try { o[s] = fs.readFileSync(TGT + s); } catch (e) {} }); return o; }
function put(snap) { clear(); Object.keys(snap).forEach(function (s) { fs.writeFileSync(TGT + s, snap[s], { mode: s ? 0o600 : 0o755 }); }); }

function patcher(d, args) {
    var r = cp.spawnSync(process.execPath, [path.join(d, 'carplay_patch.js')].concat(args), { encoding: 'utf8', env: ENV });
    return { code: r.status, out: (r.stdout || '') + (r.stderr || '') };
}
/* The patch each version writes: the app's (high panel, stock AVB sink),
 * --unproven on a synthetic build (Patch button). */
function apply(d) { return patcher(d, ['apply', '--panel', 'high', '--target', TGT, '--yes'].concat(BIN ? [] : ['--unproven'])); }
function removePatch(d) { return patcher(d, ['revert', '--target', TGT, '--yes']); }
function uninstall(d) { return patcher(d, ['revert', '--target', TGT, '--yes', '--drop-backup']); }

/* Boot recovery's restore of each release, as its boot step and watcher run
 * it once carplay failed enough: D-F plan from the trusted backup
 * (restoreTarget), C and earlier classify carplay with their patcher's
 * status and, when it is restorable, run their realRestore. */
var DRIVER = path.join(dir, 'recover.js');
fs.writeFileSync(DRIVER, [
    '"use strict";',
    'var d = process.argv[2], g = process.argv[3], cp = require("child_process"), rec = require(d + "/recovery.js");',
    'function done(e, r) { process.stdout.write("RESULT " + (e ? "error " + e.message : "ok " + (r || "")) + "\\n"); process.exit(0); }',
    'if (g === "D" || g === "E" || g === "F" || g === "N") {',
    '    var t = rec.TARGETS.carplay;',
    '    rec.restoreTarget(t, false, 3, done);',
    '} else {',
    '    var out = cp.spawnSync(process.execPath, [d + "/carplay_patch.js", "status", "--target", process.env.WOC_CARPLAY], { encoding: "utf8" }).stdout || "";',
    '    var restorable = g === "C" ? /^restorable:\\s+yes$/m.test(out) : /variant:\\s+PATCHED/.test(out) && !/known:\\s+NO/.test(out) && /^backup:\\s+\\//m.test(out);',
    '    if (!restorable) { process.stdout.write("RESULT not restorable\\n"); process.exit(0); }',
    '    if (rec.realRestore.length >= 2) rec.realRestore(false, done); else rec.realRestore(done);',
    '}'].join('\n'));
function recover(d, g) {
    var r = cp.spawnSync(process.execPath, [DRIVER, d, g], { encoding: 'utf8', env: ENV, timeout: 60000 });
    var m = /^RESULT (.*)$/m.exec(r.stdout || '');
    return m ? m[1] : 'no result: ' + ((r.stdout || '') + (r.stderr || '')).slice(-200);
}

/* ---- what carplay holds ------------------------------------------------- */

var REF = locator.locate(STOCK).mapurl;
if (!REF.ok) { process.stdout.write('FAIL the map URL edit is not located in the build the test runs on: ' + REF.reason + '\n'); process.exit(1); }
var MAPURL = [[REF.slot, REF.len], [REF.lea, 7]];
function inMap(i) { return MAPURL.some(function (m) { return i >= m[0] && i < m[0] + m[1]; }); }
/* 'stock'; 'map URL left' (every byte stock but some of the map URL edit's);
 * 'patched' (other bytes changed) */
function holds() {
    var b = fs.readFileSync(TGT);
    if (b.equals(STOCK)) return 'stock';
    for (var i = 0; i < b.length; i++) if (b[i] !== STOCK[i] && !inMap(i)) return 'patched';
    return 'map URL left';
}
function mapUrlIn() { var b = fs.readFileSync(TGT); return MAPURL.every(function (m) { return !b.slice(m[0], m[0] + m[1]).equals(STOCK.slice(m[0], m[0] + m[1])); }); }

/* ---- the patches -------------------------------------------------------- */

clear(); fs.writeFileSync(TGT, STOCK, { mode: 0o755 });
var r = apply(NEW);
ok('this version patches the build, the map URL edit included', r.code === 0 && mapUrlIn(), r.out);
var NEW_PATCH = snapshot();

/* Whether a version writes the map URL edit on the build the test runs on:
 * 0.9.11 knows it at carplay 22.28.3's offsets only, 0.9.12 and later
 * wherever its sites are found. */
function knowsMapUrl(v) { return v === 'this' || group(v) === 'F' || (group(v) === 'E' && !!BIN); }

var table = [];
RELEASES.forEach(function (v) {
    var d = path.join(dir, 'rel', v), g = group(v), row = { v: v, g: g };

    /* 1. The deploy tool's rollback, downgrade or removal of this version:
     * its deactivate first, then the release's activate; a crash loop under
     * the release; its Remove patch; its removal. */
    put(NEW_PATCH);
    r = uninstall(NEW);
    var deact = r.code === 0 && holds() === 'stock' && !fs.existsSync(TGT + '.woc-orig');
    r = apply(d);
    var relPatched = r.code === 0 && holds() === 'patched' && mapUrlIn() === knowsMapUrl(v);
    var relPatch = snapshot();
    var rec = recover(d, g), recStock = holds() === 'stock';
    put(relPatch); r = removePatch(d); var rmStock = r.code === 0 && holds() === 'stock';
    put(relPatch); r = uninstall(d); var unStock = r.code === 0 && holds() === 'stock';
    ok(v + ' (' + g + ') after this version\'s deactivate: its patch, a crash loop ends stock, Remove patch and removal stock',
        deact && relPatched && recStock && rmStock && unStock, [deact, relPatched, rec, recStock, rmStock, unStock].join(' '));

    /* 2. This version over the release's patch (an update, one skipping
     * versions, the same patch installed again, an update cut short): a
     * crash loop, Remove patch, removal, and its activate. */
    put(relPatch);
    rec = recover(NEW, 'N');
    var newRec = holds() === 'stock';
    put(relPatch); r = removePatch(NEW); var newRm = r.code === 0 && holds() === 'stock';
    put(relPatch); r = uninstall(NEW); var newUn = r.code === 0 && holds() === 'stock';
    put(relPatch); r = apply(NEW); var newUp = r.code === 0 && holds() === 'patched' && mapUrlIn();
    r = removePatch(NEW); var newUpRm = r.code === 0 && holds() === 'stock';
    ok(v + ' (' + g + ')\'s patch under this version: a crash loop ends stock, Remove patch and removal stock, an update adds the map URL edit and reverts whole',
        newRec && newRm && newUn && newUp && newUpRm, [rec, newRec, newRm, newUn, newUp, newUpRm].join(' '));

    /* 3. The release over this version's patch with no deactivate in
     * between (the open paths PATCH.md "Rollback" lists): what it does. */
    put(NEW_PATCH); rec = recover(d, g); row.recovery = holds() + (/error|not restorable/.test(rec) ? ' (' + rec + ')' : '');
    put(NEW_PATCH); r = removePatch(d); row.remove = r.code ? 'refused' : holds();
    put(NEW_PATCH); r = uninstall(d); row.uninstall = r.code ? 'refused' : holds();
    put(NEW_PATCH); r = apply(d); row.reapply = r.code ? 'refused' : holds() + (mapUrlIn() ? ', map URL kept' : '');
    var wantRec = /^[DEF]$/.test(g) ? 'stock' : 'map URL left', wantRm = knowsMapUrl(v) ? 'stock' : 'map URL left';
    ok(v + ' (' + g + ') over this version\'s full patch, no deactivate between: crash loop ' + wantRec + ', Remove patch and removal ' + wantRm +
        ', its activate keeps the map URL edit', row.recovery === wantRec && row.remove === wantRm && row.uninstall === wantRm &&
        row.reapply === 'patched, map URL kept', JSON.stringify(row));
    table.push(row);
});

/* 4. Release to release, both ways and to itself: the first and last
 * release of each group and this version. The one that patched is P, the
 * one installed after it Q, with no deactivate between: a crash loop under
 * Q, its Remove patch and its activate. Where P wrote the map URL edit and
 * Q does not know it, part 3 says what Q does. */
var REPS = [];
['A', 'B', 'C', 'D', 'E', 'F'].forEach(function (g) {
    var inG = RELEASES.filter(function (v) { return group(v) === g; });
    if (inG.length) REPS.push(inG[0]);
    if (inG.length > 1) REPS.push(inG[inG.length - 1]);
});
REPS.push('this');
function relDir(v) { return v === 'this' ? NEW : path.join(dir, 'rel', v); }
function relGroup(v) { return v === 'this' ? 'N' : group(v); }
REPS.forEach(function (p) {
    clear(); fs.writeFileSync(TGT, STOCK, { mode: 0o755 });
    r = apply(relDir(p));
    var pPatch = snapshot(), withMap = mapUrlIn();
    ok(p + ' patches', r.code === 0 && holds() === 'patched', r.out);
    REPS.forEach(function (q) {
        var qd = relDir(q), left = withMap && !knowsMapUrl(q);
        put(pPatch); var rec = recover(qd, relGroup(q)), recH = holds();
        put(pPatch); r = removePatch(qd); var rmH = r.code ? 'refused' : holds();
        put(pPatch); r = apply(qd); var upOk = r.code === 0 && holds() === 'patched';
        var wantRec = !left || /^[DEFN]$/.test(relGroup(q)) ? 'stock' : 'map URL left', wantRm = left ? 'map URL left' : 'stock';
        ok(p + '\'s patch under ' + q + ': a crash loop ends ' + wantRec + ', Remove patch ' + wantRm + ', its activate patches',
            recH === wantRec && rmH === wantRm && upOk, [rec, recH, rmH, upOk].join(' '));
    });
});

process.stdout.write('\nA release over this version\'s full patch with no deactivate between (part 3):\n');
table.forEach(function (row) {
    process.stdout.write('  ' + (row.v + ' (' + row.g + ')          ').slice(0, 14) + ' crash loop: ' + row.recovery + '; Remove patch: ' + row.remove +
        '; removal: ' + row.uninstall + '; activate: ' + row.reapply + '\n');
});
process.stdout.write((BIN ? '' : '\n(group A needs CARPLAY_BIN)\n') + (fails ? ('\n' + fails + ' check(s) failed\n') : '\nall checks passed\n'));
process.exit(fails ? 1 : 0);
