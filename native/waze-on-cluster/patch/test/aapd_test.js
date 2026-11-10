#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mlurker@gmail.com
// Copyright (C) 2026 Robert Nisipeanu
/* The Android Auto patch end to end on temporary copies of the unit's aapd
 * (AAPD_BIN, aapd 22.39.3) and aapd.conf (AAPD_CONF): the patcher's
 * refusals (coding, a site not found, a modified proven build, a build
 * outside the proven list without --unproven, an aapd.conf without what
 * the edit needs or changed by something else, a backup that is not its
 * record's, too little space), a build outside the proven list patched with
 * --unproven, its byte-exact undo without the locator, patchd's hooks and
 * socket next to carplay's (CARPLAY_BIN, optional), the head-unit restart
 * guard, and boot recovery for aapd with /, /proc, mount and systemctl
 * faked. The build outside the proven list is the reference with its GNU
 * build-id changed. Firmware is never in this repository; without AAPD_BIN
 * and AAPD_CONF the test skips.
 *
 *   AAPD_BIN=<aapd> AAPD_CONF=<aapd.conf> [CARPLAY_BIN=<carplay>] node aapd_test.js
 */
'use strict';

var headUnit = require('./head_unit_env.js');   /* a HIGH head unit for every patcher and patchd this runs */

var fs = require('fs');
var os = require('os');
var net = require('net');
var path = require('path');
var cp = require('child_process');
var crypto = require('crypto');

var PATCH = path.join(__dirname, '..');
var BIN = process.env.AAPD_BIN || '', CONF = process.env.AAPD_CONF || '', CARPLAY = process.env.CARPLAY_BIN || '';
if (!fs.existsSync(BIN) || !fs.existsSync(CONF)) { process.stdout.write('SKIP: set AAPD_BIN and AAPD_CONF to copies of the unit\'s /usr/bin/aapd and /etc/aapd.conf\n'); process.exit(0); }

var ap = require('../aapd_patch.js');
var conf = require('../aapd_conf.js');
var locator = require('../aapd_locate.js');
var SRC = fs.readFileSync(BIN), SRCCONF = fs.readFileSync(CONF);
var LOC = locator.locate(SRC);
if (!LOC.ok) { process.stdout.write('FAIL AAPD_BIN is not located: ' + LOC.reason + '\n'); process.exit(1); }
var REGIONS = locator.EDITS.map(function (e) { return [LOC[e].offset, LOC[e].len]; });
/* The reference with its GNU build-id changed: a build outside the proven
 * list whose sites the locator finds. */
var UNPROVEN = (function () {
    var b = Buffer.from(SRC), id = require('../carplay_locate.js').buildId(SRC), at = b.indexOf(Buffer.from(id, 'hex'));
    b[at] ^= 0xff;
    return b;
})();
var fails = 0, dirs = [], children = [];
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}
process.on('exit', function () {
    children.forEach(function (c) { try { c.kill('SIGKILL'); } catch (e) {} });
    dirs.forEach(function (d) { try { cp.execFileSync('rm', ['-rf', d]); } catch (e) {} });
});
function sha(x) { return crypto.createHash('sha256').update(Buffer.isBuffer(x) ? x : fs.readFileSync(x)).digest('hex'); }
function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

/* A coding image with KI_KOMBI_VARIANT v (flags 0x09: valid). */
function coding(v, flags) {
    var b = Buffer.alloc(ap.CODING.size);
    b[ap.CODING.flags] = flags === undefined ? 0x09 : flags;
    b[ap.CODING.at] = (v << ap.CODING.shift) | 0x01;
    return b;
}

/* A fresh unit: aapd, aapd.conf, the coding, a df reporting plenty. */
function unit(bin) {
    var d = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-aa-'));
    dirs.push(d);
    var t = { d: d, bin: path.join(d, 'aapd'), conf: path.join(d, 'aapd.conf'), coding: path.join(d, 'coding'), df: path.join(d, 'df') };
    fs.writeFileSync(t.bin, bin || SRC);
    fs.chmodSync(t.bin, 0o755);
    fs.writeFileSync(t.conf, SRCCONF);
    fs.chmodSync(t.conf, 0o644);
    fs.writeFileSync(t.coding, coding(7));
    setFree(t, 1 << 30);
    return t;
}
function setFree(t, bytes) {
    fs.writeFileSync(t.df, '#!/bin/sh\necho "Filesystem 1024-blocks Used Available Capacity Mounted"\necho "/dev/root 999999 1 ' + Math.floor(bytes / 1024) + ' 1% /"\n');
    fs.chmodSync(t.df, 0o755);
}
function patcher(t, args) {
    var r = cp.spawnSync(process.execPath, [path.join(PATCH, 'aapd_patch.js')].concat(args, ['--target', t.bin, '--conf', t.conf, '--coding', t.coding]),
        { encoding: 'utf8', env: Object.assign({}, process.env, { WOC_DF: t.df }) });
    return { code: r.status, out: r.stdout + r.stderr };
}
function status(t) { return patcher(t, ['status']).out; }
function untouchedAs(t, bin) { return sha(t.bin) === sha(bin) && sha(t.conf) === sha(SRCCONF) && !fs.existsSync(t.bin + '.woc-orig') && !fs.existsSync(t.conf + '.woc-orig'); }
function untouched(t) { return untouchedAs(t, SRC); }
/* An untested aapd that aapd.service does not start. */
function foreignAapd(u) {
    fs.writeFileSync(u.bin, UNPROVEN);
    fs.mkdirSync(path.join(u.d, 'units', 'aapd.service.d'));
    fs.writeFileSync(path.join(u.d, 'units', 'aapd.service.d', 'x.conf'), '[Service]\nExecStart=\nExecStart=/opt/other/aapd\n');
}
/* aapd_patch.js status slowed to 3 s in the patchd of unit u. */
function slowStatus(u) {
    var js = path.join(u.d, 'slow-status.js');
    fs.writeFileSync(js, 'if (/aapd_patch\\.js$/.test(process.argv[1] || "") && process.argv[2] === "status") { var t = Date.now() + 3000; while (Date.now() < t); }\n');
    u.env.NODE_OPTIONS = '--require=' + js;
}
function onlyEdits(a, b) {
    for (var i = 0; i < a.length; i++) {
        if (a[i] === b[i]) continue;
        if (!REGIONS.some(function (r) { return i >= r[0] && i < r[0] + r[1]; })) return false;
    }
    return a.length === b.length;
}

/* ---- modes under umask 077: aapd runs as cia_13006 ---- */
(function modeTests() {
    var mask = process.umask(0o077);
    function mode(f) { return fs.statSync(f).mode & 0o7777; }
    function modes(t) { return mode(t.bin).toString(8) + ' ' + mode(t.conf).toString(8); }
    try {
        var t = unit(), owner = fs.statSync(t.bin);
        var r = patcher(t, ['apply', '--yes']);
        ok('umask 077: apply writes aapd 0755 and aapd.conf 0644, owner kept; the records keep the originals\' modes',
            r.code === 0 && modes(t) === '755 644' && fs.statSync(t.bin).uid === owner.uid && fs.statSync(t.bin).gid === owner.gid &&
            JSON.parse(fs.readFileSync(t.bin + '.woc-orig.json', 'utf8')).mode === 0o755 && JSON.parse(fs.readFileSync(t.conf + '.woc-orig.json', 'utf8')).mode === 0o644,
            r.out + ' ' + modes(t));
        fs.chmodSync(t.bin, 0o700); fs.chmodSync(t.conf, 0o600);
        ok('umask 077: an apply with nothing to change refuses to set the modes without --yes', patcher(t, ['apply']).code === 1 && modes(t) === '700 600');
        r = patcher(t, ['apply', '--yes']);
        ok('umask 077: an apply with nothing to change sets aapd 0755 and aapd.conf 0644', r.code === 0 && modes(t) === '755 644', r.out + ' ' + modes(t));
        fs.chmodSync(t.bin, 0o700); fs.chmodSync(t.conf, 0o600);
        fs.writeFileSync(t.bin + '.woc-tmp', 'cut short', { mode: 0o600 });
        r = patcher(t, ['revert', '--yes']);
        ok('umask 077: revert over 0700/0600 and a stale temporary file writes the originals at 0755 and 0644', r.code === 0 && sha(t.bin) === sha(SRC) &&
            sha(t.conf) === sha(SRCCONF) && modes(t) === '755 644' && !fs.existsSync(t.bin + '.woc-tmp'), r.out + ' ' + modes(t));
        fs.chmodSync(t.bin, 0o700); fs.chmodSync(t.conf, 0o600);
        r = patcher(t, ['revert', '--yes']);
        ok('umask 077: revert of the originals at 0700/0600 sets 0755 and 0644, bytes untouched', r.code === 0 && sha(t.bin) === sha(SRC) && modes(t) === '755 644', r.out + ' ' + modes(t));
        fs.chmodSync(t.bin, 0o555); fs.chmodSync(t.conf, 0o444);
        r = patcher(t, ['revert']);
        ok('originals at 0555/0444, which aapd\'s user can run and read, keep their modes', r.code === 0 && modes(t) === '555 444', r.out + ' ' + modes(t));
    } finally { process.umask(mask); }
})();

/* ---- the patcher ---- */
(function patcherTests() {
    var t = unit();
    var s = status(t);
    ok('status: the pristine proven build, coding 7, nothing to restore', /known:\s+yes, pristine aapd 22\.39\.3/.test(s) && /coding:\s+KI_KOMBI_VARIANT 7/.test(s) &&
        /restorable:\s+no/.test(s) && /geometry:\s+ORIGINAL/.test(s), s);

    [[coding(9), 'KI_KOMBI_VARIANT 9 is not the proven'], [coding(2), 'KI_KOMBI_VARIANT 2 is not the proven'], [coding(7, 0x0b), 'coding not valid'],
     [Buffer.alloc(16), 'is 16 bytes']].forEach(function (c) {
        var u = unit();
        fs.writeFileSync(u.coding, c[0]);
        var r = patcher(u, ['apply', '--yes']);
        ok('coding: refused (' + c[1] + '), nothing written', r.code === 1 && r.out.indexOf(c[1]) >= 0 && untouched(u), r.out);
    });
    var nc = unit();
    fs.unlinkSync(nc.coding);
    ok('coding: an unreadable coding refuses', patcher(nc, ['apply', '--yes']).code === 1 && untouched(nc));

    /* A site the locator does not find (aapd_locate_test.js perturbs each):
     * refused with the reason, nothing written. */
    var nf = unit(), nb = Buffer.from(SRC);
    nb[LOC.gate.offset + 6] = 3;
    fs.writeFileSync(nf.bin, nb);
    var r = patcher(nf, ['apply', '--unproven', '--yes']);
    ok('a site not found: refused with the reason, nothing written', r.code === 1 && /refusing: gate: variant check not found/.test(r.out) &&
        sha(nf.bin) === sha(nb) && !fs.existsSync(nf.bin + '.woc-orig'), r.out);
    ok('a site not found: status says which', /known:\s+NO — gate: variant check not found/.test(status(nf)) && /gate:\s+UNKNOWN/.test(status(nf)), status(nf));
    [['bytes appended', Buffer.concat([SRC, Buffer.alloc(16)])], ['a byte changed outside the sites', (function () { var b = Buffer.from(SRC); b[0x1000] ^= 1; return b; })()]].forEach(function (m) {
        var u = unit(m[1]), x = patcher(u, ['apply', '--unproven', '--yes']);
        ok('the proven build with ' + m[0] + ': refused (first edit needs the pristine build), also with --unproven', x.code === 1 && /pristine build: sha256/.test(x.out) &&
            !fs.existsSync(u.bin + '.woc-orig'), x.out);
        ok('the proven build with ' + m[0] + ': status reads modified, nothing restorable', /known:\s+yes, modified build/.test(status(u)) && /restorable:\s+no/.test(status(u)));
    });
    var text = SRCCONF.toString('utf8');
    [['not JSON', text.slice(0, -10), /not JSON/],
     ['a number the edit sets missing', text.replace(/"SA_WIDTHPIXELS": 612/, '"SA_WIDTHPIXELS": "612"'), /no number SCREENS\/KOMBI_DISPLAY\/DIMENSIONS\/12\.3\/SA_WIDTHPIXELS/],
     ['two codecs for the 12.3" screen', text.replace(/"HIGH_1080P"\s*\]/, '"HIGH_1080P", "MID_1080P"]'), /no single codec for the 12\.3 screen/],
     ['a key twice', text.replace(/"FRAME_RATE"/, '"CODEC_WIDTH"'), /twice/],
     ['our geometry without our backup', conf.transform(text, 200, 'shown'), /holds our geometry without our backup/]].forEach(function (c) {
        var u = unit();
        fs.writeFileSync(u.conf, c[1]);
        var x = patcher(u, ['apply', '--yes']);
        ok('conf with ' + c[0] + ': refused before anything is written', x.code === 1 && c[2].test(x.out) && sha(u.bin) === sha(SRC) && !fs.existsSync(u.bin + '.woc-orig'), x.out);
        ok('conf with ' + c[0] + ': status says why', new RegExp('geometry:\\s+UNKNOWN \\(').test(status(u)) && c[2].test(status(u)), status(u));
    });
    var cc = unit();
    fs.appendFileSync(cc.conf, '\n');
    r = patcher(cc, ['apply', '--yes']);
    ok('conf: an aapd.conf with what the edit needs is edited, whatever else it holds', r.code === 0 &&
        conf.edited(SRCCONF.toString('utf8') + '\n', fs.readFileSync(cc.conf, 'utf8')) !== null, r.out);
    var lo = unit();
    setFree(lo, 10 << 20);
    r = patcher(lo, ['apply', '--yes']);
    ok('disk: too little free space refuses, nothing written', r.code === 1 && /bytes free/.test(r.out) && untouched(lo), r.out);
    ok('dpi: a density off the steps is refused', patcher(unit(), ['apply', '--dpi', '155', '--yes']).code === 1);

    /* Apply: backups first, each with its record, then the four edits only. */
    r = patcher(t, ['apply', '--yes']);
    var meta = JSON.parse(fs.readFileSync(t.bin + '.woc-orig.json', 'utf8')), cmeta = JSON.parse(fs.readFileSync(t.conf + '.woc-orig.json', 'utf8'));
    ok('apply: both backups are the originals, each with its recorded SHA-256', r.code === 0 && sha(t.bin + '.woc-orig') === sha(SRC) && meta.sha256_original === sha(SRC) &&
        sha(t.conf + '.woc-orig') === sha(SRCCONF) && cmeta.sha256_original === sha(SRCCONF) && !fs.existsSync(t.bin + '.woc-orig.tmp'), r.out);
    ok('apply: aapd differs from the original only in the four edits', onlyEdits(SRC, fs.readFileSync(t.bin)) && !fs.readFileSync(t.bin).equals(SRC));
    var k = JSON.parse(fs.readFileSync(t.conf, 'utf8')).SCREENS.KOMBI_DISPLAY, c = k.CODEC_DEFINITION.HIGH_1080P, sa = k.DIMENSIONS['12.3'];
    var g = conf.GEOMETRY.shown;
    ok('apply: aapd.conf asks for the geometry at 180 dpi; 10.25" and MID left stock', c.CODEC_WIDTH === g.codec.w && c.CODEC_HEIGHT === g.codec.h &&
        c.REPORTED_SCREEN_DENSITY === 180 && c.WIDTH_TARGET_VIEWPORT === g.ui.w && c.HEIGHT_TARGET_VIEWPORT === g.ui.h && sa.SA_WIDTHPIXELS === g.safe.w &&
        sa.SA_OFFSET_TOPPIXELS === g.safe.y && JSON.stringify(k.CODEC_DEFINITION.MID_1080P) === JSON.stringify(JSON.parse(SRCCONF).SCREENS.KOMBI_DISPLAY.CODEC_DEFINITION.MID_1080P));
    s = status(t);
    ok('status: patched, every edit, 180 dpi, card shown, restorable', /restorable:\s+yes/.test(s) && (s.match(/PATCHED/g) || []).length === 5 &&
        /PATCHED \(180 dpi, card shown, crop 1280x720:0,0,1280,564\)/.test(s), s);
    ok('apply again: nothing to do', /nothing to do/.test(patcher(t, ['apply', '--yes']).out));
    var binAt = sha(t.bin);
    conf.DENSITIES.forEach(function (d) {
        var x = patcher(t, ['apply', '--dpi', String(d), '--yes']);
        if (x.code || JSON.parse(fs.readFileSync(t.conf, 'utf8')).SCREENS.KOMBI_DISPLAY.CODEC_DEFINITION.HIGH_1080P.REPORTED_SCREEN_DENSITY !== d) ok('density ' + d, false, x.out);
    });
    ok('density: every step from 150 to 400 rewrites only aapd.conf', sha(t.bin) === binAt && /PATCHED \(400 dpi, card shown/.test(status(t)));

    /* The turn card: hidden is the whole picture as UI with the safe area
     * from the top, and the crop under the card's rows, which grow with the
     * density (the car's values). */
    r = patcher(t, ['apply', '--dpi', '240', '--card', 'hidden', '--yes']);
    k = JSON.parse(fs.readFileSync(t.conf, 'utf8')).SCREENS.KOMBI_DISPLAY; c = k.CODEC_DEFINITION.HIGH_1080P; sa = k.DIMENSIONS['12.3'];
    ok('card hidden: 1280x720 drawn whole, safe area 592x672 at 336,0, 240 dpi', r.code === 0 && c.VERTICAL_MARGIN_SIZE === 0 && c.HEIGHT_TARGET_VIEWPORT === 720 &&
        [sa.SA_OFFSET_LEFTPIXELS, sa.SA_OFFSET_TOPPIXELS, sa.SA_WIDTHPIXELS, sa.SA_HEIGHTPIXELS].join() === '336,0,592,672' && c.REPORTED_SCREEN_DENSITY === 240, r.out);
    ok('card hidden: status names the crop', /PATCHED \(240 dpi, card hidden, crop 1280x720:4,160,1270,560\)/.test(status(t)));
    [[180, '1280x720:0,156,1280,564'], [200, '1280x720:0,156,1280,564'], [240, '1280x720:4,160,1270,560'], [300, '1280x720:46,196,1188,524'],
     [400, '1280x720:116,258,1048,462']].forEach(function (x) {
        var n = x[1].split(':')[1].split(',').map(Number);
        ok('card hidden at ' + x[0] + ' dpi: crop ' + x[1] + ', even, inside, the slot\'s shape', conf.srcCrop('hidden', x[0]) === x[1] &&
            n.every(function (v) { return v % 2 === 0; }) && n[0] + n[2] <= 1280 && n[1] + n[3] <= 720 && Math.abs(n[2] / n[3] / (1088 / 480) - 1) < 0.01);
    });
    var shipped = /^src_crop = (.*)$/m.exec(fs.readFileSync(path.join(PATCH, '..', 'waze-on-cluster.conf'), 'utf8'));
    ok('the shipped conf crops the card-shown geometry', shipped && shipped[1] === conf.srcCrop('shown', 180));
    var origText = SRCCONF.toString('utf8'), every = true;
    conf.DENSITIES.forEach(function (d) {
        conf.CARDS.forEach(function (cd) {
            var e = conf.edited(origText, conf.transform(origText, d, cd));
            if (!e || e.dpi !== d || e.card !== cd || conf.otherFile(conf.transform(origText, d, cd), origText)) every = false;
        });
    });
    ok('every density and card is recognised and is the original\'s file', every);

    /* Undo needs no anchors: the patched aapd with an anchor changed (an
     * older release's edit there), and aapd.conf with KOMBI_DISPLAY numbers
     * no geometry of ours writes. */
    var pb = fs.readFileSync(t.bin);
    pb[LOC.gate.offset - 1] ^= 0x5a;
    fs.writeFileSync(t.bin, pb);
    var pc = JSON.parse(fs.readFileSync(t.conf, 'utf8'));
    fs.writeFileSync(t.conf, fs.readFileSync(t.conf, 'utf8').replace('"SA_OFFSET_TOPPIXELS": ' + pc.SCREENS.KOMBI_DISPLAY.DIMENSIONS['12.3'].SA_OFFSET_TOPPIXELS, '"SA_OFFSET_TOPPIXELS": 7'));
    s = status(t);
    ok('undo: anchors and geometry not ours, still restorable', /restorable:\s+yes/.test(s) && /UNKNOWN/.test(s), s);
    r = patcher(t, ['revert', '--yes']);
    ok('undo: revert writes both originals byte-exact', r.code === 0 && sha(t.bin) === sha(SRC) && sha(t.conf) === sha(SRCCONF), r.out);

    /* A backup that is not its record's original is never trusted: on a
     * build outside the proven list aapd is then not undone; the proven
     * build is rebuilt from its own bytes. */
    var ub = unit(UNPROVEN);
    patcher(ub, ['apply', '--unproven', '--yes']);
    var bad = fs.readFileSync(ub.bin + '.woc-orig');
    bad[5] ^= 1;
    fs.writeFileSync(ub.bin + '.woc-orig', bad);
    var patched = sha(ub.bin);
    ok('backup: one that differs from its record refuses apply', patcher(ub, ['apply', '--dpi', '200', '--yes']).code === 1);
    r = patcher(ub, ['revert', '--yes']);
    ok('backup: revert never writes it, restores aapd.conf on its own and fails: aapd is not undone', r.code === 1 && /recorded/.test(r.out) && /not undone: aapd untrusted, aapd\.conf original/.test(r.out) && sha(ub.bin) === patched && sha(ub.conf) === sha(SRCCONF), r.out);
    r = patcher(ub, ['revert', '--yes']);
    ok('backup: with nothing else to restore, revert fails and says why', r.code === 1 && /recorded/.test(r.out) && /restorable:\s+no \(backup sha256/.test(status(ub)), r.out);
    patcher(t, ['apply', '--yes']);
    bad = fs.readFileSync(t.bin + '.woc-orig');
    bad[5] ^= 1;
    fs.writeFileSync(t.bin + '.woc-orig', bad);
    s = status(t);
    ok('backup not its record\'s, on the proven build: restorable from its shipped bytes', /^restorable:\s+yes$/m.test(s) && /^undo:\s+aapd restorable, aapd\.conf restorable$/m.test(s), s);
    r = patcher(t, ['revert', '--yes']);
    ok('backup not its record\'s, on the proven build: revert writes the shipped bytes, never the backup; both files byte-exact', r.code === 0 &&
        /restored .*aapd from the proven file's shipped content/.test(r.out) && sha(t.bin) === sha(SRC) && sha(t.conf) === sha(SRCCONF), r.out);
    fs.writeFileSync(t.bin + '.woc-orig', SRC);
    patcher(t, ['revert', '--yes']);
    patcher(t, ['apply', '--yes']);

    /* A firmware update: another aapd over the backup is left, aapd.conf
     * still restored; --force writes the backup over it. */
    var other = Buffer.concat([SRC, Buffer.from('new firmware')]);
    fs.writeFileSync(t.bin, other);
    r = patcher(t, ['revert', '--yes']);
    ok('another build: aapd left as it is, aapd.conf restored on its own', r.code === 0 && sha(t.bin) === sha(other) && sha(t.conf) === sha(SRCCONF) && /left as it is/.test(r.out), r.out);
    r = patcher(t, ['revert', '--yes']);
    ok('another build: nothing else to restore is an error, nothing written', r.code === 1 && sha(t.bin) === sha(other), r.out);
    r = patcher(t, ['revert', '--yes', '--force', '--drop-backup']);
    ok('another build: --force writes the backup, --drop-backup drops both', r.code === 0 && sha(t.bin) === sha(SRC) && !fs.existsSync(t.bin + '.woc-orig') &&
        !fs.existsSync(t.conf + '.woc-orig') && !fs.existsSync(t.bin + '.woc-orig.json'), r.out);

    /* A build outside the proven list: patched only with --unproven, then
     * like a proven one; undone byte-exact. */
    var u = unit(UNPROVEN);
    s = status(u);
    ok('unproven build: pristine by its sites, not proven, its build-id and version', /known:\s+yes, pristine build [0-9a-f]{40}/.test(s) && /proven:\s+no/.test(s) &&
        /version:\s+22\.39\.3/.test(s) && /geometry:\s+ORIGINAL/.test(s), s);
    r = patcher(u, ['apply', '--yes']);
    ok('unproven build: apply without --unproven refuses, nothing written', r.code === 1 && /is not a proven build; pass --unproven/.test(r.out) && untouchedAs(u, UNPROVEN), r.out);
    r = patcher(u, ['apply', '--unproven', '--yes']);
    var ub = fs.readFileSync(u.bin);
    ok('unproven build: apply --unproven writes the four edits only, with the backup', r.code === 0 && onlyEdits(UNPROVEN, ub) && !ub.equals(UNPROVEN) &&
        sha(u.bin + '.woc-orig') === sha(UNPROVEN) && JSON.parse(fs.readFileSync(u.bin + '.woc-orig.json', 'utf8')).buildId === locator.locate(UNPROVEN).buildId, r.out);
    ok('unproven build: the edits are the proven build\'s', locator.EDITS.every(function (e) { return ub.toString('hex', LOC[e].offset, LOC[e].offset + LOC[e].len) === LOC[e].patched; }));
    r = patcher(u, ['apply', '--dpi', '200', '--yes']);
    ok('unproven build: a later apply (our backup present) needs no --unproven', r.code === 0 && /PATCHED \(200 dpi/.test(status(u)), r.out);
    s = status(u);
    ok('unproven build: status patched, restorable', (s.match(/PATCHED/g) || []).length === 5 && /restorable:\s+yes/.test(s) && /known:\s+yes, modified build/.test(s), s);
    r = patcher(u, ['revert', '--yes', '--drop-backup']);
    ok('unproven build: revert restores both byte-exact and drops the backups', r.code === 0 && untouchedAs(u, UNPROVEN), r.out);

    /* A locator that throws: status still prints every line, and revert
     * restores both files without it. */
    var th = unit();
    patcher(th, ['apply', '--yes']);
    var pre = path.join(th.d, 'throwing-locator.js');
    fs.writeFileSync(pre, 'var p = require.resolve(' + JSON.stringify(path.join(PATCH, 'aapd_locate.js')) + ');\n' +
        'require.cache[p] = { id: p, filename: p, loaded: true, exports: { locate: function () { throw new Error("boom"); } } };\n');
    function throwing(args) {
        var x = cp.spawnSync(process.execPath, ['-r', pre, path.join(PATCH, 'aapd_patch.js')].concat(args, ['--target', th.bin, '--conf', th.conf, '--coding', th.coding]),
            { encoding: 'utf8', env: Object.assign({}, process.env, { WOC_DF: th.df }) });
        return { code: x.status, out: x.stdout + x.stderr };
    }
    r = throwing(['status']);
    ok('a throwing locator: status prints every line, restorable, the backup named', r.code === 0 &&
        ['target', 'size', 'sha256', 'build-id', 'version', 'conf', 'coding', 'restorable', 'undo', 'known', 'proven', 'gate', 'selfstart-call', 'selfstart-guard', 'screen', 'geometry', 'backup']
            .every(function (k) { return new RegExp('^' + k + ':', 'm').test(r.out); }) &&
        /^restorable:\s+yes$/m.test(r.out) && /^known:\s+NO — locator failed: boom$/m.test(r.out) && /^gate:\s+UNKNOWN \(locator failed: boom\)$/m.test(r.out) &&
        r.out.split('\n').some(function (l) { return /^backup:/.test(l) && l.indexOf(th.bin + '.woc-orig') > 0; }), r.out);
    r = throwing(['revert', '--yes']);
    ok('a throwing locator: revert restores both files byte-exact', r.code === 0 && sha(th.bin) === sha(SRC) && sha(th.conf) === sha(SRCCONF), r.out);

    /* An apply cut short: a temporary backup without its record. */
    var cut = unit();
    fs.writeFileSync(cut.bin + '.woc-orig.tmp', 'partial');
    r = patcher(cut, ['apply', '--yes']);
    ok('cut short: the next apply replaces the temporary files', r.code === 0 && !fs.existsSync(cut.bin + '.woc-orig.tmp') && sha(cut.bin + '.woc-orig') === sha(SRC), r.out);
    var nr = unit(UNPROVEN);
    patcher(nr, ['apply', '--unproven', '--yes']);
    fs.unlinkSync(nr.bin + '.woc-orig.json');
    var nrBin = sha(nr.bin);
    r = patcher(nr, ['revert', '--yes']);
    ok('record: a backup without its record is not trusted', /has no record/.test(r.out) && sha(nr.bin) === nrBin && sha(nr.conf) === sha(SRCCONF), r.out);

    /* No backup at all: the proven aapd and aapd.conf are rebuilt from
     * their own content and the shipped mode, also under umask 077. */
    var nb = unit();
    patcher(nb, ['apply', '--dpi', '240', '--card', 'hidden', '--yes']);
    [nb.bin, nb.conf].forEach(function (f) { fs.unlinkSync(f + '.woc-orig'); fs.unlinkSync(f + '.woc-orig.json'); });
    fs.chmodSync(nb.bin, 0o700);
    s = status(nb);
    ok('no backup, the proven files patched: restorable', /^restorable:\s+yes$/m.test(s) && /^undo:\s+aapd restorable, aapd\.conf restorable$/m.test(s), s);
    var um = cp.spawnSync('sh', ['-c', 'umask 077; exec "$0" "$@"', process.execPath, path.join(PATCH, 'aapd_patch.js'), 'revert', '--yes',
        '--target', nb.bin, '--conf', nb.conf, '--coding', nb.coding], { encoding: 'utf8' });
    ok('no backup: revert restores both byte-exact with the shipped modes under umask 077', um.status === 0 && sha(nb.bin) === sha(SRC) && sha(nb.conf) === sha(SRCCONF) &&
        (fs.statSync(nb.bin).mode & 0o7777) === 0o755 && (fs.statSync(nb.conf).mode & 0o7777) === 0o644, um.stdout + um.stderr);
    ok('no backup, the originals: nothing left to undo', /^undo:\s+aapd original, aapd\.conf original$/m.test(status(nb)) && /^restorable:\s+no/m.test(status(nb)), status(nb));
    fs.chmodSync(nb.bin, 0o700);
    r = patcher(nb, ['revert', '--yes']);
    ok('the original aapd with another mode and no backup of ours: not restorable, revert leaves the mode', /^restorable:\s+no/m.test(status(nb)) &&
        r.code === 0 && /nothing to do/.test(r.out) && (fs.statSync(nb.bin).mode & 0o7777) === 0o700 && sha(nb.bin) === sha(SRC), r.out);
    /* Beside a backup of ours (Remove patch keeps it), the mode is ours to
     * repair, and the repair is nothing for patchd to restart aapd for. */
    var mb = unit();
    patcher(mb, ['apply', '--yes']);
    patcher(mb, ['revert', '--yes']);
    fs.chmodSync(mb.bin, 0o700);
    s = status(mb);
    r = patcher(mb, ['revert', '--yes']);
    ok('the original aapd with another mode beside our backup: restorable; revert sets 0755 and reports nothing to do', /^restorable:\s+yes$/m.test(s) &&
        r.code === 0 && /^set \S+'s mode and owner \(it had mode 0700, not 0755\)$/m.test(r.out) && /nothing to do/.test(r.out) && !/^restored /m.test(r.out) &&
        (fs.statSync(mb.bin).mode & 0o7777) === 0o755 && sha(mb.bin) === sha(SRC), s + r.out);
    /* woc-patch.sh, the by-hand path: aa-status, and aa-revert with / read-write
     * only around the write, on the proven files without backups. */
    var wp = unit();
    patcher(wp, ['apply', '--yes']);
    [wp.bin, wp.conf].forEach(function (f) { fs.unlinkSync(f + '.woc-orig'); fs.unlinkSync(f + '.woc-orig.json'); });
    fs.mkdirSync(path.join(wp.d, 'bin'));
    fs.writeFileSync(path.join(wp.d, 'bin', 'mount'), '#!/bin/sh\necho "$*" >> "' + path.join(wp.d, 'mounts') + '"\n', { mode: 0o755 });
    var wenv = Object.assign({}, process.env, { PATH: path.join(wp.d, 'bin') + path.delimiter + process.env.PATH, WOC_AAPD: wp.bin, WOC_AAPD_CONF: wp.conf, WOC_CODING: wp.coding });
    var ws = cp.spawnSync('sh', [path.join(PATCH, 'woc-patch.sh'), 'aa-status'], { env: wenv, encoding: 'utf8' });
    ok('woc-patch.sh aa-status: the status, restorable without backups', ws.status === 0 && /^restorable:\s+yes$/m.test(ws.stdout) && /^gate:\s+PATCHED$/m.test(ws.stdout), ws.stdout + ws.stderr);
    ws = cp.spawnSync('sh', [path.join(PATCH, 'woc-patch.sh'), 'aa-revert'], { env: wenv, encoding: 'utf8' });
    ok('woc-patch.sh aa-revert: both files byte-exact, / read-write then read-only', ws.status === 0 && sha(wp.bin) === sha(SRC) && sha(wp.conf) === sha(SRCCONF) &&
        fs.readFileSync(path.join(wp.d, 'mounts'), 'utf8') === '-o remount,rw /\n-o remount,ro /\n', ws.stdout + ws.stderr);

    /* No backup of ours: aapd.conf is rebuilt only from a geometry this tool
     * writes. One another tool changed is left as it is, with nothing to
     * undo. */
    var fc = unit();
    var foreign = SRCCONF.toString('utf8').replace('"SA_PHYSICALDENSITY": 208', '"SA_PHYSICALDENSITY": 211');
    ok('foreign aapd.conf fixture differs in one KOMBI_DISPLAY number', foreign !== SRCCONF.toString('utf8'));
    fs.writeFileSync(fc.conf, foreign);
    s = status(fc);
    ok('aapd.conf changed by another tool, no backup: geometry original, nothing to undo, not restorable', /^geometry:\s+ORIGINAL$/m.test(s) &&
        /^undo:\s+aapd original, aapd\.conf none$/m.test(s) && /^restorable:\s+no/m.test(s), s);
    r = patcher(fc, ['revert', '--yes']);
    ok('aapd.conf changed by another tool, no backup: revert leaves it byte for byte', fs.readFileSync(fc.conf, 'utf8') === foreign && sha(fc.bin) === sha(SRC), r.out);
    var gc = unit();
    patcher(gc, ['apply', '--dpi', '260', '--card', 'hidden', '--yes']);
    [gc.bin, gc.conf].forEach(function (f) { fs.unlinkSync(f + '.woc-orig'); fs.unlinkSync(f + '.woc-orig.json'); });
    fs.writeFileSync(gc.bin, SRC);
    s = status(gc);
    ok('aapd.conf at a geometry of ours, no backup, aapd stock: aapd.conf restorable', /^undo:\s+aapd original, aapd\.conf restorable$/m.test(s) &&
        /^restorable:\s+yes$/m.test(s), s);
    r = patcher(gc, ['revert', '--yes']);
    ok('aapd.conf at a geometry of ours, no backup: revert rebuilds the shipped file', r.code === 0 && sha(gc.conf) === sha(SRCCONF) && sha(gc.bin) === sha(SRC), r.out);
    var fb = unit();
    var foreignBin = Buffer.from(SRC);
    Buffer.from('83bb1c02000003', 'hex').copy(foreignBin, 0x998bd);
    fs.writeFileSync(fb.bin, foreignBin);
    s = status(fb);
    ok('aapd with a site changed by another tool, no backup: nothing to undo, not restorable', /^undo:\s+aapd none, aapd\.conf original$/m.test(s) &&
        /^restorable:\s+no/m.test(s), s);
    r = patcher(fb, ['revert', '--yes']);
    ok('aapd with a site changed by another tool, no backup: revert leaves it byte for byte', fs.readFileSync(fb.bin).equals(foreignBin), r.out);

    var nu = unit(UNPROVEN);
    patcher(nu, ['apply', '--unproven', '--yes']);
    fs.unlinkSync(nu.bin + '.woc-orig'); fs.unlinkSync(nu.bin + '.woc-orig.json');
    var nuBin = sha(nu.bin);
    r = patcher(nu, ['revert', '--yes']);
    ok('no backup, a build outside the proven list: aapd left as it is, aapd.conf restored', sha(nu.bin) === nuBin && sha(nu.conf) === sha(SRCCONF) &&
        /^undo:\s+aapd none, aapd\.conf original$/m.test(status(nu)), r.out + status(nu));
})();

/* ---- patchd: hooks and socket, next to carplay ---- */
var HAVE_CP = fs.existsSync(CARPLAY);
function patchdUnit(opts) {
    var t = unit();
    var d = t.d;
    ['shm', 'var', 'proc', 'units', 'bin'].forEach(function (x) { fs.mkdirSync(path.join(d, x)); });
    t.carplay = path.join(d, 'carplay');
    fs.writeFileSync(t.carplay, HAVE_CP ? fs.readFileSync(CARPLAY) : 'not a carplay build\n', { mode: 0o755 });
    t.calls = path.join(d, 'calls');
    fs.writeFileSync(t.calls, '');
    t.mounts = path.join(d, 'mounts');
    fs.writeFileSync(t.mounts, '/dev/root / ext4 ro 0 0\n');
    var mount = path.join(d, 'bin', 'mount');
    /* remount,ro fails while a process runs a file no name links any more,
     * as the kernel refuses it. */
    fs.writeFileSync(mount, '#!' + process.execPath + '\n' + [
        "var fs = require('fs'), path = require('path'), a = process.argv.slice(2).join(' ');",
        "var P = " + JSON.stringify(path.join(d, 'proc')) + ", M = " + JSON.stringify(t.mounts) + ";",
        "if (/rw/.test(a)) fs.writeFileSync(M, '/dev/root / ext4 rw 0 0\\n');",
        "else {",
        "    var open = fs.readdirSync(P).some(function (p) { try { return fs.statSync(path.join(P, p, 'exe')).nlink <= 1; } catch (e) { return false; } });",
        "    if (open) { process.stderr.write('mount: / is busy\\n'); process.exit(32); }",
        "    fs.writeFileSync(M, '/dev/root / ext4 ro 0 0\\n');",
        "}",
    ].join('\n') + '\n');
    fs.chmodSync(mount, 0o755);
    /* systemctl: show answers from unit.json, start makes the unit active,
     * every call recorded. */
    t.unitState = path.join(d, 'unit.json');
    fs.writeFileSync(t.unitState, JSON.stringify({ pid: 0 }));
    fs.writeFileSync(path.join(d, 'bin', 'systemctl'), '#!' + process.execPath + '\n' + [
        "var fs = require('fs'), a = process.argv.slice(2), u = JSON.parse(fs.readFileSync(" + JSON.stringify(t.unitState) + ", 'utf8'));",
        "fs.appendFileSync(" + JSON.stringify(t.calls) + ", 'systemctl ' + a.join(' ') + '\\n');",
        "if (a[0] === 'show') process.stdout.write('MainPID=' + u.pid + '\\nInvocationID=run-' + u.pid + '\\nExecMainStartTimestampMonotonic=1\\nActiveState=active\\nDropInPaths=\\n');",
        "else if (a[0] === 'is-active') process.stdout.write('active\\n');",
    ].join('\n') + '\n');
    fs.chmodSync(path.join(d, 'bin', 'systemctl'), 0o755);
    t.reboot = path.join(d, 'bin', 'reboot');
    fs.writeFileSync(t.reboot, '#!/bin/sh\necho reboot >> "' + t.calls + '"\n' + (opts && opts.rebootFails ? 'exit 1\n' : ''));
    fs.chmodSync(t.reboot, 0o755);
    t.recmgr = path.join(d, 'recmgr.conf');
    var names = (opts && opts.recmgr) || [];
    fs.writeFileSync(t.recmgr, '[Whitelist]\n' + names.join('\n') + '\n[WhitelistExitSuccess]\n' + names.join('\n') + '\n');
    var boot = Math.floor(Date.now() / 1000) - 60;
    fs.utimesSync(t.recmgr, boot - 3600, boot - 3600);
    fs.writeFileSync(path.join(d, 'stat'), 'btime ' + boot + '\n');
    t.shm = path.join(d, 'shm');
    t.env = Object.assign({}, process.env, {
        PATH: path.join(d, 'bin') + path.delimiter + process.env.PATH,
        WOC_PATCH_SOCK: path.join(t.shm, 'patch.sock'), WOC_CARPLAY: t.carplay, WOC_AAPD: t.bin, WOC_AAPD_CONF: t.conf, WOC_CODING: t.coding,
        WOC_DF: t.df, WOC_MOUNT: mount, WOC_PROC_MOUNTS: t.mounts, WOC_PROC: path.join(d, 'proc'), WOC_RECMGR_CONF: t.recmgr,
        WOC_RECMGR_AT_BOOT: path.join(d, 'none.json'), WOC_PROC_STAT: path.join(d, 'stat'), WOC_BOOT_ID: path.join(d, 'none'),
        WOC_UNIT_DIRS: path.join(d, 'units'), WOC_CAPTURE_SOCK: path.join(d, 'none.sock'), WOC_DLT_RECEIVE: path.join(d, 'none'),
        WOC_DLT_RETRY_MS: '600000', WOC_REBOOT_CMD: t.reboot, WOC_REMOUNT_LOCK: path.join(t.shm, 'remount.lock'),
        WOC_STATE_DIR: path.join(d, 'var'), WOC_RESTART_POLL_MS: '20', WOC_CARPLAY_DROPIN: path.join(d, 'dropin.conf'),
    });
    return t;
}
function hook(t, which) {
    var r = cp.spawnSync(process.execPath, [path.join(PATCH, 'patchd.js'), which], { env: t.env, encoding: 'utf8', timeout: 90000 });
    var lines = (r.stdout || '').trim().split('\n'), last = {};
    try { last = JSON.parse(lines[lines.length - 1]); } catch (e) {}
    return { code: r.status, result: last, out: r.stdout + r.stderr };
}
function calls(t) { return fs.readFileSync(t.calls, 'utf8').split('\n').filter(Boolean); }
function marker(t, n) { try { return JSON.parse(fs.readFileSync(path.join(t.shm, n), 'utf8')); } catch (e) { return null; } }
function ro(t) { return /\bro\b/.test(fs.readFileSync(t.mounts, 'utf8')); }

(function hooks() {
    var t = patchdUnit({ recmgr: ['carplay', 'aapd'] });
    var r = hook(t, 'activate');
    ok('activate: Android Auto written, pending its own marker, / read-only', r.code === 0 && /Android Auto patch written/.test(r.result.aa && r.result.aa.status) &&
        marker(t, 'aa-pending').action === 'apply' && /next head-unit start/.test(r.result.reboot || '') && ro(t), r.out);
    ok('activate: CarPlay ' + (HAVE_CP ? 'written alongside' : 'refused on its own (not a carplay build)'),
        HAVE_CP ? /map patch written/.test(r.result.status) && marker(t, 'activation-pending').action === 'apply' : /not patchable/.test(r.result.conflict || ''), JSON.stringify(r.result));
    ok('activate: no systemctl, kill or reboot even with recmgr\'s entries loaded', calls(t).length === 0, calls(t).join('; '));
    r = hook(t, 'activate');
    ok('activate again: Android Auto left pending, nothing rewritten', r.code === 0 && /already waits/.test(r.result.aa.status), JSON.stringify(r.result));
    r = hook(t, 'deactivate');
    ok('deactivate: aapd and aapd.conf restored byte-exact, backups dropped', r.code === 0 && sha(t.bin) === sha(SRC) && sha(t.conf) === sha(SRCCONF) &&
        !fs.existsSync(t.bin + '.woc-orig') && !fs.existsSync(t.conf + '.woc-orig') && ro(t), r.out);
    if (HAVE_CP) ok('deactivate: carplay restored byte-exact too', sha(t.carplay) === sha(CARPLAY));

    /* Not supported, untested, absent: CarPlay is patched as ever, except
     * on a car not coded 7, where CarPlay's patch is left too, with the
     * same conflict. */
    [['coding 9', function (u) { fs.writeFileSync(u.coding, coding(9)); }, /not supported on this car \(KI_KOMBI_VARIANT 9\)/],
     ['an untested aapd', function (u) { fs.writeFileSync(u.bin, UNPROVEN); }, /"conflict":"untested aapd build"/],
     ['an aapd the locator refuses', function (u) { var b = Buffer.from(SRC); b[LOC.gate.offset + 6] = 3; fs.writeFileSync(u.bin, b); }, /not patchable: gate: variant check not found/],
     ['the proven aapd modified', function (u) { fs.writeFileSync(u.bin, Buffer.concat([SRC, Buffer.alloc(8)])); }, /aapd was modified by something else/],
     ['an aapd.conf without the 12.3" codec', function (u) { fs.writeFileSync(u.conf, SRCCONF.toString('utf8').replace(/"HIGH_1080P"\s*\]/, '"NONE"]')); }, /aapd\.conf not usable: no number/],
     /* an untested build is the conflict only when nothing else stands */
     ['an untested aapd and an aapd.conf without the 12.3" codec', function (u) {
         fs.writeFileSync(u.bin, UNPROVEN);
         fs.writeFileSync(u.conf, SRCCONF.toString('utf8').replace(/"HIGH_1080P"\s*\]/, '"NONE"]'));
     }, /"conflict":"aapd\.conf not usable: no number/],
     ['an untested aapd that aapd.service does not start', foreignAapd, /"conflict":"aapd\.service starts \/opt\/other\/aapd/],
     ['no aapd', function (u) { fs.unlinkSync(u.bin); }, /no Android Auto/]].forEach(function (c) {
        var u = patchdUnit();
        c[1](u);
        var before = fs.existsSync(u.bin) ? sha(u.bin) : null, confBefore = sha(u.conf);
        var x = hook(u, 'activate');
        ok('activate with ' + c[0] + ': Android Auto left as it is, exit 0', x.code === 0 && c[2].test(JSON.stringify(x.result.aa)) &&
            (before === null || sha(u.bin) === before) && sha(u.conf) === confBefore && !marker(u, 'aa-pending'), x.out);
        if (HAVE_CP && c[0] === 'coding 9') ok('activate with ' + c[0] + ': CarPlay left as it is, the same conflict', x.result.conflict === 'not supported on this car (KI_KOMBI_VARIANT 9)' &&
            sha(u.carplay) === sha(CARPLAY) && !marker(u, 'activation-pending'), JSON.stringify(x.result));
        else if (HAVE_CP) ok('activate with ' + c[0] + ': CarPlay patched as ever', /map patch written/.test(x.result.status) && sha(u.carplay) !== sha(CARPLAY), JSON.stringify(x.result));
    });

    /* A running aapd keeps its file linked and is handed over at removal. */
    var w = patchdUnit();
    fs.mkdirSync(path.join(w.d, 'proc', '4242'));
    fs.linkSync(w.bin, path.join(w.d, 'proc', '4242', 'exe'));
    hook(w, 'activate');
    ok('activate: the running aapd stays linked as aapd.woc-running', fs.existsSync(w.bin + '.woc-running') && sha(w.bin + '.woc-running') === sha(SRC));
    /* The next start runs the patched aapd. */
    fs.unlinkSync(path.join(w.d, 'proc', '4242', 'exe'));
    fs.linkSync(w.bin, path.join(w.d, 'proc', '4242', 'exe'));
    fs.unlinkSync(path.join(w.shm, 'aa-pending'));
    r = hook(w, 'deactivate');
    ok('deactivate: the link aapd still runs goes to the deploy tool', (r.result.removeWhenUnused || []).indexOf(w.bin + '.woc-running') >= 0, JSON.stringify(r.result));

    /* A removal that cannot undo Android Auto is refused. */
    var f = patchdUnit();
    hook(f, 'activate');
    fs.writeFileSync(path.join(f.d, 'rw-fails'), '');
    fs.writeFileSync(f.env.WOC_MOUNT, '#!/bin/sh\ncase "$*" in *rw*) exit 32;; esac\n');
    r = hook(f, 'deactivate');
    ok('deactivate: a remount failure that leaves Android Auto patched refuses the removal', r.code === 1 && sha(f.bin) !== sha(SRC) &&
        /patch failed/.test(JSON.stringify(r.result)), r.out);

    /* A removal never leaves aapd patched behind: out of the hook's time
     * for Android Auto's step, or its state unreadable, it is refused. */
    var late = patchdUnit();
    hook(late, 'activate');
    late.env.WOC_HOOK_DEADLINE_MS = '39000';
    r = hook(late, 'deactivate');
    ok('deactivate out of time for Android Auto: refused, aapd still patched (the tool keeps its recovery)', r.code === 1 &&
        /out of time/.test(JSON.stringify(r.result.aa)) && sha(late.bin) !== sha(SRC), r.out);
    var a2 = hook(late, 'activate');
    ok('activate out of time for Android Auto: the install goes on (exit 0)', a2.code === 0 && /out of time/.test(JSON.stringify(a2.result.aa)), a2.out);
    delete late.env.WOC_HOOK_DEADLINE_MS;
    fs.chmodSync(late.bin, 0);
    r = hook(late, 'deactivate');
    fs.chmodSync(late.bin, 0o755);
    ok('deactivate with aapd\'s state unreadable: refused', r.code === 1 && /unreadable/.test(JSON.stringify(r.result.aa)), r.out);
    r = hook(late, 'deactivate');
    ok('deactivate once readable again: restored byte-exact', r.code === 0 && sha(late.bin) === sha(SRC) && sha(late.conf) === sha(SRCCONF), r.out);

    /* A status cut short after its header (the patcher stopped before its
     * edit lines) is not read whole: a removal is refused until one is. */
    var cs = patchdUnit();
    hook(cs, 'activate');
    var cutJs = path.join(cs.d, 'cut-status.js');
    fs.writeFileSync(cutJs, [
        "var m = /(aapd|carplay)_patch\\.js$/.exec(process.argv[1] || '');",
        "if (m && process.argv[2] === 'status' && process.env.WOC_CUT === m[1]) {",
        "    var w = process.stdout.write.bind(process.stdout), cut = false;",
        "    process.stdout.write = function (s) { if (cut) return true; if (/^proven:/.test(s)) { cut = true; if (process.env.WOC_CUT_EXIT) process.exitCode = 1; } return w(s); };",
        "}"].join('\n') + '\n');
    cs.env.NODE_OPTIONS = '--require=' + cutJs;
    cs.env.WOC_CUT = 'aapd';
    cs.env.WOC_CUT_EXIT = '';
    r = hook(cs, 'deactivate');
    ok('deactivate with aapd\'s status cut short, exit 0: refused, aapd still patched', r.code === 1 && /aapd status cut short/.test(JSON.stringify(r.result.aa)) &&
        sha(cs.bin) !== sha(SRC), r.out);
    cs.env.WOC_CUT_EXIT = '1';
    r = hook(cs, 'deactivate');
    ok('deactivate with aapd\'s status cut short, exit 1: refused, aapd still patched', r.code === 1 && /aapd status cut short/.test(JSON.stringify(r.result.aa)) &&
        sha(cs.bin) !== sha(SRC), r.out);
    if (HAVE_CP) {
        cs.env.WOC_CUT = 'carplay';
        r = hook(cs, 'deactivate');
        ok('deactivate with carplay\'s status cut short: refused', r.code === 1 && /carplay status cut short/.test(JSON.stringify(r.result)), r.out);
    }
    cs.env.WOC_CUT = '';
    r = hook(cs, 'deactivate');
    ok('deactivate once both statuses read whole: done, aapd and aapd.conf byte-exact', r.code === 0 && sha(cs.bin) === sha(SRC) && sha(cs.conf) === sha(SRCCONF) &&
        (!HAVE_CP || sha(cs.carplay) === sha(CARPLAY)), r.out);

    /* A firmware update replaced aapd under our backups: a removal leaves
     * it, as carplay's does; aapd.conf of ours is still restored. */
    var fw = patchdUnit();
    hook(fw, 'activate');
    var newer = Buffer.concat([SRC, Buffer.from('firmware update')]);
    fs.writeFileSync(fw.bin, newer);
    r = hook(fw, 'deactivate');
    ok('deactivate over an aapd firmware update: exit 0, the new aapd left as it is, our aapd.conf restored', r.code === 0 && sha(fw.bin) === sha(newer) &&
        sha(fw.conf) === sha(SRCCONF) && /another build than its backup/.test(r.out), r.out);
    ok('a second removal attempt passes too', hook(fw, 'deactivate').code === 0);
    /* Our aapd.conf already back too: only the stale backups and another
     * build; the removal leaves them. */
    var fw2 = patchdUnit();
    hook(fw2, 'activate');
    fs.writeFileSync(fw2.bin, newer);
    fs.writeFileSync(fw2.conf, SRCCONF);
    r = hook(fw2, 'deactivate');
    ok('deactivate with only another aapd build beside our backups: exit 0, left as it is', r.code === 0 && sha(fw2.bin) === sha(newer) &&
        /another build than our backup/.test(JSON.stringify(r.result.aa)), r.out);

    /* A removal passes only when nothing of Android Auto's is left to undo:
     * aapd restored but aapd.conf not is refused. */
    var half = patchdUnit();
    hook(half, 'activate');
    fs.mkdirSync(half.conf + '.woc-tmp');
    r = hook(half, 'deactivate');
    ok('deactivate with aapd restored but aapd.conf\'s write failing: refused, aapd.conf\'s backup kept', r.code === 1 && sha(half.bin) === sha(SRC) &&
        sha(half.conf) !== sha(SRCCONF) && fs.existsSync(half.conf + '.woc-orig') && /not undone/.test(JSON.stringify(r.result.aa)), r.out);
    fs.rmdirSync(half.conf + '.woc-tmp');
    r = hook(half, 'deactivate');
    ok('deactivate once aapd.conf can be written: restored byte-exact', r.code === 0 && sha(half.bin) === sha(SRC) && sha(half.conf) === sha(SRCCONF), r.out);
    /* No backups of ours: the proven aapd and aapd.conf are rebuilt and the
     * removal passes; a build outside the proven list with our edits in it
     * cannot be, and the removal is refused. */
    var nbk = patchdUnit();
    hook(nbk, 'activate');
    [nbk.bin, nbk.conf].forEach(function (f) { fs.unlinkSync(f + '.woc-orig'); fs.unlinkSync(f + '.woc-orig.json'); });
    r = hook(nbk, 'deactivate');
    ok('deactivate with no backups, the proven files patched: both byte-exact, exit 0', r.code === 0 && sha(nbk.bin) === sha(SRC) && sha(nbk.conf) === sha(SRCCONF), r.out);
    var nbu = patchdUnit();
    fs.writeFileSync(nbu.bin, UNPROVEN);
    patcher(nbu, ['apply', '--unproven', '--yes']);
    fs.unlinkSync(nbu.bin + '.woc-orig'); fs.unlinkSync(nbu.bin + '.woc-orig.json');
    var nbuBin = sha(nbu.bin);
    r = hook(nbu, 'deactivate');
    ok('deactivate with an untested aapd patched and its backup gone: refused, exit 1, our edits named, aapd as it was', r.code === 1 && sha(nbu.bin) === nbuBin &&
        sha(nbu.conf) === sha(SRCCONF) && /Android Auto not undone \(aapd none, aapd\.conf original, our edits read in it\)/.test(JSON.stringify(r.result.aa)), r.out);

    /* aapd.conf with a change of its own (not the shipped file, so not
     * rebuilt from it) and its backup unrecorded. */
    var norec = patchdUnit();
    fs.writeFileSync(norec.conf, SRCCONF.toString('utf8').replace(/"RACK_MODE"\s*:\s*\w+/, '"RACK_MODE": 1'));
    var norecConf = sha(norec.conf);
    hook(norec, 'activate');
    fs.unlinkSync(norec.conf + '.woc-orig.json');
    r = hook(norec, 'deactivate');
    ok('deactivate with aapd.conf\'s backup unrecorded: refused, its backup kept', r.code === 1 && sha(norec.bin) === sha(SRC) &&
        sha(norec.conf) !== norecConf && fs.existsSync(norec.conf + '.woc-orig') && /not undone/.test(JSON.stringify(r.result.aa)), r.out);
    var st = patcher(norec, ['revert', '--yes']);
    ok('the patcher\'s revert with aapd.conf unrecorded: exit non-zero', st.code !== 0 && /aapd\.conf/.test(st.out), st.out);
    ok('status names what is left to undo', /^undo:\s+aapd original, aapd\.conf untrusted$/m.test(status(norec)), status(norec));
})();

/* patchd's socket: one connection, its status events and replies. */
function daemon(t) {
    try { fs.unlinkSync(t.env.WOC_PATCH_SOCK); } catch (e) {}
    var c = cp.spawn(process.execPath, [path.join(PATCH, 'patchd.js')], { env: t.env, stdio: ['ignore', 'pipe', 'pipe'] });
    children.push(c);
    var log = '';
    c.stdout.on('data', function (b) { log += b; });
    c.stderr.on('data', function (b) { log += b; });
    return new Promise(function (resolve) {
        (function wait(n) {
            if (fs.existsSync(t.env.WOC_PATCH_SOCK)) {
                var s = net.connect(t.env.WOC_PATCH_SOCK), buf = '', waiters = [];
                var api = { msgs: [], log: function () { return log; }, proc: c,
                    send: function (o) { s.write(JSON.stringify(o) + '\n'); },
                    next: function (pred, ms) {
                        return new Promise(function (res, rej) {
                            var tm = setTimeout(function () { rej(new Error('timeout; log: ' + log.slice(-800))); }, ms || 20000);
                            waiters.push({ pred: pred, res: function (m) { clearTimeout(tm); res(m); } });
                        });
                    } };
                s.setEncoding('utf8');
                s.on('data', function (d) {
                    buf += d;
                    var i;
                    while ((i = buf.indexOf('\n')) >= 0) {
                        var m = JSON.parse(buf.slice(0, i));
                        buf = buf.slice(i + 1);
                        api.msgs.push(m);
                        waiters = waiters.filter(function (w) { if (w.pred(m)) { w.res(m); return false; } return true; });
                    }
                });
                s.on('connect', function () { resolve(api); });
                return;
            }
            if (n > 200) throw new Error('patchd did not listen: ' + log);
            setTimeout(function () { wait(n + 1); }, 50);
        })(0);
    });
}
function cropEnv(t) { try { return fs.readFileSync(path.join(t.d, 'var', 'aa-crop.env'), 'utf8'); } catch (e) { return null; } }
function aaDone(m) { return m.ev === 'status' && m.aa && m.aa.state !== 'busy' && !/^(applying|reverting)$/.test(m.aa.reason || ''); }

async function socketTests() {
    /* Without recmgr's aapd entry: written, deferred, aapd never touched. */
    var t = patchdUnit({ recmgr: ['carplay'] });
    var p = await daemon(t);
    var st = await p.next(function (m) { return m.ev === 'status'; });
    ok('status: Android Auto original, its density and range', st.aa.state === 'original' && st.aa.density === 180, JSON.stringify(st.aa));
    p.send({ cmd: 'aa-apply' });
    st = await p.next(function (m) { return aaDone(m) && m.aa.aapd; });
    ok('aa-apply without recmgr\'s aapd entry: deferred, pending marker, no systemctl', st.aa.aapd === 'deferred' && st.aa.pending === 'apply' &&
        marker(t, 'aa-pending').action === 'apply' && !calls(t).some(function (l) { return /aapd/.test(l); }) && ro(t), JSON.stringify(st.aa) + calls(t).join(';'));
    ok('a pending patch is removable', st.aa.removable === true, JSON.stringify(st.aa));

    /* aapd restartable, its patch cut short after aapd was replaced (the
     * aapd.conf write fails): aapd still runs the old file, kept named, and
     * the change waits for its start; aapd is not restarted for it. */
    var cut = patchdUnit({ recmgr: ['carplay', 'aapd'] });
    fs.mkdirSync(path.join(cut.d, 'proc', '4343'));
    fs.linkSync(cut.bin, path.join(cut.d, 'proc', '4343', 'exe'));
    var cutIno = fs.statSync(cut.bin).ino;
    fs.mkdirSync(cut.conf + '.woc-tmp');
    var cp2 = await daemon(cut);
    await cp2.next(function (m) { return m.ev === 'status'; });
    cp2.send({ cmd: 'aa-apply' });
    var cst = await cp2.next(function (m) { return aaDone(m) && (m.aa.error || m.aa.state === 'error' || m.aa.pending); });
    await new Promise(function (r) { setTimeout(r, 500); });
    cp2.send({ cmd: 'status' });
    cst = await cp2.next(function (m) { return m.ev === 'status' && m.aa && m.aa.state !== 'busy'; });
    ok('aapd restartable, the patch cut short after aapd was replaced: the change waits for its start, aapd not restarted, its file named, / read-only',
        cst.aa.pending === 'apply' && (marker(cut, 'aa-pending') || {}).action === 'apply' && !calls(cut).some(function (l) { return /start aapd|aapd\.service/.test(l) && !/^systemctl show/.test(l); }) &&
        fs.statSync(path.join(cut.d, 'proc', '4343', 'exe')).ino === cutIno && fs.statSync(cut.bin + '.woc-running').ino === cutIno &&
        sha(cut.bin) !== sha(SRC) && sha(cut.conf) === sha(SRCCONF) && ro(cut), JSON.stringify(cst.aa) + ' ' + calls(cut).join(';'));
    cp2.proc.kill('SIGKILL');
    fs.rmdirSync(cut.conf + '.woc-tmp');

    /* aapd.conf another tool changed, no backup of ours: patchd reports
     * nothing to remove, and a removal of the tool leaves it as it is. */
    var fu = patchdUnit({ recmgr: ['carplay'] });
    var fuText = SRCCONF.toString('utf8').replace('"SA_PHYSICALDENSITY": 208', '"SA_PHYSICALDENSITY": 211');
    fs.writeFileSync(fu.conf, fuText);
    var fp = await daemon(fu);
    var fst = await fp.next(function (m) { return m.ev === 'status' && m.aa && m.aa.state !== 'unknown'; });
    ok('aapd.conf changed by another tool, no backup: Not patched and not removable', fst.aa.state === 'original' && fst.aa.removable === false, JSON.stringify(fst.aa));
    fp.proc.kill('SIGKILL');
    var fr = hook(fu, 'deactivate');
    ok('aapd.conf changed by another tool, no backup: a removal leaves it byte for byte, exit 0', fr.code === 0 && fs.readFileSync(fu.conf, 'utf8') === fuText &&
        sha(fu.bin) === sha(SRC) && !marker(fu, 'aa-pending'), fr.out);

    /* Remove patch while that patch waits: the running aapd is the original,
     * so both files are restored at once and nothing waits; then Patch again. */
    var pend = patchdUnit({ recmgr: ['carplay'] });
    fs.mkdirSync(path.join(pend.d, 'proc', '4242'));
    fs.linkSync(pend.bin, path.join(pend.d, 'proc', '4242', 'exe'));
    /* With the flag file, aapd starts again as a revert starts (a crash
     * restart): its exe is the file the revert replaces. */
    var crashFlag = path.join(pend.d, 'crash-restart'), crashJs = path.join(pend.d, 'crash-restart.js');
    var crashWritten = path.join(pend.d, 'crash-written');
    /* With crash-written, aapd starts again as the revert ends, before
     * patchd puts its running file back: its exe is the file the revert wrote. */
    fs.writeFileSync(crashJs, "var fs = require('fs'), F = " + JSON.stringify(crashFlag) + ", W = " + JSON.stringify(crashWritten) + ", E = " + JSON.stringify(path.join(pend.d, 'proc', '4242', 'exe')) + ";\n" +
        "function start() { fs.unlinkSync(E); fs.linkSync(" + JSON.stringify(pend.bin) + ", E); }\n" +
        "if (/aapd_patch\\.js$/.test(process.argv[1] || '') && process.argv[2] === 'revert') {\n" +
        "  if (fs.existsSync(F)) { fs.unlinkSync(F); start(); }\n" +
        "  process.on('exit', function () { if (fs.existsSync(W)) { fs.unlinkSync(W); start(); } });\n}\n");
    function exeIno() { return fs.statSync(path.join(pend.d, 'proc', '4242', 'exe')).ino; }
    function heldAa() { return ['', '.1', '.2'].filter(function (s) { return fs.existsSync(pend.bin + '.woc-running' + s); }); }
    pend.env.NODE_OPTIONS = '--require=' + crashJs;
    var pp = await daemon(pend);
    await pp.next(function (m) { return m.ev === 'status'; });
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.aapd; });
    ok('Patch without recmgr\'s aapd entry: pending, the running aapd kept linked', st.aa.pending === 'apply' && fs.existsSync(pend.bin + '.woc-running'), JSON.stringify(st.aa));
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.state === 'original'; });
    ok('Remove patch of the pending patch: both files byte-exact at once, nothing pending, no marker, no link, not removable', !st.aa.pending && !st.aa.aapd &&
        sha(pend.bin) === sha(SRC) && sha(pend.conf) === sha(SRCCONF) && !marker(pend, 'aa-pending') && !fs.existsSync(pend.bin + '.woc-running') &&
        st.aa.removable === false, JSON.stringify(st.aa));
    /* After a head-unit start runs the patch: a waiting revert, then aapd
     * started again from the reverted files (what the revert's marker says
     * aapd read is stale), then Patch and Remove patch: nothing waits. */
    function aapdRuns(file) {
        cp.execFileSync('rm', ['-rf', path.join(pend.d, 'proc', '4242')]);
        fs.mkdirSync(path.join(pend.d, 'proc', '4242'));
        fs.linkSync(file, path.join(pend.d, 'proc', '4242', 'exe'));
    }
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'apply'; });
    fs.unlinkSync(path.join(pend.shm, 'aa-pending'));
    aapdRuns(pend.bin);
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'revert'; });
    aapdRuns(pend.bin);
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'apply'; });
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && (m.aa.state === 'original' || m.aa.pending === 'revert'); });
    ok('aapd started again after a waiting revert, then Patch and Remove patch: nothing waits, both files byte-exact', st.aa.state === 'original' && !st.aa.pending &&
        sha(pend.bin) === sha(SRC) && sha(pend.conf) === sha(SRCCONF) && !marker(pend, 'aa-pending'), JSON.stringify(st.aa));
    /* Remove patch of a waiting patch while aapd restarts from the patched
     * file: aapd runs the patch, so the removal waits. */
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'apply'; });
    var waitingIno = fs.statSync(pend.bin).ino;
    fs.writeFileSync(crashFlag, '');
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && (m.aa.state === 'original' || m.aa.pending === 'revert' || m.aa.state === 'error'); });
    ok('aapd restarted from the patch while its Remove patch ran: the removal waits for the start, both files byte-exact', st.aa.pending === 'revert' &&
        (marker(pend, 'aa-pending') || {}).action === 'revert' && sha(pend.bin) === sha(SRC) && sha(pend.conf) === sha(SRCCONF), JSON.stringify(st.aa));
    ok('... the patch aapd runs kept linked, no other name, / read-only, not Failed', st.aa.state !== 'error' && !st.aa.error && exeIno() === waitingIno &&
        fs.statSync(pend.bin + '.woc-running').ino === waitingIno && heldAa().length === 1 && ro(pend), heldAa().join(',') + ' ' + JSON.stringify(st.aa));
    try { fs.unlinkSync(path.join(pend.shm, 'aa-pending')); } catch (e) {}
    aapdRuns(pend.bin);
    ['', '.1', '.2'].forEach(function (s) { try { fs.unlinkSync(pend.bin + '.woc-running' + s); } catch (e) {} });
    /* Remove patch of a waiting patch while aapd restarts from the file the
     * revert wrote, before the put-back: that file stays linked, and the
     * removal waits (aapd may have read aapd.conf before the revert did). */
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'apply'; });
    fs.writeFileSync(crashWritten, '');
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && (m.aa.state === 'original' || m.aa.pending === 'revert' || m.aa.state === 'error'); });
    ok('aapd restarted from the reverted file before the put-back: it stays linked, the removal waits, / read-only, both files byte-exact', st.aa.pending === 'revert' &&
        st.aa.state !== 'error' && fs.readFileSync(path.join(pend.d, 'proc', '4242', 'exe')).equals(SRC) && fs.statSync(pend.bin + '.woc-running').ino === exeIno() &&
        fs.statSync(pend.bin).ino !== exeIno() && heldAa().length === 1 && ro(pend) && sha(pend.bin) === sha(SRC) && sha(pend.conf) === sha(SRCCONF),
        heldAa().join(',') + ' ' + JSON.stringify(st.aa));
    try { fs.unlinkSync(path.join(pend.shm, 'aa-pending')); } catch (e) {}
    aapdRuns(pend.bin);
    ['', '.1', '.2'].forEach(function (s) { try { fs.unlinkSync(pend.bin + '.woc-running' + s); } catch (e) {} });
    /* The same, with both extra names already holding files other processes
     * run: no name is free for the written file, so it is not put back. It
     * stays at the target, named, and the removal waits. */
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'apply'; });
    [1, 2].forEach(function (i) {
        fs.writeFileSync(pend.bin + '.woc-running.' + i, 'another program ' + i);
        fs.mkdirSync(path.join(pend.d, 'proc', '500' + i));
        fs.linkSync(pend.bin + '.woc-running.' + i, path.join(pend.d, 'proc', '500' + i, 'exe'));
    });
    fs.writeFileSync(crashWritten, '');
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && (m.aa.state === 'original' || m.aa.pending === 'revert' || m.aa.state === 'error'); });
    ok('no name free for the written file: no put-back, it stays at the target, the removal waits, / read-only', st.aa.pending === 'revert' && st.aa.state !== 'error' &&
        fs.statSync(pend.bin).ino === exeIno() && sha(pend.bin) === sha(SRC) && sha(pend.conf) === sha(SRCCONF) && ro(pend) &&
        ['4242', '5001', '5002'].every(function (p) { return fs.statSync(path.join(pend.d, 'proc', p, 'exe')).nlink > 1; }),
        heldAa().join(',') + ' ' + JSON.stringify(st.aa));
    [1, 2].forEach(function (i) { cp.execFileSync('rm', ['-rf', path.join(pend.d, 'proc', '500' + i)]); });
    try { fs.unlinkSync(path.join(pend.shm, 'aa-pending')); } catch (e) {}
    aapdRuns(pend.bin);
    ['', '.1', '.2'].forEach(function (s) { try { fs.unlinkSync(pend.bin + '.woc-running' + s); } catch (e) {} });
    /* A waiting patch removed once aapd's restart entry is loaded: aapd
     * runs the original already, so it is put back, not restarted. */
    pp.send({ cmd: 'aa-apply' });
    st = await pp.next(function (m) { return aaDone(m) && m.aa.pending === 'apply'; });
    var recmgrTime = fs.statSync(pend.recmgr).mtime;
    fs.writeFileSync(pend.recmgr, '[Whitelist]\ncarplay\naapd\n[WhitelistExitSuccess]\ncarplay\naapd\n');
    fs.utimesSync(pend.recmgr, recmgrTime, recmgrTime);
    pp.send({ cmd: 'aa-revert' });
    st = await pp.next(function (m) { return aaDone(m) && (m.aa.state === 'original' || m.aa.pending === 'revert' || m.aa.state === 'error'); });
    ok('Remove patch of a waiting patch with aapd restartable: put back, nothing waits, aapd not restarted', st.aa.state === 'original' && !st.aa.pending &&
        !marker(pend, 'aa-pending') && !calls(pend).some(function (l) { return /aapd/.test(l); }) && sha(pend.bin) === sha(SRC), JSON.stringify(st.aa) + calls(pend).join(';'));
    pp.proc.kill('SIGKILL');

    p.send({ cmd: 'aa-density', value: 155 });
    st = await p.next(function (m) { return m.ev === 'refused'; });
    ok('aa-density: a value off the steps is refused', /150-400/.test(st.reason), JSON.stringify(st));
    p.send({ cmd: 'aa-density', value: 240 });
    st = await p.next(function (m) { return aaDone(m) && m.aa.aapd; });
    ok('aa-density: aapd.conf rewritten at 240 dpi', JSON.parse(fs.readFileSync(t.conf, 'utf8')).SCREENS.KOMBI_DISPLAY.CODEC_DEFINITION.HIGH_1080P.REPORTED_SCREEN_DENSITY === 240 &&
        fs.readFileSync(path.join(t.d, 'var', 'aa-density'), 'utf8').trim() === '240');
    p.send({ cmd: 'status' });
    var spaced = await p.next(function (m) { return m.ev === 'status'; });
    p.send({ cmd: 'aa-density', value: 200 });
    st = await p.next(function (m) { return m.ev === 'refused'; });
    ok('aa-density: a second change within 10 s is refused, nothing rewritten; status says when one is taken again', /moments ago/.test(st.reason) &&
        spaced.aa.changeAt > Date.now() && spaced.aa.changeAt <= Date.now() + 10000 &&
        JSON.parse(fs.readFileSync(t.conf, 'utf8')).SCREENS.KOMBI_DISPLAY.CODEC_DEFINITION.HIGH_1080P.REPORTED_SCREEN_DENSITY === 240, JSON.stringify([st, spaced.aa]));
    ok('aa-density: pending, the disk at 240 dpi, aapd still running the original', spaced.aa.pending === 'apply' && spaced.aa.dpi === 240 &&
        spaced.aa.runDpi === null && marker(t, 'aa-pending').run === null, JSON.stringify(spaced.aa));

    /* Restart head unit: once, then refused, and refused again by a new patchd within 10 min. */
    fs.writeFileSync(t.env.WOC_BOOT_ID, 'boot-R\n');
    p.send({ cmd: 'reboot' });
    var rb = await p.next(function (m) { return m.ev === 'reboot-reply'; });
    for (var i = 0; i < 250 && calls(t).indexOf('reboot') < 0; i++) await sleep(20);
    ok('reboot: a pending Android Auto change is enough; one reboot', rb.ok === true && calls(t).filter(function (l) { return l === 'reboot'; }).length === 1, JSON.stringify(rb));
    ok('reboot: the boot it was asked in is recorded for boot recovery', fs.readFileSync(path.join(t.d, 'var', 'restart-requested'), 'utf8') === 'boot-R\n');
    p.send({ cmd: 'reboot' });
    rb = await p.next(function (m) { return m.ev === 'reboot-reply'; });
    ok('reboot: a second request is refused while the first runs', rb.ok === false && /already in progress/.test(rb.reason), JSON.stringify(rb));
    p.proc.kill('SIGKILL');
    await sleep(100);
    var p2 = await daemon(t);
    p2.send({ cmd: 'reboot' });
    rb = await p2.next(function (m) { return m.ev === 'reboot-reply'; });
    ok('reboot: a restarted patchd refuses within 10 minutes of the last request', rb.ok === false && /possible again in \d+ min/.test(rb.reason) &&
        calls(t).filter(function (l) { return l === 'reboot'; }).length === 1, JSON.stringify(rb));
    p2.proc.kill('SIGKILL');

    /* A reboot that fails takes out only its own record. */
    var f = patchdUnit({ rebootFails: true });
    fs.mkdirSync(path.join(f.d, 'var'), { recursive: true });
    var older = Date.now() - 11 * 60 * 1000;
    fs.writeFileSync(path.join(f.d, 'var', 'reboots.json'), JSON.stringify({ at: [older] }));
    fs.writeFileSync(path.join(f.shm, 'aa-pending'), JSON.stringify({ action: 'apply' }));
    var pf = await daemon(f);
    pf.send({ cmd: 'reboot' });
    rb = await pf.next(function (m) { return m.ev === 'reboot-reply' && m.ok === false; });
    var rec = JSON.parse(fs.readFileSync(path.join(f.d, 'var', 'reboots.json'), 'utf8')).at;
    ok('reboot: a failed reboot takes out its own record only, and can be asked again', rec.length === 0 || (rec.length === 1 && rec[0] === older), JSON.stringify(rec));
    ok('reboot: a failed reboot leaves no restart for boot recovery', !fs.existsSync(path.join(f.d, 'var', 'restart-requested')));
    pf.send({ cmd: 'reboot' });
    rb = await pf.next(function (m) { return m.ev === 'reboot-reply'; });
    ok('reboot: after a failed reboot a new request is accepted', rb.ok === true, JSON.stringify(rb));
    await pf.next(function (m) { return m.ev === 'reboot-reply' && m.ok === false; });
    pf.proc.kill('SIGKILL');

    /* With recmgr's aapd entry: SIGKILL, start, the killed run recorded. */
    var r = patchdUnit({ recmgr: ['aapd'] });
    var victim = cp.spawn('sleep', ['300']);
    children.push(victim);
    fs.writeFileSync(r.unitState, JSON.stringify({ pid: victim.pid }));
    var died = new Promise(function (res) { victim.on('exit', function (code, sig) { res(sig); }); });
    var pr = await daemon(r);
    pr.send({ cmd: 'aa-apply' });
    st = await pr.next(function (m) { return aaDone(m) && m.aa.aapd; });
    var sig = await Promise.race([died, sleep(3000)]);
    ok('aa-apply with recmgr\'s aapd entry: aapd SIGKILLed and started, nothing pending', st.aa.aapd === 'restarted' && sig === 'SIGKILL' &&
        calls(r).indexOf('systemctl start aapd.service') >= 0 && !marker(r, 'aa-pending') && ro(r), JSON.stringify(st.aa) + calls(r).join(';'));
    ok('aa-apply: the killed run is recorded for the watcher', fs.readFileSync(path.join(r.shm, 'aapd-killed'), 'utf8').trim() === 'run-' + victim.pid);
    ok('aa-apply: never a systemctl stop, restart or reboot', !calls(r).some(function (l) { return /systemctl (stop|restart)|reboot/.test(l); }), calls(r).join(';'));
    ok('aa-apply: carplay untouched', !calls(r).some(function (l) { return /carplay/.test(l); }) && (HAVE_CP ? sha(r.carplay) === sha(CARPLAY) : true));
    /* Hiding the card changes the render's crop, which it reads at start:
     * aapd.conf and the crop wait for the next head-unit start. */
    var before = calls(r).length;
    pr.send({ cmd: 'aa-card', value: 'hidden' });
    st = await pr.next(function (m) { return aaDone(m) && m.aa.aapd; });
    ok('aa-card hidden: the crop override written, deferred, aapd not restarted', st.aa.aapd === 'deferred' && st.aa.card === 'hidden' &&
        cropEnv(r) === 'WOC_SRC_CROP=-s src_crop=1280x720:0,156,1280,564\n' && marker(r, 'aa-pending').action === 'apply' &&
        !calls(r).slice(before).some(function (l) { return /aapd/.test(l); }), JSON.stringify(st.aa) + calls(r).slice(before).join(';'));
    pr.send({ cmd: 'aa-revert' });
    await pr.next(function (m) { return aaDone(m) && m.aa.state === 'original' || m.aa && m.aa.pending === 'revert'; });
    ok('aa-revert: originals back byte-exact, the crop override gone', sha(r.bin) === sha(SRC) && sha(r.conf) === sha(SRCCONF) && cropEnv(r) === null);
    pr.proc.kill('SIGKILL');

    /* Android Auto's Patch button patches a build outside the proven list, which an
     * install leaves. */
    var un = patchdUnit();
    fs.writeFileSync(un.bin, UNPROVEN);
    var pu = await daemon(un);
    st = await pu.next(function (m) { return m.ev === 'status'; });
    ok('untested aapd: status original with the conflict', st.aa.state === 'original' && st.aa.conflict === 'untested aapd build', JSON.stringify(st.aa));
    pu.send({ cmd: 'aa-apply' });
    st = await pu.next(function (m) { return aaDone(m) && m.aa.aapd; });
    ok('untested aapd: Patch patches it (--unproven), the conflict gone', st.aa.pending === 'apply' && !st.aa.conflict && sha(un.bin + '.woc-orig') === sha(UNPROVEN) &&
        sha(un.bin) !== sha(UNPROVEN), JSON.stringify(st.aa));
    pu.proc.kill('SIGKILL');
    fs.unlinkSync(path.join(un.shm, 'aa-pending'));
    var ua = hook(un, 'activate');
    ok('untested aapd once enabled: an install keeps it patched like a proven one', ua.code === 0 && !ua.result.aa.conflict && sha(un.bin + '.woc-orig') === sha(UNPROVEN), ua.out);
    ua = hook(un, 'deactivate');
    ok('untested aapd: a removal restores it byte-exact', ua.code === 0 && sha(un.bin) === sha(UNPROVEN) && sha(un.conf) === sha(SRCCONF) && !fs.existsSync(un.bin + '.woc-orig'), ua.out);

    /* Patch refuses what the app does not offer it for: an
     * untested aapd that aapd.service does not start, an unusable aapd.conf. */
    var refused = [['an untested aapd that aapd.service does not start', foreignAapd, /aapd\.service starts \/opt\/other\/aapd/], ['an aapd.conf without the 12.3" codec', function (u) {
        fs.writeFileSync(u.conf, SRCCONF.toString('utf8').replace(/"HIGH_1080P"\s*\]/, '"NONE"]'));
    }, /aapd\.conf not usable/]];
    for (var ri = 0; ri < refused.length; ri++) {
        var fu = patchdUnit();
        refused[ri][1](fu);
        var binBefore = sha(fu.bin), confBefore = sha(fu.conf);
        var pf2 = await daemon(fu);
        await pf2.next(function (m) { return m.ev === 'status'; });
        pf2.send({ cmd: 'aa-apply' });
        st = await pf2.next(function (m) { return m.ev === 'status' && m.aa && m.aa.state === 'error'; });
        ok('Patch with ' + refused[ri][0] + ': refused with the conflict, nothing written', refused[ri][2].test(st.aa.reason) &&
            sha(fu.bin) === binBefore && sha(fu.conf) === confBefore && !fs.existsSync(fu.bin + '.woc-orig') && !marker(fu, 'aa-pending'), JSON.stringify(st.aa));
        pf2.proc.kill('SIGKILL');
    }

    /* Patch while the connect-time read is still running
     * (aapd_patch.js status slowed to 3 s) checks a read made after it
     * arrived, not the daemon's placeholder state. */
    var slow = patchdUnit();
    foreignAapd(slow);
    slowStatus(slow);
    var ps = await daemon(slow);
    await sleep(1000);
    ps.send({ cmd: 'aa-apply' });
    st = await ps.next(function (m) { return m.ev === 'status' && m.aa && m.aa.state === 'error'; }, 30000);
    ok('Patch during the connect-time read: refused with the foreign start, nothing written', /aapd\.service starts \/opt\/other\/aapd/.test(st.aa.reason) &&
        sha(slow.bin) === sha(UNPROVEN) && sha(slow.conf) === sha(SRCCONF) && !fs.existsSync(slow.bin + '.woc-orig'), JSON.stringify(st.aa));
    ps.proc.kill('SIGKILL');

    /* A density change during the connect-time read on a patched unit
     * applies it, whatever the daemon's placeholder state says. */
    var sd = patchdUnit();
    patcher(sd, ['apply', '--yes']);
    slowStatus(sd);
    var psd = await daemon(sd);
    await sleep(1000);
    psd.send({ cmd: 'aa-density', value: 200 });
    st = await psd.next(function (m) { return aaDone(m) && m.aa.dpi === 200; }, 40000);
    ok('a density change during the connect-time read: applied to aapd.conf', JSON.parse(fs.readFileSync(sd.conf, 'utf8')).SCREENS.KOMBI_DISPLAY.CODEC_DEFINITION.HIGH_1080P.REPORTED_SCREEN_DENSITY === 200, JSON.stringify(st.aa));
    psd.proc.kill('SIGKILL');

    /* Zoom and the turn card under a conflict other than an untested
     * build (aapd.service starting another program): each refused with the
     * conflict, neither the choice nor aapd or aapd.conf written. */
    var zc = patchdUnit();
    patcher(zc, ['apply', '--yes']);
    fs.mkdirSync(path.join(zc.d, 'units', 'aapd.service.d'));
    fs.writeFileSync(path.join(zc.d, 'units', 'aapd.service.d', 'x.conf'), '[Service]\nExecStart=\nExecStart=/opt/other/aapd\n');
    var zBin = sha(zc.bin), zConf = sha(zc.conf);
    var pz = await daemon(zc);
    await pz.next(function (m) { return m.ev === 'status'; });
    pz.send({ cmd: 'aa-density', value: 200 });
    st = await pz.next(function (m) { return m.ev === 'refused' && m.cmd === 'aa-density'; });
    ok('Zoom under a foreign start: refused with the conflict, nothing written', /aapd\.service starts \/opt\/other\/aapd/.test(st.reason) &&
        sha(zc.bin) === zBin && sha(zc.conf) === zConf && !fs.existsSync(path.join(zc.d, 'var', 'aa-density')), JSON.stringify(st));
    await pz.next(function (m) { return m.ev === 'status' && m.aa && m.aa.state !== 'busy'; });
    pz.send({ cmd: 'aa-card', value: 'hidden' });
    st = await pz.next(function (m) { return m.ev === 'refused' && m.cmd === 'aa-card'; });
    ok('the turn card under a foreign start: refused with the conflict, not for the spacing; nothing written', /aapd\.service starts \/opt\/other\/aapd/.test(st.reason) &&
        sha(zc.bin) === zBin && sha(zc.conf) === zConf && !fs.existsSync(path.join(zc.d, 'var', 'aa-card')) && cropEnv(zc) === null, JSON.stringify(st));
    pz.proc.kill('SIGKILL');

    /* A density or card change is busy from the moment it is accepted:
     * nothing else starts meanwhile, a reboot included. */
    var b = patchdUnit();
    fs.writeFileSync(path.join(b.shm, 'activation-pending'), JSON.stringify({ action: 'apply' }));
    hook(b, 'activate');
    var pb = await daemon(b);
    await pb.next(function (m) { return m.ev === 'status'; });
    pb.send({ cmd: 'aa-density', value: 200 });
    pb.send({ cmd: 'aa-revert' });
    st = await pb.next(function (m) { return m.ev === 'refused' && m.cmd === 'aa-revert'; });
    ok('Zoom then Android Auto\'s Remove patch button: the second is refused as busy', /another operation/.test(st.reason), JSON.stringify(st));
    pb.send({ cmd: 'status' });
    st = await pb.next(function (m) { return m.ev === 'status'; });
    ok('while it runs, status says busy for Android Auto, keeps the values on disk and leaves CarPlay\'s state', st.busy === 'aa' &&
        st.aa.state === 'busy' && st.aa.dpi === 180 && st.aa.diskCard === 'shown' && st.state !== 'busy', JSON.stringify(st));
    pb.send({ cmd: 'revert' });
    st = await pb.next(function (m) { return m.ev === 'refused' && m.cmd === 'revert'; });
    ok('Zoom then CarPlay\'s Remove patch button: refused as busy', /another operation/.test(st.reason), JSON.stringify(st));
    pb.send({ cmd: 'reboot' });
    var rb2 = await pb.next(function (m) { return m.ev === 'reboot-reply'; });
    ok('Zoom then Restart head unit: refused while it runs', rb2.ok === false && /another operation/.test(rb2.reason), JSON.stringify(rb2));
    await pb.next(function (m) { return aaDone(m) && m.aa.dpi === 200; });
    ok('the zoom itself completes, the conf at 200 dpi, carplay untouched', JSON.parse(fs.readFileSync(b.conf, 'utf8')).SCREENS.KOMBI_DISPLAY.CODEC_DEFINITION.HIGH_1080P.REPORTED_SCREEN_DENSITY === 200);
    pb.proc.kill('SIGKILL');

    /* What aapd runs, taken before each change and kept in the marker:
     * the first deferred change's stays through a second; a marker
     * without it (an older patchd's) stays without. */
    var n = patchdUnit();
    n.env.WOC_DENSITY_SPACING_MS = '1';
    hook(n, 'activate');
    fs.unlinkSync(path.join(n.shm, 'aa-pending'));   /* the next head-unit start ran it */
    var pn = await daemon(n);
    await pn.next(function (m) { return aaDone(m) && m.aa.state === 'patched'; });
    pn.send({ cmd: 'aa-density', value: 200 });
    st = await pn.next(function (m) { return aaDone(m) && m.aa.aapd; });
    var run = marker(n, 'aa-pending').run;
    ok('a deferred change: the marker\'s run is what was on disk before it', !!run && run.dpi === 180 && run.card === 'shown' &&
        st.aa.runDpi === 180 && st.aa.dpi === 200, JSON.stringify([marker(n, 'aa-pending'), st.aa]));
    pn.send({ cmd: 'aa-card', value: 'hidden' });
    st = await pn.next(function (m) { return aaDone(m) && m.aa.diskCard === 'hidden'; });
    run = marker(n, 'aa-pending').run;
    ok('a second deferred change keeps the first run', !!run && run.dpi === 180 && run.card === 'shown' &&
        st.aa.runDpi === 180 && st.aa.runCard === 'shown', JSON.stringify([marker(n, 'aa-pending'), st.aa]));
    fs.writeFileSync(path.join(n.shm, 'aa-pending'), JSON.stringify({ action: 'apply' }) + '\n');
    pn.send({ cmd: 'aa-density', value: 190 });
    st = await pn.next(function (m) { return aaDone(m) && m.aa.dpi === 190; });
    ok('a marker without run stays without', !marker(n, 'aa-pending').hasOwnProperty('run') && st.aa.runDpi === undefined,
        JSON.stringify([marker(n, 'aa-pending'), st.aa]));
    pn.proc.kill('SIGKILL');

    /* An operation takes what aapd runs from a read that starts after its
     * command: aa-apply sent while a status read runs (patchd just started,
     * nothing read yet) still finds the pending marker and keeps its run. */
    var f = patchdUnit();
    hook(f, 'activate');
    fs.writeFileSync(path.join(f.shm, 'aa-pending'), JSON.stringify({ action: 'apply', run: { dpi: 180, card: 'shown' } }) + '\n');
    fs.writeFileSync(path.join(f.d, 'var', 'aa-density'), '200\n');
    var pf2 = await daemon(f);
    pf2.send({ cmd: 'status' });
    pf2.send({ cmd: 'status' });
    pf2.send({ cmd: 'aa-apply' });
    st = await pf2.next(function (m) { return aaDone(m) && m.aa.dpi === 200; });
    run = marker(f, 'aa-pending').run;
    ok('aa-apply during a status read: run from a read after the command, the marker\'s', !!run && run.dpi === 180 && run.card === 'shown',
        JSON.stringify(marker(f, 'aa-pending')));
    pf2.proc.kill('SIGKILL');

    /* The install hook's failure after the patcher wrote (here the crop
     * override cannot be saved) still writes the marker with run. */
    var h = patchdUnit();
    hook(h, 'activate');
    fs.unlinkSync(path.join(h.shm, 'aa-pending'));   /* the next head-unit start ran it */
    fs.writeFileSync(path.join(h.d, 'var', 'aa-card'), 'hidden\n');
    fs.chmodSync(path.join(h.d, 'var'), 0o555);
    var hr2 = hook(h, 'activate');
    fs.chmodSync(path.join(h.d, 'var'), 0o755);
    run = (marker(h, 'aa-pending') || {}).run;
    ok('the hook\'s failure path keeps run in the marker', /aa-crop\.env/.test(hr2.out) && !!run && run.dpi === 180 && run.card === 'shown',
        JSON.stringify(marker(h, 'aa-pending')) + hr2.out.slice(-600));

    /* Nothing writes / without the remount lock: held by another process,
     * an operation gives up and writes nothing. */
    var l = patchdUnit();
    l.env.WOC_OP_LOCK_WAIT_MS = '500';
    var pl = await daemon(l);
    await pl.next(function (m) { return m.ev === 'status'; });
    fs.writeFileSync(l.env.WOC_REMOUNT_LOCK, '1 held by another\n');
    pl.send({ cmd: 'aa-apply' });
    st = await pl.next(function (m) { return m.ev === 'status' && m.aa && m.aa.state === 'error'; });
    ok('lock held elsewhere: Android Auto\'s Patch button gives up, nothing written, / never remounted', /another change to \/ is running/.test(st.aa.reason) &&
        sha(l.bin) === sha(SRC) && !fs.existsSync(l.bin + '.woc-orig') && ro(l), JSON.stringify(st.aa));
    fs.unlinkSync(l.env.WOC_REMOUNT_LOCK);

    /* A deferred change whose pending marker cannot be written: patchd
     * reports it and stays up. */
    fs.mkdirSync(path.join(l.shm, 'aa-pending'));
    pl.send({ cmd: 'aa-apply' });
    st = await pl.next(function (m) { return aaDone(m) && (m.aa.aapd || m.aa.state === 'error'); });
    pl.send({ cmd: 'status' });
    var alive = await pl.next(function (m) { return m.ev === 'status'; });
    ok('a pending marker that cannot be written does not take patchd down', !!alive && pl.proc.exitCode === null, JSON.stringify(st.aa));
    pl.proc.kill('SIGKILL');
}

/* ---- boot recovery for aapd ---- */
var recovery = require('../recovery.js');
var record = require('../patch_record.js');
/* The aapd watcher on a patched aapd of ours (its backup and record) in a
 * folder of its own, with systemd and the restore stubbed. */
function fakeRec(opts) {
    var d = opts.dir || fs.mkdtempSync(path.join(os.tmpdir(), 'woc-aarec-'));
    dirs.push(d);
    var t = recovery.targets({ state: path.join(d, 'boot-recovery.json'), sock: path.join(d, 'patch.sock'), carplay: path.join(d, 'carplay'),
        aapd: path.join(d, 'aapd'), aapdConf: path.join(d, 'aapd.conf') }).aapd;
    if (!opts.dir) {
        fs.writeFileSync(t.target + '.woc-orig', 'aapd\n');
        fs.writeFileSync(t.target + '.woc-orig.json', JSON.stringify({ sha256_original: sha(Buffer.from('aapd\n')) }));
        fs.writeFileSync(t.target, 'patched aapd\n');
        record.write(t.target, { patched: true, proven: true });
    }
    if (opts.killed) fs.writeFileSync(t.killed, opts.killed + '\n');
    var now = 0, ev = [], info = opts.info || { state: 'active', run: 'r1', pid: 1 };
    var rec = new recovery.Recovery({
        target: t, bootId: opts.boot || 'B1', healthyMs: 60000,
        restore: function (f, cb) { ev.push('restore'); cb(null, 'restored'); },
        probe: function (cb) { cb(rec.info); }, systemState: function (cb) { cb('running'); },
        start: function (cb) { ev.push('start'); cb(null); },
        alive: function () { return false; },
        recmgrLoaded: function (n) { ev.push('recmgr ' + n); return opts.recmgr !== false; },
        lock: { tryAcquire: function () { return true; }, release: function () {} }, now: function () { return now; }, log: function () {},
    });
    rec.info = info;
    rec.ev = ev;
    rec.dir = d;
    rec.advance = function (ms) { now += ms; };
    return rec;
}
function tick(rec) { return new Promise(function (res) { rec.tick(res); }); }

async function recoveryTests() {
    var a = fakeRec({});
    await new Promise(function (r) { a.startBoot(r); });
    a.info = { state: 'failed', result: 'signal', code: '2', status: '9', run: 'r1', pid: 0 };
    a.advance(6000);
    var x = await tick(a);
    ok('aapd SIGKILLed by another (Result=signal, status 9): no crash, started again', x.action === 'killed' && a.ev.indexOf('recmgr aapd') >= 0, JSON.stringify(x));
    for (var i = 0; i < 6; i++) { a.advance(5000); await tick(a); }
    ok('aapd killed: started once 5 s later', a.ev.filter(function (e) { return e === 'start'; }).length === 1 && a.ev.indexOf('restore') < 0, a.ev.join(','));

    var b = fakeRec({});
    await new Promise(function (r) { b.startBoot(r); });
    var crashes = 0;
    for (var run = 1; run <= 5; run++) {
        b.info = { state: 'failed', result: 'core-dump', code: '3', status: '11', run: 'c' + run, pid: 0 };
        b.advance(70000);
        var y = await tick(b);
        if (y.action === 'crash') crashes++;
        await tick(b);   /* the same run seen again: no second count */
        b.advance(70000);
        await tick(b);
    }
    var starts = b.ev.filter(function (e) { return e === 'start'; }).length;
    ok('aapd crashes: each run counted once, restored at the second, before more starts', crashes === 5 && b.ev.indexOf('restore') >= 0 &&
        b.ev.indexOf('restore') < b.ev.lastIndexOf('start'), b.ev.join(','));
    ok('aapd crashes: started again at most three times a boot', starts === 3, String(starts));
    var st = JSON.parse(fs.readFileSync(b.statePath, 'utf8'));
    ok('aapd crashes: the boot\'s starts are on disk', st.starts && st.starts.n === 3, JSON.stringify(st));

    var k = fakeRec({ killed: 'pk' });
    k.info = { state: 'failed', result: 'signal', code: '2', status: '9', run: 'pk', pid: 0 };
    await new Promise(function (r) { k.startBoot(r); });
    k.advance(6000);
    var z = await tick(k);
    ok('patchd\'s own kill (aapd-killed) is neither a crash nor started again', z.action === 'watch' && k.ev.indexOf('start') < 0, JSON.stringify(z));

    var n = fakeRec({ recmgr: false });
    await new Promise(function (r) { n.startBoot(r); });
    n.info = { state: 'failed', result: 'core-dump', code: '3', status: '6', run: 'x1', pid: 0 };
    n.advance(6000);
    await tick(n);
    for (i = 0; i < 20; i++) { n.advance(5000); await tick(n); }
    ok('without recmgr\'s aapd entry: the crash recorded, aapd not started', n.ev.indexOf('start') < 0 && JSON.parse(fs.readFileSync(n.statePath, 'utf8')).arm.crashes === 1);

    /* Across boots: a crash at start (mark failed) twice restores before
     * aapd; a switch-off (mark running) counts nothing. */
    var d = fakeRec({ boot: 'B1' });
    await new Promise(function (r) { d.startBoot(r); });
    fs.writeFileSync(path.join(d.dir, 'shutdown-aapd'), 'B1 failed crash\n');
    var r2 = await new Promise(function (r) { fakeRec({ boot: 'B2', dir: d.dir }).startBoot(r); });
    fs.writeFileSync(path.join(d.dir, 'shutdown-aapd'), 'B2 failed crash\n');
    var r3 = await new Promise(function (r) { fakeRec({ boot: 'B3', dir: d.dir }).startBoot(r); });
    ok('boot step: two boots whose aapd failed restore before aapd starts', r2.action === 'armed' && r3.action === 'restored', JSON.stringify([r2, r3]));
    var s1 = fakeRec({ boot: 'S1' });
    await new Promise(function (r) { s1.startBoot(r); });
    fs.writeFileSync(path.join(s1.dir, 'shutdown-aapd'), 'S1 running\n');
    var rs = await new Promise(function (r) { fakeRec({ boot: 'S2', dir: s1.dir }).startBoot(r); });
    ok('boot step: a switch-off within seconds (mark running) is no failure', rs.action === 'armed' && rs.fails === 0, JSON.stringify(rs));

    /* The real boot step on copies: the restore before aapd, / read-only. */
    var t = patchdUnit();
    patcher(t, ['apply', '--yes']);
    var state = path.join(t.d, 'var', 'boot-recovery.json');
    fs.writeFileSync(path.join(t.d, 'var', 'aa-boot-recovery.json'), JSON.stringify({ fails: 1, unknown: 0, arm: { boot: 'old', crashes: 1 } }));
    fs.writeFileSync(path.join(t.d, 'bootid'), 'new\n');
    var env = Object.assign({}, t.env, { WOC_RECOVERY_STATE: state, WOC_BOOT_ID: path.join(t.d, 'bootid') });
    var res = cp.spawnSync(process.execPath, [path.join(PATCH, 'recovery.js'), 'boot', 'aapd'], { env: env, encoding: 'utf8', timeout: 60000 });
    ok('real boot step at the threshold: originals restored byte-exact, / read-only, exit 0', res.status === 0 && sha(t.bin) === sha(SRC) && sha(t.conf) === sha(SRCCONF) && ro(t) &&
        /aapd boot check: restored/.test(res.stdout), res.stdout + res.stderr);
    var cpState = fs.existsSync(state);
    ok('real boot step: carplay\'s counter untouched', !cpState);

    /* A restore cut short after aapd (aapd.conf still patched): patchd
     * reports the restore, and an install does not apply the patch again. */
    var q = patchdUnit();
    patcher(q, ['apply', '--yes']);
    var patchedConf = fs.readFileSync(q.conf);
    var qr = cp.spawnSync(process.execPath, ['-e', 'var r = require(process.argv[1]); r.restoreTarget(r.TARGETS.aapd, true, 2, function (e, o) { process.stdout.write(String(e || o)); });',
        path.join(PATCH, 'recovery.js')], { env: Object.assign({}, q.env, { WOC_RECOVERY_STATE: path.join(q.d, 'var', 'boot-recovery.json') }), encoding: 'utf8' });
    fs.writeFileSync(q.conf, patchedConf);
    var qh = hook(q, 'activate');
    ok('a restore that left aapd.conf patched: an install leaves aapd off and says why', qr.stdout === 'restored' && sha(q.bin) === sha(SRC) &&
        /boot recovery removed the patch after 2 failed head-unit starts/.test((qh.result.aa || {}).status || ''), qr.stdout + qr.stderr + ' ' + JSON.stringify(qh.result));

    /* The boot step's SIGTERM waits for a restore in progress. */
    var u = patchdUnit();
    patcher(u, ['apply', '--yes']);
    fs.writeFileSync(path.join(u.d, 'var', 'aa-boot-recovery.json'), JSON.stringify({ fails: 2, unknown: 0, arm: null }));
    fs.writeFileSync(u.env.WOC_MOUNT, '#!/bin/sh\nsleep 1\ncase "$*" in *rw*) echo "/dev/root / ext4 rw 0 0" > "' + u.mounts + '";; *) echo "/dev/root / ext4 ro 0 0" > "' + u.mounts + '";; esac\n');
    var envU = Object.assign({}, u.env, { WOC_RECOVERY_STATE: path.join(u.d, 'var', 'boot-recovery.json'), WOC_BOOT_ID: path.join(u.d, 'bootid') });
    fs.writeFileSync(path.join(u.d, 'bootid'), 'x\n');
    var ch = cp.spawn(process.execPath, [path.join(PATCH, 'recovery.js'), 'boot', 'aapd'], { env: envU });
    children.push(ch);
    await sleep(500);
    ch.kill('SIGTERM');
    await new Promise(function (res2) { ch.on('exit', res2); });
    ok('boot step: SIGTERM mid-restore lets the restore finish, / read-only', sha(u.bin) === sha(SRC) && ro(u));

    /* Both boot steps at their threshold in the same boot: they share the
     * remount lock, and each waits for the other's restore. */
    if (HAVE_CP) {
        var both = patchdUnit();
        patcher(both, ['apply', '--yes']);
        var cpr = cp.spawnSync(process.execPath, [path.join(PATCH, 'carplay_patch.js'), 'apply', '--panel', 'high', '--target', both.carplay, '--yes'], { encoding: 'utf8' });
        fs.writeFileSync(both.env.WOC_MOUNT, '#!/bin/sh\nsleep 1\ncase "$*" in *rw*) echo "/dev/root / ext4 rw 0 0" > "' + both.mounts + '";; *) echo "/dev/root / ext4 ro 0 0" > "' + both.mounts + '";; esac\n');
        var bv = path.join(both.d, 'var');
        fs.writeFileSync(path.join(bv, 'boot-recovery.json'), JSON.stringify({ fails: 2, unknown: 0, arm: null }));
        fs.writeFileSync(path.join(bv, 'aa-boot-recovery.json'), JSON.stringify({ fails: 2, unknown: 0, arm: null }));
        fs.writeFileSync(path.join(both.d, 'bootid'), 'b\n');
        var be = Object.assign({}, both.env, { WOC_RECOVERY_STATE: path.join(bv, 'boot-recovery.json'), WOC_BOOT_ID: path.join(both.d, 'bootid') });
        var ends = await Promise.all(['carplay', 'aapd'].map(function (n) {
            return new Promise(function (res) {
                var c = cp.spawn(process.execPath, [path.join(PATCH, 'recovery.js'), 'boot', n], { env: be });
                var o = '';
                c.stdout.on('data', function (d) { o += d; });
                c.on('exit', function () { res(o); });
            });
        }));
        ok('both boot steps at the threshold at once: each restores, one after the other', cpr.status === 0 && /carplay boot check: restored/.test(ends[0]) &&
            /aapd boot check: restored/.test(ends[1]) && sha(both.carplay) === sha(CARPLAY) && sha(both.bin) === sha(SRC) && ro(both), ends.join(' | '));
    }

    /* A boot cut off with no record of its own counts for the service
     * that crashed in it, not for the other; with no crash of either it is
     * unknown for carplay (power lost), and the second such boot in a row
     * counts one. */
    [[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 1]].forEach(function (c) {
        var x = patchdUnit();
        var vs = path.join(x.d, 'var');
        fs.writeFileSync(x.carplay + '.woc-orig', 'x');
        fs.writeFileSync(path.join(vs, 'boot-recovery.json'), JSON.stringify({ fails: 0, unknown: c[3], arm: { boot: 'old', crashes: 0 } }));
        fs.writeFileSync(path.join(vs, 'aa-boot-recovery.json'), JSON.stringify({ fails: 0, unknown: 0, prev: null, arm: { boot: 'old', crashes: c[0] } }));
        fs.writeFileSync(path.join(x.d, 'bootid'), 'new\n');
        var e = Object.assign({}, x.env, { WOC_RECOVERY_STATE: path.join(vs, 'boot-recovery.json'), WOC_BOOT_ID: path.join(x.d, 'bootid') });
        cp.spawnSync(process.execPath, [path.join(PATCH, 'recovery.js'), 'boot', 'carplay'], { env: e, encoding: 'utf8', timeout: 30000 });
        var cst = JSON.parse(fs.readFileSync(path.join(vs, 'boot-recovery.json'), 'utf8'));
        ok(c[0] ? 'an unrecorded end after aapd crashed in that boot is not carplay\'s failure'
            : c[3] ? 'a second unrecorded end in a row with no aapd crash counts one for carplay' : 'a first unrecorded end with no aapd crash is unknown for carplay (power lost)',
            cst.fails === c[1] && cst.unknown === c[2] && cst.arm.boot === 'new', JSON.stringify(cst));
    });

    /* shutdown_mark.sh: aapd SIGKILLed is stopped (no crash), as for its
     * watcher; carplay's record is unchanged. */
    var md = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-mark-'));
    dirs.push(md);
    fs.writeFileSync(path.join(md, 'boot_id'), 'B\n');
    fs.writeFileSync(path.join(md, 'systemctl'), '#!/bin/sh\nprintf "ActiveState=failed\\nResult=signal\\nExecMainCode=2\\nExecMainStatus=9\\n"\n');
    fs.chmodSync(path.join(md, 'systemctl'), 0o755);
    fs.mkdirSync(path.join(md, 'state'));
    var mr = cp.spawnSync('sh', [path.join(PATCH, 'shutdown_mark.sh')], { env: Object.assign({}, process.env, { WOC_STATE_DIR: path.join(md, 'state'),
        WOC_BOOT_ID: path.join(md, 'boot_id'), WOC_SYSTEMCTL: path.join(md, 'systemctl'), WOC_MARK_WAIT: '2' }) });
    ok('shutdown mark: a SIGKILLed aapd is stopped, a SIGKILLed carplay (no drop-in) failed', mr.status === 0 &&
        /^B stopped up \d+\n$/.test(fs.readFileSync(path.join(md, 'state', 'shutdown-aapd'), 'utf8')) && /^B failed up \d+\n$/.test(fs.readFileSync(path.join(md, 'state', 'shutdown'), 'utf8')));
    fs.writeFileSync(path.join(md, 'systemctl'), '#!/bin/sh\nprintf "ActiveState=failed\\nResult=signal\\nExecMainCode=2\\nExecMainStatus=90\\n"\n');
    cp.spawnSync('sh', [path.join(PATCH, 'shutdown_mark.sh')], { env: Object.assign({}, process.env, { WOC_STATE_DIR: path.join(md, 'state'),
        WOC_BOOT_ID: path.join(md, 'boot_id'), WOC_SYSTEMCTL: path.join(md, 'systemctl'), WOC_MARK_WAIT: '2' }) });
    ok('shutdown mark: another signal end of aapd is failed', /^B failed up \d+\n$/.test(fs.readFileSync(path.join(md, 'state', 'shutdown-aapd'), 'utf8')));
    fs.writeFileSync(path.join(md, 'state', 'shutdown-aapd'), 'B stopped\n');
    fs.writeFileSync(path.join(md, 'systemctl'), '#!/bin/sh\nprintf "ActiveState=failed\\nResult=oom-kill\\nExecMainCode=2\\nExecMainStatus=9\\n"\n');
    cp.spawnSync('sh', [path.join(PATCH, 'shutdown_mark.sh')], { env: Object.assign({}, process.env, { WOC_STATE_DIR: path.join(md, 'state'),
        WOC_BOOT_ID: path.join(md, 'boot_id'), WOC_SYSTEMCTL: path.join(md, 'systemctl'), WOC_MARK_WAIT: '2' }) });
    ok('shutdown mark: an oom-killed aapd (status 9) is failed', /^B failed up \d+\n$/.test(fs.readFileSync(path.join(md, 'state', 'shutdown-aapd'), 'utf8')));

    ok('the units\' timeouts cover the restore bound', [/TimeoutStartSec=(\d+)/, /TimeoutStopSec=(\d+)/].every(function (re) {
        return Number(re.exec(fs.readFileSync(path.join(PATCH, '..', 'systemd', 'waze-on-cluster-aa-boot.service'), 'utf8'))[1]) * 1000 > recovery.BOOT_BOUND_MS + 5000;
    }));
    var unitText = fs.readFileSync(path.join(PATCH, '..', 'systemd', 'waze-on-cluster-aa-boot.service'), 'utf8');
    ok('the aa boot unit runs before aapd.service, a oneshot', /^Before=aapd\.service$/m.test(unitText) && /^Type=oneshot$/m.test(unitText) && /recovery\.js boot aapd$/m.test(unitText));
    var comp = JSON.parse(fs.readFileSync(path.join(PATCH, '..', 'component.json'), 'utf8'));
    ok('component.json: aapd and every unit in recmgr, aapd a crash name, no boot unit restarted on update',
        ['aapd', 'carplay'].concat(comp.units.filter(function (x) { return /\.(service|socket)$/.test(x.name); })
            .map(function (x) { return x.name.replace(/\.(service|socket)$/, ''); })).every(function (n) { return comp.recmgr.indexOf(n) >= 0; }) &&
        comp.crashNames.indexOf('aapd') >= 0 && !comp.restart.some(function (x) { return /boot/.test(x); }), JSON.stringify(comp.recmgr));
}

/* Android Auto on a head unit that refuses our parts, and on MID and HIGH F. */
function headUnitTests() {
    var NOT = headUnit.REFUSED;
    var t = unit();
    headUnit.set(headUnit.NONE);
    var r = patcher(t, ['apply', '--yes']);
    ok('no hwvariant=: aapd apply refuses with the reason line, writes nothing', r.code !== 0 && r.out === 'error: refusing: ' + NOT + '\n' && untouched(t), r.out);
    headUnit.set(null);
    r = patcher(t, ['apply', '--yes']);
    ok('no kernel command line: aapd apply refuses, writes nothing', r.code !== 0 && /unknown variant is not supported \(kernel command line unreadable\)/.test(r.out) && untouched(t), r.out);
    [['MID', headUnit.MID], ['HIGH F', headUnit.HIGHF]].forEach(function (v) {
        headUnit.set(v[1]);
        patcher(t, ['revert', '--yes']);
        r = patcher(t, ['apply', '--yes']);
        ok(v[0] + ': aapd apply patches', r.code === 0 && sha(t.bin) !== sha(SRC), r.out);
    });
    headUnit.set(headUnit.NONE);
    r = patcher(t, ['revert', '--yes']);
    ok('no hwvariant=: aapd revert restores both files', r.code === 0 && sha(t.bin) === sha(SRC) && sha(t.conf) === sha(SRCCONF), r.out);

    var u = patchdUnit();
    r = hook(u, 'activate');
    ok('no hwvariant=: activate leaves Android Auto, the conflict, no marker', r.code === 0 && r.result.aa && r.result.aa.conflict === 'not supported on this car (' + NOT + ')' &&
        untouched(u) && !marker(u, 'aa-pending'), JSON.stringify(r.result));

    var b = patchdUnit();
    headUnit.set(headUnit.HIGH);
    patcher(b, ['apply', '--yes']);
    headUnit.set(headUnit.NONE);
    fs.writeFileSync(path.join(b.d, 'bootid'), 'new\n');
    var env = Object.assign({}, b.env, { WOC_RECOVERY_STATE: path.join(b.d, 'var', 'boot-recovery.json'), WOC_BOOT_ID: path.join(b.d, 'bootid') });
    r = cp.spawnSync(process.execPath, [path.join(PATCH, 'recovery.js'), 'boot', 'aapd'], { env: env, encoding: 'utf8', timeout: 60000 });
    ok('no hwvariant=: the boot step restores aapd and aapd.conf before aapd starts, below the threshold, / read-only', r.status === 0 &&
        sha(b.bin) === sha(SRC) && sha(b.conf) === sha(SRCCONF) && ro(b) && /aapd boot check: restored/.test(r.stdout), r.stdout + r.stderr);

    var c = patchdUnit();
    headUnit.set(headUnit.HIGH);
    patcher(c, ['apply', '--yes']);
    fs.appendFileSync(c.conf, '# another tool\n');
    var conf2 = sha(c.conf);
    headUnit.set(headUnit.NONE);
    var calls = path.join(c.d, 'mount.calls');
    fs.writeFileSync(c.env.WOC_MOUNT, '#!/bin/sh\necho "$*" >> "' + calls + '"\n' +
        'case "$*" in *rw*) echo "/dev/root / ext4 rw 0 0" > "' + c.mounts + '";; *) echo "/dev/root / ext4 ro 0 0" > "' + c.mounts + '";; esac\n');
    var bootEnv = Object.assign({}, c.env, { WOC_RECOVERY_STATE: path.join(c.d, 'var', 'boot-recovery.json'), WOC_BOOT_ID: path.join(c.d, 'bootid') });
    function boot(id) {
        fs.writeFileSync(path.join(c.d, 'bootid'), id + '\n');
        try { fs.unlinkSync(calls); } catch (e) {}
        return cp.spawnSync(process.execPath, [path.join(PATCH, 'recovery.js'), 'boot', 'aapd'], { env: bootEnv, encoding: 'utf8', timeout: 60000 });
    }
    r = boot('b1');
    ok('no hwvariant=, aapd.conf changed by another tool: aapd restored, the conf left', r.status === 0 && sha(c.bin) === sha(SRC) && sha(c.conf) === conf2 &&
        fs.existsSync(calls) && ro(c), r.stdout + r.stderr);
    r = boot('b2');
    ok('no hwvariant=, aapd.conf changed by another tool: the next boot leaves / alone', r.status === 0 && !fs.existsSync(calls) && /aapd boot check: idle/.test(r.stdout), r.stdout + r.stderr);

    /* A car 0.8.2's boot recovery restored on HIGH F: the first install there
     * applies Android Auto's patch again, pending; none after it, nor on
     * HIGH, nor over an untrusted backup. */
    function restored(unit, mark) {
        var x = patchdUnit();
        headUnit.set(headUnit.HIGH);
        hook(x, 'activate');
        ['aa-pending', 'activation-pending'].forEach(function (n) { try { fs.unlinkSync(path.join(x.shm, n)); } catch (e) {} });
        patcher(x, ['revert', '--yes']);
        if (mark) fs.writeFileSync(path.join(x.d, 'var', 'highf-repatched'), '{"carplay":"applied","aa":"applied"}\n');
        headUnit.set(unit);
        return x;
    }
    var x = restored(headUnit.HIGHF);
    r = hook(x, 'activate');
    ok('HIGH F restored by 0.8.2: Android Auto applied again, pending', r.code === 0 && r.result.aa && r.result.aa.reset === true &&
        sha(x.bin) !== sha(SRC) && marker(x, 'aa-pending') && marker(x, 'aa-pending').action === 'apply' &&
        JSON.parse(fs.readFileSync(path.join(x.d, 'var', 'highf-repatched'), 'utf8')).aa === 'applied', JSON.stringify(r.result));
    [['HIGH F, marker present', function () { return restored(headUnit.HIGHF, true); }], ['HIGH', function () { return restored(headUnit.HIGH); }]].forEach(function (c) {
        var y = c[1]();
        r = hook(y, 'activate');
        ok(c[0] + ': Android Auto stays off', r.code === 0 && sha(y.bin) === sha(SRC) && !marker(y, 'aa-pending') && /turned off/.test(r.result.aa.status),
            JSON.stringify(r.result));
    });
    var z = restored(headUnit.HIGHF);
    fs.writeFileSync(z.bin + '.woc-orig.json', JSON.stringify({ sha256_original: '0'.repeat(64) }));
    r = hook(z, 'activate');
    var zMark = {};
    try { zMark = JSON.parse(fs.readFileSync(path.join(z.d, 'var', 'highf-repatched'), 'utf8')); } catch (e) {}
    ok('HIGH F, untrusted aapd backup: Android Auto stays off, left for the next install', r.code === 0 && sha(z.bin) === sha(SRC) &&
        !marker(z, 'aa-pending') && !zMark.aa, JSON.stringify(r.result));
    headUnit.set(headUnit.HIGH);
}

(async function () {
    headUnitTests();
    await socketTests();
    await recoveryTests();
    process.stdout.write(fails ? fails + ' FAILED\n' : 'all passed\n');
    process.exit(fails ? 1 : 0);
})().catch(function (e) { ok('no exception', false, e.stack); process.exit(1); });
