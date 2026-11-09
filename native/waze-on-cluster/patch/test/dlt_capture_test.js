// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
var assert = require('assert'), fs = require('fs'), os = require('os');
var net = require('net'), path = require('path'), cp = require('child_process');
var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'wc-'));
var helper = path.join(__dirname, '..', 'dlt_capture.js');
var children = [], sockets = [], modes = [], levels = [], reqs = [], behavior = 'ok', tcp, level = 4;
var env = Object.assign({}, process.env, {
    WOC_CAPTURE_DIR: path.join(dir, 'capture'), WOC_DLT_SOCKET: path.join(dir, 'ctrl'),
    WOC_TRACE_DIR: dir, WOC_CAPTURE_MS: '400',
    WOC_CAPTURE_RETRY_MS: '80', WOC_DLT_PROC: path.join(dir,'proc')
});
function delay(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
function listen(server, address) { return new Promise(function (r) { server.listen(address, r); }); }
async function until(fn) {
    for (var i = 0; i < 250; i++) { if (await fn()) return; await delay(20); }
    throw new Error('condition timed out');
}
function state() { try { return JSON.parse(fs.readFileSync(path.join(env.WOC_CAPTURE_DIR, 'state.json'))); } catch (e) { return {}; } }
function command(cmd) {
    return new Promise(function (resolve, reject) {
        var c = net.connect(path.join(env.WOC_CAPTURE_DIR, 'control.sock')), buf = '';
        c.on('error', reject); c.on('connect', function () { c.write(JSON.stringify({cmd:cmd}) + '\n'); });
        c.on('data', function (d) { buf += d; if (buf.indexOf('\n') >= 0) { c.destroy(); resolve(JSON.parse(buf)); } });
    });
}
function spawn(args) {
    var child = cp.spawn(process.execPath, [helper].concat(args || []), { env: env, stdio: ['ignore', 'pipe', 'pipe'] });
    children.push(child); var text = '';
    child.stderr.on('data', function(d) { text += d; });
    child.result = new Promise(function (resolve) { child.on('exit', function (code, sig) { resolve({code:code, signal:sig, text:text}); }); });
    return child;
}
function trace(mode) {
    fs.writeFileSync(path.join(dir, '12_dltd.idlt'), mode === 0 ? '2026/10/26 12:00:00 1 Tracing OFF: Switched to log mode off state \n' : '2026/10/26 12:00:00 1 Tracing ON: Switched to buffer state for socket connections \n');
}
// Replaces every iDLT file of the trace directory with the given ones.
function records(files) {
    fs.readdirSync(dir).filter(function (n) { return /\.idlt$/.test(n); }).forEach(function (n) { fs.unlinkSync(path.join(dir, n)); });
    Object.keys(files).forEach(function (n) { fs.writeFileSync(path.join(dir, n), files[n].join('\n') + '\n'); });
}
function line(ms, text) { return '2026/11/07 14:58:42 ' + ms + ' ' + text; }
// The records a resume left on the car: the manager's switch-off, no daemon mode record.
var RESUME = {
    '10673_dltd.idlt': ['2026/11/07 14:58:42 2203678 Control message received: Default log level set: OFF',
        '2026/11/07 14:58:42 2203883 Control message received: Default trace status set: OFF'],
    '10673_LTM.idlt': ['2026/11/07 14:58:42 2203661 Sending logmode OFF to daemon successful ',
        '2026/11/07 14:58:42 2204079 Setting default loglevel and trace status successful ',
        '2026/11/07 14:58:42 2204079 Trace mode off active ',
        '2026/11/07 14:58:42 2204088 Clear DTC : Internal Trace B7F8F3',
        '2026/11/07 14:58:42 2204262 Unconditional internal inactive ']
};
// Starts a capture and waits until the helper's own state leaves opening.
async function settled() {
    await command('start');
    var reply;
    await until(async function() { reply = await command('status'); return reply.state !== 'opening'; });
    return reply;
}
// A capture from these records opens with this mode saved, and restores it.
async function capturesFrom(files, mode, what) {
    records(files); var from = reqs.length;
    assert.strictEqual((await settled()).state, 'open', what + ': ' + JSON.stringify(state()));
    assert.strictEqual(state().restore_mode, mode, what);
    await command('stop'); await until(function() { return state().state === 'closed'; });
    assert.strictEqual(modes[modes.length-1], mode, what);
    return reqs.slice(from);
}
// A capture from these records is refused before any change reaches the daemon.
async function refusedFrom(files, pattern, what) {
    records(files); var from = reqs.length;
    assert.strictEqual((await settled()).state, 'refused', what);
    assert(pattern.test(state().error), what + ': ' + state().error);
    assert.deepStrictEqual(reqs.slice(from).filter(function (r) { return r !== 'get'; }), [], what);
}
function netState(open) {fs.writeFileSync(path.join(env.WOC_DLT_PROC,'net','tcp'),open ? '0: 0100007F:'+Number(env.WOC_DLT_PORT).toString(16)+' 00000000:0000 0A\n' : 'sl local_address rem_address st\n');}
async function openTCP() {
    if (tcp) return;
    tcp = net.createServer(function(c) { sockets.push(c); c.on('error', function() {}); c.on('data', function() {}); });
    await listen(tcp, {host:'127.0.0.1', port:Number(env.WOC_DLT_PORT)});
    netState(true);
}
async function closeTCP() {
    if (!tcp) return;
    netState(false);
    sockets.forEach(function(c) { c.destroy(); }); sockets = [];
    var old = tcp; tcp = null;
    await new Promise(function(r) { old.close(r); });
}
(async function() {
    fs.mkdirSync(env.WOC_DLT_PROC);fs.mkdirSync(path.join(env.WOC_DLT_PROC,'9'));
    fs.writeFileSync(path.join(env.WOC_DLT_PROC,'9','comm'),'dlt-daemon\n');
    fs.mkdirSync(path.join(env.WOC_DLT_PROC,'9','fd'));
    var clientFD=path.join(env.WOC_DLT_PROC,'9','fd','10');
    fs.symlinkSync('/run/dlt-files/dltpipes/dlt99',clientFD);
    var reserve = net.createServer(); await listen(reserve, {host:'127.0.0.1',port:0});
    env.WOC_DLT_PORT = String(reserve.address().port);fs.mkdirSync(path.join(env.WOC_DLT_PROC,'net'));netState(false); await new Promise(function(r) { reserve.close(r); });
    var ctrl = net.createServer(function(c) {
        var buf = Buffer.alloc(0);
        c.on('error', function() {});
        c.on('data', async function(d) {
            buf = Buffer.concat([buf,d]); if (buf.length < 4 || buf.length < buf.readUInt16BE(2)) return;
            var req = buf.slice(0, buf.readUInt16BE(2)); buf = Buffer.alloc(0);
            var service = req.readUInt32LE(22), reply;
            // Frozen request fixtures: BMW libdlt's UEH/WEID/WTMS control envelope.
            if (service === 0x04) {
                assert.strictEqual(req.toString('hex'), '3500001a4d47485300000000160141505000434f4e0004000000');
                reqs.push('get');
                if (behavior === 'get-reject') reply = '3500001c4d47485300000000260141505000434f4e00040000000100';
                else reply = '3500001c4d47485300000000260141505000434f4e000400000000' + ('0' + level.toString(16)).slice(-2);
            } else if (service === 0x11) {
                assert.strictEqual(req.slice(0,26).toString('hex'), '3500001f4d47485300000000160141505000434f4e0011000000');
                assert.strictEqual(req.slice(27).toString(), 'remo');
                reqs.push('level ' + req[26]); levels.push(req[26]);
                if (behavior === 'hang' || (behavior === 'restore-hang' && req[26] !== 4)) return;
                var refuse = behavior === 'reject' || (behavior === 'restore-level-reject' && req[26] !== 4);
                if (!refuse) level = req[26];
                reply = '3500001b4d47485300000000260141505000434f4e00110000000' + (refuse ? '1' : '0');
            } else {
                assert.strictEqual(req.slice(0,26).toString('hex'), '3500001b4d47485300000000160141505000434f4e00130f0000');
                var mode = req[26]; modes.push(mode); reqs.push('mode ' + mode);
                if (behavior === 'hang' || (behavior === 'restore-hang' && mode !== 1)) return;
                if (behavior !== 'reject') {
                    if (mode === 1) await openTCP(); else { await closeTCP(); trace(mode); }
                }
                reply = '3500001b4d47485300000000260141505000434f4e00130f00000' + (behavior === 'reject' ? '1' : '0');
            }
            reply = Buffer.from(reply,'hex');
            // Fragmented acknowledgement exercises stream reassembly.
            c.write(reply.slice(0,3)); setTimeout(function() { c.end(reply.slice(3)); }, 5);
        });
    });
    await listen(ctrl, env.WOC_DLT_SOCKET);
    trace(2);
    var daemon = spawn();
    await until(function() { return fs.existsSync(path.join(env.WOC_CAPTURE_DIR,'control.sock')); });
    await command('status'); await delay(60); assert.deepStrictEqual(modes, [], 'startup/status must not toggle DLT');
    await command('start'); await until(function() { return state().state === 'open'; });
    var deadline = state().closes_at;
    for (var i=0;i<4;i++) { await command('start'); await command('status'); }
    assert.strictEqual(state().closes_at, deadline, 'requests cannot extend capture');
    await until(function() { return state().state === 'closed'; }); assert.deepStrictEqual(modes,[1,2]);
    console.log('ok fixed deadline, passive reads, acknowledgement and verified restore');
    await openTCP(); var before = modes.length; await command('start');
    await until(function() { return state().state === 'refused'; }); assert.strictEqual(modes.length,before);
    await closeTCP(); fs.writeFileSync(path.join(dir,'12_dltd.idlt'),'no state record\n');
    await command('start'); await until(function() { return state().state === 'refused' && /unknown/.test(state().error); });
    assert.strictEqual(modes.length,before); console.log('ok pre-existing trace and unknown baseline refused without mutation');
    fs.unlinkSync(clientFD);trace(2);await command('start');
    await until(function() { return state().state === 'refused'; });
    assert.strictEqual(modes.length,before,'an empty daemon gets no request');
    fs.symlinkSync('/run/dlt-files/dltpipes/dlt99',clientFD);
    console.log('ok empty daemon receives no control request');
    trace(0); await command('start'); await until(function() { return state().state === 'open'; });
    await command('stop'); await until(function() { return state().state === 'closed'; });
    assert.strictEqual(modes[modes.length-1],0); console.log('ok OFF baseline restored as OFF');
    trace(0); fs.writeFileSync(path.join(dir,'13_dltd.idlt'),'2026/10/26 12:00:00 1 iDLT ~ new lifecycle\n');
    await command('start'); await until(function() { return state().state === 'open'; });
    await command('stop'); await until(function() { return state().state === 'closed'; });
    assert.strictEqual(modes[modes.length-1],0); fs.unlinkSync(path.join(dir,'13_dltd.idlt'));
    console.log('ok baseline from an earlier lifecycle record');
    trace(2); behavior='reject'; await command('start');
    await until(function() { return state().state === 'restore_failed'; });
    assert.strictEqual(state().owned,true); assert(/rejected/.test(state().error));
    behavior='ok'; await until(function() { return state().state === 'closed'; });
    console.log('ok rejected acknowledgement, visible failed restore, recovery');
    trace(2); await command('start'); await until(function() { return state().state === 'open'; });
    daemon.kill('SIGKILL'); await daemon.result;
    assert.strictEqual(state().owned,true);
    var cleanup = spawn(['restore']); assert.strictEqual((await cleanup.result).code,0);
    assert.strictEqual(state().owned,false); assert.strictEqual(tcp,null);
    console.log('ok independent ExecStopPost restores owned lease after SIGKILL');
    before=modes.length; cleanup=spawn(['restore']); assert.strictEqual((await cleanup.result).code,0);
    assert.strictEqual(modes.length,before,'no owned lease means no restore command');
    daemon=spawn(); await until(async function() { try { await command('status'); return true; } catch(e) { return false; } }); behavior='hang'; trace(2); await command('start');
    await until(function() { return state().owned; }); await command('stop');
    behavior='ok'; await until(function() { return state().state === 'closed'; });
    assert.strictEqual(modes[modes.length-1],2); console.log('ok stop during pending start and ambiguous timeout restores baseline');
    assert.deepStrictEqual(levels, [], 'a default log level of INFO is left alone');

    level = 0;
    assert.deepStrictEqual(await capturesFrom(RESUME, 0, 'after resume'), ['get', 'level 4', 'mode 1', 'mode 0', 'level 0']);
    assert.strictEqual(level, 0);
    console.log('ok after-resume records: OFF, default log level raised to INFO for the window, both restored');
    behavior = 'restore-level-reject'; records(RESUME);
    await command('start'); await until(function() { return state().state === 'open'; });
    await command('stop'); await until(function() { return state().state === 'restore_failed'; });
    assert.strictEqual(state().owned, true); assert(/default log level/.test(state().error));
    behavior = 'ok'; await until(function() { return state().state === 'closed'; });
    assert.strictEqual(level, 0); assert.strictEqual(modes[modes.length-1], 0);
    console.log('ok a rejected default log level restore stays visible and is retried');
    level = 4;

    await capturesFrom({ '20_dltd.idlt': [line(100, 'Tracing ON: Switched to buffer state for socket connections ')],
        '21_dltd.idlt': [line(300, 'Tracing OFF: Switched to log mode off state ')],
        '21_LTM.idlt': [line(299, 'Sending logmode OFF to daemon successful ')],
        '22_dltd.idlt': [line(500, 'Control message received: Default log level set: OFF')],
        '22_LTM.idlt': [line(400, 'Setting default loglevel and trace status successful ')] }, 0, 'newest lifecycle with a record');
    await capturesFrom({ '21_dltd.idlt': [line(100, 'Tracing OFF: Switched to log mode off state '),
        line(100, 'Control message received: Log mode set: OFF'),
        line(200, 'Tracing ON: Switched to buffer state for socket connections '),
        line(200, 'Control message received: Log mode set: BUFFER')],
        '21_LTM.idlt': [line(101, 'Sending logmode OFF to daemon successful '),
        line(201, 'Sending logmode BUFFER to daemon successful ')] }, 2, 'manager sends in order');
    await capturesFrom({ '21_dltd.idlt': [line(100, 'Tracing OFF: Switched to log mode off state ')],
        '21_LTM.idlt': [line(101, 'Sending logmode OFF to daemon successful ')],
        '22_dltd.idlt': [line(200, 'Tracing ON: Switched to buffer state for socket connections ')] }, 2,
        'a newer daemon record overrides the manager');
    await capturesFrom({ '21_dltd.idlt': [line(100, 'Tracing ON: Switched to send direct state '),
        line(200, 'Tracing OFF: Switched to log mode off state '),
        line(200, 'Tracing ON: Switched to buffer state for socket connections '),
        line(200, 'Control message received: Log mode set: OFF')] }, 0, 'OFF while a client was attached');
    console.log('ok newest record wins across numbered files and writers');
    await capturesFrom({ '30_LTM.idlt': [line(100, 'Sending logmode BUFFER to daemon successful ')] }, 2, 'manager only, BUFFER');
    await capturesFrom({ '30_dltd.idlt': [line(100, 'Control message received: Log mode set: OFF')] }, 0, 'daemon request only');
    await capturesFrom({ '30_dltd.idlt': [line(100, 'Control message received: Default log level set: INFO')],
        '29_dltd.idlt': [line(50, 'Tracing ON: Switched to buffer state for socket connections ')],
        '30_LTM.idlt': [line(90, 'Sending logmode BUFFER to daemon successful ')] }, 2, 'manager agrees with an older lifecycle');
    console.log('ok a manager record without its daemon record, and the reverse');

    await refusedFrom({ '30_LTM.idlt': [line(100, 'Sending logmode OFF to daemon unsuccessful ')] }, /unknown/, 'failed send');
    await refusedFrom({ '30_dltd.idlt': [line(100, 'Tracing OFF: Switched to log mode off state ')],
        '30_LTM.idlt': [line(100, 'Sending logmode OFF to daemon successful '),
        line(200, 'Sending logmode OFF to daemon unsuccessful ')] }, /disagree/, 'failed send after a daemon record');
    await refusedFrom({ '30_dltd.idlt': [line(100, 'Tracing ON: Switched to buffer state for socket connections ')],
        '30_LTM.idlt': [line(200, 'Sending logmode OFF to daemon successful ')] }, /disagree/, 'manager contradicts daemon');
    await refusedFrom({ '29_dltd.idlt': [line(100, 'Tracing ON: Switched to buffer state for socket connections ')],
        '30_LTM.idlt': [line(200, 'Sending logmode OFF to daemon successful ')] }, /disagree/, 'contradiction across lifecycles');
    await refusedFrom({ '30_dltd.idlt': [line(100, 'Tracing OFF: Switched to log mode off state ')],
        '30_LTM.idlt': ['Sending logmode BUFFER to daemon successful '] }, /disagree/, 'an untimed manager record');
    await refusedFrom({ '30_dltd.idlt': [line(100, 'Control message received: Log mode set: ON')] }, /unknown/, 'ON, listener closed');
    await refusedFrom({ '30_LTM.idlt': [line(100, 'Sending logmode ON to daemon successful ')] }, /unknown/, 'manager ON');
    await refusedFrom({ '30_LTM.idlt': [line(100, 'Trace mode off active '), line(101, 'Unconditional internal inactive ')] },
        /unknown/, 'manager lines that name no mode');
    var full = []; for (var j = 0; j < 500; j++) full.push(line(j, 'Setting default loglevel and trace status successful '));
    await refusedFrom({ '30_LTM.idlt': full.concat([line(600, 'Sending logmode OFF to daemon successful ')]) },
        /saturated/, 'saturated manager file');
    behavior = 'get-reject';
    await refusedFrom(RESUME, /default log level read/, 'unreadable default log level');
    behavior = 'ok';
    console.log('ok ambiguous, contradictory and unreadable records refused without mutation');

    level = 0; records(RESUME);
    await command('start'); await until(function() { return state().state === 'open'; });
    assert.strictEqual(level, 4);
    daemon.kill('SIGKILL'); await daemon.result;
    cleanup = spawn(['restore']); assert.strictEqual((await cleanup.result).code, 0);
    assert.strictEqual(state().owned, false); assert.strictEqual(level, 0); assert.strictEqual(modes[modes.length-1], 0);
    console.log('ok ExecStopPost restores the default log level after SIGKILL');
    daemon.kill('SIGTERM'); await daemon.result;
    await new Promise(function(r) { ctrl.close(r); }); await closeTCP();
    console.log('DLT capture lifecycle passed');
})().catch(function(err) { console.error(err.stack); process.exitCode=1; }).then(function() {
    children.forEach(function(c) { try { c.kill('SIGKILL'); } catch(e) {} });
    // This directory is exclusively created by this test.
    cp.execFileSync('rm',['-rf',dir]);
    process.exit(process.exitCode || 0);
});
