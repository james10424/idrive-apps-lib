#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
// Copyright (C) 2026 mlurker@gmail.com
/* aapd_patch.js — the Android Auto patch: four reversible edits to the
 * Android Auto Projected Daemon (/usr/bin/aapd) and the cluster geometry in
 * /etc/aapd.conf (aapd_conf.js), which together make Google Maps send its
 * cluster view to the stream waze-on-cluster shows (PATCH.md "Android
 * Auto").
 *
 *   gate       `cmp dword [rbx+0x21c],1 ; setbe al` in secondVideoEnabled()
 *              (featureFlag && KombiVariant <= 1): the immediate 1 -> 2.
 *              aapd maps raw KI_KOMBI_VARIANT 9 -> 0, 10 -> 1, 1..8 and
 *              11 -> 2, 0 -> 3, so this admits every raw value of class 2;
 *              apply refuses unless the coding is the proven 7.
 *   selfstart  onNewNavigationStatus()'s call of setNaviStreamStatus
 *              retargeted to requestStream() (same `this`), and
 *              requestStream()'s `je` on a StreamCommandChanged field that is
 *              never set made a `jmp`: aapd starts the cluster stream itself
 *              once navigation is active.
 *   screen     the KombiVariant -> screen-size helper's fall-through: class 2
 *              reads 12.3 as class 0 does, so aapd uses aapd.conf's 12.3
 *              geometry instead of its built-in 800x480 default.
 *
 * The sites are found in each build by aapd_locate.js; a build where any of
 * them is not found is refused. A build on the proven list (by build-id)
 * needs the whole file to be the shipped one for its first edit; the first
 * edit of any other build needs --unproven. aapd.conf is edited when it has
 * the entries the geometry needs (aapd_conf.js check). Backups of both are
 * taken before the first edit, each with its record, and trusted by the
 * recorded SHA-256; revert needs no locator. apply is refused on a head
 * unit that refuses our parts (head_unit.js); revert works on any.
 *
 *   aapd_patch.js status [--target <aapd>] [--conf <aapd.conf>] [--coding <shm>]
 *   aapd_patch.js apply  [--dpi <n>] [--card shown|hidden] [--target ...] [--conf ...] [--coding ...] [--unproven] [--yes]
 *   aapd_patch.js revert [--target ...] [--conf ...] [--yes] [--force] [--drop-backup]
 *
 * Every write is an atomic rename; the running aapd keeps its old binary
 * until it next starts. Each file keeps its original's owner and mode, aapd
 * executable and aapd.conf readable by aapd.service's user (file_mode.js).
 * Node 8 on the unit, no dependencies. */
'use strict';

var fs = require('fs');
var crypto = require('crypto');
var path = require('path');
var cp = require('child_process');
var conf = require('./aapd_conf.js');
var locator = require('./aapd_locate.js');
var cpLocate = require('./carplay_locate.js'), buildId = cpLocate.buildId;
var headUnit = require('./head_unit.js');
var record = require('./patch_record.js');
var fileMode = require('./file_mode.js');

/* Builds proven as shipped, by GNU build-id: aapd 22.39.3 of MGU
 * P18_23w44.2-1-3. A first edit of one needs the whole file to be the
 * shipped one, and anything else counts as modified. A first edit of any
 * other build needs --unproven: its sites pass the locator's checks, but no
 * car has run it patched. */
var PROVEN = {
    '2dfb9881706bff435b05f621486e8eb8b6c55984': {
        version: 'aapd 22.39.3+gitrAUTOINC+d5a1db18c4-r0',
        sha256: '6880f54c3abba40c1368f2bf254c77bd4a169323c0d4cab8f9662a7c912dcf89',
        stock: [[0x998bd, '83bb1c02000001'], [0x1d43eb, 'e800f8ffff'], [0x1d3f5d, '0f848d000000'], [0x15b440, '660fefc0ebe1662e0f1f840000000000']],
    },
};

/* The locator's edits and their names on the status lines. */
var EDITS = [['gate', 'gate'], ['call', 'selfstart call'], ['guard', 'selfstart guard'], ['screen', 'screen']];
/* The most bytes aapd may differ in from its backup and still be its build:
 * the edits' bytes (gate 7, call 5, guard 6, screen at most 16), twice
 * over. */
var SAME_BUILD_MAX_DIFF = 2 * (7 + 5 + 6 + 16);

/* /dev/shm/rawcodingdata, the coding image CodingShmApp publishes: a 0x2c
 * byte header (flags at 0x28, 0x09 = valid coding) and the blocks. aapd's
 * and carplay's CodingKOMBI_HUD is the 150 bytes at block offset 0x75a;
 * KI_KOMBI_VARIANT is getBits(0, 0x7e, 1) of it. carplay_patch.js reads
 * it through readCoding too. */
var CODING = { size: 0x1030, flags: 0x28, valid: 0x09, at: 0x2c + 0x75a, mask: 0x7e, shift: 1, proven: 7 };

var MIN_FREE_MARGIN = 1 << 20;

var hashes = new WeakMap();
function sha256(buf) {
    if (!hashes.has(buf)) hashes.set(buf, crypto.createHash('sha256').update(buf).digest('hex'));
    return hashes.get(buf);
}
function die(msg) { process.stderr.write('error: ' + msg + '\n'); process.exit(1); }
function backupPath(t) { return t + '.woc-orig'; }
function metaPath(t) { return t + '.woc-orig.json'; }
function readOr(p) { try { return fs.readFileSync(p); } catch (e) { return null; } }

/* {variant} or {err}. */
function readCoding(p) {
    var b = readOr(p);
    if (!b) return { err: p + ' unreadable' };
    if (b.length !== CODING.size) return { err: p + ' is ' + b.length + ' bytes, not ' + CODING.size };
    if ((b[CODING.flags] & 0x0f) !== CODING.valid) return { err: 'coding not valid (flags 0x' + b[CODING.flags].toString(16) + ')' };
    return { variant: (b[CODING.at] & CODING.mask) >> CODING.shift };
}

/* Why a coding readCoding read ({variant} or {err}) is not the proven one,
 * or null. */
function codingRefusal(cd) {
    return cd.err ? 'the cluster coding cannot be read (' + cd.err + ')'
        : cd.variant !== CODING.proven ? 'KI_KOMBI_VARIANT ' + cd.variant + ' is not the proven cluster coding (' + CODING.proven + ')' : null;
}

/* The located binary: { loc, size, sha256, bad: [why an edit is not
 * found] }, once per content. A locator failure is a reason like any
 * other, so a revert never depends on it. */
var classified = {};
function classify(buf) {
    var h = sha256(buf);
    if (!classified[h]) {
        var loc, bad = [];
        try { loc = locator.locate(buf); } catch (e) {
            loc = { elf: false, buildId: null, version: null, reason: 'locator failed: ' + e.message };
            EDITS.forEach(function (x) { loc[x[0]] = { ok: false, reason: loc.reason }; });
        }
        if (!loc.elf) bad.push(loc.reason);
        else EDITS.forEach(function (e) { if (!loc[e[0]].ok && bad.indexOf(loc[e[0]].reason) < 0) bad.push(loc[e[0]].reason); });
        classified[h] = { loc: loc, size: buf.length, sha256: h, bad: bad };
    }
    return classified[h];
}

/* The byte ranges the edits write: [[offset, length]]. */
function regions(loc) { return EDITS.map(function (e) { return [loc[e[0]].offset, loc[e[0]].len]; }); }

/* Two classified images are the same build: the same size, build-id and
 * edit sites. */
function sameBuild(a, b) {
    return !a.bad.length && !b.bad.length && a.size === b.size && a.loc.buildId === b.loc.buildId &&
        JSON.stringify(regions(a.loc)) === JSON.stringify(regions(b.loc));
}

/* Why the image is not the pristine build, or null: an edit not found or
 * present, or for a proven build any byte not as shipped. */
function notPristine(c) {
    if (c.bad.length) return c.bad[0];
    var edited = EDITS.filter(function (e) { return c.loc[e[0]].state !== 'original'; });
    if (edited.length) return edited.map(function (e) { return e[1]; }).join(', ') + ' edited';
    var proven = PROVEN[c.loc.buildId];
    return proven && c.sha256 !== proven.sha256 ? 'sha256 ' + proven.sha256 + ', got ' + c.sha256 : null;
}

function label(c) {
    var proven = PROVEN[c.loc.buildId];
    return proven ? proven.version : c.loc.buildId ? 'build ' + c.loc.buildId : 'build without a build-id';
}

/* The backup with its record, trusted by the recorded SHA-256: {buf, meta}
 * or {err}; null when there is no backup. */
function trusted(file) {
    var buf = readOr(backupPath(file));
    if (!buf) return null;
    var meta = null;
    try { meta = JSON.parse(fs.readFileSync(metaPath(file), 'utf8')); } catch (e) {}
    if (!meta || !meta.sha256_original) return { err: 'backup ' + backupPath(file) + ' has no record' };
    if (sha256(buf) !== meta.sha256_original) return { err: 'backup sha256 ' + sha256(buf) + ' != recorded ' + meta.sha256_original };
    return { buf: buf, meta: meta };
}

function binOther(cur, orig) {
    if (cur.length !== orig.length) return 'its size differs';
    var a = buildId(cur), b = buildId(orig);
    if (a && b) return a === b ? null : 'its build-id differs';
    for (var i = 0, n = 0; i < cur.length; i++) if (cur[i] !== orig[i] && ++n > SAME_BUILD_MAX_DIFF) return 'more than ' + SAME_BUILD_MAX_DIFF + ' bytes differ';
    return null;
}

/* What a revert writes over `cur` from its backup `b` (trusted()): {out,
 * other} or {err}; null without a backup. The binary: when the locator
 * reads it and its pristine backup as the same build, only the edits' bytes
 * (another tool's changes stay), else the whole backup. */
function restorePlan(b, cur, isBin) {
    if (!b || b.err) return b;
    if (!cur) return { out: b.buf, other: 'it is missing' };
    var other = isBin ? binOther(cur, b.buf) : conf.otherFile(cur.toString('utf8'), b.buf.toString('utf8'));
    var out = b.buf;
    if (isBin && !other) {
        var cc = classify(cur), bc = classify(b.buf);
        if (!notPristine(bc) && sameBuild(cc, bc)) {
            out = Buffer.from(cur);
            regions(cc.loc).forEach(function (r) { b.buf.copy(out, r[0], r[0], r[0] + r[1]); });
        }
    }
    return { out: out, other: other };
}

function oursIn(c) { return EDITS.some(function (e) { return c.loc[e[0]].ok && c.loc[e[0]].state === 'patched'; }); }

/* Each file's undo as it stands, aapd then aapd.conf: { file, cur, b (its
 * backup), p (the restore plan), floor (the bits aapd.service's user needs,
 * file_mode.js), modeBad (the file is exactly its trusted backup of ours
 * but lacks those bits; without a backup of ours its mode is left) },
 * each file read once; `bin`, when given, is aapd as already read. Without
 * a backup to trust, a proven aapd holding an edit of ours, or the proven
 * aapd.conf holding a geometry of ours, is rebuilt from its own content
 * (the shipped file is its own original); anything else of it is left as
 * it is. */
function undos(t, bin) {
    return [[t.target, true], [t.conf, false]].map(function (f) {
        var cur = f[1] && bin ? bin : readOr(f[0]), b = trusted(f[0]), p = restorePlan(b, cur, f[1]);
        if ((!p || p.err) && cur) {
            var so = f[1] ? cpLocate.stockOriginal(PROVEN, cur) : conf.stockOriginal(cur.toString('utf8'));
            if (so && f[1] && !so.equals(cur) && !oursIn(classify(cur))) so = null;
            if (so) p = { out: f[1] ? so : Buffer.from(so, 'utf8'), other: null, stock: true };
        }
        var floor = f[1] ? fileMode.EXEC : fileMode.READ, orig = p && !p.err && !p.other && !p.stock ? b.buf : null;
        return { file: f[0], cur: cur, b: b, p: p, floor: floor, modeBad: !!cur && !!orig && cur.equals(orig) && fileMode.lacks(f[0], floor) };
    });
}

/* Why the revert would not change anything, or null when it would. */
function notRestorable(us) {
    var bad = [], any = false;
    us.forEach(function (u) {
        if (!u.p) return;
        if (u.p.err) bad.push(u.p.err);
        else if (u.p.other) bad.push(path.basename(u.file) + ' is another build than its backup: ' + u.p.other);
        else if (!u.cur || !u.p.out.equals(u.cur) || u.modeBad) any = true;
    });
    return any ? null : bad.length ? bad.join('; ') : 'nothing of ours to undo';
}

/* What undoing one file would find: none (no backup of ours, not a proven
 * file), untrusted (a backup whose record does not vouch for it), other
 * (another build than the backup), original (nothing to restore) or
 * restorable (its bytes, or the original's mode). */
function undoState(u) {
    return !u.p ? 'none' : u.p.err ? 'untrusted' : u.p.other ? 'other' : u.cur && u.p.out.equals(u.cur) && !u.modeBad ? 'original' : 'restorable';
}
function undoLine(us) { return 'aapd ' + undoState(us[0]) + ', aapd.conf ' + undoState(us[1]); }

/* The backup and its record, which keeps the original's SHA-256, mode and
 * owner. */
function saveBackup(file, buf, meta) {
    var bak = backupPath(file), bakTmp = bak + '.tmp', metaTmp = metaPath(file) + '.tmp', st = fs.statSync(file);
    [bakTmp, metaTmp].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    fs.writeFileSync(bakTmp, buf, { mode: 0o600 });
    fsyncFile(bakTmp);
    if (!fs.readFileSync(bakTmp).equals(buf)) { try { fs.unlinkSync(bakTmp); } catch (e) {} die('backup verification failed; no change made'); }
    fs.writeFileSync(metaTmp, JSON.stringify(Object.assign({ target: path.resolve(file), sha256_original: sha256(buf),
        mode: st.mode & 0o7777, uid: st.uid, gid: st.gid, savedAt: new Date().toISOString() }, meta), null, 2) + '\n', { mode: 0o600 });
    fsyncFile(metaTmp);
    fs.renameSync(metaTmp, metaPath(file));
    fs.renameSync(bakTmp, bak);
}

function fsyncFile(p) {
    var fd = fs.openSync(p, 'r+');
    try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
}

/* file's mode and owner (file_mode.js): its original's, recorded with the
 * backup, with at least floor's bits. */
function wantedMode(file, floor) {
    var meta = null;
    try { meta = JSON.parse(fs.readFileSync(metaPath(file), 'utf8')); } catch (e) {}
    return fileMode.wanted(file, meta, floor);
}

function writeInPlace(target, buf, floor) {
    try { fileMode.replace(target, buf, wantedMode(target, floor)); } catch (e) { die('writing ' + target + ': ' + e.message); }
    if (!fs.readFileSync(target).equals(buf)) die('post-write verification failed: ' + target + '; restore from ' + backupPath(target));
}

/* Each of files ([file, floor]) that aapd.service's user cannot use given
 * its original's mode and owner, with --yes; files holding bytes this tool
 * did not write are the caller's to leave out. */
function repairModes(files, yes) {
    files.forEach(function (f) {
        if (!fileMode.lacks(f[0], f[1])) return;
        var want = wantedMode(f[0], f[1]), fix = fileMode.mismatch(f[0], want);
        if (!yes) die(f[0] + ' has ' + fix + '; pass --yes to set them');
        try { fileMode.repair(f[0], want); } catch (e) { die('setting ' + f[0] + '\'s mode and owner: ' + e.message); }
        process.stdout.write('set ' + f[0] + '\'s mode and owner (it had ' + fix + ')\n');
    });
}

/* Free bytes on the file system holding dir, or null. */
function freeBytes(dir) {
    try {
        var out = cp.execFileSync(process.env.WOC_DF || 'df', ['-Pk', dir], { encoding: 'utf8', timeout: 10000 });
        var f = out.trim().split('\n').pop().split(/\s+/);
        var kb = parseInt(f[3], 10);
        return isNaN(kb) ? null : kb * 1024;
    } catch (e) { return null; }
}

/* Why aapd.conf without a backup of ours cannot be taken as its original,
 * or null: it lacks what the edit needs, or it already holds a geometry of
 * ours. */
function confNotOriginal(text) {
    return conf.check(text) || (conf.edited(text, text) ? 'it holds our geometry without our backup' : null);
}

function geometryText(cur, b) {
    if (!cur) return 'MISSING';
    var text = cur.toString('utf8');
    if (!b || b.err) { var why = confNotOriginal(text); return why ? 'UNKNOWN (' + why + ')' : 'ORIGINAL'; }
    if (cur.equals(b.buf)) return 'ORIGINAL';
    var g = conf.edited(b.buf.toString('utf8'), text);
    return g ? 'PATCHED (' + g.dpi + ' dpi, card ' + g.card + ', crop ' + conf.srcCrop(g.card, g.dpi) + ')' : 'UNKNOWN (not our edit of the backup)';
}

function line(k, v) { process.stdout.write((k + ':                ').slice(0, 17) + v + '\n'); }

function cmdStatus(t) {
    var buf = readOr(t.target);
    if (!buf) die('cannot read ' + t.target);
    var c = classify(buf), cd = readCoding(t.coding), us = undos(t, buf), bak = us[0].b;
    line('target', t.target);
    line('size', buf.length);
    line('sha256', c.sha256);
    line('build-id', c.loc.buildId || 'none');
    line('version', c.loc.version || 'unknown');
    line('conf', t.conf);
    line('coding', cd.err ? 'unreadable (' + cd.err + ')' : 'KI_KOMBI_VARIANT ' + cd.variant);
    var why = notRestorable(us);
    line('restorable', why ? 'no (' + why + ')' : 'yes');
    line('undo', undoLine(us));
    line('known', c.bad.length ? 'NO — ' + c.bad[0] : notPristine(c) ? 'yes, modified build' : 'yes, pristine ' + label(c));
    line('proven', PROVEN[c.loc.buildId] ? 'yes' : 'no');
    EDITS.forEach(function (e) {
        var s = c.loc[e[0]];
        line(e[1].replace(' ', '-'), s.ok ? s.state.toUpperCase() : 'UNKNOWN (' + s.reason + ')');
    });
    line('geometry', geometryText(us[1].cur, us[1].b));
    line('backup', (bak ? backupPath(t.target) + (bak.err ? ' (' + bak.err + ')' : '') : 'none') +
        (fs.existsSync(backupPath(t.conf)) ? ', ' + backupPath(t.conf) : ''));
}

function cmdApply(t, dpi, card, yes, unproven) {
    var hu = headUnit.refused();
    if (hu) die('refusing: ' + hu);
    if (conf.DENSITIES.indexOf(dpi) < 0) die('--dpi must be ' + conf.DENSITY_MIN + '-' + conf.DENSITY_MAX + ' in steps of ' + conf.DENSITY_STEP);
    if (conf.CARDS.indexOf(card) < 0) die('--card must be ' + conf.CARDS.join(' or '));
    var codingWhy = codingRefusal(readCoding(t.coding));
    if (codingWhy) die('refusing: ' + codingWhy);

    var buf = readOr(t.target);
    if (!buf) die('cannot read ' + t.target);
    var c = classify(buf);
    if (c.bad.length) die('refusing: ' + c.bad.join('; '));
    var bb = trusted(t.target);
    if (bb && bb.err) die('refusing: ' + bb.err);
    if (bb && (notPristine(classify(bb.buf)) || binOther(buf, bb.buf) || !sameBuild(c, classify(bb.buf))))
        die('refusing: backup ' + backupPath(t.target) + ' is not the original of this build');
    if (!bb) {
        var why = notPristine(c);
        if (why) die('first edit requires the pristine build: ' + why);
        if (!PROVEN[c.loc.buildId] && !unproven) die(label(c) + ' (aapd ' + (c.loc.version || 'version unknown') + ') is not a proven build; pass --unproven to patch it anyway');
    }

    var text = readOr(t.conf);
    if (!text) die('cannot read ' + t.conf);
    var cb = trusted(t.conf), origText = cb ? cb.buf : text;
    if (cb && cb.err) die('refusing: ' + cb.err);
    var confWhy = cb ? conf.check(origText.toString('utf8')) : confNotOriginal(origText.toString('utf8'));
    if (confWhy) die('refusing: ' + (cb ? 'backup ' + backupPath(t.conf) : t.conf) + ': ' + confWhy);
    if (cb && !text.equals(cb.buf) && !conf.edited(cb.buf.toString('utf8'), text.toString('utf8'))) die('refusing: ' + t.conf + ' was modified by something else');

    var outBin = Buffer.from(buf);
    EDITS.forEach(function (e) { Buffer.from(c.loc[e[0]].patched, 'hex').copy(outBin, c.loc[e[0]].offset); });
    var oc = classify(outBin);
    if (!sameBuild(c, oc) || EDITS.some(function (e) { return oc.loc[e[0]].state !== 'patched'; }))
        die('refusing: the edited aapd does not verify; no change made');
    var outConf = Buffer.from(conf.transform(origText.toString('utf8'), dpi, card), 'utf8');
    var binChange = !outBin.equals(buf), confChange = !outConf.equals(text);
    var rec = { patched: true, proven: !!PROVEN[c.loc.buildId] };
    if (!binChange && !confChange) {
        repairModes([[t.target, fileMode.EXEC], [t.conf, fileMode.READ]], yes);
        process.stdout.write('nothing to do; already in the requested state\n'); record.ensure(t.target, rec); return;
    }

    var need = (binChange ? (bb ? 0 : buf.length) + buf.length : 0) + (confChange ? (cb ? 0 : text.length) + outConf.length : 0) + MIN_FREE_MARGIN;
    var free = freeBytes(path.dirname(t.target));
    if (free === null) die('refusing: free space on ' + path.dirname(t.target) + ' unknown');
    if (free < need) die('refusing: ' + free + ' bytes free on ' + path.dirname(t.target) + ', ' + need + ' needed');

    process.stdout.write('About to patch ' + t.target + ' and ' + t.conf + ' (' + dpi + ' dpi, card ' + card + ')\n');
    if (!yes) die('pass --yes to proceed');
    if (binChange && !bb) saveBackup(t.target, buf, { version: label(c), buildId: c.loc.buildId });
    if (confChange && !cb) saveBackup(t.conf, text, {});
    if (binChange) writeInPlace(t.target, outBin, fileMode.EXEC);
    if (confChange) writeInPlace(t.conf, outConf, fileMode.READ);
    repairModes([[t.target, fileMode.EXEC], [t.conf, fileMode.READ]], yes);
    record.ensure(t.target, rec);
    process.stdout.write('patched OK. aapd sha256 ' + sha256(outBin) + ', aapd.conf ' + dpi + ' dpi, card ' + card + '\n');
}

/* Each file is restored on its own: one whose backup is not trusted, or
 * that is another build than its backup (a firmware update replaced it;
 * --force overwrites that one), is left as it is while the other is still
 * restored. */
function cmdRevert(t, yes, force, dropBackup) {
    var plans = undos(t), wrote = false, repaired = false, left = [];
    if (!plans.some(function (x) { return x.p; })) die('no backup at ' + backupPath(t.target) + ' or ' + backupPath(t.conf) + ', and neither file is a proven one to rebuild; cannot revert');
    plans.forEach(function (x) {
        if (x.p && x.p.err) { left.push(x.p.err + '; ' + x.file + ' left as it is'); return; }
        if (!x.p || (x.cur && x.p.out.equals(x.cur) && !x.modeBad)) return;
        if (x.p.other && !force) { left.push(x.file + ' is another build than its backup (' + x.p.other + '); left as it is (--force overwrites it)'); return; }
        if (!yes) die('pass --yes to proceed');
        /* At its original already: only the mode and owner. */
        if (x.cur && x.p.out.equals(x.cur)) { repairModes([[x.file, x.floor]], yes); repaired = true; return; }
        writeInPlace(x.file, x.p.out, x.floor);
        wrote = true;
        process.stdout.write('restored ' + x.file + (x.p.stock ? ' from the proven file\'s shipped content' : '') + '\n');
    });
    if (!wrote && !repaired && left.length) die(left.join('; '));
    left.forEach(function (l) { process.stdout.write(l + '\n'); });
    if (!wrote) process.stdout.write('already at the original; nothing to do\n');
    var after = undos(t);
    if (/^(original|other)$/.test(undoState(after[0]))) record.ensure(t.target, { patched: false });
    /* A file left restorable or with an untrusted backup keeps its backup,
     * and the revert fails: it is not undone. */
    var kept = after.map(function (x) { var u = undoState(x); return u === 'restorable' || u === 'untrusted'; });
    if (dropBackup) {
        after.forEach(function (x, i) {
            if (kept[i]) return;
            [backupPath(x.file), metaPath(x.file), backupPath(x.file) + '.tmp', metaPath(x.file) + '.tmp', x.file + '.woc-tmp', record.recordPath(x.file)].forEach(function (f) {
                try { fs.unlinkSync(f); } catch (e) { if (e.code !== 'ENOENT') die('dropping ' + f + ': ' + e.message); }
            });
        });
        process.stdout.write('backup dropped\n');
    }
    if (kept.some(Boolean)) die('not undone: ' + undoLine(after));
}

function main() {
    var argv = process.argv.slice(2), a = { _: [] };
    for (var i = 0; i < argv.length; i++) {
        var k = argv[i];
        if (k === '--yes' || k === '--force' || k === '--drop-backup' || k === '--unproven') a[k.slice(2)] = true;
        else if (k === '--target' || k === '--conf' || k === '--coding' || k === '--dpi' || k === '--card') a[k.slice(2)] = argv[++i];
        else a._.push(k);
    }
    var t = { target: a.target || '/usr/bin/aapd', conf: a.conf || '/etc/aapd.conf', coding: a.coding || '/dev/shm/rawcodingdata' };
    if (a._[0] === 'status') { cmdStatus(t); line('end', 'complete'); }
    else if (a._[0] === 'apply') cmdApply(t, a.dpi === undefined ? conf.DEFAULT_DPI : Number(a.dpi), a.card || conf.DEFAULT_CARD, a.yes, a.unproven);
    else if (a._[0] === 'revert') cmdRevert(t, a.yes, a.force, a['drop-backup']);
    else { process.stderr.write('usage: aapd_patch.js status|apply|revert [--target <aapd>] [--conf <aapd.conf>] [--coding <shm>] [--dpi <n>] [--card shown|hidden] [--unproven] [--yes] [--force] [--drop-backup]\n'); process.exit(2); }
}

module.exports = { CODING: CODING, PROVEN: PROVEN, trusted: trusted, binOther: binOther, readCoding: readCoding, codingRefusal: codingRefusal };

if (require.main === module) main();
