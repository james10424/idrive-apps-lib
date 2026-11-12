// SPDX-License-Identifier: GPL-3.0-or-later
/* The ETC2 car test (TEST-ETC2.md) on the HUD's visible window. Each layer
 * is an effect with a one-read shader. Prints the layers' and targets'
 * states, refused messages and the service's asset lines, and stays up
 * until Ctrl+C. Node 8, no deps.
 *
 *   node etc2_client.js [DIR] [--socket PATH] [--etc2-only | --atlas [--pan]]
 *
 * - default: one frame twice, side by side: left from an rgba8 asset, right
 *   from the same frame as an etc2rgba asset (DIR: frame.rgba, frame.etc2,
 *   frame.json; etc2_encode.py frame);
 * - --etc2-only: the right one alone;
 * - --atlas: one 2048x1024 etc2rgba asset (DIR: atlas.etc2, atlas.json;
 *   etc2_encode.py atlas) drawn whole, scaled to 560x280; --pan: a 560x280
 *   window of it at 1:1 drifting over it (the renderer's clock).
 *
 * The client module is found next to this file or in ../../js. */
'use strict';

const fs = require('fs');
const path = require('path');

function findClient() {
    const near = [path.join(__dirname, 'cluster_render_client.js'),
        path.join(__dirname, '..', '..', 'js', 'cluster_render_client.js')];
    for (const p of near) if (fs.existsSync(p)) return require(p);
    throw new Error('cluster_render_client.js not found next to this file');
}
const { ClusterRender, DEFAULT_PATH } = findClient();

const args = process.argv.slice(2);
let dir = __dirname, sock = DEFAULT_PATH, etc2Only = false, atlas = false, pan = false;
for (let i = 0; i < args.length; i++) {
    if (args[i] === '--socket') sock = args[++i];
    else if (args[i] === '--etc2-only') etc2Only = true;
    else if (args[i] === '--atlas') atlas = true;
    else if (args[i] === '--pan') pan = true;
    else dir = args[i];
}

function ts() { return new Date().toISOString().substr(11, 12); }
function log(s) { console.log(ts() + ' ' + s); }

const cr = new ClusterRender('etc2_test', sock);
const names = {};
if (atlas) atlasLayer(); else frameLayers();

/* One 2048x1024 etc2rgba asset, the HUD's window (560x280, the same 2:1). */
function atlasLayer() {
    const ai = JSON.parse(fs.readFileSync(path.join(dir, 'atlas.json'), 'utf8'));
    const AW = ai.size[0], AH = ai.size[1];
    const data = fs.readFileSync(path.join(dir, 'atlas.etc2'));
    if (data.length !== (AW / 4) * (AH / 4) * 16) {
        console.error('atlas.etc2 is not ' + AW + 'x' + AH);
        process.exit(2);
    }
    const fsWhole = 'precision mediump float;\n' +
        'uniform sampler2D u_img;\n' +
        'varying vec2 v_uv;\n' +
        'void main() { gl_FragColor = texture2D(u_img, v_uv); }\n';
    /* A 560x280 window at 1:1, its corner drifting over the whole. */
    const fsPan = '#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n' +
        'uniform sampler2D u_img;\n' +
        'uniform highp float u_time;\n' +
        'varying vec2 v_uv;\n' +
        'void main() {\n' +
        '    vec2 at = vec2(' + (AW - 560) + '.0 * (0.5 - 0.5 * cos(u_time * 0.25)), ' +
        (AH - 280) + '.0 * (0.5 - 0.5 * cos(u_time * 0.17)));\n' +
        '    gl_FragColor = texture2D(u_img, (at + v_uv * vec2(560.0, 280.0)) / vec2(' + AW + '.0, ' + AH + '.0));\n' +
        '}\n';
    const a = cr.asset({ target: 'hud', format: 'etc2rgba', w: AW, h: AH, sampling: pan ? 'nearest' : 'bilinear' });
    const l = cr.effect({ target: 'hud', x: 120, y: 100, w: 560, h: 280, fs: pan ? fsPan : fsWhole, time: pan });
    if (!a || !l) {
        console.error('the client refused the etc2rgba asset or its layer (old cluster_render_client.js?)');
        process.exit(2);
    }
    cr.bind(l, 'u_img', a);
    names[a] = 'asset etc2rgba ' + AW + 'x' + AH;
    names[l] = 'atlas layer' + (pan ? ' (pan)' : '');
    cr.pixels(a, null, data);
    log('atlas ' + AW + 'x' + AH + ': etc2rgba ' + data.length + ' bytes (rgba8 would be ' + AW * AH * 4 +
        '); layer 560x280 at 120,100 on hud, ' + (pan ? 'a 1:1 window drifting' : 'the whole, scaled'));
}

function frameLayers() {
    const info = JSON.parse(fs.readFileSync(path.join(dir, 'frame.json'), 'utf8'));
    const W = info.padded[0], H = info.padded[1];
    const rgba = fs.readFileSync(path.join(dir, 'frame.rgba'));
    const etc2 = fs.readFileSync(path.join(dir, 'frame.etc2'));
    if (rgba.length !== W * H * 4 || etc2.length !== (W / 4) * (H / 4) * 16) {
        console.error('frame.rgba or frame.etc2 is not ' + W + 'x' + H);
        process.exit(2);
    }

    /* One texture read: well inside the KOMBI's shader limits. */
    const FS = 'precision mediump float;\n' +
        'uniform sampler2D u_img;\n' +
        'varying vec2 v_uv;\n' +
        'void main() { gl_FragColor = texture2D(u_img, v_uv); }\n';

    /* The HUD's visible window: x 120..680, y 100..380 of 800x480. */
    const GAP = 16;
    const X0 = 120 + Math.floor((560 - (2 * W + GAP)) / 2), Y0 = 100;

    let left = 0;
    if (!etc2Only) {
        const a = cr.asset({ target: 'hud', format: 'rgba8', w: W, h: H, sampling: 'nearest' });
        left = cr.effect({ target: 'hud', x: X0, y: Y0, w: W, h: H, fs: FS });
        cr.bind(left, 'u_img', a);
        names[a] = 'asset rgba8';
        names[left] = 'left (rgba8)';
        cr.pixels(a, null, rgba);
    }
    const b = cr.asset({ target: 'hud', format: 'etc2rgba', w: W, h: H, sampling: 'nearest' });
    const right = cr.effect({ target: 'hud', x: X0 + W + GAP, y: Y0, w: W, h: H, fs: FS });
    cr.bind(right, 'u_img', b);
    names[b] = 'asset etc2rgba';
    names[right] = 'right (etc2rgba)';
    if (!b || !right) {
        console.error('the client refused the etc2rgba asset or its layer (old cluster_render_client.js?)');
        process.exit(2);
    }
    cr.pixels(b, null, etc2);

    log('frame ' + W + 'x' + H + ': rgba8 ' + rgba.length + ' bytes, etc2rgba ' + etc2.length + ' bytes; layers at x ' +
        X0 + ' and ' + (X0 + W + GAP) + ', y ' + Y0 + ' on hud');
}

cr.on('ready', () => {
    log('ready: connected to ' + sock);
    setTimeout(() => cr.status(), 3000);
});
cr.on('disconnected', () => log('disconnected (the client reconnects)'));
cr.on('layer', (ref, state, reason) => log('layer ' + (names[ref] || ref) + ': ' + state + (reason ? ' - ' + reason : '')));
cr.on('target', (name, state, holder, reason, guideDisabled, disabled) => {
    if (name !== 'hud') return;
    log('target hud: ' + state + (holder ? ' holder ' + holder : '') + (reason ? ' - ' + reason : '') +
        (disabled ? ' (mapping guard: disabled)' : ''));
});
cr.on('refused', (op, ref, reason) => log('REFUSED ' + op + ' ' + (names[ref] || ref) + ': ' + reason));
cr.on('status', (text) => {
    text.split('\n').filter((l) => /^asset |etc2|layer /.test(l)).forEach((l) => log('status: ' + l));
});
cr.connect();

function quit() {
    log('closing');
    cr.close();
    setTimeout(() => process.exit(0), 200);
}
process.on('SIGINT', quit);
process.on('SIGTERM', quit);
