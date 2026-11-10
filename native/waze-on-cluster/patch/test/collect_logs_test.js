#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* collect-logs.sh's wifi.txt, with stand-in /proc/net/dev, patchd's band
 * file, journalctl and dmesg: the band as patchd wrote it, each P2P
 * interface's counters and no other interface's, and the kernel's 802.1X
 * and DHCP lines for the P2P interfaces from the journal, from dmesg when
 * the journal has none, the last 60; "(none)" for what is missing. */
'use strict';
var assert = require('assert');
var cp = require('child_process');
var fs = require('fs');
var os = require('os');
var path = require('path');

var SCRIPT = path.join(__dirname, '..', '..', 'tools', 'collect-logs.sh');
var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-collect-'));
var bin = path.join(dir, 'bin');
fs.mkdirSync(bin);
var DEV = path.join(dir, 'net-dev'), BAND = path.join(dir, 'wifi-band.json');
var KERNEL = path.join(dir, 'journal-kernel'), DMESG = path.join(dir, 'dmesg-out');

/* journalctl -k prints KERNEL; other journalctl calls print nothing */
fs.writeFileSync(path.join(bin, 'journalctl'), '#!/bin/sh\ncase " $* " in *" -k "*) cat ' + KERNEL + ' 2>/dev/null ;; esac\nexit 0\n', { mode: 0o755 });
fs.writeFileSync(path.join(bin, 'dmesg'), '#!/bin/sh\ncat ' + DMESG + ' 2>/dev/null\n', { mode: 0o755 });

var ENV = Object.assign({}, process.env, { PATH: bin + ':' + process.env.PATH, WOC_PROC_NET_DEV: DEV, WOC_WIFI_FILE: BAND });

function collect() {
    var out = path.join(dir, 'out-' + Date.now() + Math.random().toString(16).slice(2));
    fs.mkdirSync(out);
    var archive = cp.execFileSync('sh', [SCRIPT, '-o', out], { env: ENV, encoding: 'utf8' }).trim();
    cp.execFileSync('tar', ['-xzf', archive, '-C', out]);
    var name = path.basename(archive).replace(/\.tar\.gz$/, '');
    return fs.readFileSync(path.join(out, name, 'wifi.txt'), 'utf8');
}

var lines = [];
for (var i = 0; i < 70; i++) lines.push('Nov 05 06:53:' + String(i % 60).padStart(2, '0') + '.000000 mgu kernel: DHCP[p2p-wlan0-1] - REQUEST [TX] ' + i);
fs.writeFileSync(KERNEL, '[   11.575296] wlan_bridge: port 2(p2p-wlan0-0) entered forwarding state\n' +
    'Nov 05 06:53:00.000000 mgu kernel: ETHER_TYPE_802_1X[p2p-wlan0-1] [TX]: ver 2, type 3, replay 1\n' +
    'Nov 05 06:53:00.100000 mgu kernel: DHCP[wlan0] - DISCOVER [TX]\n' + lines.join('\n') + '\n');
fs.writeFileSync(DEV, 'Inter-|   Receive\n face |bytes packets\n    lo: 1 1 0 0 0 0 0 0 1 1 0 0 0 0 0 0\n' +
    '  wlan0: 5 1 0 0 0 0 0 0 6 1 0 0 0 0 0 0\n  p2p-wlan0-0: 100 1 0 0 0 0 0 0 200 1 0 0 0 0 0 0\n' +
    '  p2p-wlan0-1: 9000000 1 0 0 0 0 0 0 400000 1 0 0 0 0 0 0\n');
fs.writeFileSync(BAND, '{"band":"2.4","iface":"p2p-wlan0-1","freq":2462}\n');

var w = collect();
var sections = w.split(/^-- /m).slice(1);
assert.strictEqual(sections.length, 3, w);
assert(/^CarPlay Wi-Fi band \(patchd, .*wifi-band\.json\)\n\{"band":"2\.4","iface":"p2p-wlan0-1","freq":2462\}\n$/.test(sections[0]), sections[0]);
assert.deepStrictEqual(sections[1].split('\n').slice(1, -1).map(function (l) { return l.trim().split(':')[0]; }), ['p2p-wlan0-0', 'p2p-wlan0-1'],
    'the P2P interfaces only');
var k = sections[2].split('\n').slice(1, -1);
assert.strictEqual(k.length, 60, 'the last 60');
assert(/REQUEST \[TX\] 69$/.test(k[59]) && /REQUEST \[TX\] 10$/.test(k[0]), k[0]);
assert(!/DHCP\[wlan0\]|entered forwarding/.test(sections[2]), 'only the P2P interfaces\' 802.1X and DHCP lines');
console.log('ok   wifi.txt: patchd\'s band, the P2P interfaces\' counters, their last 60 802.1X and DHCP lines from the journal');

fs.writeFileSync(KERNEL, '');
fs.writeFileSync(DMESG, '[   29.966111] ETHER_TYPE_802_1X[p2p-wfd0-1] [TX]: ver 2, type 3, replay 1\n[   30.111144] DHCP[p2p-wfd0-1] - REQUEST [TX]\n');
w = collect();
assert(/802\.1X and DHCP lines.*\n\[   29\.966111\] ETHER_TYPE_802_1X\[p2p-wfd0-1\].*\n\[   30\.111144\] DHCP\[p2p-wfd0-1\] - REQUEST \[TX\]\n$/.test(w), w);
console.log('ok   none in the journal: dmesg\'s, NXP\'s p2p-wfd0-N');

fs.unlinkSync(BAND);
fs.unlinkSync(DMESG);
fs.writeFileSync(DEV, 'Inter-|   Receive\n face |bytes packets\n  eth0: 1 1 0 0 0 0 0 0 1 1 0 0 0 0 0 0\n');
w = collect();
assert.strictEqual(w.replace(/\(patchd, [^)]*\)/, '(patchd)').replace(/in \S+\n/, 'in DEV\n'),
    '-- CarPlay Wi-Fi band (patchd)\n(none)\n-- P2P interfaces in DEV\n(none)\n-- 802.1X and DHCP lines for P2P interfaces (the last 60)\n(none)\n');
console.log('ok   no band file, no P2P interface, no kernel line: each "(none)"');

/* The evidence logs_extra.js adds, from a stand-in unit (WOC_LOGS_ROOT) whose
 * files hold fake secrets: a GitHub token, a Wi-Fi passphrase, a private
 * key, a VIN and its short form, and a position. collect-logs.sh runs from
 * a copy of the installed folder (the script beside the patch tools). Not
 * root: the root-only part comes from the firewall unit's snapshot; as root
 * it takes it itself. Either way the archive holds the patch records, the
 * aapd recovery's state, the coding's variant, the aapd status, the deploy
 * tool's versions and state, the restart records, recmgr's crash table and
 * the list of kept core dumps, and none of the secrets. */
var SECRETS = ['ghp_FAKEtoken0123456789abcdefghijklmnop', 'hunter2-wifi-passphrase', 'BEGIN PRIVATE KEY', 'WBA8E9C50GK123456',
    'GK12345', '48.137154', '11.576124', 'CORE-DUMP-BYTES'];
var VINS = ['WBA8E9C50GK123456', 'GK12345'];
var U = path.join(dir, 'unit'), CUR = path.join(dir, 'current'), SNAP = path.join(dir, 'snapshot');
function mk(rel, text) {
    var f = path.join(U, rel);
    fs.mkdirSync(path.dirname(f), { recursive: true });
    fs.writeFileSync(f, text);
    return f;
}
fs.mkdirSync(CUR);
fs.copyFileSync(SCRIPT, path.join(CUR, 'collect-logs.sh'));
fs.readdirSync(path.join(__dirname, '..')).filter(function (n) { return /\.js$/.test(n); }).forEach(function (n) {
    fs.copyFileSync(path.join(__dirname, '..', n), path.join(CUR, n));
});
mk('usr/bin/carplay.woc-patch.json', '{"patched":true,"proven":true,"trial":false,"ino":5,"size":3484912,"mtime":1794170822000}\n');
mk('usr/bin/aapd.woc-patch.json', '{"patched":false,"proven":true,"ino":6,"size":7367952,"mtime":1794170823000}\n');
mk('var/opt/waze-on-cluster/aa-boot-recovery.json', '{"fails":1,"unknown":0,"arm":null,"prev":"b1","starts":null,"ended":null}\n');
/* the coding image: KI_KOMBI_VARIANT 7, a VIN elsewhere in it */
var coding = Buffer.alloc(0x1030);
coding[0x28] = 0x09;
coding[0x2c + 0x75a] = 7 << 1;
coding.write('WBA8E9C50GK123456', 0x100);
mk('dev/shm/rawcodingdata', coding);
/* the deploy tool: its version, the tools, state.json with what must stay */
fs.mkdirSync(path.join(U, 'var/opt/mgu-deploy/versions/0.8.9'), { recursive: true });
fs.symlinkSync('versions/0.8.9', path.join(U, 'var/opt/mgu-deploy/current'));
['waze-on-cluster@0.9.7', 'cluster-render@0.4.3'].forEach(function (t) {
    var id = t.split('@')[0], v = t.split('@')[1];
    mk('var/opt/' + id + '/versions/' + v + '/component.json', JSON.stringify({ id: id, kind: 'tool', version: v, token: 'ghp_FAKEtoken0123456789abcdefghijklmnop' }));
    fs.symlinkSync('versions/' + v, path.join(U, 'var/opt', id, 'current'));
});
mk('var/opt/mgu-deploy/state.json', JSON.stringify({
    tools: { 'waze-on-cluster': { version: '0.9.7', firmware: 'S18A-23-07-550', token: 'ghp_FAKEtoken0123456789abcdefghijklmnop' } },
    apps: { robertn_store: { uid: 12001, version: '1.4.0', passphrase: 'hunter2-wifi-passphrase' } },
    pending_reboot: [{ id: 'waze-on-cluster', reason: 'new units', boot: 'boot-now' }, { id: 'cluster-render', reason: 'done', boot: 'boot-old' }],
    lastBounceAt: 1794170000,
    vin: 'WBA8E9C50GK123456', position: { lat: 48.137154, lon: 11.576124 }, data_stash: { a: [{ path: '/var/opt/x' }] } }));
mk('var/opt/mgu-deploy/config.json', '{"token":"ghp_FAKEtoken0123456789abcdefghijklmnop"}\n');
mk('var/opt/mgu-updater/credentials/token-0a1b2c3d', 'ghp_FAKEtoken0123456789abcdefghijklmnop\n');
mk('var/opt/mgu-updater/sources.json', '{"wifi":{"psk":"hunter2-wifi-passphrase"},"key":"-----BEGIN PRIVATE KEY-----"}\n');
/* BMW's records: restart records, recmgr's database, kept core dumps */
mk('var/opt/public/sin/recmgr/restartInfo.xml', '<RestartInfo><name>carplay</name><crashID>WBA8E9C50GK123456-carplay</crashID>' +
    '<vin7>GK12345</vin7><resets>3</resets></RestartInfo>\n');
mk('var/opt/public/sin/crashhandler/restartInfo.xml', '<RestartInfo>' + 'x'.repeat(200000) + '</RestartInfo>\n');
var DB = mk('var/opt/public/sin/recmgr/recmgr_stats.db', 'SQLite format 3\0');
fs.utimesSync(DB, 1794298365, 1794298365);
/* restart records the boot step kept */
mk('var/opt/waze-on-cluster/restart-records/1-boot-old-recmgr.xml', '<RestartInfo><crashID>WBA8E9C50GK123456</crashID></RestartInfo>\n');
mk('var/opt/waze-on-cluster/restart-records/1-boot-old-crashhandler.xml', '<RestartInfo><name>carplay</name></RestartInfo>\n');
mk('boot_id', 'boot-now\n');
mk('var/opt/public/sin/crashhandler/coredumps/20261108204714_000_carplay/core.gz', 'CORE-DUMP-BYTES');
mk('var/opt/public/sin/crashhandler/coredumps/20261108204714_000_carplay/context_WBA8E9C50GK123456.txt', 'CORE-DUMP-BYTES');
mk('var/opt/public/sin/dlt-cdh/core.1794170822.carplay.4222.gz', 'CORE-DUMP-BYTES');
for (i = 0; i < 130; i++) mk('var/opt/kerneldump/dump-' + i, 'CORE-DUMP-BYTES');
/* sqlite3: records its arguments; rows as the firmware's would print them */
var SQL_ARGS = path.join(dir, 'sqlite-args');
fs.writeFileSync(path.join(bin, 'sqlite3'), '#!/bin/sh\nprintf "%s\\n" "$@" > ' + SQL_ARGS + '\n' +
    'printf "CrashKey|Timestamp|Name|CrashID|LC\\n2|2026-11-05 06:53:00|carplay|WBA8E9C50GK123456-7|41\\n1|2026-11-05 06:51:00|carplay|c1|40\\n"\n', { mode: 0o755 });

var XENV = Object.assign({}, ENV, { WOC_LOGS_ROOT: U, WOC_LOGS_SNAPSHOT: SNAP, WOC_BOOT_ID: path.join(U, 'boot_id') });
var asRoot = process.getuid && process.getuid() === 0;
if (!asRoot) {
    fs.mkdirSync(SNAP);
    cp.execFileSync(process.execPath, [path.join(CUR, 'logs_extra.js'), 'snapshot', SNAP], { env: XENV });
}
var out = path.join(dir, 'out-extra');
fs.mkdirSync(out);
var archive = cp.execFileSync('sh', [path.join(CUR, 'collect-logs.sh'), '-o', out], { env: XENV, encoding: 'utf8' }).trim();
cp.execFileSync('tar', ['-xzf', archive, '-C', out]);
var A = path.join(out, path.basename(archive).replace(/\.tar\.gz$/, ''));
function get(n) { return fs.readFileSync(path.join(A, n), 'utf8'); }

assert.strictEqual(get('patch-record-carplay.json'), fs.readFileSync(path.join(U, 'usr/bin/carplay.woc-patch.json'), 'utf8'));
assert.strictEqual(get('patch-record-aapd.json'), fs.readFileSync(path.join(U, 'usr/bin/aapd.woc-patch.json'), 'utf8'));
assert.strictEqual(get('aa-boot-recovery.json'), fs.readFileSync(path.join(U, 'var/opt/waze-on-cluster/aa-boot-recovery.json'), 'utf8'));
assert.strictEqual(get('coding.txt'), 'KI_KOMBI_VARIANT 7\n');
assert(/^target: +\/usr\/bin\/aapd$|^error: /m.test(get('aapd-status.txt')), get('aapd-status.txt'));
assert.strictEqual(get('deploy.txt'), 'mgu-deploy 0.8.9\ntools installed (/var/opt/<id>/current):\n  tool cluster-render 0.4.3\n' +
    '  tool waze-on-cluster 0.9.7\nstate.json:\n  tool waze-on-cluster 0.9.7 firmware S18A-23-07-550\n  app robertn_store 1.4.0\n' +
    '  pending restart: waze-on-cluster - new units\n  last bounce: 2026-11-08T20:33:20Z\n');
assert.strictEqual(get('restartInfo-recmgr.xml'), '<RestartInfo><name>carplay</name><crashID><VIN>-carplay</crashID>' +
    '<vin7><VIN></vin7><resets>3</resets></RestartInfo>\n');
var big = fs.statSync(path.join(A, 'restartInfo-crashhandler.xml')).size;
assert(big <= 64 * 1024, 'capped: ' + big);
assert.strictEqual(get('restartInfo-crashhandler.xml').match(/\(cut at/g).length, 1, 'one note for the cut');
assert(/^<RestartInfo>x+\n\.\.\. \(cut at 65472 bytes\)\n$/.test(get('restartInfo-crashhandler.xml')));
assert.strictEqual(get('restart-record-1-boot-old-recmgr.xml'), '<RestartInfo><crashID><VIN></crashID></RestartInfo>\n');
assert.strictEqual(get('restart-record-1-boot-old-crashhandler.xml'), '<RestartInfo><name>carplay</name></RestartInfo>\n');
assert.strictEqual(get('recmgr-crashes.txt'), '-- /var/opt/public/sin/recmgr/recmgr_stats.db, modified 2026-11-10T08:12:45Z\n' +
    '-- Crash, newest first (at most 200)\n' +
    'CrashKey|Timestamp|Name|CrashID|LC\n2|2026-11-05 06:53:00|carplay|<VIN>-7|41\n1|2026-11-05 06:51:00|carplay|c1|40\n');
var sqlArgs = fs.readFileSync(SQL_ARGS, 'utf8').split('\n');
assert.strictEqual(sqlArgs[1], 'file:' + path.join(U, 'var/opt/public/sin/recmgr/recmgr_stats.db') + '?mode=ro', 'opened read-only');
assert(/^SELECT /.test(sqlArgs[2]) && !/UserID/.test(sqlArgs[2]) && /LIMIT 200;$/.test(sqlArgs[2]), sqlArgs[2]);
var dumps = get('core-dumps.txt').split('\n');
assert(dumps.indexOf('-- /var/opt/public/sin/crashhandler/coredumps') >= 0 && dumps.indexOf('-- /var/opt/public/sin/dlt-cdh') >= 0);
assert(dumps.some(function (l) { return /Z  20261108204714_000_carplay\/context_<VIN>\.txt$/.test(l); }), 'two levels, the VIN in a name hidden');
assert(dumps.some(function (l) { return /Z  core\.1794170822\.carplay\.4222\.gz$/.test(l); }));
assert(dumps.indexOf('-- /var/opt/kerneldump (the newest 100 of 130)') >= 0, 'at most 100 a folder');
assert.strictEqual(dumps.filter(function (l) { return / {2}dump-\d+$/.test(l); }).length, 100);
/* a VIN across the cap is scrubbed whole before the cut */
var edge = path.join(dir, 'edge');
fs.mkdirSync(edge);
mk('var/opt/public/sin/crashhandler/restartInfo.xml', 'y'.repeat(65472 - 8) + ' WBA8E9C50GK123456 ' + 'y'.repeat(300));
cp.execFileSync(process.execPath, [path.join(CUR, 'logs_extra.js'), 'snapshot', edge], { env: XENV });
var cut = fs.readFileSync(path.join(edge, 'restartInfo-crashhandler.xml'), 'utf8');
assert(cut.indexOf('WBA8E9') < 0 && / <VIN/.test(cut) && /\(cut at 65472 bytes\)\n$/.test(cut), cut.slice(-120));
/* a source past what is read that scrubbing shrinks under the cap still says it was cut */
mk('var/opt/public/sin/crashhandler/restartInfo.xml', new Array(5000).join('WBA8E9C50GK123456 '));
var edge2 = path.join(dir, 'edge2');
fs.mkdirSync(edge2);
cp.execFileSync(process.execPath, [path.join(CUR, 'logs_extra.js'), 'snapshot', edge2], { env: XENV });
var shrunk = fs.readFileSync(path.join(edge2, 'restartInfo-crashhandler.xml'), 'utf8');
assert(shrunk.length < 30000 && shrunk.indexOf('WBA') < 0 && /\n\.\.\. \(cut at \d+ bytes\)\n$/.test(shrunk), shrunk.slice(-80));
var everything = fs.readdirSync(A).map(get).join('\n');
SECRETS.forEach(function (x) { assert(everything.indexOf(x) < 0, 'the archive holds ' + x); });
console.log('ok   the archive adds the patch records, aapd recovery, coding, aapd status, deploy state, restart records, ' +
    'recmgr crashes and core-dump names (' + (asRoot ? 'as root' : 'from the snapshot') + '), capped, no secret or VIN');

/* sqlite3 that hangs is killed at 2 s; a missing database or folder is left out */
fs.writeFileSync(path.join(bin, 'sqlite3'), '#!/bin/sh\nexec sleep 30\n', { mode: 0o755 });
var snap2 = path.join(dir, 'snap2');
fs.mkdirSync(snap2);
var t0 = Date.now();
cp.execFileSync(process.execPath, [path.join(CUR, 'logs_extra.js'), 'snapshot', snap2], { env: XENV });
assert(Date.now() - t0 < 6000, 'bounded: ' + (Date.now() - t0) + ' ms');
assert(/\(sqlite3 killed after 2000 ms\)\n$/.test(fs.readFileSync(path.join(snap2, 'recmgr-crashes.txt'), 'utf8')));
cp.execFileSync('rm', ['-rf', path.join(U, 'var/opt/public/sin/recmgr'), path.join(U, 'var/opt/kerneldump')]);
var snap3 = path.join(dir, 'snap3');
fs.mkdirSync(snap3);
cp.execFileSync(process.execPath, [path.join(CUR, 'logs_extra.js'), 'snapshot', snap3], { env: XENV });
assert.deepStrictEqual(fs.readdirSync(snap3).sort(), ['core-dumps.txt', 'restartInfo-crashhandler.xml']);
assert(fs.readFileSync(path.join(snap3, 'core-dumps.txt'), 'utf8').indexOf('kerneldump') < 0);
console.log('ok   a hung sqlite3 is killed at 2 s; what is absent is left out');

try { fs.rmSync ? fs.rmSync(dir, { recursive: true, force: true }) : null; } catch (e) { /* left in tmp */ }
