#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* patchd's map settings ({"cmd":"cluster"}, "hud", "hud-size", "hud-pos",
 * "hud-keep-shown"):
 * each saved in map.state.conf for the render's next start, replacing an
 * earlier value and keeping the other lines, waze-on-cluster.local.conf
 * untouched; a bad value is refused and nothing written. A burst of changes
 * is one write after the last, every reply following it with the values
 * saved for its keys; a failed write is in every reply it held; a stop
 * writes what waits. At its start patchd moves any map setting found in the
 * local conf into the state file (one there already kept, one out of range
 * dropped), the local conf's other lines kept, once: the local conf is then
 * one waze-on-cluster 0.8.7, the version "Undo update" goes back to,
 * starts with. */
'use strict';
var assert = require('assert');
var cp = require('child_process');
var fs = require('fs');
var net = require('net');
var os = require('os');
var path = require('path');

var PATCHD = path.join(__dirname, '..', 'patchd.js');
var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-hud-'));
var SOCK = path.join(dir, 'sock', 'patch.sock');
var CONF = path.join(dir, 'waze-on-cluster.local.conf');
var STATE = path.join(dir, 'map.state.conf');
fs.writeFileSync(CONF, 'upload_kb_per_s = 3000\nhud = 1\n# the HUD\nhud_width_pct = 70\ntarget_fps = 15\ncluster = 2\n');
fs.writeFileSync(STATE, 'hud_width_pct = 50\n');

/* The keys waze-on-cluster 0.8.7 starts with in its local conf (any other is
 * an error): the shared Config's DEFAULTS, its define_keys and kIgnoredKeys,
 * at main 6477bb9. */
var KEYS_087 = ['local_conf', 'socket_path', 'socket_mode', 'socket_group', 'start_timeout_ms', 'ramses_user', 'hud_scene_id',
    'sail_scene_id', 'showcase_step', 'fps', 'page_ms', 'video_size', 'dcsm_client_handle', 'dcsm_heartbeat_replies', 'hud_slot',
    'hud_content_class', 'hud_size', 'sail_slot', 'sail_content_class', 'sail_size', 'verbose',
    'fifo_path', 'carplay_group', 'udp_port', 'avb_stream', 'drm_device', 'tex_size', 'src_crop', 'map_rect', 'dirty_threshold',
    'dirty_settle_ms', 'stall_ms', 'idle_ms', 'freeze_ms', 'freeze_min_fps', 'blank_ms', 'alt_screen_port', 'alt_screen_uid',
    'target_fps', 'pan_blocks', 'tex_level_bits', 'gpu_copy', 'target', 'hide_bmw_guidance', 'state_file', 'render_socket', 'log_file',
    'log_file_kb', 'tex_format', 'upload', 'dirty_tiles', 'max_frame_textures', 'resource_linger_ms', 'resource_fetch_log',
    'texture_replay', 'texture_replay_frames', 'mutable_r8', 'upload_kb_per_s', 'upload_burst_ms'];
var SAVE_MS = 200;
function keys(file) {
    return fs.readFileSync(file, 'utf8').split('\n').map(function (l) { return l.replace(/#.*/, '').trim(); })
        .filter(Boolean).map(function (l) { return l.split('=')[0].trim(); });
}

var ENV = Object.assign({}, process.env, {
        WOC_PATCH_SOCK: SOCK,
        WOC_PATCH_GROUP: '',
        WOC_LOCAL_CONF: CONF,
        WOC_STATE_DIR: dir,
        WOC_CARPLAY: path.join(dir, 'no-carplay'),
        WOC_STREAM_STATE: path.join(dir, 'no-stream.json'),
        WOC_RESYNC_FILE: path.join(dir, 'resync.json'),
        WOC_RECMGR_CONF: path.join(dir, 'no-recmgr'),
        WOC_RECMGR_AT_BOOT: path.join(dir, 'no-recmgr-at-boot.json'),
        WOC_PROC_STAT: path.join(dir, 'no-stat'),
        WOC_BOOT_ID: path.join(dir, 'no-boot-id'),
        WOC_MOUNT: path.join(dir, 'no-mount'),
        WOC_PROC_MOUNTS: path.join(dir, 'no-mounts'),
        WOC_CAPTURE_SOCK: path.join(dir, 'no-capture.sock'),
        WOC_DLT_RECEIVE: path.join(dir, 'no-dlt-receive'),
        WOC_DLT_RETRY_MS: '600000',
        WOC_REMOUNT_LOCK: path.join(dir, 'remount.lock'),
        WOC_MAP_SAVE_MS: String(SAVE_MS),
});
var out = '';
function start() {
    var c = cp.spawn(process.execPath, [PATCHD], { env: ENV, stdio: ['ignore', 'pipe', 'pipe'] });
    c.stdout.on('data', function (d) { out += d; });
    c.stderr.on('data', function (d) { out += d; });
    return c;
}
var p = start();

function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

/* One request on a fresh connection; resolves to the first reply. */
function ask(obj) {
    return new Promise(function (resolve, reject) {
        var s = net.connect(SOCK), buf = '';
        s.setEncoding('utf8');
        s.on('connect', function () { s.write(JSON.stringify(obj) + '\n'); });
        s.on('data', function (d) {
            buf += d;
            buf.split('\n').forEach(function (l) {
                var m = null;
                try { m = JSON.parse(l); } catch (e) {}
                if (m && /^(cluster|hud|hud-size|hud-pos|hud-keep-shown)-reply$/.test(m.ev)) { s.end(); resolve(m); }
            });
        });
        s.on('error', reject);
        setTimeout(function () { reject(new Error('no reply')); }, 5000);
    });
}

var passed = 0, total = 0;
function ok(name, cond) {
    total++;
    if (cond) passed++;
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + '\n');
}

(async function () {
    for (var t = 0; t < 5000 && !fs.existsSync(SOCK); t += 50) await sleep(50);
    ok('at the start: the local conf\'s map settings moved, one already saved kept, one out of range dropped',
       fs.readFileSync(STATE, 'utf8') === 'hud_width_pct = 50\nhud = 1\n');
    ok('the local conf\'s other lines kept', fs.readFileSync(CONF, 'utf8') === 'upload_kb_per_s = 3000\n# the HUD\ntarget_fps = 15\n');
    ok('the local conf: only keys waze-on-cluster 0.8.7 starts with', keys(CONF).every(function (k) { return KEYS_087.indexOf(k) >= 0; }));
    ok('both files readable by the capture (0644), whatever the umask', [CONF, STATE].every(function (f) { return (fs.statSync(f).mode & 0o777) === 0o644; }),
        [CONF, STATE].map(function (f) { return (fs.statSync(f).mode & 0o777).toString(8); }).join(' '));
    var conf = fs.readFileSync(CONF, 'utf8');
    var r = await ask({ cmd: 'hud', on: 0 });
    ok('the HUD off is saved in the state file, replacing the earlier value, the other lines kept',
       r.ok && r.hud === 0 && fs.readFileSync(STATE, 'utf8') === 'hud_width_pct = 50\nhud = 0\n');
    r = await ask({ cmd: 'cluster', on: 0 });
    var state = fs.readFileSync(STATE, 'utf8');
    ok('the cluster off is saved next to it', r.ok && r.cluster === 0 && /hud = 0\ncluster = 0\n$/.test(state));
    r = await ask({ cmd: 'cluster', on: 2 });
    ok('a cluster on that is not 0 or 1 is refused and nothing written', !r.ok && fs.readFileSync(STATE, 'utf8') === state);
    r = await ask({ cmd: 'hud', on: 'yes' });
    ok('an on that is not 0 or 1 is refused and nothing written', !r.ok && fs.readFileSync(STATE, 'utf8') === state);
    fs.unlinkSync(STATE);
    r = await ask({ cmd: 'hud', on: 1 });
    ok('no state file yet: it is created with the setting', r.ok && fs.readFileSync(STATE, 'utf8') === 'hud = 1\n');
    r = await ask({ cmd: 'hud-size', hud_width_pct: 70 });
    ok('the HUD size is saved next to it', r.ok && fs.readFileSync(STATE, 'utf8') === 'hud = 1\nhud_width_pct = 70\n');
    r = await ask({ cmd: 'hud-size', hud_width_pct: 85 });
    ok('a new size replaces it', r.ok && fs.readFileSync(STATE, 'utf8') === 'hud = 1\nhud_width_pct = 85\n');
    r = await ask({ cmd: 'hud-size', hud_width_pct: 100 });
    ok('100 % is saved as the widest, 85 %', r.ok && r.hud_width_pct === 85 && fs.readFileSync(STATE, 'utf8') === 'hud = 1\nhud_width_pct = 85\n');
    r = await ask({ cmd: 'hud-size', hud_width_pct: 10 });
    ok('an out-of-range size is refused, nothing written', !r.ok && /hud_width_pct = 85\n$/.test(fs.readFileSync(STATE, 'utf8')));
    r = await ask({ cmd: 'hud-pos', hud_x: 40, hud_y: -20 });
    ok('the HUD position is saved next to the size, in one write', r.ok && r.hud_offset_x === 40 && r.hud_offset_y === -20 &&
        fs.readFileSync(STATE, 'utf8') === 'hud = 1\nhud_width_pct = 85\nhud_offset_x = 40\nhud_offset_y = -20\n');
    r = await ask({ cmd: 'hud-pos', hud_x: 0, hud_y: 0 });
    ok('centred again: both replaced', r.ok && /hud_offset_x = 0\nhud_offset_y = 0\n$/.test(fs.readFileSync(STATE, 'utf8')));
    r = await ask({ cmd: 'hud-pos', hud_x: 900, hud_y: 0 });
    ok('an out-of-range position is refused, nothing written', !r.ok && /hud_offset_x = 0\n/.test(fs.readFileSync(STATE, 'utf8')));
    r = await ask({ cmd: 'hud-pos', hud_x: 1.5, hud_y: 0 });
    ok('a position that is not whole pixels is refused', !r.ok);
    r = await ask({ cmd: 'hud-keep-shown', on: 1 });
    ok('the HUD map kept shown over pop-ups is saved next to them', r.ok && r.hud_keep_shown === 1 &&
        /hud_offset_y = 0\nhud_keep_shown = 1\n$/.test(fs.readFileSync(STATE, 'utf8')));
    r = await ask({ cmd: 'hud-keep-shown', on: 0 });
    ok('giving way again replaces it', r.ok && r.hud_keep_shown === 0 && /hud_keep_shown = 0\n$/.test(fs.readFileSync(STATE, 'utf8')));
    state = fs.readFileSync(STATE, 'utf8');
    r = await ask({ cmd: 'hud-keep-shown', on: '1' });
    ok('a keep-shown that is not 0 or 1 is refused, nothing written', !r.ok && fs.readFileSync(STATE, 'utf8') === state);
    ok('the local conf untouched by every setting', fs.readFileSync(CONF, 'utf8') === conf);
    /* A burst: one write SAVE_MS after the last change, every reply after it. */
    var saves = function () { return out.split('(saved for the next start)').length - 1; };
    var before = fs.readFileSync(STATE, 'utf8'), n = saves(), t0 = Date.now();
    var burst = [ask({ cmd: 'hud-pos', hud_x: 10, hud_y: 0 }),
        sleep(50).then(function () { return ask({ cmd: 'hud-pos', hud_x: 20, hud_y: 0 }); }),
        sleep(100).then(function () { return ask({ cmd: 'hud-size', hud_width_pct: 60 }); })];
    await sleep(130);
    ok('a burst: nothing written while changes keep coming', fs.readFileSync(STATE, 'utf8') === before && saves() === n);
    var r3 = await Promise.all(burst);
    await sleep(50);   /* patchd's log line, on its own pipe */
    var saved = fs.readFileSync(STATE, 'utf8');
    ok('a burst: one write after the last change', saves() === n + 1 && Date.now() - t0 >= 100 + SAVE_MS &&
        ['hud_width_pct = 60', 'hud_offset_x = 20', 'hud_offset_y = 0'].every(function (l) { return saved.indexOf(l + '\n') >= 0; }));
    ok('a burst: every reply after the write, ok, with the values saved for its keys',
        r3.every(function (x) { return x.ok; }) && r3[0].hud_offset_x === 20 && r3[1].hud_offset_x === 20 && r3[2].hud_width_pct === 60);
    /* A write that fails: its replies say why, the file as it was. */
    fs.mkdirSync(STATE + '.tmp');
    before = fs.readFileSync(STATE, 'utf8');
    r = await ask({ cmd: 'hud-size', hud_width_pct: 70 });
    for (t = 0; t < 1000 && !/saving the HUD map size failed/.test(out); t += 50) await sleep(50);
    ok('a failed write: the reply says so and why, the file as it was', !r.ok && !!r.reason && r.hud_width_pct === 70 &&
        fs.readFileSync(STATE, 'utf8') === before && /saving the HUD map size failed/.test(out));
    fs.rmdirSync(STATE + '.tmp');
    /* A stop writes what waits. */
    var w = net.connect(SOCK);
    await new Promise(function (resolve) { w.on('connect', resolve); });
    w.write(JSON.stringify({ cmd: 'hud-size', hud_width_pct: 75 }) + '\n');
    await sleep(30);
    p.kill();
    await new Promise(function (resolve) { p.on('exit', resolve); });
    w.destroy();
    ok('stopped before the write was due: written at the stop', /hud_width_pct = 75\n/.test(fs.readFileSync(STATE, 'utf8')));
    state = fs.readFileSync(STATE, 'utf8');
    p = start();
    for (t = 0; t < 5000 && out.split('patchd listening').length < 3; t += 50) await sleep(50);
    ok('started again: nothing to move, both files as they were',
       fs.readFileSync(CONF, 'utf8') === conf && fs.readFileSync(STATE, 'utf8') === state);
})().catch(function (e) {
    ok('no error: ' + e.message, false);
}).then(function () {
    p.kill();
    try { fs.rmSync ? fs.rmSync(dir, { recursive: true, force: true }) : null; } catch (e) {}
    process.stdout.write(passed + '/' + total + ' passed\n');
    if (passed !== total) { process.stdout.write(out); process.exitCode = 1; }
});
