#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Proves carplay_patch.js against the unit's carplay binary (CARPLAY_BIN): the
 * located sites are the documented ones, the proven build's SHA gate, variant
 * apply (high and mid), the sink redirect, the self-start call retarget, the
 * resolution geometry and safe-area rewrite, region-bounded diffs, idempotence,
 * verify, partial patches, byte-exact revert of all four edits, a build not in
 * the proven list, a read-only probe, the cluster-coding gate, and the
 * patch record's proven and trial. Without CARPLAY_BIN it skips.
 *
 *   node patch_test.js
 */
'use strict';

var car = require('./head_unit_env.js');   /* a HIGH head unit, coded KI_KOMBI_VARIANT 7, for every patcher and patchd this runs */
var record = require('../patch_record.js');

var fs = require('fs');
var os = require('os');
var path = require('path');
var crypto = require('crypto');
var cp = require('child_process');

var SCRIPT = path.join(__dirname, '..', 'carplay_patch.js');
var SRC = process.env.CARPLAY_BIN || '';
var ORIG_SHA = 'ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50';
var SLOT = 0x2ba4f0, SINK = 0x2a20b0, SINK_LEN = 59;
/* Edit 3 (self-start): the 5-byte call at 0x18176c, stock -> setStreamStatus,
 * patched -> requestStream. Only its 4-byte rel32 differs. */
var CALL = 0x18176c, CALL_LEN = 5;
var CALL_STOCK = 'e8effdffff', CALL_SELF = 'e89f000000';
function callBytes(buf) { return buf.slice(CALL, CALL + CALL_LEN).toString('hex'); }
/* Edit 4 (resolution): the four movabs immediates of the IC_HIGH geometry
 * builder. Only the 8 immediate bytes after each 2-byte opcode change. */
var RES_OFFS = [0x20ca30, 0x20ca52, 0x20ca64, 0x20ca72];
function resImmediates(buf) { return RES_OFFS.map(function (o) { return buf.slice(o, o + 10).toString('hex'); }); }
var RES_STOCK = ['48baea0000006e000000', '48b88007000084030000', '48b80000000064020000', '48b80e0200000202c000'];
/* 1088x480 at 138x61 mm, view offset (0,0), safe 504x368 at (286,72). */
var RES_DEFAULT = ['48ba8a0000003d000000', '48b840040000e0010000', '48b800000000f8010000', '48b8700100001e014800'];
/* The same size with the safe area the whole frame (--safe full). */
var RES_FULLSAFE = ['48ba8a0000003d000000', '48b840040000e0010000', '48b80000000040040000', '48b8e001000000000000'];
function inResRegion(x) { return RES_OFFS.some(function (o) { return x >= o + 2 && x < o + 10; }); }
/* Build an "older patch" binary: variant + sink applied, self-start still stock
 * (what the car runs today). Patches the bytes directly, bypassing the patcher. */
function makeEdit12Only(panel, sinkUrl) {
    var b = Buffer.from(SRCBUF);
    b.writeUInt32LE((panel === 'mid' ? 0xfff7f30c : 0xfff7f2bc) >>> 0, SLOT);
    var s = Buffer.alloc(SINK_LEN, 0); s.write(sinkUrl, 'latin1'); s.copy(b, SINK);
    return b;   /* CALL left stock */
}

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}
function sha256(f) { return crypto.createHash('sha256').update(fs.readFileSync(f)).digest('hex'); }
function run(args) {
    var r = cp.spawnSync(process.execPath, [SCRIPT].concat(args), { encoding: 'utf8' });
    return { code: r.status, out: (r.stdout || '') + (r.stderr || '') };
}
function diffs(a, b) { var d = []; for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) d.push(i); return d; }
function inRegion(d, off, len) { return d.every(function (x) { return x >= off && x < off + len; }); }

if (!fs.existsSync(SRC)) { process.stdout.write('SKIP: set CARPLAY_BIN to a copy of the unit\'s /usr/bin/carplay (carplay 22.28.3)\n'); process.exit(0); }
ok('source is the known pristine build', sha256(SRC) === ORIG_SHA, sha256(SRC));

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-patch-'));
var tgt = path.join(dir, 'carplay');
fs.writeFileSync(tgt, fs.readFileSync(SRC));
var SRCBUF = fs.readFileSync(SRC);

try {
    var r;
    r = run(['status', '--target', tgt]);
    ok('status: pristine, variant ORIGINAL, sink ORIGINAL, selfstart ORIGINAL, res ORIGINAL',
        /variant:\s+ORIGINAL/.test(r.out) && /sink:\s+ORIGINAL/.test(r.out) && /selfstart:\s+ORIGINAL/.test(r.out) && /res:\s+ORIGINAL \(IC_HIGH advertises 1920x900\)/.test(r.out) && /pristine/.test(r.out), r.out);
    ok('status: proven, version 22.28.3', /proven:\s+yes/.test(r.out) && /version:\s+22\.28\.3/.test(r.out), r.out);
    ok('stock res immediates are the proven ones', JSON.stringify(resImmediates(SRCBUF)) === JSON.stringify(RES_STOCK), JSON.stringify(resImmediates(SRCBUF)));

    r = run(['verify', '--target', tgt]);
    ok('verify: original exits 0, self-start off, res stock', r.code === 0 && /variant original, sink original \(AVB 15\), self-start off, res stock/.test(r.out), r.out);

    r = run(['apply', '--target', tgt, '--panel', 'high']);
    ok('apply without --yes refuses', r.code !== 0 && /--yes/.test(r.out) && sha256(tgt) === ORIG_SHA, r.out);

    /* avb mode: variant + self-start + resolution, stock sink (no --sink/--capture) */
    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('apply high (avb mode) succeeds', r.code === 0 && /patched OK/.test(r.out) && /self-start -> requestStream/.test(r.out) && /resolution -> 1088x480 at 138x61mm/.test(r.out), r.out);
    ok('backup is pristine', sha256(tgt + '.woc-orig') === ORIG_SHA);
    var d = diffs(SRCBUF, fs.readFileSync(tgt));
    ok('avb-mode diff stays in the slot, the call rel32 and the res immediates', d.length >= 2 &&
        d.every(function (x) { return (x >= SLOT && x < SLOT + 4) || (x >= CALL && x < CALL + CALL_LEN) || inResRegion(x); }),
        JSON.stringify(d.map(function (x){return '0x'+x.toString(16);})));
    ok('slot = IC_HIGH (bcf2f7ff)', fs.readFileSync(tgt).slice(SLOT, SLOT + 4).toString('hex') === 'bcf2f7ff');
    ok('call retargeted to requestStream', callBytes(fs.readFileSync(tgt)) === CALL_SELF, callBytes(fs.readFileSync(tgt)));
    ok('res immediates advertise 1088x480 at 138x61mm, safe 504x368 at 286,72',
        JSON.stringify(resImmediates(fs.readFileSync(tgt))) === JSON.stringify(RES_DEFAULT), JSON.stringify(resImmediates(fs.readFileSync(tgt))));
    r = run(['status', '--target', tgt]);
    ok('status: avb mode is SELF-START + sink ORIGINAL + res PATCHED', /selfstart:\s+SELF-START/.test(r.out) && /sink:\s+ORIGINAL/.test(r.out) && /res:\s+PATCHED \(IC_HIGH advertises 1088x480 at 138x61mm, safe 504x368 at 286,72\)/.test(r.out), r.out);
    r = run(['verify', '--target', tgt]);
    ok('verify: high avb exits 0 with res 1088x480', r.code === 0 && /res 1088x480/.test(r.out), r.out);

    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert restores byte-exact original (all four edits)', r.code === 0 && sha256(tgt) === ORIG_SHA && callBytes(fs.readFileSync(tgt)) === CALL_STOCK &&
        JSON.stringify(resImmediates(fs.readFileSync(tgt))) === JSON.stringify(RES_STOCK));

    /* variant + capture together */
    var fifo = '/run/woc/alt.h264';
    r = run(['apply', '--target', tgt, '--panel', 'mid', '--capture', fifo, '--yes']);
    ok('apply mid + capture succeeds', r.code === 0 && /variant -> IC_MID/.test(r.out) && /sink -> file:\/\//.test(r.out), r.out);
    var b = fs.readFileSync(tgt);
    ok('slot = IC_MID (0cf3f7ff)', b.slice(SLOT, SLOT + 4).toString('hex') === '0cf3f7ff');
    var nul = b.slice(SINK, SINK + SINK_LEN).indexOf(0);
    var url = b.slice(SINK, SINK + (nul >= 0 ? nul : SINK_LEN)).toString('latin1');
    ok('sink url redirected to the FIFO', url === 'file://' + fifo + '?mime_type=video/H264', url);
    ok('redirect NUL-padded, no overrun into the next string', b.slice(SINK + SINK_LEN, SINK + SINK_LEN + 5).equals(SRCBUF.slice(SINK + SINK_LEN, SINK + SINK_LEN + 5)) && b.slice(SINK + SINK_LEN + 5, SINK + SINK_LEN + 16).toString('latin1') === 'mCarPlay->G');
    ok('call retargeted to requestStream', callBytes(b) === CALL_SELF, callBytes(b));
    var d2 = diffs(SRCBUF, b);
    ok('all diffs within slot, sink or call regions', d2.every(function (x) { return (x >= SLOT && x < SLOT + 4) || (x >= SINK && x < SINK + SINK_LEN) || (x >= CALL && x < CALL + CALL_LEN); }), 'n=' + d2.length);

    ok('mid leaves the res immediates stock', JSON.stringify(resImmediates(fs.readFileSync(tgt))) === JSON.stringify(RES_STOCK));
    r = run(['status', '--target', tgt]);
    ok('status: PATCHED IC_MID + REDIRECTED + SELF-START + res ORIGINAL', /variant:\s+PATCHED -> IC_MID/.test(r.out) && /sink:\s+REDIRECTED -> file:\/\//.test(r.out) && /selfstart:\s+SELF-START/.test(r.out) && /res:\s+ORIGINAL/.test(r.out), r.out);
    r = run(['verify', '--target', tgt]);
    ok('verify: patched exits 0, slot matches live IC target, self-start on, res stock', r.code === 0 && /IC_MID/.test(r.out) && /self-start on/.test(r.out) && /res stock/.test(r.out), r.out);

    r = run(['apply', '--target', tgt, '--panel', 'mid', '--capture', fifo, '--yes']);
    ok('apply same state is idempotent', r.code === 0 && /nothing to do/.test(r.out), r.out);

    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert undoes both edits byte-exact', r.code === 0 && sha256(tgt) === ORIG_SHA);

    /* a path too long for the slot is refused before any change */
    r = run(['apply', '--target', tgt, '--panel', 'high', '--capture', '/run/woc/' + 'x'.repeat(60) + '.h264', '--yes']);
    ok('over-long capture path refused, no change', r.code !== 0 && /too long/.test(r.out) && sha256(tgt) === ORIG_SHA, r.out);

    /* AVB mode: apply without --capture resets the sink to stock, even after a
     * prior --capture redirect (so fifo -> avb leaves the stock avb:// sink). */
    run(['revert', '--target', tgt, '--yes']);
    run(['apply', '--target', tgt, '--panel', 'high', '--capture', '/run/woc/alt.h264', '--yes']);
    r = run(['status', '--target', tgt]);
    ok('fifo apply redirected the sink', /sink:\s+REDIRECTED/.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('avb apply (no --capture) resets sink to stock', r.code === 0 && /sink -> stock AVB/.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('status: variant still IC_HIGH, sink ORIGINAL', /variant:\s+PATCHED -> IC_HIGH/.test(r.out) && /sink:\s+ORIGINAL/.test(r.out), r.out);
    var av = fs.readFileSync(tgt);
    ok('sink bytes are the stock avb url', av.slice(SINK, SINK + 59).toString('latin1').indexOf('avb://media_transport.avb_streaming.15') === 0);
    run(['revert', '--target', tgt, '--yes']);
    ok('revert after avb mode restores original', sha256(tgt) === ORIG_SHA);

    /* UDP mode: --sink <url> redirects to a stream sink (the bridge accepts a
     * stream where it rejects a seekable file:// — see README). */
    var udp = 'udp://127.0.0.1:20557?mime_type=video/H264';
    r = run(['apply', '--target', tgt, '--panel', 'high', '--sink', udp, '--yes']);
    ok('udp apply redirects to the stream sink', r.code === 0 && /sink -> udp:\/\//.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('status: PATCHED IC_HIGH + REDIRECTED udp://', /variant:\s+PATCHED -> IC_HIGH/.test(r.out) && /sink:\s+REDIRECTED -> udp:\/\//.test(r.out), r.out);
    ok('udp sink bytes are the url, NUL-padded', fs.readFileSync(tgt).slice(SINK, SINK + udp.length).toString('latin1') === udp &&
        fs.readFileSync(tgt)[SINK + udp.length] === 0);
    r = run(['verify', '--target', tgt]);
    ok('verify: udp sink exits 0', r.code === 0 && /udp:\/\//.test(r.out), r.out);
    var beforeSha = sha256(tgt);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--sink', 'ws://x', '--yes']);
    ok('unknown sink scheme refused, no change', r.code !== 0 && /--sink needs/.test(r.out) && sha256(tgt) === beforeSha, r.out);
    run(['revert', '--target', tgt, '--yes']);
    ok('revert after udp mode restores original', sha256(tgt) === ORIG_SHA);

    /* a tampered build is refused for the first edit */
    var tampered = Buffer.from(SRCBUF); tampered[0x1000] ^= 0xff;
    var tt = path.join(dir, 'carplay_tampered'); fs.writeFileSync(tt, tampered);
    r = run(['apply', '--target', tt, '--panel', 'high', '--yes']);
    ok('tampered build refused', r.code !== 0 && /pristine build/.test(r.out), r.out);

    /* --capture with no value is an error, not a silent variant-only patch */
    r = run(['apply', '--target', tgt, '--panel', 'high', '--capture', '--yes']);
    ok('--capture without a value is refused', r.code !== 0 && /--capture needs/.test(r.out) && sha256(tgt) === ORIG_SHA, r.out);

    /* A binary of the backup's build the locator does not recognise: revert
     * writes the whole backup. */
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    var mangled = Buffer.from(fs.readFileSync(tgt));
    mangled.writeUInt32LE(0x11223344, 0x2ba4d4);   /* corrupt the converter table -> unknown variant */
    fs.writeFileSync(tgt, mangled);
    r = run(['status', '--target', tgt]);
    ok('status: an unrecognised binary of the backup\'s build is restorable', /known:\s+NO/.test(r.out) && /restorable:\s+yes/.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes', '--drop-backup']);
    ok('revert restores it whole, without --force, and drops the backup', r.code === 0 && sha256(tgt) === ORIG_SHA &&
        !fs.existsSync(tgt + '.woc-orig') && !fs.existsSync(tgt + '.woc-orig.json'), r.out);

    /* Another build over the backup (a firmware update replaced carplay and
     * left the backup behind): refused; --force writes the backup. */
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    var replaced = Buffer.from(fs.readFileSync(tgt));
    replaced[replaced.indexOf(Buffer.from('5de62156575d737391ea42254e778a97dbbb51f8', 'hex'))] ^= 0xff;
    fs.writeFileSync(tgt, replaced);
    r = run(['status', '--target', tgt]);
    ok('status: another build is not restorable', /restorable:\s+no/.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert refuses another build, keeping it and the backup', r.code !== 0 && /another build/.test(r.out) &&
        fs.readFileSync(tgt).equals(replaced) && fs.existsSync(tgt + '.woc-orig'), r.out);
    r = run(['revert', '--target', tgt, '--yes', '--force']);
    ok('revert --force restores it to the backed-up original', r.code === 0 && sha256(tgt) === ORIG_SHA, r.out);

    /* Another tool's edit outside our regions, made after our patch: revert
     * restores our regions only and keeps it; with only theirs left there is
     * nothing to revert. */
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    var withTheirs = Buffer.from(fs.readFileSync(tgt));
    Buffer.from('e900000000', 'hex').copy(withTheirs, 0x9f9a0);
    fs.writeFileSync(tgt, withTheirs);
    r = run(['revert', '--target', tgt, '--yes']);
    var reverted = fs.readFileSync(tgt);
    ok('revert keeps another tool\'s edit outside our regions', r.code === 0 && /changes outside them kept/.test(r.out) &&
        reverted.slice(0x9f9a0, 0x9f9a5).toString('hex') === 'e900000000' && /variant original, sink original \(AVB 15\), self-start off, res stock/.test(run(['verify', '--target', tgt]).out), r.out);
    var expect = Buffer.from(SRCBUF);
    Buffer.from('e900000000', 'hex').copy(expect, 0x9f9a0);
    ok('revert: the result is the original with only their edit', reverted.equals(expect));
    r = run(['status', '--target', tgt]);
    ok('status with only their edit left: not restorable', /^restorable: no\b/m.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert with only their edit left: nothing to do, the file untouched', r.code === 0 && /nothing to do/.test(r.out) && fs.readFileSync(tgt).equals(expect), r.out);
    r = run(['revert', '--target', tgt, '--yes', '--force']);
    ok('revert --force writes the whole original', r.code === 0 && sha256(tgt) === ORIG_SHA, r.out);

    /* --drop-backup (a removal of the tool): the backup goes once the target
     * is the original, whether the revert restored it or found it so. */
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    r = run(['revert', '--target', tgt, '--yes', '--drop-backup']);
    ok('revert --drop-backup restores, then drops the backup and its record', r.code === 0 && sha256(tgt) === ORIG_SHA &&
        !fs.existsSync(tgt + '.woc-orig') && !fs.existsSync(tgt + '.woc-orig.json'), r.out);
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    run(['revert', '--target', tgt, '--yes']);
    r = run(['revert', '--target', tgt, '--yes', '--drop-backup']);
    ok('revert --drop-backup on the original drops the backup', r.code === 0 && sha256(tgt) === ORIG_SHA &&
        !fs.existsSync(tgt + '.woc-orig'), r.out);
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    fs.writeFileSync(tgt, Buffer.concat([fs.readFileSync(tgt).slice(0, 0x9f9a0), Buffer.from('e900000000', 'hex'), fs.readFileSync(tgt).slice(0x9f9a5)]));
    r = run(['revert', '--target', tgt, '--yes', '--drop-backup']);
    ok('revert --drop-backup with another tool\'s edit: our edits gone, theirs kept, the backup dropped', r.code === 0 &&
        fs.readFileSync(tgt).equals(expect) && !fs.existsSync(tgt + '.woc-orig'), r.out);
    fs.writeFileSync(tgt, SRCBUF);

    /* An Edit-1/2-only binary (what the car runs today): variant + sink patched,
     * self-start still stock. It must NOT read as fully patched, and the Patch button must
     * add the self-start edit — not report "nothing to do". */
    var e12 = path.join(dir, 'carplay_edit12');
    fs.writeFileSync(e12, makeEdit12Only('high', 'udp://127.0.0.1:20557?mime_type=video/H264'));
    fs.writeFileSync(e12 + '.woc-orig', SRCBUF);   /* the car keeps the pristine backup */
    fs.writeFileSync(e12 + '.woc-orig.json', JSON.stringify({ sha256_original: ORIG_SHA }) + '\n');
    r = run(['status', '--target', e12]);
    ok('edit-1/2-only: variant+sink patched but self-start and res ORIGINAL', /variant:\s+PATCHED -> IC_HIGH/.test(r.out) && /sink:\s+REDIRECTED -> udp/.test(r.out) && /selfstart:\s+ORIGINAL/.test(r.out) && /res:\s+ORIGINAL/.test(r.out), r.out);
    r = run(['verify', '--target', e12]);
    ok('edit-1/2-only: verify FAILS as partially patched', r.code !== 0 && /partially patched/.test(r.out), r.out);
    var e12buf = fs.readFileSync(e12);
    r = run(['apply', '--target', e12, '--panel', 'high', '--sink', 'udp://127.0.0.1:20557?mime_type=video/H264', '--yes']);
    ok('edit-1/2-only: the Patch button applies self-start and resolution (not "nothing to do")', r.code === 0 && /self-start -> requestStream/.test(r.out) && /resolution -> 1088x480/.test(r.out) && !/nothing to do/.test(r.out), r.out);
    var e12now = fs.readFileSync(e12);
    var d3 = diffs(e12buf, e12now);
    ok('edit-1/2-only: the Patch button changed only the call rel32 and the res immediates', d3.length >= 1 &&
        d3.every(function (x) { return (x >= CALL && x < CALL + CALL_LEN) || inResRegion(x); }), JSON.stringify(d3.map(function (x){return '0x'+x.toString(16);})));
    ok('edit-1/2-only: now SELF-START, variant+sink unchanged', callBytes(e12now) === CALL_SELF && e12now.slice(SLOT, SLOT + 4).toString('hex') === 'bcf2f7ff');
    r = run(['verify', '--target', e12]);
    ok('edit-1/2-only: verify now passes', r.code === 0 && /self-start on/.test(r.out), r.out);
    r = run(['revert', '--target', e12, '--yes']);
    ok('edit-1/2-only: revert restores byte-exact stock', r.code === 0 && sha256(e12) === ORIG_SHA);

    /* The pre-resolution patch shape (variant + sink + self-start, stock
     * geometry): verify calls it partial on the high panel, and the Patch button adds
     * only the resolution immediates. */
    var e123 = path.join(dir, 'carplay_edit123');
    fs.writeFileSync(e123, SRCBUF);
    run(['apply', '--target', e123, '--panel', 'high', '--yes']);
    var b123 = Buffer.from(fs.readFileSync(e123));
    [0x20ca30, 0x20ca52, 0x20ca64, 0x20ca72].forEach(function (o, i) {
        Buffer.from(RES_STOCK[i], 'hex').copy(b123, o);   /* geometry back to stock, the rest patched */
    });
    fs.writeFileSync(e123, b123);
    r = run(['status', '--target', e123]);
    ok('pre-resolution patch: SELF-START but res ORIGINAL', /selfstart:\s+SELF-START/.test(r.out) && /res:\s+ORIGINAL/.test(r.out), r.out);
    r = run(['verify', '--target', e123]);
    ok('pre-resolution patch: verify FAILS as partially patched', r.code !== 0 && /partially patched/.test(r.out) && /res original/.test(r.out), r.out);
    r = run(['apply', '--target', e123, '--panel', 'high', '--yes']);
    ok('pre-resolution patch: the Patch button adds the resolution edit', r.code === 0 && /resolution -> 1088x480/.test(r.out) && !/variant ->/.test(r.out), r.out);
    var d4 = diffs(b123, fs.readFileSync(e123));
    ok('pre-resolution patch: the Patch button changed only the res immediates', d4.length >= 1 && d4.every(inResRegion),
        JSON.stringify(d4.map(function (x){return '0x'+x.toString(16);})));
    r = run(['verify', '--target', e123]);
    ok('pre-resolution patch: verify now passes', r.code === 0 && /res 1088x480/.test(r.out), r.out);

    /* Panel switch high -> mid restores the stock geometry, and a custom
     * --res lands in the immediates at the same physical size. */
    r = run(['apply', '--target', e123, '--panel', 'mid', '--yes']);
    ok('high->mid restores stock resolution', r.code === 0 && /resolution -> stock 1920x900/.test(r.out) &&
        JSON.stringify(resImmediates(fs.readFileSync(e123))) === JSON.stringify(RES_STOCK), r.out);
    r = run(['apply', '--target', e123, '--panel', 'high', '--res', '800x360', '--yes']);
    ok('custom --res 800x360 applies at 138x61mm', r.code === 0 && /resolution -> 800x360 at 138x61mm/.test(r.out), r.out);
    r = run(['status', '--target', e123]);
    ok('custom --res reads back as PATCHED 800x360 with the safe area scaled', /res:\s+PATCHED \(IC_HIGH advertises 800x360 at 138x61mm, safe 371x276 at 210,54\)/.test(r.out), r.out);
    r = run(['apply', '--target', e123, '--panel', 'high', '--res', '1920x1080', '--yes']);
    ok('a 1920-wide custom res is labelled by its size, not stock', r.code === 0 && /resolution -> 1920x1080 at 138x61mm/.test(r.out) && !/resolution -> stock/.test(r.out), r.out);
    /* A unit patched at an earlier default size: the Patch button (no --res) rewrites
     * it to the default. */
    r = run(['apply', '--target', e123, '--panel', 'high', '--res', '720x320', '--yes']);
    ok('--res 720x320 applies', r.code === 0 && /resolution -> 720x320 at 138x61mm/.test(r.out), r.out);
    r = run(['apply', '--target', e123, '--panel', 'high', '--yes']);
    ok('720x320 unit: the Patch button rewrites the geometry to 1088x480', r.code === 0 && /resolution -> 1088x480 at 138x61mm/.test(r.out) &&
        JSON.stringify(resImmediates(fs.readFileSync(e123))) === JSON.stringify(RES_DEFAULT), r.out);
    run(['apply', '--target', e123, '--panel', 'high', '--res', '800x360', '--yes']);
    var e123sha = sha256(e123);
    r = run(['apply', '--target', e123, '--panel', 'high', '--res', '719x320', '--yes']);
    ok('odd --res refused, no change', r.code !== 0 && /out of range/.test(r.out) && sha256(e123) === e123sha, r.out);
    r = run(['apply', '--target', e123, '--panel', 'mid', '--res', '720x320', '--yes']);
    ok('--res with --panel mid refused, no change', r.code !== 0 && /--panel high/.test(r.out) && sha256(e123) === e123sha, r.out);
    r = run(['apply', '--target', e123, '--panel', 'high', '--res', '--yes']);
    ok('--res without a value refused, no change', r.code !== 0 && /--res needs/.test(r.out) && sha256(e123) === e123sha, r.out);
    r = run(['revert', '--target', e123, '--yes']);
    ok('revert after res edits restores byte-exact stock', r.code === 0 && sha256(e123) === ORIG_SHA);

    /* The safe area. A unit patched at 1088x480 with the whole frame as the
     * safe area reads as patched (res still 1088x480); the Patch button rewrites only
     * the two safe-area immediates to the default, and the Patch button again does
     * nothing. --safe full and --safe WxH+X+Y write other safe areas. */
    var fs1 = path.join(dir, 'carplay_fullsafe');
    fs.writeFileSync(fs1, SRCBUF);
    run(['apply', '--target', fs1, '--panel', 'high', '--yes']);
    var bfs = Buffer.from(fs.readFileSync(fs1));
    [0x20ca30, 0x20ca52, 0x20ca64, 0x20ca72].forEach(function (o, i) {
        Buffer.from(RES_FULLSAFE[i], 'hex').copy(bfs, o);
    });
    fs.writeFileSync(fs1, bfs);
    r = run(['status', '--target', fs1]);
    ok('full-frame safe: status PATCHED 1088x480 with safe 1088x480 at 0,0',
        /res:\s+PATCHED \(IC_HIGH advertises 1088x480 at 138x61mm, safe 1088x480 at 0,0\)/.test(r.out) && /known:\s+yes/.test(r.out), r.out);
    r = run(['verify', '--target', fs1]);
    ok('full-frame safe: verify passes', r.code === 0 && /res 1088x480/.test(r.out), r.out);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--yes']);
    ok('full-frame safe: the Patch button writes the default safe area', r.code === 0 &&
        /resolution -> 1088x480 at 138x61mm, safe 504x368 at 286,72/.test(r.out) && !/variant ->|self-start ->|sink ->/.test(r.out), r.out);
    var d5 = diffs(bfs, fs.readFileSync(fs1));
    var SAFE_OFFS = [0x20ca64, 0x20ca72];
    ok('full-frame safe: the Patch button changed only the safe-area immediates', d5.length >= 1 &&
        d5.every(function (x) { return SAFE_OFFS.some(function (o) { return x >= o + 2 && x < o + 10; }); }),
        JSON.stringify(d5.map(function (x){return '0x'+x.toString(16);})));
    ok('full-frame safe: immediates now the default geometry', JSON.stringify(resImmediates(fs.readFileSync(fs1))) === JSON.stringify(RES_DEFAULT),
        JSON.stringify(resImmediates(fs.readFileSync(fs1))));
    var fsSha = sha256(fs1);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--yes']);
    ok('default safe: the Patch button again is nothing to do', r.code === 0 && /nothing to do/.test(r.out) && sha256(fs1) === fsSha, r.out);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--safe', 'full', '--yes']);
    ok('--safe full writes the full-frame safe area', r.code === 0 && /safe 1088x480 at 0,0/.test(r.out) &&
        JSON.stringify(resImmediates(fs.readFileSync(fs1))) === JSON.stringify(RES_FULLSAFE), r.out);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--safe', '600x400+244+40', '--yes']);
    ok('--safe WxH+X+Y applies and reads back', r.code === 0 && /safe 600x400 at 244,40/.test(run(['status', '--target', fs1]).out), r.out);
    var fsSha2 = sha256(fs1);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--safe', '600x400+500+40', '--yes']);
    ok('--safe outside the frame refused, no change', r.code !== 0 && /out of range/.test(r.out) && sha256(fs1) === fsSha2, r.out);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--safe', '600x400', '--yes']);
    ok('malformed --safe refused, no change', r.code !== 0 && /--safe needs/.test(r.out) && sha256(fs1) === fsSha2, r.out);
    r = run(['apply', '--target', fs1, '--panel', 'mid', '--safe', 'full', '--yes']);
    ok('--safe with --panel mid refused, no change', r.code !== 0 && /--panel high/.test(r.out) && sha256(fs1) === fsSha2, r.out);
    /* A safe area that overruns the frame is not this tool's geometry. */
    var bad = Buffer.from(fs.readFileSync(fs1));
    Buffer.from('48b800000000f8010000', 'hex').copy(bad, 0x20ca64);   /* safe width 504 */
    Buffer.from('48b870010000a0024800', 'hex').copy(bad, 0x20ca72);   /* safe x 672: 672+504 > 1088 */
    fs.writeFileSync(fs1, bad);
    r = run(['status', '--target', fs1]);
    ok('safe area outside the frame: status res UNKNOWN', /res:\s+UNKNOWN/.test(r.out), r.out);
    r = run(['apply', '--target', fs1, '--panel', 'high', '--yes']);
    ok('safe area outside the frame: apply refused, no write', r.code !== 0 && /resolution: geometry not recognised/.test(r.out) && fs.readFileSync(fs1).equals(bad), r.out);

    /* a corrupted resolution anchor is refused before any write */
    var badres = Buffer.from(SRCBUF); badres[0x20ca5c] ^= 0xff;   /* the store after the px movabs */
    var br = path.join(dir, 'carplay_badres'); fs.writeFileSync(br, badres);
    r = run(['status', '--target', br]);
    ok('bad res anchor: status reports res UNKNOWN', /res:\s+UNKNOWN/.test(r.out), r.out);
    var brSha = sha256(br);
    r = run(['apply', '--target', br, '--panel', 'high', '--yes']);
    ok('bad res anchor: apply refused, no write', r.code !== 0 && /resolution/.test(r.out) && sha256(br) === brSha, r.out);

    /* a self-start call without its AVAILABLE argument is refused before any write */
    var badcall = Buffer.from(SRCBUF); badcall[CALL - 12] = 0x02;   /* mov dword [rsp+8],1 -> 2 */
    var bc = path.join(dir, 'carplay_badcall'); fs.writeFileSync(bc, badcall);
    r = run(['status', '--target', bc]);
    ok('damaged self-start site: status reports selfstart UNKNOWN with the reason', /selfstart:\s+UNKNOWN \(self-start: call has no AVAILABLE argument\)/.test(r.out) &&
        /known:\s+NO — self-start: call has no AVAILABLE argument/.test(r.out), r.out);
    var bcSha = sha256(bc);
    r = run(['apply', '--target', bc, '--panel', 'high', '--yes']);
    ok('damaged self-start site: apply refused, no write', r.code !== 0 && /refusing: self-start: call has no AVAILABLE argument/.test(r.out) && sha256(bc) === bcSha, r.out);

    /* An Edit-1-only binary (the variant slot alone): partially patched, and
     * without our backup the first edit refuses it. */
    var e1 = path.join(dir, 'carplay_edit1');
    var e1buf = Buffer.from(SRCBUF); e1buf.writeUInt32LE(0xfff7f2bc >>> 0, SLOT);
    fs.writeFileSync(e1, e1buf);
    r = run(['status', '--target', e1]);
    ok('edit-1-only: variant PATCHED, the rest ORIGINAL, a modified build', /variant:\s+PATCHED -> IC_HIGH/.test(r.out) && /sink:\s+ORIGINAL/.test(r.out) &&
        /selfstart:\s+ORIGINAL/.test(r.out) && /res:\s+ORIGINAL/.test(r.out) && /known:\s+yes, modified build/.test(r.out), r.out);
    r = run(['verify', '--target', e1]);
    ok('edit-1-only: verify FAILS as partially patched', r.code !== 0 && /partially patched — variant patched, self-start original, res original/.test(r.out), r.out);
    r = run(['apply', '--target', e1, '--panel', 'high', '--yes']);
    ok('edit-1-only: the first edit refuses it', r.code !== 0 && /first edit requires the pristine build: variant edited/.test(r.out) && fs.readFileSync(e1).equals(e1buf), r.out);

    /* probe reads only, and names the documented sites */
    var before = fs.statSync(SRC).mtime.getTime();
    r = run(['probe', SRC]);
    ok('probe: the documented sites, proven, patchable', r.code === 0 && /variant:\s+slot 0x2ba4f0 \(table 0x2ba4d4, 12 slots\): ORIGINAL/.test(r.out) &&
        /sink:\s+slot 0x2a20b0 \(59 bytes\): ORIGINAL/.test(r.out) &&
        /selfstart:\s+call 0x18176c in onSuggestUi 0x1816f0 \(setStreamStatus 0x181560, requestStream 0x181810\): ORIGINAL/.test(r.out) &&
        /res:\s+immediates 0x20ca32 0x20ca54 0x20ca66 0x20ca74 \(builder 0x20ca30\): ORIGINAL/.test(r.out) &&
        /proven:\s+yes, carplay 22\.28\.3/.test(r.out) && /patchable:\s+yes/.test(r.out) && /build-id:\s+5de62156/.test(r.out), r.out);
    ok('probe: the file untouched', sha256(SRC) === ORIG_SHA && fs.statSync(SRC).mtime.getTime() === before);
    r = run(['probe', bc]);
    ok('probe: a damaged site makes the build not patchable, with the reason', /patchable:\s+no — self-start: call has no AVAILABLE argument/.test(r.out), r.out);

    /* A build outside the proven list (another build-id) with every site
     * stock: patched without the whole-file gate, and reverted byte-exact. */
    var other = Buffer.from(SRCBUF);
    var bid = other.indexOf(Buffer.from('5de62156575d737391ea42254e778a97dbbb51f8', 'hex'));
    other[bid] ^= 0xff;
    other[0x9f9a0] ^= 0xff;   /* and a byte that differs from the proven build elsewhere */
    var ot = path.join(dir, 'carplay_other');
    fs.writeFileSync(ot, other);
    r = run(['status', '--target', ot]);
    ok('unproven build: pristine by its stock sites, not proven, its version', /known:\s+yes, pristine build a2e62156/.test(r.out) &&
        /proven:\s+no/.test(r.out) && /version:\s+22\.28\.3/.test(r.out), r.out);
    r = run(['apply', '--target', ot, '--panel', 'high', '--yes']);
    ok('unproven build: apply without --unproven refuses, no write, no backup', r.code !== 0 && /is not a proven build; pass --unproven/.test(r.out) &&
        fs.readFileSync(ot).equals(other) && !fs.existsSync(ot + '.woc-orig'), r.out);
    r = run(['apply', '--target', ot, '--panel', 'high', '--sink', 'udp://127.0.0.1:20557?mime_type=video/H264', '--unproven', '--yes']);
    ok('unproven build: apply writes all four edits', r.code === 0 && /patched OK/.test(r.out) && /variant -> IC_HIGH/.test(r.out) &&
        /self-start -> requestStream/.test(r.out) && /resolution -> 1088x480/.test(r.out), r.out);
    var od = diffs(other, fs.readFileSync(ot));
    ok('unproven build: the diff stays in the edit regions', od.length > 0 && od.every(function (x) {
        return (x >= SLOT && x < SLOT + 4) || (x >= SINK && x < SINK + SINK_LEN) || (x >= CALL && x < CALL + CALL_LEN) || inResRegion(x); }));
    r = run(['apply', '--target', ot, '--panel', 'mid', '--yes']);
    ok('unproven build: a later apply (our backup present) needs no --unproven', r.code === 0 && /variant -> IC_MID/.test(r.out), r.out);
    r = run(['apply', '--target', ot, '--panel', 'high', '--sink', 'udp://127.0.0.1:20557?mime_type=video/H264', '--yes']);
    r = run(['verify', '--target', ot]);
    ok('unproven build: verify passes', r.code === 0 && /IC_HIGH, sink udp:\/\/127\.0\.0\.1:20557/.test(r.out), r.out);
    r = run(['revert', '--target', ot, '--yes']);
    ok('unproven build: revert restores it byte-exact', r.code === 0 && fs.readFileSync(ot).equals(other), r.out);

    /* A backup of another build is never restored over the target. */
    fs.writeFileSync(tgt, SRCBUF);
    [tgt + '.woc-orig', tgt + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    fs.writeFileSync(tgt + '.woc-orig', other);
    fs.unlinkSync(tgt + '.woc-orig.json');
    var tSha = sha256(tgt);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('a backup of another build: revert refuses, no write', r.code !== 0 && /another build/.test(r.out) && sha256(tgt) === tSha, r.out);
    r = run(['apply', '--target', tgt, '--panel', 'mid', '--yes']);
    ok('a backup of another build: apply refuses, no write', r.code !== 0 && /original of another build/.test(r.out) && sha256(tgt) === tSha, r.out);

    codingAndRecord();
} finally {
    try { fs.readdirSync(dir).forEach(function (f) { fs.unlinkSync(path.join(dir, f)); }); fs.rmdirSync(dir); } catch (e) {}
}

/* The cluster-coding gate and the patch record: a car not coded
 * KI_KOMBI_VARIANT 7, or whose coding cannot be read, is patched only with
 * --unproven, and then recorded unproven; the first patch of the proven
 * build on a variant-7 car is on trial (proven: false) until the watcher
 * records it proven; a patch that ran healthy, or a record of an earlier
 * release saying proven, keeps proven across applies and a revert. */
function codingAndRecord() {
    var r, ct = path.join(dir, 'carplay_coding');
    function fresh() {
        [ct, ct + '.woc-orig', ct + '.woc-orig.json', record.recordPath(ct)].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
        fs.writeFileSync(ct, SRCBUF);
    }
    function rec() { var x = record.current(ct); return x ? JSON.stringify({ patched: x.patched, proven: x.proven, trial: x.trial, healthy: x.healthy }) : 'none'; }
    function want(o) { return JSON.stringify({ patched: o.patched, proven: o.proven, trial: o.trial, healthy: o.healthy }); }
    try {
        fresh();
        r = run(['status', '--target', ct]);
        ok('coding: status names it', /^coding:\s+KI_KOMBI_VARIANT 7$/m.test(r.out), r.out);
        [[function () { car.coding(9); }, 'KI_KOMBI_VARIANT 9 is not the proven cluster coding (7)', /^coding:\s+KI_KOMBI_VARIANT 9$/m],
         [function () { car.coding(7, 0x0b); }, 'the cluster coding cannot be read (coding not valid (flags 0xb))', /^coding:\s+unreadable \(coding not valid/m],
         [function () { car.coding(null); }, 'the cluster coding cannot be read', /^coding:\s+unreadable \(.* unreadable\)$/m]].forEach(function (c) {
            fresh();
            c[0]();
            r = run(['status', '--target', ct]);
            ok('coding: status reads ' + c[2], c[2].test(r.out), r.out);
            r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
            ok('coding: apply refused (' + c[1] + '), nothing written, no backup', r.code !== 0 && r.out.indexOf('refusing: ' + c[1]) >= 0 && /pass --unproven/.test(r.out) &&
                sha256(ct) === ORIG_SHA && !fs.existsSync(ct + '.woc-orig') && rec() === 'none', r.out);
            r = run(['apply', '--target', ct, '--panel', 'high', '--unproven', '--yes']);
            /* A coding that cannot be read may be a variant-7 car's: on trial,
             * so a healthy run proves it. Another variant: unproven for good. */
            var unread = /cannot be read/.test(c[1]);
            ok('coding: with --unproven (the Patch button) patched, recorded ' + (unread ? 'on trial' : 'unproven'), r.code === 0 && /patched OK/.test(r.out) &&
                rec() === want(unread ? { patched: true, proven: false, trial: true } : { patched: true, proven: false }), r.out + rec());
            r = run(['apply', '--target', ct, '--panel', 'mid', '--yes']);
            ok('coding: a later apply without --unproven refused too', r.code !== 0 && r.out.indexOf('refusing: ' + c[1]) >= 0, r.out);
            car.coding(7);
        });

        fresh();
        r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        ok('record: the first patch on a variant-7 car is on trial, unproven', r.code === 0 && rec() === want({ patched: true, proven: false, trial: true }), r.out + rec());
        r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        ok('record: an apply with nothing to do keeps the trial', /nothing to do/.test(r.out) && rec() === want({ patched: true, proven: false, trial: true }), r.out + rec());
        r = run(['apply', '--target', ct, '--panel', 'mid', '--yes']);
        ok('record: an apply that rewrites a patch on trial keeps it on trial', r.code === 0 && /patched OK/.test(r.out) && rec() === want({ patched: true, proven: false, trial: true }), r.out + rec());
        r = run(['revert', '--target', ct, '--yes']);
        ok('record: reverting a patch on trial leaves no healthy mark', r.code === 0 && rec() === want({ patched: false }), r.out + rec());
        r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        ok('record: patched again, on trial again', r.code === 0 && rec() === want({ patched: true, proven: false, trial: true }), r.out + rec());

        /* The watcher's record once carplay ran it healthy. */
        record.write(ct, { patched: true, proven: true });
        r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        ok('record: a patch that ran healthy stays proven (nothing to do)', /nothing to do/.test(r.out) && rec() === want({ patched: true, proven: true }), r.out + rec());
        r = run(['apply', '--target', ct, '--panel', 'mid', '--yes']);
        ok('record: and when an apply rewrites it', r.code === 0 && /patched OK/.test(r.out) && rec() === want({ patched: true, proven: true }), r.out + rec());
        r = run(['revert', '--target', ct, '--yes']);
        ok('record: its revert remembers it ran healthy', r.code === 0 && sha256(ct) === ORIG_SHA && rec() === want({ patched: false, healthy: true }), r.out + rec());
        r = run(['revert', '--target', ct, '--yes']);
        ok('record: a revert with nothing to do keeps that', /already at the original/.test(r.out) && rec() === want({ patched: false, healthy: true }), r.out + rec());
        r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        ok('record: patched again, the same build is proven at once', r.code === 0 && rec() === want({ patched: true, proven: true }), r.out + rec());
        car.coding(9);
        r = run(['apply', '--target', ct, '--panel', 'mid', '--unproven', '--yes']);
        ok('record: on a car coded otherwise, the Patch button\'s patch is unproven whatever ran before', r.code === 0 && rec() === want({ patched: true, proven: false }), r.out + rec());
        car.coding(7);

        /* A patch an earlier release wrote and recorded proven (no trial):
         * kept proven across an update's apply. */
        fresh();
        run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        record.write(ct, { patched: true, proven: true });
        var m0 = fs.statSync(record.recordPath(ct)).mtime.getTime();
        r = run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        ok('record: an earlier release\'s proven record stays proven, not rewritten', /nothing to do/.test(r.out) && rec() === want({ patched: true, proven: true }) &&
            fs.statSync(record.recordPath(ct)).mtime.getTime() === m0, r.out + rec());

        /* A patch on trial on a car whose coding then reads otherwise: the
         * Patch button's apply ends the trial. */
        fresh();
        run(['apply', '--target', ct, '--panel', 'high', '--yes']);
        car.coding(10);
        r = run(['apply', '--target', ct, '--panel', 'high', '--unproven', '--yes']);
        ok('record: a trial ends when the coding is not the proven one', /nothing to do/.test(r.out) && rec() === want({ patched: true, proven: false, trial: false }), r.out + rec());
        car.coding(7);
    } finally {
        car.coding(7);
        [ct, ct + '.woc-orig', ct + '.woc-orig.json', record.recordPath(ct)].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    }
}

process.stdout.write(fails ? ('\n' + fails + ' check(s) failed\n') : '\nall checks passed\n');
process.exit(fails ? 1 : 0);
