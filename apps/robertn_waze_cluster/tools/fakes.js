// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Stand-ins for the two native waze-on-cluster sockets, for tests and desk
 * runs of the app's client (src/waze_cluster_control.js). Both speak
 * newline-delimited JSON on a Unix stream socket.
 *
 *   node tools/fakes.js [render-socket] [patch-socket]
 *
 * FakeRender speaks the render control protocol: on connect a status and the
 * log backlog. Like the render, it maps on its own: stream(true) -> starting,
 * then showing the "Waze map" page at 30 updates/s; stream(false) -> idle. The map
 * stays when the connection closes. guide -> the status with the new
 * hide_bmw_guidance (none with `hide` null, as a tool before the switch).
 *
 * FakePatch speaks the patch daemon protocol: on connect the log backlog and
 * the current state; apply -> busy, then patched with the asked panel;
 * revert -> busy, then original; an Android Auto change -> busy, then
 * done after STEP_MS, or with `holdAa` set when the test calls
 * releaseAa(failure); status -> the current state; a command
 * while one runs -> refused, to its asker; reboot -> restarting, or with
 * `rebootFails` set, failed with that code. The map settings per driver
 * profile as patchd keeps them: `map` the settings in effect, `shared` and
 * `own` (by id) the rest, `current` the profile last told; profile and
 * profile-own answer as patchd does (profile-own refused for another id), or
 * not at all with `profileMute` set (a patchd before profiles); cluster,
 * hud, hud-size and hud-pos change `map`.
 *
 * FakeLogs speaks the logs socket: on connect its state; save -> saving,
 * then the new archive first in the list (or `failWith` as the error).
 *
 * Every command received is kept in `received`.
 */
const EventEmitter = require('events');
const fs = require('fs');
const net = require('net');

const STEP_MS = 40;
const BACKLOG = 60;
const RENDER_PAGE = 'Waze map';
const RENDER_FPS = '30 updates/s';

class LineServer extends EventEmitter {
    constructor(path) {
        super();
        this.path = path;
        this.received = [];
        this.backlog = [];
        this._clients = new Set();
        this._server = net.createServer((socket) => this._onClient(socket));
    }

    listen() {
        try {
            fs.unlinkSync(this.path);
        } catch (e) {
            /* no stale socket */
        }
        return new Promise((resolve) => this._server.listen(this.path, resolve));
    }

    close() {
        this.dropClients();
        return new Promise((resolve) => this._server.close(() => resolve()));
    }

    /* Drop every connection, as a service restart would. */
    dropClients() {
        this._clients.forEach((s) => s.destroy());
    }

    count(cmd) {
        return this.received.filter((m) => m.cmd === cmd).length;
    }

    log(line) {
        this.backlog.push(line);
        this.backlog = this.backlog.slice(-BACKLOG);
        this._broadcast({ ev: 'log', line });
    }

    _broadcast(obj) {
        this._clients.forEach((s) => this._send(s, obj));
    }

    _send(socket, obj) {
        if (!socket.destroyed) {
            socket.write(JSON.stringify(obj) + '\n');
        }
    }

    _onClient(socket) {
        this._clients.add(socket);
        let buf = '';
        socket.setEncoding('utf8');
        socket.on('data', (chunk) => {
            buf += chunk;
            let nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
                const line = buf.slice(0, nl);
                buf = buf.slice(nl + 1);
                if (line.trim()) {
                    this._onCommand(JSON.parse(line), socket);
                }
            }
        });
        socket.on('error', () => {});
        socket.on('close', () => this._onClose(socket));
        this._onConnect(socket);
    }

    /* subclasses override */
    _onConnect() {}
    _onClose() {}
    _onCommand() {}
}

class FakeRender extends LineServer {
    constructor(path) {
        super(path);
        this.state = 'idle';
        this.target = null;
        this.page = null;
        this.reason = null;
        this.hide = 1;
        this.guideDisabled = 0;
        /* the HUD: off by default; its state while on (hudState null: as the map) */
        this.hud = 0;
        this.cluster = 1;
        this.hudState = null;
        /* the HUD map kept shown over BMW's pop-ups (null: a tool without the field) */
        this.hudKeep = 0;
        this._showTimer = null;
    }

    announce(state, reason) {
        this._set(state, this.target, null, reason);
    }

    /* The CarPlay stream starts (its first decoded frame) or stops. */
    stream(flowing) {
        clearTimeout(this._showTimer);
        if (flowing) {
            this._set('starting', 'sail', null, null);
            this._showTimer = setTimeout(() => this._set('showing', 'sail', RENDER_PAGE, RENDER_FPS), STEP_MS);
        } else {
            this._set('idle', 'sail', null, '');
        }
    }

    _status() {
        const s = { ev: 'status', state: this.state, target: this.target, page: this.page, reason: this.reason };
        /* hud null: a tool without the HUD's fields (one refused by its head unit) */
        if (this.hud !== null) {
            Object.assign(s, { cluster: this.cluster, hud: this.hud, hud_state: this.hud ? this.hudState || this.state : 'off',
                hud_width_pct: this.hudWidth || 0, hud_x: this.hudX || 0, hud_y: this.hudY || 0 });
            if (this.hudKeep !== null) s.hud_keep_shown = this.hudKeep;
        }
        if (this.hide !== null) {
            s.hide_bmw_guidance = this.hide;
            s.guide_disabled = this.guideDisabled;
        }
        return s;
    }

    _set(state, target, page, reason) {
        this.state = state;
        this.target = target;
        this.page = page;
        this.reason = reason;
        this._broadcast(this._status());
    }

    _onConnect(socket) {
        this._send(socket, this._status());
        this.backlog.forEach((line) => this._send(socket, { ev: 'log', line }));
    }

    _onClose(socket) {
        this._clients.delete(socket);
    }

    _onCommand(msg) {
        this.received.push(msg);
        this.emit('command', msg);
        this.log(`control: ${JSON.stringify(msg)}`);
        if (msg.cmd === 'guide' && this.hide !== null && (msg.hide === 0 || msg.hide === 1)) {
            this.hide = msg.hide;
            this._broadcast(this._status());
        }
        if (msg.cmd === 'cluster' && msg.on !== undefined) {
            this.cluster = Number(msg.on);
            this._broadcast(this._status());
        }
        if (msg.cmd === 'hud') {
            if (msg.on !== undefined) this.hud = Number(msg.on);
            if (msg.hud_width_pct !== undefined) this.hudWidth = Number(msg.hud_width_pct);
            /* held inside the HUD, as the render does, when hudMaxX is set */
            const lim = (v) => (this.hudMaxX ? Math.max(-this.hudMaxX, Math.min(this.hudMaxX, v)) : v);
            if (msg.hud_x !== undefined) this.hudX = lim(Number(msg.hud_x));
            if (msg.hud_y !== undefined) this.hudY = Number(msg.hud_y);
            if (msg.hud_keep_shown !== undefined && this.hudKeep !== null) this.hudKeep = Number(msg.hud_keep_shown);
            this._broadcast(this._status());
        }
    }
}

class FakePatch extends LineServer {
    constructor(path) {
        super(path);
        this.state = 'original';   /* original | patched | unknown | error */
        this.panel = null;
        this.res = null;    /* the advertised geometry patchd reports: "WxH" */
        this.safe = null;   /* and its safe area: "WxH+X+Y" */
        this.carplay = null;  /* 'deferred' once a patch is written but not active */
        this.pending = null;  /* 'apply' | 'revert': the change waiting for a head-unit start */
        this.conflict = null; /* why the install left carplay as it was */
        this.recovered = null; /* boot recovery removed the patch after that many failed starts */
        this.removable = null;   /* Remove patch would change carplay; null: whether one of ours is on disk, undefined: a daemon that does not send it */
        this.busy = false;
        this.reason = null;
        this.aa = { state: 'original', density: 180, densities: [150, 400, 10], removable: false };   /* Android Auto's patch */
        this.reboots = 0;     /* reboot commands honoured */
        this.restart = { state: 'idle' };
        this.wifi = null;          /* CarPlay's Wi-Fi band: { band: '5' | '2.4', iface, freq } or null */
        this.rebootFails = null;   /* 'refused' | 'stayed' | 'error': the restart fails so */
        this.mute = false;         /* commands received and not answered */
        this.holdAa = false;       /* an Android Auto change stays busy until releaseAa(failure) */
        this.releaseAa = null;
        this.map = { cluster: 1, hud: 0, hud_width_pct: 0, hud_offset_x: 0, hud_offset_y: 0, hud_keep_shown: 0 };
        this.shared = null;
        this.own = {};
        this.active = null;
        this.current = undefined;
        this.profileMute = false;
        this._rebooting = false;
        this._timer = null;
    }

    announce(state, panel, reason, carplay) {
        this.state = state;
        this.panel = panel;
        this.reason = reason;
        this.carplay = carplay || null;
        this._broadcast(this._status());
    }

    _status() {
        return { ev: 'status', state: this.busy ? 'busy' : this.state, panel: this.panel,
            res: this.res, safe: this.safe, carplay: this.carplay, reason: this.reason,
            pending: this.pending, conflict: this.conflict, recovered: this.recovered, aa: this.aa,
            removable: this.removable !== null ? this.removable : this.pending === 'apply' || (!this.pending && ['patched', 'unknown', 'error'].includes(this.state)),
            busy: this.busy ? 'carplay' : this.aa.state === 'busy' ? 'aa' : this._rebooting ? 'restart' : null,
            restart: this.restart, wifi: this.wifi };
    }

    /* patchd's profileCmd: the settings in effect saved into the entry they
     * belong to, the profile's own or the shared ones put in effect. */
    _profile(msg, socket) {
        const id = msg.id == null ? null : msg.id;
        if (msg.cmd === 'profile-own' && id !== this.current) {
            this._send(socket, { ev: 'profile-own-reply', ok: false, reason: 'not the driver profile in effect' });
            return;
        }
        if (msg.cmd === 'profile') this.current = id;
        if (this.active) this.own[this.active] = this.map; else this.shared = this.map;
        if (msg.cmd === 'profile-own') {
            if (msg.on) this.own[id] = this.own[id] || this.map;
            else delete this.own[id];
        }
        const target = id && this.own[id] ? id : null;
        const want = target ? this.own[target] : this.shared;
        const changed = JSON.stringify(want) !== JSON.stringify(this.map);
        this.active = target;
        this.map = want;
        this._send(socket, { ev: `${msg.cmd}-reply`, ok: true, id, own: !!target, changed, settings: want });
    }

    /* CarPlay's Wi-Fi band changes, as patchd sends it: a status. */
    setWifi(wifi) {
        this.wifi = wifi;
        this._broadcast(this._status());
    }

    _onConnect(socket) {
        this.backlog.forEach((line) => this._send(socket, { ev: 'log', line }));
        this._send(socket, this._status());
    }

    _onClose(socket) {
        this._clients.delete(socket);
    }

    _onCommand(msg, socket) {
        this.received.push(msg);
        this.emit('command', msg);
        if (this.mute) {
            return;
        }
        if (msg.cmd === 'status') {
            this._broadcast(this._status());
            return;
        }
        if (msg.cmd === 'profile' || msg.cmd === 'profile-own') {
            if (!this.profileMute) this._profile(msg, socket);
            return;
        }
        const MAP = { cluster: ['cluster'], hud: ['hud'], 'hud-size': ['hud_width_pct'], 'hud-pos': ['hud_offset_x', 'hud_offset_y'],
            'hud-keep-shown': ['hud_keep_shown'] };
        if (MAP[msg.cmd]) {
            const v = msg.cmd === 'hud-pos' ? [msg.hud_x, msg.hud_y] : [msg.cmd === 'hud-size' ? msg.hud_width_pct : msg.on];
            this.map = Object.assign({}, this.map);
            MAP[msg.cmd].forEach((k, i) => { this.map[k] = v[i]; });
            return;
        }
        if (msg.cmd === 'reboot') {
            const deny = this._rebooting ? 'a restart is already in progress'
                : this.carplay !== 'deferred' && !this.aa.pending ? 'no restart is pending' : null;
            if (deny) { this._send(socket, { ev: 'reboot-reply', ok: false, reason: deny }); return; }
            this._rebooting = true;
            this.reboots++;
            this.restart = { state: 'restarting' };
            this._send(socket, { ev: 'reboot-reply', ok: true });
            this._broadcast(this._status());
            if (this.rebootFails) {
                this._rebooting = false;
                this.restart = { state: 'failed', code: this.rebootFails };
                if (this.rebootFails === 'stayed') this.restart.possibleAt = Date.now() + 10 * 60000;
                this._send(socket, { ev: 'reboot-reply', ok: false, reason: this.rebootFails });
                this._broadcast(this._status());
            }
            return;
        }
        if (this.busy || this.aa.state === 'busy' || this._rebooting) {
            this._send(socket, { ev: 'refused', cmd: msg.cmd, reason: 'another operation is running' });
            return;
        }
        clearTimeout(this._timer);
        if (/^aa-/.test(msg.cmd)) {
            const density = msg.cmd === 'aa-density' ? msg.value : this.aa.density;
            const card = msg.cmd === 'aa-card' ? msg.value : this.aa.card || 'shown';
            const after = msg.cmd === 'aa-revert' ? { state: 'original' } : { state: 'patched', dpi: density, diskCard: card };
            const before = this.aa;
            this.aa = Object.assign({}, this.aa, { state: 'busy', reason: msg.cmd === 'aa-revert' ? 'reverting' : 'applying' });
            this._broadcast(this._status());
            /* failure: the change failed so, the values on disk as they were */
            const finish = (failure) => {
                this.aa = failure ? Object.assign({}, before, { state: 'error', reason: failure, error: failure })
                    : { state: after.state, dpi: after.dpi, diskCard: after.diskCard, density, card, densities: this.aa.densities, aapd: 'restarted', removable: after.state === 'patched' };
                this.log(msg.cmd + ' done');
                this._broadcast(this._status());
            };
            if (this.holdAa) this.releaseAa = finish;
            else this._timer = setTimeout(finish, STEP_MS);
        } else if (msg.cmd === 'apply') {
            this.busy = true;
            this.reason = 'applying IC_' + String(msg.panel).toUpperCase();
            this.log(this.reason);
            this._broadcast(this._status());
            this._timer = setTimeout(() => {
                this.busy = false;
                this.state = 'patched';
                this.panel = msg.panel;
                /* The geometry the patcher writes: the high panel's size and
                 * safe area; the mid panel keeps the stock geometry. */
                this.res = msg.panel === 'high' ? '1088x480' : null;
                this.safe = msg.panel === 'high' ? '504x368+286+72' : null;
                this.reason = 'restarted';
                this.log('apply done');
                this._broadcast(this._status());
            }, STEP_MS);
        } else if (msg.cmd === 'revert') {
            this.busy = true;
            this.reason = 'reverting';
            this.log(this.reason);
            this._broadcast(this._status());
            this._timer = setTimeout(() => {
                this.busy = false;
                this.state = 'original';
                this.panel = null;
                this.res = null;
                this.safe = null;
                this.reason = 'restarted';
                this.log('revert done');
                this._broadcast(this._status());
            }, STEP_MS);
        }
    }
}

class FakeLogs extends LineServer {
    constructor(path) {
        super(path);
        this.url = 'http://172.16.222.97:8475/0123456789abcdef0123456789abcdef/';
        this.archives = [];
        this.saving = false;
        this.error = null;
        this.saved = null;
        this.failWith = null;
    }

    state() {
        return { ev: 'state', url: this.url, saving: this.saving, error: this.error, saved: this.saved,
            archives: this.archives };
    }

    announce() {
        this._broadcast(this.state());
    }

    _onConnect(socket) {
        this._send(socket, this.state());
    }

    _onClose(socket) {
        this._clients.delete(socket);
    }

    _onCommand(msg) {
        this.received.push(msg);
        this.emit('command', msg);
        if (msg.cmd !== 'save' || this.saving) {
            return;
        }
        this.saving = true;
        this.error = null;
        this.announce();
        setTimeout(() => {
            this.saving = false;
            this.error = this.failWith;
            if (!this.failWith) {
                this.saved = `woc-logs-20261103T07${String(this.archives.length).padStart(2, '0')}00Z.tar.gz`;
                this.archives = [{ name: this.saved, size: 412 * 1024, mtime: Date.now() }].concat(this.archives).slice(0, 5);
            }
            this.announce();
        }, STEP_MS);
    }
}

module.exports = { FakeRender, FakePatch, FakeLogs, RENDER_PAGE, RENDER_FPS };

if (require.main === module) {
    const render = new FakeRender(process.argv[2] || '/tmp/woc-render.sock');
    const patch = new FakePatch(process.argv[3] || '/tmp/woc-patch.sock');
    render.on('command', (m) => console.log(`render <- ${JSON.stringify(m)}`));
    patch.on('command', (m) => console.log(`patch  <- ${JSON.stringify(m)}`));
    render.log('fake render tool started');
    patch.log('fake patch daemon started');
    process.stdin.on('data', () => render.stream(render.state === 'idle'));
    Promise.all([render.listen(), patch.listen()])
        .then(() => console.log(`fake render on ${render.path}\nfake patch on ${patch.path}\n` +
            'Enter starts or stops the CarPlay stream'));
}
