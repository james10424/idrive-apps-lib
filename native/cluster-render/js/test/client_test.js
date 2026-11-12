// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The Node client (../cluster_render_client.js) against the service on the
 * host with fake display backends (test/host_service.cpp): hello; a video
 * layer stalled until its first frame, then shown; an image overlay of a
 * second client in the same target; the stock HMI taking the slot and a
 * contending layer winning it back; the budget event; refusals; an asset
 * sampled by an effect with the renderer's u_time and a uniform array; a
 * mesh layer with a part, its geometry and an animation; the service
 * restarting under the client, whose layers, assets, parts and binds come
 * back by themselves; a synthetic scene file on the panel, shown with the
 * layers there waiting, its edit lines kept by key, a malformed one and one
 * changing nothing reported, and dropped; close() and connect() in one tick
 * making one connection. */
'use strict';

const assert = require('assert');
const { spawn } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { ClusterRender } = require('../cluster_render_client.js');

const bin = process.argv[2];
const sock = '/tmp/crs-js-test-' + process.pid + '.sock';
let failures = 0;
function ok(name, cond) {
    console.log((cond ? 'ok   ' : 'FAIL ') + name);
    if (!cond) failures++;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(cond, ms) {
    for (let t = 0; t < ms; t += 20) {
        if (cond()) return true;
        await sleep(20);
    }
    return cond();
}

/* Scene files come from here; a synthetic gauge (fake_graph.h FakeFileScene). */
const scenes = fs.mkdtempSync('/tmp/crs-js-scenes-');
const GAUGE = 'scene 0x010004f8\nnode gauge -\nnode needle_pivot gauge\nmesh needle needle_pivot u_color\n' +
    'mesh quad - u_color\n';

function startService() {
    const p = spawn(bin, [sock, scenes], { stdio: ['pipe', 'pipe', 'ignore'] });
    return new Promise((resolve) => p.stdout.once('data', () => resolve(p)));
}

function track(c) {
    const s = { layer: {}, target: {}, holder: {}, refused: [], budget: null, ready: 0, gone: 0 };
    c.on('ready', () => s.ready++);
    c.on('disconnected', () => s.gone++);
    c.on('layer', (ref, state) => { s.layer[ref] = state; });
    c.on('target', (t, state, holder) => { s.target[t] = state; s.holder[t] = holder; });
    c.on('refused', (op, ref, why) => s.refused.push(op + ': ' + why));
    c.on('budget', (cap, max, why) => { s.budget = { cap, max, why }; });
    return s;
}

async function main() {
    let svc = await startService();
    const waze = new ClusterRender('waze', sock), ovl = new ClusterRender('overlay', sock);
    const ew = track(waze), eo = track(ovl);
    waze.connect();
    ovl.connect();
    ok('hello: ready, backend and targets', await until(() => ew.ready && eo.ready, 2000) &&
        waze.backend === 'host' && waze.targets.indexOf('map:1088x480') === 0);
    ok('the budget event: 5500 KB/s of 5500', await until(() => ew.budget, 1000) &&
        ew.budget.cap === 5500 && ew.budget.max === 5500);

    const map = waze.video({ target: 'map', contend: true, stall_ms: 1000 });
    ok('a video layer without frames is stalled', await until(() => ew.layer[map] === 'stalled', 1000));
    const atlas = Buffer.alloc(waze.frameBytes(map), 128);
    ok('a frame of the wrong size is refused here', !waze.frame(map, Buffer.alloc(10), 1));
    const feed = setInterval(() => waze.frame(map, atlas, Date.now()), 40);
    ok('its first frames: shown', await until(() => ew.layer[map] === 'shown' && ew.target.map === 'shown', 2000));

    const badge = ovl.image({ target: 'map', klass: 'overlay', x: 900, y: 400, w: 64, h: 64, iw: 64, ih: 64 });
    ovl.pixels(badge, null, Buffer.alloc(64 * 64 * 4, 255));
    ok('another client\'s overlay in the same target: shown', await until(() => eo.layer[badge] === 'shown', 1000));
    ok('a partial update is accepted', ovl.pixels(badge, [{ x: 0, y: 0, w: 8, h: 8 }], Buffer.alloc(256, 9)));
    ok('one outside the image is not', !ovl.pixels(badge, [{ x: 60, y: 60, w: 8, h: 8 }], Buffer.alloc(256)));

    svc.stdin.write('steal map\n');
    ok('the stock HMI takes the slot: waiting, held by the stock HMI',
        await until(() => eo.layer[badge] === 'waiting' && eo.holder.map === 'stock', 1000));
    ok('the contending video wins it back after 2 s', await until(() => eo.layer[badge] === 'shown', 3500));

    ovl.effect({ target: 'nowhere', fs: 'x' });
    ok('a layer on an unknown target is refused with the reason',
        await until(() => eo.refused.some((r) => r.indexOf('unknown target') >= 0), 1000));

    const font = ovl.asset({ target: 'panel', format: 'r8', w: 64, h: 32, sampling: 'bilinear' });
    ok('an asset takes its pixels', font > 0 && ovl.pixels(font, null, Buffer.alloc(64 * 32, 200)) &&
        ovl.frameBytes(font) === 64 * 32);
    ok('an etc2rgba asset is 4x4 blocks', ovl.asset({ target: 'panel', format: 'etc2rgba', w: 250, h: 8 }) === 0);
    const tex = ovl.asset({ target: 'panel', format: 'etc2rgba', w: 2048, h: 1024 });
    ok('a 2048x1024 one takes its blocks whole, 16 bytes each, and no rects',
        tex > 0 && !ovl.pixels(tex, null, Buffer.alloc(2048 * 1024 - 16)) &&
        !ovl.pixels(tex, [{ x: 0, y: 0, w: 4, h: 4 }], Buffer.alloc(16)) &&
        ovl.pixels(tex, null, Buffer.alloc(2048 * 1024, 7)) && ovl.frameBytes(tex) === 2048 * 1024);
    const label = ovl.effect({ target: 'panel', klass: 'overlay', x: 40, y: 600, w: 128, h: 64, time: true,
        fs: '#version 100\nprecision mediump float;\nuniform sampler2D u_atlas;\nuniform vec4 u_cells[2];\n' +
            'uniform float u_time;\nvarying vec2 v_uv;\n' +
            'void main(){gl_FragColor=texture2D(u_atlas,v_uv)*u_cells[0]*u_time;}' });
    ovl.bind(label, 'u_atlas', font);
    ovl.uniform(label, 'u_cells', [1, 1, 1, 1, 0, 0, 0, 1], { type: 'vec4' });
    ok('an effect sampling it is shown', await until(() => eo.layer[label] === 'shown', 1000));
    const car = ovl.mesh({ target: 'panel', x: 700, y: 200, w: 400, h: 300, cz: 6 });
    ok('a mesh layer\'s part', ovl.part(car, 1, {
        vs: '#version 100\nattribute vec3 a_position;\nuniform mat4 u_mvp;\n' +
            'void main(){gl_Position=u_mvp*vec4(a_position,1.0);}',
        fs: '#version 100\nprecision mediump float;\nvoid main(){gl_FragColor=vec4(1.0);}' }));
    ok('geometry with an index past its vertices is refused here',
        !ovl.geometry(car, 1, { positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 3] }));
    ok('a mesh layer waits for its geometry', await until(() => eo.layer[car] === 'stalled', 1000));
    ovl.geometry(car, 1, { positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 2] });
    ovl.transform(car, 1, { ry: 30 });
    ovl.transform(car, 1, { tx: 2 });
    const replay = ovl.layers.get(car).parts.get(1).transform;
    ok('transforms merge into what a reconnect replays', replay.ry === 30 && replay.tx === 2);
    ok('a transform out of the service\'s range is refused here and not kept',
        !ovl.transform(car, 1, { ry: 40000 }) && ovl.transform(car, 1, { tz: 1 }) &&
        ovl.layers.get(car).parts.get(1).transform.ry === 30);
    ok('an animation is numbered', ovl.animate(car, { part: 1, property: 'rotate', keys: [[0, 0, 0, 0], [4000, 0, 360, 0]],
        loop: true }) === 1);
    ok('then is shown', await until(() => eo.layer[car] === 'shown', 1000));
    ovl.drop(font);
    ok('an asset an effect samples is not dropped',
        await until(() => eo.refused.some((r) => r === 'drop: asset in use'), 1000));
    let status = '';
    ovl.on('status', (t) => { status = t; });
    ovl.status();
    ok('the status names the asset', await until(() => status.indexOf('asset') >= 0 && status.indexOf('r8 64x32') >= 0, 1000));

    svc.kill();
    await new Promise((r) => svc.once('exit', r));
    ok('the service gone: both clients notice', await until(() => eo.gone === 1 && ew.gone === 1, 1000));
    eo.layer = {};
    ew.layer = {};
    svc = await startService();
    ok('it comes back: both clients reconnect and their layers are shown again',
        await until(() => eo.layer[badge] === 'shown' && ew.layer[map] === 'shown', 5000) && eo.ready >= 2);
    ok('the asset, its effect, the mesh part and its geometry are made again',
        await until(() => eo.layer[label] === 'shown' && eo.layer[car] === 'shown', 2000));

    const file = path.join(scenes, 'gauge.scene');
    fs.writeFileSync(file, GAUGE);
    fs.chmodSync(file, 0o644);
    const sha256 = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
    ok('a scene needs its file and hash', ovl.scene({ target: 'panel', file: file }) === 0);
    const gauge = ovl.scene({ target: 'panel', file: file, sha256: [sha256] });
    ovl.edit(gauge, '--rotate ^needle_pivot$=0,0,30\n--hide ^quad$\r\n');
    ovl.edit(gauge, '--rotate ^needle_pivot$=0,0,45');
    ok('a scene file on the panel: shown, the layers there waiting for it',
        await until(() => eo.layer[gauge] === 'shown' && eo.layer[label] === 'waiting', 3000));
    ok('its edits kept by key, the latest last',
        Array.from(ovl.layers.get(gauge).edits.values()).join('|') === '--hide ^quad$|--rotate ^needle_pivot$=0,0,45');
    ovl.edit(gauge, '--spin x\n--show ^nothing$');
    ok('a malformed line refused, one that changes nothing reported, both named',
        await until(() => eo.refused.some((r) => r.indexOf('edit: \'--spin x\'') === 0) &&
            eo.refused.some((r) => r.indexOf('edit: \'--show ^nothing$\' changed nothing') === 0), 1000));
    ovl.drop(gauge);
    ok('dropped: the layers there shown again', await until(() => eo.layer[label] === 'shown', 3000));

    clearInterval(feed);
    ok('frames stopping: the video is stalled after its stall time',
        await until(() => ew.layer[map] === 'stalled', 3000));
    let opens = 0;
    const open = waze._open.bind(waze);
    waze._open = () => { opens++; open(); };
    waze.close();
    waze.connect();   /* in the same tick: the old socket's close comes later */
    await sleep(750);
    ok('closed and connected again at once: one connection, ready', opens === 1 && waze.ready && waze.sock);
    waze.close();
    ovl.close();
    svc.kill();
    try { fs.unlinkSync(sock); } catch (e) { /* the service removed it */ }
    fs.unlinkSync(file);
    fs.rmdirSync(scenes);
    console.log(failures ? failures + ' FAILED' : 'all ok');
    process.exit(failures ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
