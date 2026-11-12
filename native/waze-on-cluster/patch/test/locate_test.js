#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Proves carplay_locate.js and carplay_patch.js on synthetic builds
 * (synthetic_carplay.js): every site found where the build put it, also with
 * other offsets, addresses and code order, and in its patched state; a
 * signature found twice refused as ambiguous; a damaged site, a broken
 * cross-check or a relocation over an edit refused with the reason; apply
 * (only with --unproven), verify, probe and revert on a build outside the
 * proven list, byte-exact; a file with carplay 21.27.4's build-id but not its
 * shipped bytes proven yet refused; the map URL sites found by structure,
 * patched, partly patched, and each of their checks broken refused while the
 * other edits apply; carplay 22.28.3's stock bytes of them checked against
 * the lea they encode. With CARPLAY_BIN, the reference build's sites are the
 * documented offsets.
 *
 *   node locate_test.js
 */
'use strict';

require('./head_unit_env.js');   /* a HIGH head unit for every patcher and patchd this runs */

var fs = require('fs');
var os = require('os');
var path = require('path');
var cp = require('child_process');
var locator = require('../carplay_locate.js');
var synthetic = require('./synthetic_carplay.js');

var SCRIPT = path.join(__dirname, '..', 'carplay_patch.js');

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}
function run(args) {
    var r = cp.spawnSync(process.execPath, [SCRIPT].concat(args), { encoding: 'utf8' });
    return { code: r.status, out: (r.stdout || '') + (r.stderr || '') };
}
function json(x) { return JSON.stringify(x); }

/* The sites the locator reports, in the fixture's terms. */
function sites(loc) {
    return {
        buildId: loc.buildId, table: loc.variant.table, slot: loc.variant.slot,
        original: loc.variant.original, high: loc.variant.high, mid: loc.variant.mid, sink: loc.sink.offset,
        call: loc.selfstart.offset, setStreamStatus: loc.selfstart.setStreamStatus, requestStream: loc.selfstart.requestStream,
        onSuggestUi: loc.selfstart.onSuggestUi, callOriginal: loc.selfstart.original, callPatched: loc.selfstart.patched,
        builder: loc.res.builder, mm: loc.res.mm, px: loc.res.px, safe1: loc.res.safe1, safe2: loc.res.safe2,
    };
}
function expected(at) { var e = {}; Object.keys(sites({ variant: {}, sink: {}, selfstart: {}, res: {} })).forEach(function (k) { e[k] = at[k]; }); return e; }
/* The map URL sites the locator reports, and where the fixture put them. */
function mapSites(loc) { var m = loc.mapurl; return m.ok ? { spare: m.slot, urlLea: m.lea, info: m.builder, log: m.log } : m.reason; }
function mapExpected(at) { return { spare: at.spare, urlLea: at.urlLea, info: at.info, log: at.log }; }
/* The fixture's map URL edit written with the bytes the locator reports for
 * it (the patched string and the lea at the URL); patch_test.js checks those
 * bytes against carplay 22.28.3's. */
function writeMapUrl(b, at, which) {
    var m = locator.locate(b).mapurl;
    if (which !== 'lea') Buffer.from(m.slotBytes, 'hex').copy(b, at.spare);
    if (which !== 'slot') Buffer.from(m.leaBytes, 'hex').copy(b, at.urlLea);
}
function reasons(loc) { return ['variant', 'sink', 'selfstart', 'res'].filter(function (e) { return !loc[e].ok; }).map(function (e) { return loc[e].reason; }); }

var SHIFTED = { pad: 0x123, codeVa: 0x7a000, dataVa: 0x1234000, dataAt: 0x2800, highViaJne: true, midFirst: true };

/* Found where the build put them. */
[['synthetic build', {}], ['shifted build (other offsets, addresses, code order, jne branch)', SHIFTED]].forEach(function (t) {
    var s = synthetic.build(t[1]), loc = locator.locate(s.buf);
    ok(t[0] + ': every site found where it is', loc.ok && json(sites(loc)) === json(expected(s.at)), json(loc));
    ok(t[0] + ': the map URL sites found where they are, stock', json(mapSites(loc)) === json(mapExpected(s.at)) &&
        !loc.mapurl.slotPatched && !loc.mapurl.leaPatched, json(loc.mapurl));
});
var plain = synthetic.build({}), shifted = synthetic.build(SHIFTED);
ok('the shifted build moves every site', ['slot', 'sink', 'call', 'mm'].every(function (k) { return plain.at[k] !== shifted.at[k]; }));

/* The patched state is found at the same sites. */
(function () {
    var s = synthetic.build(SHIFTED), b = Buffer.from(s.buf);
    b.writeUInt32LE(s.at.high, s.at.slot);
    Buffer.alloc(59).copy(b, s.at.sink); b.write('udp://127.0.0.1:20557?mime_type=video/H264', s.at.sink, 'latin1');
    Buffer.from(s.at.callPatched, 'hex').copy(b, s.at.call);
    b.writeUInt32LE(138, s.at.mm); b.writeUInt32LE(1088, s.at.px);
    writeMapUrl(b, s.at);
    var loc = locator.locate(b);
    ok('patched build: the same sites', loc.ok && json(sites(loc)) === json(expected(s.at)), json(loc));
    ok('patched build: the same map URL sites, both patched, the lea at the URL', json(mapSites(loc)) === json(mapExpected(s.at)) &&
        loc.mapurl.slotPatched && loc.mapurl.leaPatched && b.toString('latin1', loc.mapurl.url, loc.mapurl.url + 32) === locator.MAP_URL + '\0', json(loc.mapurl));
    ['slot', 'lea'].forEach(function (w) {
        var h = Buffer.from(s.buf);
        writeMapUrl(h, s.at, w);
        var m = locator.locate(h).mapurl;
        ok('only the ' + (w === 'slot' ? 'spare string' : 'lea') + ' patched: found, one site patched', m.ok && m.slotPatched === (w === 'slot') &&
            m.leaPatched === (w === 'lea'), json(m));
    });
    b.writeUInt32LE(s.at.mid, s.at.slot);
    ok('slot 7 set to slot 10\'s value: found', locator.locate(b).variant.ok);
})();

/* A signature found twice is ambiguous. */
[['two sink URL slots', { twoSinks: true }, 'sink: URL slot found 2 times'],
 ['a second converter switch outside codingToInternal', { twoConverters: true }, 'variant: converter switch also outside codingToInternal']].forEach(function (t) {
    var loc = locator.locate(synthetic.build(t[1]).buf);
    ok(t[0] + ': refused as ambiguous, the other edits found', !loc.ok && json(reasons(loc)) === json([t[2]]), json(reasons(loc)));
});

/* Each map URL check broken: that edit refused with what is wrong, the
 * other four found. */
[
    ['the log string twice', { twoSpares: true }, 'map URL: log string found 2 times'],
    ['the log string read twice', { spareReadTwice: true }, 'map URL: log string loaded 2 times (want 1)'],
    ['a data pointer into the log string', { pointSpare: true }, 'map URL: a data pointer into the log string'],
    ['the gate not variant <= 1', { gateCmp: 2 }, 'map URL: log not on the alternate-display gate\'s false branch'],
    ['the log block fallen into', { logFallsIn: true }, 'map URL: log block entered by falling through'],
    ['the log block jumped to from elsewhere', { logJumpedTo: true }, 'map URL: log block entered from elsewhere'],
    ['the initial URL built twice', { urlBuiltTwice: true }, 'map URL: initial URL built 2 times'],
    ['the initial URL built from another string', { urlFrom: 's.maps' }, 'map URL: initial URL built from neither "" nor the URL'],
    ['the display builder loads no string global', { noUrlGlobal: true }, 'map URL: the cluster display builder loads 0 string globals (want 1)'],
    ['the initial URL global without a destructor', { noUrlDtor: true }, 'map URL: initial URL global not registered for destruction'],
    ['the initial URL built by a function no string global is built by', { urlCtorOther: true }, 'map URL: initial URL global not built like the other string globals'],
    ['a dynamic relocation over the initial URL\'s lea', { relocUrl: true }, 'map URL: a dynamic relocation over the edit'],
].forEach(function (t) {
    var loc = locator.locate(synthetic.build(t[1]).buf);
    ok('map URL (' + t[0] + '): ' + t[2], loc.ok && !loc.mapurl.ok && loc.mapurl.reason === t[2], json([reasons(loc), loc.mapurl]));
});
(function () {
    var s = synthetic.build({}), b = Buffer.from(s.buf);
    b.write('Head-Unit does not support Alternate Displays Feature.', s.at.spare, 'latin1');
    var loc = locator.locate(b);
    ok('map URL (another log text): log string not found', loc.ok && loc.mapurl.reason === 'map URL: log string not found', json(loc.mapurl));
    b = Buffer.from(s.buf); b[s.at.getter2c + 3] = 0x2a;
    loc = locator.locate(b);
    ok('map URL (no cluster display builder): refused with the resolution edit', loc.mapurl.reason === 'map URL: no cluster display builder (resolution: geometry getters not found)', json(loc.mapurl));
})();

/* A damaged site is refused with what is wrong. */
[
    ['the IC_HIGH handler stores OTHER', function (b, at) { b.writeUInt32LE(2, at.highHandler + 2); }, 'variant: slot 9 is not IC_HIGH'],
    ['slot 7 points at the NOT_ACTIVE handler', function (b, at) { b.writeUInt32LE(b.readUInt32LE(at.table), at.slot); }, 'variant: slot 7 not OTHER/IC_HIGH/IC_MID'],
    ['another sink URL', function (b, at) { b.write('ws://', at.sink, 'latin1'); }, 'sink: URL slot not found'],
    ['no AVAILABLE argument', function (b, at) { b[at.call - 12] = 2; }, 'self-start: call has no AVAILABLE argument'],
    ['onSuggestUi calls something else', function (b, at) { b.writeInt32LE(b.readInt32LE(at.call + 1) + 0x40, at.call + 1); }, 'self-start: onSuggestUi call not found'],
    ['no ForceKeyframe check', function (b) { var i = b.indexOf('ForceKeyframe'); b.write('ForceKeyFrame', i, 'latin1'); }, 'self-start: ForceKeyframe string not found'],
    ['a geometry store broken', function (b, at) { b[at.px + 8] = 0x90; }, 'resolution: 1 geometry builders (want 2)'],
    ['requestStream only reached through onSuggestUi', function (b, at) { b.fill(0x90, at.cmdCall, at.cmdCall + 5); }, 'self-start: requestStream not called directly'],
    ['a getter reads another field', function (b, at) { b[at.getter2c + 3] = 0x2a; }, 'resolution: geometry getters not found'],
].forEach(function (t) {
    var s = synthetic.build({}), b = Buffer.from(s.buf);
    t[1](b, s.at);
    var loc = locator.locate(b);
    ok('damaged (' + t[0] + '): ' + t[2], json(reasons(loc)) === json([t[2]]), json(reasons(loc)));
});
(function () {
    var loc = locator.locate(synthetic.build({ noEhFrame: true }).buf);
    ok('no function table: variant, self-start and resolution say so',
        json(reasons(loc)) === json(['variant: no function table', 'self-start: no function table', 'resolution: no function table']), json(reasons(loc)));
    loc = locator.locate(synthetic.build({ relocSink: true }).buf);
    ok('a dynamic relocation over the sink slot: refused', json(reasons(loc)) === json(['sink: a dynamic relocation over the edit']), json(reasons(loc)));
    var rel = synthetic.build({ relocSink: true });
    rel.buf.writeUInt32LE(17, rel.at.dyn);   /* DT_REL in place of DT_RELA */
    loc = locator.locate(rel.buf);
    ok('relocations it cannot read: every edit refused', json(reasons(loc)) ===
        json(['variant: relocations not readable', 'sink: relocations not readable', 'self-start: relocations not readable', 'resolution: relocations not readable']), json(reasons(loc)));
    rel = synthetic.build({ relocSink: true });
    for (var ph = 64; rel.buf.readUInt32LE(ph) !== 2; ph += 56);
    rel.buf.writeUInt32LE(rel.buf.length - 8, ph + 8);   /* PT_DYNAMIC running past the end of the file */
    loc = locator.locate(rel.buf);
    ok('a dynamic segment outside the file: every edit refused, no throw', json(reasons(loc)) ===
        json(['variant: relocations not readable', 'sink: relocations not readable', 'self-start: relocations not readable', 'resolution: relocations not readable']), json(reasons(loc)));
    loc = locator.locate(Buffer.from('#!/bin/sh\nexec carplay "$@"\n'));
    ok('not an ELF', loc.elf === false && loc.reason === 'not an ELF file' && !loc.ok, json(loc));
})();

/* The patcher on a build outside the proven list. */
var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-locate-'));
try {
    var tgt = path.join(dir, 'carplay'), orig = shifted.buf, at = shifted.at, r;
    fs.writeFileSync(tgt, orig, { mode: 0o755 });
    function hx(n) { return '0x' + n.toString(16); }

    r = run(['status', '--target', tgt]);
    ok('status: pristine by its stock sites, every edit ORIGINAL', /known:\s+yes, pristine build [0-9a-f]{40}/.test(r.out) &&
        /variant:\s+ORIGINAL/.test(r.out) && /sink:\s+ORIGINAL/.test(r.out) && /selfstart:\s+ORIGINAL/.test(r.out) &&
        /res:\s+ORIGINAL \(IC_HIGH advertises 1600x800\)/.test(r.out), r.out);
    r = run(['probe', tgt]);
    ok('probe: the sites, not proven, patchable', r.code === 0 && /proven:\s+no/.test(r.out) && /patchable:\s+yes/.test(r.out) &&
        r.out.indexOf('slot ' + hx(at.slot) + ' (table ' + hx(at.table) + ', 12 slots)') >= 0 && r.out.indexOf('slot ' + hx(at.sink) + ' (59 bytes)') >= 0 &&
        r.out.indexOf('call ' + hx(at.call) + ' in onSuggestUi ' + hx(at.onSuggestUi)) >= 0 &&
        r.out.indexOf('immediates ' + [at.mm, at.px, at.safe1, at.safe2].map(hx).join(' ')) >= 0 &&
        r.out.indexOf('build-id:  ' + at.buildId) >= 0, r.out);

    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('apply without --unproven: refused, no write, no backup', r.code !== 0 && /is not a proven build; pass --unproven/.test(r.out) &&
        fs.readFileSync(tgt).equals(orig) && !fs.existsSync(tgt + '.woc-orig'), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--sink', 'udp://127.0.0.1:20557?mime_type=video/H264', '--unproven', '--yes']);
    var b = fs.readFileSync(tgt), want = Buffer.from(orig);
    writeMapUrl(want, at);
    ok('apply high: every edit written', r.code === 0 && /patched OK/.test(r.out) && /map URL -> maps:\/car\/instrumentcluster\/map/.test(r.out) &&
        b.readUInt32LE(at.slot) === at.high && b.toString('hex', at.call, at.call + 5) === at.callPatched && b.toString('latin1', at.sink, at.sink + 9) === 'udp://127' &&
        b.readUInt32LE(at.mm) === 138 && b.readUInt32LE(at.px) === 1088 &&
        b.slice(at.spare, at.spare + 55).equals(want.slice(at.spare, at.spare + 55)) && b.slice(at.urlLea, at.urlLea + 7).equals(want.slice(at.urlLea, at.urlLea + 7)), r.out);
    r = run(['status', '--target', tgt]);
    ok('status: the map URL PATCHED', /^mapurl:\s+PATCHED \(the phone streams the cluster map from connect: maps:\/car\/instrumentcluster\/map\)$/m.test(r.out), r.out);
    ok('apply high: the backup is the original', fs.readFileSync(tgt + '.woc-orig').equals(orig));
    r = run(['verify', '--target', tgt]);
    ok('verify: enabled', r.code === 0 && /OK: variant IC_HIGH, sink udp:\/\/127\.0\.0\.1:20557\?mime_type=video\/H264, self-start on, res 1088x480, map URL on/.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'mid', '--yes']);
    b = fs.readFileSync(tgt);
    ok('apply mid: the stock geometry back from the backup, the stock sink', r.code === 0 && /resolution -> stock 1600x800/.test(r.out) &&
        b.slice(at.mm, at.safe2 + 8).equals(orig.slice(at.mm, at.safe2 + 8)) && b.slice(at.sink, at.sink + 59).equals(orig.slice(at.sink, at.sink + 59)) &&
        b.readUInt32LE(at.slot) === at.mid, r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert: byte-exact original', r.code === 0 && fs.readFileSync(tgt).equals(orig), r.out);

    /* One map URL site written and not the other: unrecognised; apply
     * refuses it, revert writes the whole backup. */
    run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    b = fs.readFileSync(tgt);
    orig.copy(b, at.spare, at.spare, at.spare + 55);
    fs.writeFileSync(tgt, b);
    r = run(['status', '--target', tgt]);
    ok('partly patched map URL: not known, restorable', /^mapurl:\s+UNKNOWN \(map URL: partly patched\)$/m.test(r.out) &&
        /known:\s+NO — map URL: partly patched/.test(r.out) && /^restorable:\s+yes$/m.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('partly patched map URL: apply refuses, no write', r.code !== 0 && /refusing: map URL: partly patched/.test(r.out) && fs.readFileSync(tgt).equals(b), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('partly patched map URL: revert writes the whole backup', r.code === 0 && fs.readFileSync(tgt).equals(orig), r.out);

    /* The map URL edit written, then the target changed so the locator no
     * longer finds it there (its gate byte): revert still restores its
     * sites from the backup, and keeps that change. */
    run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    b = fs.readFileSync(tgt);
    b[at.gateCmp + 2] = 2;
    fs.writeFileSync(tgt, b);
    r = run(['status', '--target', tgt]);
    ok('map URL no longer found in the target: NOT APPLIED, restorable', /^mapurl:\s+NOT APPLIED/m.test(r.out) && /^restorable:\s+yes$/m.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    var gated = Buffer.from(orig); gated[at.gateCmp + 2] = 2;
    ok('map URL no longer found in the target: revert restores its sites, the other change kept', r.code === 0 && fs.readFileSync(tgt).equals(gated), r.out);
    fs.writeFileSync(tgt, orig);
    [tgt + '.woc-orig', tgt + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });

    /* A build where the map URL edit is not found: the other four apply,
     * verify and revert as without it. */
    var nmBuild = synthetic.build({ gateCmp: 2 }), noMap = nmBuild.buf, nm = nmBuild.at;
    fs.writeFileSync(tgt, noMap);
    [tgt + '.woc-orig', tgt + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    r = run(['status', '--target', tgt]);
    ok('map URL not found: NOT APPLIED with the reason, the build pristine', /^mapurl:\s+NOT APPLIED \(map URL: log not on the alternate-display gate's false branch\)$/m.test(r.out) &&
        /known:\s+yes, pristine build/.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    b = fs.readFileSync(tgt);
    ok('map URL not found: the other edits applied, not it', r.code === 0 && /patched OK/.test(r.out) && !/map URL ->/.test(r.out) &&
        b.readUInt32LE(nm.slot) === nm.high && b.toString('hex', nm.call, nm.call + 5) === nm.callPatched, r.out);
    var dd = []; for (var i = 0; i < b.length; i++) if (b[i] !== noMap[i]) dd.push(i);
    ok('map URL not found: the diff stays out of the log string and the lea', dd.length > 0 && dd.every(function (x) {
        return !(x >= nm.spare && x < nm.spare + 55) && !(x >= nm.urlLea && x < nm.urlLea + 7); }));
    r = run(['verify', '--target', tgt]);
    ok('map URL not found: verify passes, map URL n/a', r.code === 0 && /map URL n\/a/.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('map URL not found: revert restores it byte-exact', r.code === 0 && fs.readFileSync(tgt).equals(noMap), r.out);
    fs.writeFileSync(tgt, orig);
    [tgt + '.woc-orig', tgt + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });

    /* revert gives the target the original's mode recorded with the backup,
     * whatever mode the target has: carplay runs as a non-root user, which
     * needs group and other execute. */
    run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    fs.chmodSync(tgt, 0o700);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert: restores the recorded 0755, not the target\'s 0700',
        r.code === 0 && fs.readFileSync(tgt).equals(orig) && (fs.statSync(tgt).mode & 0o777) === 0o755,
        r.out + ' mode=' + (fs.statSync(tgt).mode & 0o777).toString(8));

    /* A build without a GNU build-id: the same build is the same size and at
     * most a few hundred bytes apart from the backup. */
    var noId = Buffer.from(orig);
    noId.writeUInt32LE(0, 0x208);   /* the build-id note's type */
    [tgt + '.woc-orig', tgt + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    fs.writeFileSync(tgt + '.woc-orig.tmp', 'cut short');        /* an earlier apply's, cut short */
    fs.writeFileSync(tgt + '.woc-orig.json.tmp', '{"sha');
    fs.writeFileSync(tgt, noId);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    ok('no build-id: apply writes the backup and its record, an earlier temporary file gone', r.code === 0 && fs.existsSync(tgt + '.woc-orig') &&
        fs.existsSync(tgt + '.woc-orig.json') && !fs.existsSync(tgt + '.woc-orig.tmp') && !fs.existsSync(tgt + '.woc-orig.json.tmp'), r.out);
    var noIdPatched = fs.readFileSync(tgt), mangled = Buffer.from(noIdPatched);
    mangled[at.highHandler] = 0x90;   /* a site the locator now refuses */
    fs.writeFileSync(tgt, mangled);
    r = run(['status', '--target', tgt]);
    ok('no build-id: a patch the locator refuses is restorable', /known:\s+NO/.test(r.out) && /^restorable: yes$/m.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('no build-id: revert writes the original whole', r.code === 0 && fs.readFileSync(tgt).equals(noId), r.out);
    fs.writeFileSync(tgt + '.woc-orig.tmp', 'cut short');
    fs.writeFileSync(tgt + '.woc-orig.json.tmp', '{"sha');
    r = run(['revert', '--target', tgt, '--yes', '--drop-backup']);
    ok('a removal drops the backup, its record and any temporary file', r.code === 0 && !fs.existsSync(tgt + '.woc-orig') &&
        !fs.existsSync(tgt + '.woc-orig.json') && !fs.existsSync(tgt + '.woc-orig.tmp') && !fs.existsSync(tgt + '.woc-orig.json.tmp'), r.out);
    run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    var far = Buffer.from(noId);
    for (var k = 0; k < 400; k++) far[0x300 + k] ^= 0xff;
    fs.writeFileSync(tgt, far);
    r = run(['status', '--target', tgt]);
    ok('no build-id: a binary hundreds of bytes away is another build', /^restorable: no \(another build: no build-id, and more than 324 bytes differ\)$/m.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('no build-id: revert refuses it without --force, saying why', r.code !== 0 && /another build than the backup \(no build-id, and more than 324 bytes differ\)/.test(r.out) &&
        fs.readFileSync(tgt).equals(far), r.out);

    /* A backup an older release left without its record, cut short: never
     * written, also with --force. */
    fs.writeFileSync(tgt, noIdPatched);
    fs.writeFileSync(tgt + '.woc-orig', noId.slice(0, noId.length - 4096));
    fs.unlinkSync(tgt + '.woc-orig.json');
    r = run(['revert', '--target', tgt, '--yes', '--force']);
    ok('a short backup with no record: revert --force refuses, no write', r.code !== 0 && /no record of the original/.test(r.out) &&
        fs.readFileSync(tgt).equals(noIdPatched), r.out);

    /* A backup whose build-id note cannot be read: status says why and goes on. */
    var badNote = Buffer.from(orig);
    badNote.writeUInt32LE(0x7ffffffe, 0x200);   /* the note's name size */
    fs.writeFileSync(tgt + '.woc-orig', badNote);
    r = run(['status', '--target', tgt]);
    ok('a backup with a malformed note: not restorable, with the reason, status complete', r.code === 0 && /^restorable: no \(.+\)$/m.test(r.out) &&
        /^backup: /m.test(r.out), r.out);
    [tgt + '.woc-orig', tgt + '.woc-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });

    var amb = synthetic.build({ twoSinks: true }).buf;
    fs.writeFileSync(tgt, amb);
    r = run(['probe', tgt]);
    ok('probe of an ambiguous build: not patchable, with the reason', /patchable:\s+no — sink: URL slot found 2 times/.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('status of an ambiguous build: known NO with the reason', /known:\s+NO — sink: URL slot found 2 times/.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('apply refuses it, no write', r.code !== 0 && /refusing: sink: URL slot found 2 times/.test(r.out) && fs.readFileSync(tgt).equals(amb), r.out);

    /* A file with carplay 21.27.4's build-id and every site stock, but not
     * that build's shipped file: on the proven list, so its first edit needs
     * the shipped file, --unproven or not. */
    var b2127 = Buffer.from(orig);
    Buffer.from('389c1d0536f8cc9283c156410f2f2634a9f952c0', 'hex').copy(b2127, b2127.indexOf(Buffer.from(at.buildId, 'hex')));
    fs.writeFileSync(tgt, b2127);
    r = run(['probe', tgt]);
    ok('carplay 21.27.4 build-id: probe reads it proven and patchable', r.code === 0 && /proven:\s+yes, carplay 21\.27\.4/.test(r.out) &&
        /patchable:\s+yes/.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('carplay 21.27.4 build-id: status proven, a modified build, the map URL found by structure', /^proven:\s+yes$/m.test(r.out) && /known:\s+yes, modified build/.test(r.out) &&
        /^mapurl:\s+ORIGINAL/m.test(r.out), r.out);
    [[], ['--unproven']].forEach(function (extra) {
        r = run(['apply', '--target', tgt, '--panel', 'high', '--yes'].concat(extra));
        ok('carplay 21.27.4 build-id, not its shipped file: apply ' + (extra[0] ? extra[0] + ' ' : '') + 'refuses, no write, no backup', r.code !== 0 &&
            /first edit requires the pristine build: sha256 fb5d6b179ad4f62a968697d21e84dbb80adc1ad8e618a6f02c6a9a0467aca1ac, got /.test(r.out) &&
            fs.readFileSync(tgt).equals(b2127) && !fs.existsSync(tgt + '.woc-orig'), r.out);
    });

} finally {
    cp.execFileSync('rm', ['-rf', dir]);
}

/* carplay 22.28.3's stock bytes of the map URL sites, for a revert without
 * the backup: the log string at 0x257838 and the lea rsi at 0x7b533 loading
 * "" (0x294793; file offset is the address there). */
(function () {
    var p = require('../carplay_patch.js').PROVEN['5de62156575d737391ea42254e778a97dbbb51f8'];
    var lea = Buffer.from('488d3500000000', 'hex'); lea.writeInt32LE(0x294793 - (0x7b533 + 7), 3);
    ok('carplay 22.28.3: the map URL sites\' stock bytes on its stock list', p.stock.some(function (x) {
        return x[0] === 0x257838 && x[1] === Buffer.from('Head-Unit does not support Alternate Displays feature.\0', 'latin1').toString('hex'); }) &&
        p.stock.some(function (x) { return x[0] === 0x7b533 && x[1] === lea.toString('hex'); }), json(p.stock));
})();

/* carplay 21.27.4's stock bytes agree with its probe: the stock sink in the
 * sink slot, the call to setStreamStatus (0x1800d0) at 0x1802dc, and IC_HIGH
 * 1920x900 in the pixel-size immediate. */
(function () {
    var p = require('../carplay_patch.js').PROVEN['389c1d0536f8cc9283c156410f2f2634a9f952c0'];
    var call = Buffer.alloc(5), px = Buffer.alloc(8);
    call[0] = 0xe8; call.writeInt32LE(0x1800d0 - (0x1802dc + 5), 1);
    px.writeUInt32LE(1920, 0); px.writeUInt32LE(900, 4);
    ok('carplay 21.27.4: its stock bytes as its probe reads them', !!p && json(p.stock) ===
        json([[0x29bb10, Buffer.from(locator.SINK_STOCK, 'latin1').toString('hex')], [0x1802dc, call.toString('hex')], [0x20b484, px.toString('hex')]]), json(p));
})();

/* The reference build: the documented offsets. */
var SRC = process.env.CARPLAY_BIN || '';
if (fs.existsSync(SRC)) {
    var loc = locator.locate(fs.readFileSync(SRC));
    ok('reference build: the documented sites', loc.ok && loc.variant.table === 0x2ba4d4 && loc.variant.slot === 0x2ba4f0 &&
        loc.variant.original === 0xfff7f2ec && loc.variant.high === 0xfff7f2bc && loc.variant.mid === 0xfff7f30c &&
        loc.sink.offset === 0x2a20b0 && loc.sink.len === 59 &&
        loc.selfstart.offset === 0x18176c && loc.selfstart.original === 'e8effdffff' && loc.selfstart.patched === 'e89f000000' &&
        loc.selfstart.setStreamStatus === 0x181560 && loc.selfstart.requestStream === 0x181810 && loc.selfstart.onSuggestUi === 0x1816f0 &&
        loc.res.builder === 0x20ca30 && json([loc.res.mm, loc.res.px, loc.res.safe1, loc.res.safe2]) === json([0x20ca32, 0x20ca54, 0x20ca66, 0x20ca74]), json(loc));
    ok('reference build: the documented map URL sites', loc.mapurl.ok && loc.mapurl.slot === 0x257838 && loc.mapurl.len === 55 && loc.mapurl.lea === 0x7b533 &&
        loc.mapurl.url === 0x25784f && loc.mapurl.builder === 0x2078b0 && loc.mapurl.log === 0x88aa2 && loc.mapurl.leaBytes === '488d3515c31d00' &&
        !loc.mapurl.slotPatched && !loc.mapurl.leaPatched, json(loc.mapurl));
} else {
    process.stdout.write('skip reference build: set CARPLAY_BIN to a copy of the unit\'s /usr/bin/carplay (carplay 22.28.3)\n');
}

process.stdout.write(fails ? ('\n' + fails + ' check(s) failed\n') : '\nall checks passed\n');
process.exit(fails ? 1 : 0);
