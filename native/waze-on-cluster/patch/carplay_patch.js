#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* carplay_patch.js — four reversible edits to the CarPlay binary that,
 * together, put the CarPlay instrument-cluster ("alternate screen") H.264 where
 * this tool can read it, at the size this tool displays (PATCH.md).
 *
 *   variant  one 4-byte jump-table slot. The converter maps raw
 *            KI_KOMBI_VARIANT 9->IC_HIGH, 10->IC_MID, everything else incl. 7
 *            ->OTHER (which fails the alternate-display gate). Remapping the
 *            variant-7 slot to a supported value makes every downstream check
 *            pass, so MGU advertises the 2nd display and the phone sends the
 *            cluster stream. Touches variant 7 only.
 *   sink     the 2nd-stream Cinemo sink URL string. As shipped it is
 *            `avb://media_transport.avb_streaming.15?mime_type=video/H264`,
 *            sending the H.264 undecoded over AVB to the cluster. Repointing it
 *            (--sink <url>, or --capture <fifo> for the file:// form) sends the
 *            same undecoded H.264 to this tool: a `udp://127.0.0.1:<port>`
 *            stream sink, or a `file://<fifo>`. Same length or shorter,
 *            NUL-padded in place.
 *   selfstart one `call` retargeted (4-byte rel32). CarPlay's AlternateDisplays
 *            waits for NavController to forward a requestStream command before it
 *            shows the cluster UI, but NavController never forwards it, so the
 *            stream never starts. Retargeting onSuggestUi's
 *            setStreamStatus(AVAILABLE) call to requestStream() makes CarPlay
 *            start the stream itself. Applied in every mode.
 *   resolution the IC_HIGH display geometry `ICSafeAreaProvider` advertises to
 *            the phone: pixel size, physical size (mm), and the view/safe
 *            areas, held as four `movabs` immediates in one builder function.
 *            The stock cluster display is larger than the part this tool
 *            shows, so `apply --panel high` rewrites the geometry to the
 *            displayed size (1088x480 px at 138x61 mm, view area the full
 *            frame) and the phone renders its cluster map at that size. The
 *            safe area, where the phone keeps its guidance UI, is the part of
 *            the frame the gauges and native readouts leave visible (504x368
 *            at 286,72). `--res WxH` picks another pixel size at the same
 *            physical size, `--safe` another safe area; `--res stock`, and
 *            `--panel mid`, keep the build's stock geometry.
 *
 * The sites are found in each build by carplay_locate.js; a build where any
 * of them is not found, or reads as neither stock nor this patch, is refused.
 * The first edit of a build outside the proven list needs --unproven. apply
 * is refused on a head unit that refuses our parts (head_unit.js), whatever
 * the flags; revert works on any.
 * All four edits are reversible from a verified full backup of the original
 * binary.
 *
 * Node 8.16.2 on the unit, no dependencies.
 *
 *   carplay_patch.js status [--target <file>]
 *   carplay_patch.js probe  [<file>]
 *   carplay_patch.js apply  --panel high|mid [--sink <url> | --capture <fifo>] [--res WxH|stock] [--safe WxH+X+Y|full] [--target <file>] [--unproven] [--yes]
 *   carplay_patch.js revert [--target <file>] [--yes] [--drop-backup]
 *   carplay_patch.js verify [--target <file>]
 *
 * Default target /usr/bin/carplay. This edits the file by atomic rename, not
 * the running process, which keeps the old binary until carplay next starts.
 * probe only reads.
 */
'use strict';

var fs = require('fs');
var crypto = require('crypto');
var path = require('path');
var locator = require('./carplay_locate.js');
var headUnit = require('./head_unit.js');
var record = require('./patch_record.js');
var fileMode = require('./file_mode.js');

/* Builds proven as shipped, by GNU build-id. A first edit of one needs the
 * whole file to be the shipped one, and anything else counts as modified. A
 * first edit of any other build needs --unproven: its sites pass the
 * locator's checks, but no car has run it patched. stock: the shipped bytes
 * at every offset any version of this tool writes in it, for a revert
 * without the backup; the rebuilt file is kept only when its SHA-256 is the
 * shipped one, so a list that lacks an offset rebuilds nothing. */
var PROVEN = {
    '5de62156575d737391ea42254e778a97dbbb51f8': {
        version: 'carplay 22.28.3+gitr624+993efc1-r0.14',
        sha256: 'ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50',
        stock: [[0x2ba4f0, 'ecf2f7ff'], [0x2a20b0, Buffer.from(locator.SINK_STOCK, 'latin1').toString('hex')], [0x18176c, 'e8effdffff'],
                [0x20ca32, 'ea0000006e000000'], [0x20ca54, '8007000084030000'], [0x20ca66, '0000000064020000'], [0x20ca74, '0e0200000202c000']],
    },
    /* Confirmed on a G07 X7 (variant 7) through CarPlay's Patch button
     * (issue #95), from its probe output. stock holds the sink, self-start
     * and pixel-size bytes the probe shows; the variant slot and the other
     * three geometry immediates are not known, so this build is reverted
     * from our backup only. */
    '389c1d0536f8cc9283c156410f2f2634a9f952c0': {
        version: 'carplay 21.27.4',
        sha256: 'fb5d6b179ad4f62a968697d21e84dbb80adc1ad8e618a6f02c6a9a0467aca1ac',
        stock: [[0x29bb10, Buffer.from(locator.SINK_STOCK, 'latin1').toString('hex')], [0x1802dc, 'e8effdffff'], [0x20b484, '8007000084030000']],
    },
};

/* The geometry `--panel high` advertises: the cluster's 1088x480 central map
 * area (138x61 mm of glass), which the render's texture covers pixel for
 * pixel. Every geometry this tool writes has this physical size, which tells
 * it from a build's stock geometry. */
var RES_DEFAULT = { w: 1088, h: 480, mmw: 138, mmh: 61 };
/* The default safe area in the 1088x480 frame: x from BMW's own IC_MID safe
 * area (which lies inside the map area), y from below the native model badge
 * to inside the gauge mask's lower edge. Other --res sizes scale it. */
var SAFE_DEFAULT = { x: 286, y: 72, w: 504, h: 368 };
var EDITS = ['variant', 'sink', 'selfstart', 'res'];

function sha256(buf) { return crypto.createHash('sha256').update(buf).digest('hex'); }
function die(msg) { process.stderr.write('error: ' + msg + '\n'); process.exit(1); }
function hex(n) { return '0x' + n.toString(16); }

function backupPath(t) { return t + '.woc-orig'; }
function metaPath(t) { return t + '.woc-orig.json'; }

function variantState(buf, v) {
    if (!v.ok) return { state: 'unknown', reason: v.reason };
    var x = buf.readUInt32LE(v.slot);
    return x === v.original ? { state: 'original' } : { state: 'patched', panel: x === v.high ? 'high' : 'mid' };
}

function sinkState(buf, s) {
    if (!s.ok) return { state: 'unknown', reason: s.reason };
    var slot = buf.slice(s.offset, s.offset + s.len), nul = slot.indexOf(0);
    var url = (nul >= 0 ? slot.slice(0, nul) : slot).toString('latin1');
    return url === locator.SINK_STOCK ? { state: 'original' } : { state: 'redirected', url: url };
}

function selfstartState(buf, s) {
    if (!s.ok) return { state: 'unknown', reason: s.reason };
    return { state: buf.toString('hex', s.offset, s.offset + 5) === s.original ? 'original' : 'patched' };
}

/* The IC_HIGH geometry in the four immediates: mm pair; px pair; view x,y
 * (u16) + safe width; safe height + safe x,y (u16). */
function readRes(buf, r) {
    return {
        mmw: buf.readUInt32LE(r.mm), mmh: buf.readUInt32LE(r.mm + 4),
        w: buf.readUInt32LE(r.px), h: buf.readUInt32LE(r.px + 4),
        viewx: buf.readUInt16LE(r.safe1), viewy: buf.readUInt16LE(r.safe1 + 2), safew: buf.readUInt32LE(r.safe1 + 4),
        safeh: buf.readUInt32LE(r.safe2), safex: buf.readUInt16LE(r.safe2 + 4), safey: buf.readUInt16LE(r.safe2 + 6),
    };
}

/* This tool's geometry has its physical size, the view area at (0,0) and a
 * safe area inside the frame; a stock one has another physical size and a
 * safe area inside the frame. */
function resState(buf, r) {
    if (!r.ok) return { state: 'unknown', reason: r.reason };
    var g = readRes(buf, r);
    var inside = g.w > 0 && g.h > 0 && g.safew > 0 && g.safeh > 0 && g.safex + g.safew <= g.w && g.safey + g.safeh <= g.h;
    var ours = g.mmw === RES_DEFAULT.mmw && g.mmh === RES_DEFAULT.mmh;
    if (inside && !ours && g.mmw > 0 && g.mmh > 0) return { state: 'original', res: g };
    if (inside && ours && g.viewx === 0 && g.viewy === 0) return { state: 'patched', res: g };
    return { state: 'unknown', reason: 'resolution: geometry not recognised', res: g };
}

function writeRes(buf, r, g) {
    buf.writeUInt32LE(g.mmw, r.mm); buf.writeUInt32LE(g.mmh, r.mm + 4);
    buf.writeUInt32LE(g.w, r.px); buf.writeUInt32LE(g.h, r.px + 4);
    buf.writeUInt16LE(0, r.safe1); buf.writeUInt16LE(0, r.safe1 + 2); buf.writeUInt32LE(g.safew, r.safe1 + 4);
    buf.writeUInt32LE(g.safeh, r.safe2); buf.writeUInt16LE(g.safex, r.safe2 + 4); buf.writeUInt16LE(g.safey, r.safe2 + 6);
}

function safeText(g) { return 'safe ' + g.safew + 'x' + g.safeh + ' at ' + g.safex + ',' + g.safey; }
function geomText(g) { return g.w + 'x' + g.h + ' at ' + g.mmw + 'x' + g.mmh + 'mm, ' + safeText(g); }

function classify(buf) {
    var loc = locator.locate(buf);
    if (!loc.elf) return { ok: false, reason: loc.reason, loc: loc, size: buf.length };
    return { ok: true, loc: loc, size: buf.length, variant: variantState(buf, loc.variant), sink: sinkState(buf, loc.sink),
             selfstart: selfstartState(buf, loc.selfstart), res: resState(buf, loc.res) };
}

/* Why the patcher does not recognise the build: [] when every edit is found
 * and reads as stock or as this patch. */
function unrecognised(c) {
    if (!c.ok) return [c.reason];
    return EDITS.filter(function (e) { return c[e].state === 'unknown'; }).map(function (e) { return c[e].reason; });
}
function brief(reasons) { return reasons[0] + (reasons.length > 1 ? ' (+' + (reasons.length - 1) + ' more)' : ''); }

/* Why `buf` is not the pristine build, or null: an edit present, or for a
 * proven build any byte not as shipped. */
function notPristine(c, buf) {
    var bad = unrecognised(c);
    if (bad.length) return brief(bad);
    var edited = EDITS.filter(function (e) { return c[e].state !== 'original'; });
    if (edited.length) return edited.join(', ') + ' edited';
    var proven = PROVEN[c.loc.buildId], hash = sha256(buf);
    return proven && hash !== proven.sha256 ? 'sha256 ' + proven.sha256 + ', got ' + hash : null;
}

function oursIn(c) {
    return c.ok && (c.variant.state === 'patched' || c.sink.state === 'redirected' || c.selfstart.state === 'patched' || c.res.state === 'patched');
}

function label(c) {
    var proven = PROVEN[c.loc.buildId];
    return proven ? proven.version : c.loc.buildId ? 'build ' + c.loc.buildId : 'build without a build-id';
}

/* The byte ranges the four edits write: apply changes only these, and revert
 * restores only these, so changes another tool made elsewhere stay. */
function regions(loc) {
    var r = loc.res;
    return [[loc.variant.slot, 4], [loc.sink.offset, loc.sink.len], [loc.selfstart.offset, 5],
        [r.mm, 8], [r.px, 8], [r.safe1, 8], [r.safe2, 8]];
}

/* Two classified images are the same build: the same size, build-id and edit
 * sites. */
function sameBuild(a, b) {
    return a.ok && b.ok && a.loc.ok && b.loc.ok && a.size === b.size && a.loc.buildId === b.loc.buildId &&
        JSON.stringify(regions(a.loc)) === JSON.stringify(regions(b.loc));
}

function readTarget(t) {
    try { return fs.readFileSync(t); } catch (e) { die('cannot read ' + t + ': ' + e.message); }
}

function pad(s) { return (s + ':          ').slice(0, Math.max(11, s.length + 2)); }
function line(name, text) { process.stdout.write(pad(name) + text + '\n'); }

function describe(name, s) {
    if (s.state === 'unknown') return 'UNKNOWN (' + s.reason + (s.res ? ': ' + geomText(s.res) : '') + ')';
    if (name === 'variant') return s.state === 'original' ? 'ORIGINAL (2nd display gated off for variant 7)' : 'PATCHED -> IC_' + s.panel.toUpperCase();
    if (name === 'sink') return s.state === 'original' ? 'ORIGINAL (2nd stream -> AVB stream 15)' : 'REDIRECTED -> ' + s.url;
    if (name === 'selfstart') return s.state === 'original' ? 'ORIGINAL (waits for NavController; stream never starts)' : 'SELF-START (CarPlay starts the cluster stream itself)';
    return s.state === 'original' ? 'ORIGINAL (IC_HIGH advertises ' + s.res.w + 'x' + s.res.h + ')' : 'PATCHED (IC_HIGH advertises ' + geomText(s.res) + ')';
}

function header(target, buf, c) {
    line('target', target);
    line('size', buf.length);
    line('sha256', sha256(buf));
    line('build-id', c.loc.buildId || 'none');
    line('version', c.loc.version || 'unknown');
}

function cmdStatus(target) {
    var buf = readTarget(target), c = classify(buf);
    header(target, buf, c);
    var p;
    try { p = restorePlan(target, buf, c); } catch (e) { p = { err: e.message }; }
    line('restorable', p.err ? 'no (' + p.err + ')' : p.b.other ? 'no (another build: ' + p.b.other + ')' : !p.out.equals(buf) || modeOnly(target, buf, p) ? 'yes' : 'no');
    var bad = unrecognised(c);
    if (!c.ok) { line('known', 'NO — ' + c.reason); return; }
    /* A backup apply would refuse (another build's original, or
     * not the original recorded) makes the build one this tool cannot patch. */
    var bak = backupPath(target), hasBak = fs.existsSync(bak), bakWhy = '';
    if (hasBak && !bad.length) {
        var b;
        try { b = checkedBackup(target, p.bc && !p.b.stock ? { buf: p.b.buf, c: p.bc } : null); } catch (e) { b = { err: e.message }; }
        bakWhy = b.err ? 'backup ' + bak + ' is not a pristine original'
            : !sameBuild(c, b.c) ? 'backup ' + bak + ' is the original of another carplay build' : '';
    }
    line('known', bad.length ? 'NO — ' + brief(bad) : bakWhy ? 'NO — ' + bakWhy
        : notPristine(c, buf) ? 'yes, modified build' : 'yes, pristine ' + label(c));
    line('proven', PROVEN[c.loc.buildId] ? 'yes' : 'no');
    EDITS.forEach(function (e) { line(e, describe(e, c[e])); });
    line('backup', hasBak ? bak : 'none');
}

/* Where each edit sits and how it reads, and whether the patcher can apply
 * to this build. Read-only. */
function cmdProbe(target) {
    var buf = readTarget(target), c = classify(buf), loc = c.loc;
    header(target, buf, c);
    if (!c.ok) { line('patchable', 'no — ' + c.reason); return; }
    line('proven', PROVEN[loc.buildId] ? 'yes, ' + PROVEN[loc.buildId].version : 'no');
    var at = {
        variant: loc.variant.ok && 'slot ' + hex(loc.variant.slot) + ' (table ' + hex(loc.variant.table) + ', ' + loc.variant.slots + ' slots): ',
        sink: loc.sink.ok && 'slot ' + hex(loc.sink.offset) + ' (' + loc.sink.len + ' bytes): ',
        selfstart: loc.selfstart.ok && 'call ' + hex(loc.selfstart.offset) + ' in onSuggestUi ' + hex(loc.selfstart.onSuggestUi) +
            ' (setStreamStatus ' + hex(loc.selfstart.setStreamStatus) + ', requestStream ' + hex(loc.selfstart.requestStream) + '): ',
        res: loc.res.ok && 'immediates ' + [loc.res.mm, loc.res.px, loc.res.safe1, loc.res.safe2].map(hex).join(' ') +
            ' (builder ' + hex(loc.res.builder) + '): ',
    };
    EDITS.forEach(function (e) { line(e, (at[e] || '') + describe(e, c[e])); });
    var bad = unrecognised(c);
    line('patchable', bad.length ? 'no — ' + bad.join('; ') : 'yes');
}

/* The sink URL a --capture/--sink apply writes, or undefined for the stock AVB
 * sink (the AVB test). --capture <fifo> is sugar for a file:// FIFO; --sink
 * <url> takes a full udp://, tcp:// or file:// URL. */
function sinkUrlFor(capture, sink) {
    if (sink !== undefined) {
        if (sink === null || !/^(file|udp|tcp):\/\/[^\0]+$/.test(sink)) die('--sink needs a file://, udp:// or tcp:// URL');
        return sink;
    }
    if (capture === undefined) return undefined;
    if (capture === null) die('--capture needs an absolute FIFO path');
    if (!/^\/[^\0]+$/.test(capture)) die('--capture must be an absolute path');
    return 'file://' + capture + '?mime_type=video/H264';
}

/* The geometry an apply writes, or null for the build's stock IC_HIGH
 * geometry (--res stock and the mid panel; the mid builder is never
 * rewritten). The high panel advertises this tool's display size: --res WxH,
 * or the default, at the map area's physical size, with the default safe
 * area scaled to that size, --safe WxH+X+Y, or --safe full (the whole frame). */
function resFor(panel, res, safe) {
    if (res !== undefined && panel !== 'high') die('--res rewrites the IC_HIGH geometry; it goes with --panel high');
    if (safe !== undefined && (panel !== 'high' || res === 'stock')) die('--safe sets the safe area of the advertised size; it goes with --panel high, not --res stock');
    if (panel !== 'high' || res === 'stock') return null;
    var w = RES_DEFAULT.w, h = RES_DEFAULT.h;
    if (res !== undefined) {
        var m = /^([0-9]+)x([0-9]+)$/.exec(res || '');
        if (!m) die('--res needs WxH (e.g. 1088x480) or "stock"');
        w = parseInt(m[1], 10); h = parseInt(m[2], 10);
        if (w < 16 || w > 1920 || h < 16 || h > 1080 || (w % 2) !== 0 || (h % 2) !== 0)
            die('--res ' + res + ' out of range: width 16..1920, height 16..1080, both even (H.264)');
    }
    /* The default safe area's edges, scaled from the default size. */
    var d = SAFE_DEFAULT, dw = RES_DEFAULT.w, dh = RES_DEFAULT.h;
    var x0 = Math.round(d.x * w / dw), x1 = Math.round((d.x + d.w) * w / dw);
    var y0 = Math.round(d.y * h / dh), y1 = Math.round((d.y + d.h) * h / dh);
    var g = { w: w, h: h, mmw: RES_DEFAULT.mmw, mmh: RES_DEFAULT.mmh, safew: x1 - x0, safeh: y1 - y0, safex: x0, safey: y0 };
    if (safe === 'full') {
        g.safew = w; g.safeh = h; g.safex = 0; g.safey = 0;
    } else if (safe !== undefined) {
        var sm = /^([0-9]+)x([0-9]+)\+([0-9]+)\+([0-9]+)$/.exec(safe || '');
        if (!sm) die('--safe needs WxH+X+Y (e.g. 504x368+286+72) or "full"');
        g.safew = parseInt(sm[1], 10); g.safeh = parseInt(sm[2], 10);
        g.safex = parseInt(sm[3], 10); g.safey = parseInt(sm[4], 10);
        if (g.safew < 16 || g.safeh < 16 || g.safex + g.safew > w || g.safey + g.safeh > h)
            die('--safe ' + safe + ' out of range: at least 16x16, inside the ' + w + 'x' + h + ' frame');
    }
    return g;
}

/* The backup, checked for an apply: its SHA-256 is the one recorded when it
 * was taken, and it is a pristine build. `pre`, when given, is the backup
 * already read and classified ({ buf, c }). { buf, hash, c } or { err }. */
function checkedBackup(target, pre) {
    var orig = pre ? pre.buf : fs.readFileSync(backupPath(target)), hash = sha256(orig), meta = null;
    try { meta = JSON.parse(fs.readFileSync(metaPath(target), 'utf8')); } catch (e) {}
    if (meta && meta.sha256_original && meta.sha256_original !== hash) return { err: 'sha256 ' + hash + ' != recorded ' + meta.sha256_original };
    var c = pre ? pre.c : classify(orig), why = notPristine(c, orig);
    return why ? { err: why } : { buf: orig, hash: hash, c: c };
}

/* The most bytes a binary may differ in from its backup and still count as
 * the backup's build when either lacks a GNU build-id: the four edits' bytes
 * (variant 4, sink 59, self-start 5, geometry 4 x 8), twice over. */
var SAME_BUILD_MAX_DIFF = 2 * (4 + 59 + 5 + 4 * 8);

/* Why `cur` is not the build `orig` is the original of, or null when it is:
 * the same build-id and size, or, when either has no build-id, the same size
 * and at most SAME_BUILD_MAX_DIFF bytes apart. */
function otherBuild(cur, orig) {
    if (cur.length !== orig.length) return 'its size differs';
    var a = locator.buildId(cur), b = locator.buildId(orig);
    if (a && b) return a === b ? null : 'its build-id differs';
    for (var i = 0, n = 0; i < cur.length; i++) {
        if (cur[i] !== orig[i] && ++n > SAME_BUILD_MAX_DIFF) return 'no build-id, and more than ' + SAME_BUILD_MAX_DIFF + ' bytes differ';
    }
    return null;
}

/* The backup, checked for a restore without the locator, so a binary any
 * version of this tool patched can be restored: its SHA-256 and GNU build-id
 * are the recorded ones (each when recorded). { buf, hash, meta (the record,
 * or null), other } or { err }; other says why `cur` is not the backup's
 * build (which writing the backup whole would replace), null when it is. */
function trustedBackup(target, cur) {
    var orig, meta = null;
    try { orig = fs.readFileSync(backupPath(target)); } catch (e) { return { err: 'no backup' }; }
    try { meta = JSON.parse(fs.readFileSync(metaPath(target), 'utf8')); } catch (e) {}
    var hash = sha256(orig), id = locator.buildId(orig);
    if (meta && meta.sha256_original && meta.sha256_original !== hash) return { err: 'sha256 ' + hash + ' != recorded ' + meta.sha256_original };
    if (meta && meta.buildId && id && meta.buildId !== id) return { err: 'build-id ' + id + ' != recorded ' + meta.buildId };
    return { buf: orig, hash: hash, meta: meta, other: otherBuild(cur, orig) };
}

/* What a revert of `cur` (classified as `c`) writes, without --force:
 * { b, bc (the backup classified), regionWise, out } or { err }. When the
 * locator recognises the target and the backup as the same build, the
 * backup's bytes in the edits' regions only (another tool's changes stay);
 * otherwise the whole backup. A backup with no record of the original (an
 * older release's, cut short by a power loss) is written whole only when the
 * locator reads it as the pristine build or it has the target's size. */
function restorePlan(target, cur, c) {
    var b = trustedBackup(target, cur);
    if (b.err) {
        /* No backup to trust: a proven build with an edit of ours is
         * rebuilt from its own bytes; the shipped file is its own original. */
        var so = locator.stockOriginal(PROVEN, cur);
        if (!so || (!so.equals(cur) && !oursIn(c))) return b;
        return { b: { buf: so, hash: sha256(so), meta: false, other: null, stock: true }, bc: classify(so), regionWise: false, out: so };
    }
    var orig = b.buf, bc = classify(orig), pristine = !notPristine(bc, orig);
    if (!b.other && !unrecognised(c).length && pristine && sameBuild(c, bc)) {
        var out = Buffer.from(cur);
        regions(c.loc).forEach(function (r) { orig.copy(out, r[0], r[0], r[0] + r[1]); });
        return { b: b, bc: bc, regionWise: true, out: out };
    }
    if (!b.meta && !pristine && orig.length !== cur.length) return { err: 'no record of the original, and it is not the pristine build' };
    return { b: b, bc: bc, regionWise: false, out: orig };
}

function cmdApply(target, panel, capture, sink, res, safe, yes, unproven) {
    var hu = headUnit.refused();
    if (hu) die('refusing: ' + hu);
    if (panel !== 'high' && panel !== 'mid') die('--panel must be high or mid');
    var wantRes = resFor(panel, res, safe);
    var sinkUrl = sinkUrlFor(capture, sink), slotLen = locator.SINK_STOCK.length;
    if (sinkUrl !== undefined && Buffer.byteLength(sinkUrl, 'latin1') > slotLen)
        die('sink URL too long: "' + sinkUrl + '" is ' + Buffer.byteLength(sinkUrl, 'latin1') + ' bytes, slot holds ' + slotLen);

    var buf = readTarget(target), c = classify(buf), bad = unrecognised(c);
    if (bad.length) die('refusing: ' + bad.join('; '));

    /* Back up the pristine original before the first edit; require that a
     * pre-existing backup is the pristine original of this build. */
    var bak = backupPath(target), orig = buf;
    if (!fs.existsSync(bak)) {
        var why = notPristine(c, buf);
        if (why) die('first edit requires the pristine build: ' + why);
        if (!PROVEN[c.loc.buildId] && !unproven) die(label(c) + ' (carplay ' + (c.loc.version || 'version unknown') + ') is not a proven build; pass --unproven to patch it anyway');
        /* The backup is written and checked under a temporary name, its record
         * written in full and renamed into place, then the backup renamed: a
         * backup never exists without its record. */
        var bakTmp = bak + '.tmp', metaTmp = metaPath(target) + '.tmp';
        [bakTmp, metaTmp].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });   /* an earlier apply's, cut short */
        fs.writeFileSync(bakTmp, buf, { mode: 0o600 });
        fsyncFile(bakTmp);
        if (!fs.readFileSync(bakTmp).equals(buf)) { try { fs.unlinkSync(bakTmp); } catch (e) {} die('backup verification failed; no change made'); }
        var origStat = fs.statSync(target);   /* the original's mode and owner, which every later write gives carplay */
        fs.writeFileSync(metaTmp, JSON.stringify({ target: path.resolve(target), version: label(c), buildId: c.loc.buildId,
            sha256_original: sha256(buf), mode: origStat.mode & 0o7777, uid: origStat.uid, gid: origStat.gid,
            savedAt: new Date().toISOString() }, null, 2) + '\n', { mode: 0o600 });
        fsyncFile(metaTmp);
        fs.renameSync(metaTmp, metaPath(target));
        fs.renameSync(bakTmp, bak);
    } else {
        var b = checkedBackup(target);
        if (b.err) die('existing backup ' + bak + ' is not a pristine original (' + b.err + '); refusing');
        if (!sameBuild(c, b.c)) die('existing backup ' + bak + ' is the original of another build; refusing');
        orig = b.buf;
    }

    var loc = c.loc, out = Buffer.from(buf), changed = [];
    function put(what, off, bytes) {
        if (out.slice(off, off + bytes.length).equals(bytes)) return;
        bytes.copy(out, off);
        if (changed.indexOf(what) < 0) changed.push(what);
    }
    var slot = Buffer.alloc(4);
    slot.writeUInt32LE(panel === 'high' ? loc.variant.high : loc.variant.mid, 0);
    put('variant -> IC_' + panel.toUpperCase(), loc.variant.slot, slot);

    /* With a sink URL, redirect the 2nd stream to this tool. Without one, ensure
     * the sink is the stock avb:// endpoint (resetting it if a previous apply
     * redirected it), so `apply --panel X` alone is the AVB test mode: variant
     * patched, stock sink. */
    var sinkWant = Buffer.alloc(slotLen, 0);
    sinkWant.write(sinkUrl !== undefined ? sinkUrl : locator.SINK_STOCK, 'latin1');
    put(sinkUrl !== undefined ? 'sink -> ' + sinkUrl : 'sink -> stock AVB', loc.sink.offset, sinkWant);

    /* Self-start is applied in every mode: without it NavController never
     * forwards the start command and the phone never streams, whatever the sink
     * is. Enabling the map therefore always ensures this edit is present. */
    put('self-start -> requestStream', loc.selfstart.offset, Buffer.from(loc.selfstart.patched, 'hex'));

    /* The advertised IC_HIGH geometry: this tool's display size on the high
     * panel, the backup's stock immediates otherwise, so the binary carries
     * one panel's geometry at a time. */
    var resWant = Buffer.from(out);
    if (wantRes) writeRes(resWant, loc.res, wantRes);
    var resText = wantRes ? 'resolution -> ' + geomText(wantRes) : 'resolution -> stock ' + readRes(orig, loc.res).w + 'x' + readRes(orig, loc.res).h;
    [loc.res.mm, loc.res.px, loc.res.safe1, loc.res.safe2].forEach(function (o) {
        put(resText, o, (wantRes ? resWant : orig).slice(o, o + 8));
    });

    var rec = { patched: true, proven: !!PROVEN[loc.buildId] };
    if (changed.length === 0) {
        repairMode(target, yes);
        process.stdout.write('nothing to do; already in the requested state\n'); record.ensure(target, rec); return;
    }

    /* The edited image must read as exactly the requested state, at the same
     * sites, changed only in the edit regions, before it is written. */
    var cc = classify(out);
    var wrong = !sameBuild(c, cc) ? 'the edit sites' : cc.variant.panel !== panel ? 'variant'
        : cc.sink.state !== (sinkUrl !== undefined ? 'redirected' : 'original') ? 'sink'
        : cc.selfstart.state !== 'patched' ? 'self-start'
        : cc.res.state !== (wantRes ? 'patched' : 'original') ? 'resolution'
        : !onlyRegionsDiffer(buf, out, regions(loc)) ? 'bytes outside the edit regions' : null;
    if (wrong) die('refusing: the edited binary does not verify (' + wrong + '); no change made');

    process.stdout.write('About to patch ' + target + ':\n');
    changed.forEach(function (x) { process.stdout.write('  ' + x + '\n'); });
    if (!yes) die('pass --yes to proceed');

    writeInPlace(target, out);
    var after = readTarget(target);
    if (!after.equals(out)) die('post-write verification failed: ' + target + ' does not hold the edited bytes; restore from ' + bak);
    record.ensure(target, rec);
    process.stdout.write('patched OK. sha256 now ' + sha256(after) + '\n');
    process.stdout.write('revert:  carplay_patch.js revert --target ' + target + ' --yes\n');
}

/* Revert writes what restorePlan says: the edits' regions of a target the
 * locator recognises as the backup's build, else the whole backup over the
 * backup's own build, which also undoes a patch any version of this tool
 * wrote at a site the locator now refuses. --force writes the whole backup
 * over another build too, never a backup restorePlan refuses. --drop-backup
 * deletes the backup once none of the edits is left in the target, for a
 * removal of the tool: nothing is left to revert. */
function cmdRevert(target, yes, force, dropBackup) {
    function drop() {
        if (!dropBackup) return;
        [bak, metaPath(target), bak + '.tmp', metaPath(target) + '.tmp', record.recordPath(target)].forEach(function (f) {
            try { fs.unlinkSync(f); } catch (e) { if (e.code !== 'ENOENT') die('dropping ' + f + ': ' + e.message); }
        });
        process.stdout.write('backup dropped\n');
    }
    var bak = backupPath(target);
    var cur = readTarget(target), p;
    try { p = restorePlan(target, cur, classify(cur)); } catch (e) { p = { err: e.message }; }
    if (p.err) die(fs.existsSync(bak) ? 'backup ' + bak + ' is not a verified original (' + p.err + '), and ' + target + ' cannot be rebuilt from its shipped bytes; refusing to restore it'
        : 'no backup at ' + bak + ', and ' + target + ' cannot be rebuilt from its shipped bytes; cannot revert');
    var b = p.b, orig = b.buf;
    if (cur.equals(orig)) {
        if (modeOnly(target, cur, p)) repairMode(target, yes);
        process.stdout.write('already at the original; nothing to do\n'); record.ensure(target, { patched: false }); drop(); return;
    }
    /* Another build (a firmware update replaced carplay and left the old
     * backup behind): restoring would downgrade it. */
    if (b.other && !force) {
        die('current ' + target + ' is another build than the backup (' + b.other + ').' +
            '\n  It may have been replaced (e.g. a firmware update). Refusing to overwrite it.' +
            '\n  Pass --force only if you are sure this backup is the right original for it.');
    }
    var regionWise = p.regionWise && !force, out = regionWise ? p.out : orig;
    if (regionWise && out.equals(cur)) { process.stdout.write('nothing to do: none of the edits is in it; other changes kept\n'); record.ensure(target, { patched: false }); drop(); return; }
    process.stdout.write('About to restore ' + (regionWise ? 'the edited regions of ' : '') + target + ' from ' +
        (b.stock ? 'the proven build\'s shipped bytes' : bak) + ' (sha256 ' + b.hash + ')\n');
    if (!yes) die('pass --yes to proceed');
    writeInPlace(target, out);
    var after = readTarget(target), ca = regionWise ? classify(after) : null;
    if (!after.equals(out) || (ca && EDITS.some(function (e) { return !ca.ok || ca[e].state !== 'original'; }))) die('restore verification failed');
    record.ensure(target, { patched: false });
    process.stdout.write(after.equals(orig) ? 'restored OK. sha256 now ' + b.hash + '\n'
        : 'restored the edited regions; changes outside them kept. sha256 now ' + sha256(after) + '\n');
    if (dropBackup) drop();
    else if (!b.stock) process.stdout.write('You may delete the backup: ' + bak + ' and ' + metaPath(target) + '\n');
}

function cmdVerify(target) {
    var c = classify(readTarget(target)), bad = unrecognised(c);
    if (bad.length) { process.stderr.write('FAIL: ' + bad.join('; ') + '\n'); process.exit(1); }
    /* A fully enabled build has every edit its panel needs. A variant/
     * sink-patched build with stock self-start (an Edit-1/2-only binary)
     * would advertise the display but never start the stream; a high-panel
     * build with stock resolution advertises a display size this tool does
     * not show. Verify calls neither enabled. The mid panel keeps the stock
     * geometry by design. */
    var resOk = c.variant.panel !== 'high' || c.res.state === 'patched';
    var enabled = c.variant.state === 'patched' && c.selfstart.state === 'patched' && resOk;
    var partial = (c.variant.state === 'patched' || c.selfstart.state === 'patched' || c.res.state === 'patched') && !enabled;
    if (partial) { process.stderr.write('FAIL: partially patched — variant ' + c.variant.state + ', self-start ' + c.selfstart.state + ', res ' + c.res.state + ' (apply applies all edits)\n'); process.exit(1); }
    process.stdout.write('OK: variant ' + (c.variant.state === 'original' ? 'original' : 'IC_' + c.variant.panel.toUpperCase()) +
        ', sink ' + (c.sink.state === 'original' ? 'original (AVB 15)' : c.sink.url) +
        ', self-start ' + (c.selfstart.state === 'patched' ? 'on' : 'off') +
        ', res ' + (c.res.state === 'patched' ? c.res.res.w + 'x' + c.res.res.h : 'stock') + '\n');
}

function fsyncFile(path) {
    var fd = fs.openSync(path, 'r+');
    try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
}

/* carplay's mode and owner (file_mode.js): its original's, recorded with
 * the backup, at least 0755. */
function wantedMode(target) {
    var meta = null;
    try { meta = JSON.parse(fs.readFileSync(metaPath(target), 'utf8')); } catch (e) {}
    return fileMode.wanted(target, meta, fileMode.EXEC);
}

/* Whether target holds exactly the verified pristine backup of ours but
 * carplay.service's user cannot run it: a revert sets only its mode. A
 * carplay without a backup of ours keeps its mode. */
function modeOnly(target, cur, p) {
    return !p.err && !p.b.other && !p.b.stock && cur.equals(p.b.buf) && !notPristine(p.bc || classify(p.b.buf), p.b.buf) && fileMode.lacks(target, fileMode.EXEC);
}

/* target given its original's mode and owner, with --yes, when
 * carplay.service's user cannot run it. */
function repairMode(target, yes) {
    if (!fileMode.lacks(target, fileMode.EXEC)) return;
    var want = wantedMode(target), fix = fileMode.mismatch(target, want);
    if (!yes) die(target + ' has ' + fix + '; pass --yes to set them');
    try { fileMode.repair(target, want); } catch (e) { die('setting ' + target + '\'s mode and owner: ' + e.message); }
    process.stdout.write('set ' + target + '\'s mode and owner (it had ' + fix + ')\n');
}

/* Write by atomic rename so a crash mid-write cannot leave carplay half
 * patched; fall back to in-place rewrite on a cross-device/busy rename.
 * Either way carplay gets its mode and owner explicitly (file_mode.js). The
 * caller remounts / read-write around this (deploy's remount window). */
function writeInPlace(target, buf) {
    try { fileMode.replace(target, buf, wantedMode(target), true); } catch (e) { die('writing ' + target + ': ' + e.message); }
}

function onlyRegionsDiffer(a, b, regions) {
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) {
        if (a[i] === b[i]) continue;
        var inside = regions.some(function (r) { return i >= r[0] && i < r[0] + r[1]; });
        if (!inside) return false;
    }
    return true;
}

function parseArgs(argv) {
    var a = { _: [] };
    for (var i = 0; i < argv.length; i++) {
        var t = argv[i];
        if (t === '--yes') a.yes = true;
        else if (t === '--force') a.force = true;
        else if (t === '--drop-backup') a.dropBackup = true;
        else if (t === '--unproven') a.unproven = true;
        else if (t === '--target') a.target = argv[++i];
        else if (t === '--panel') a.panel = argv[++i];
        else if (t === '--capture') {
            /* A missing or flag-shaped value is an error, not "no capture". */
            a.capture = (i + 1 < argv.length && argv[i + 1].indexOf('--') !== 0) ? argv[++i] : null;
        } else if (t === '--sink') {
            a.sink = (i + 1 < argv.length && argv[i + 1].indexOf('--') !== 0) ? argv[++i] : null;
        } else if (t === '--res') {
            a.res = (i + 1 < argv.length && argv[i + 1].indexOf('--') !== 0) ? argv[++i] : null;
        } else if (t === '--safe') {
            a.safe = (i + 1 < argv.length && argv[i + 1].indexOf('--') !== 0) ? argv[++i] : null;
        } else a._.push(t);
    }
    return a;
}

function main() {
    var a = parseArgs(process.argv.slice(2));
    var cmd = a._[0];
    var target = a.target || '/usr/bin/carplay';
    if (cmd === 'status') { cmdStatus(target); line('end', 'complete'); }
    else if (cmd === 'probe') cmdProbe(a.target || a._[1] || '/usr/bin/carplay');
    else if (cmd === 'apply') cmdApply(target, a.panel, a.capture, a.sink, a.res, a.safe, a.yes, a.unproven);
    else if (cmd === 'revert') cmdRevert(target, a.yes, a.force, a.dropBackup);
    else if (cmd === 'verify') cmdVerify(target);
    else { process.stderr.write('usage: carplay_patch.js status|probe|apply|revert|verify [--target <file>] [--panel high|mid] [--capture <fifo> | --sink <url>] [--res WxH|stock] [--safe WxH+X+Y|full] [--unproven] [--yes] [--force] [--drop-backup]\n'); process.exit(2); }
}

module.exports = { trustedBackup: trustedBackup, PROVEN: PROVEN };

if (require.main === module) main();
