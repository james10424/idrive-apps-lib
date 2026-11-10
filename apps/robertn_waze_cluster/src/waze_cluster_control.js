// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/**
 * waze_cluster_control - the app's client of the two native waze-on-cluster
 * sockets. No OAP runtime: the app opens both while one of its pages is
 * visible, closes them when it is hidden, and renders the status texts and
 * the can*() flags.
 *
 * LineSocket holds the connection: a Unix stream socket, newline-delimited
 * JSON, one object per line, with retry while open. One drives the render
 * control socket, one the root patch daemon.
 *
 *   Render control socket (RENDER_PATH)
 *     app  -> tool  {"cmd":"guide","hide":0|1}  BMW's turn box hidden while the map shows, or not
 *                   {"cmd":"cluster"|"hud","on":"0"|"1"}, {"cmd":"hud","hud_width_pct"|"hud_x","hud_y"|"hud_keep_shown"}
 *                    the map's settings (patchd saves them)
 *     tool -> app   {"ev":"status","state":"idle"|"starting"|"showing"|"frozen"|"error"|"unsupported",
 *                    "target","page","reason","hide_bmw_guidance","guide_disabled","cluster","hud",
 *                    "hud_state","hud_width_pct","hud_x","hud_y","hud_keep_shown"}
 *                   (hud_keep_shown 0|1, absent from a tool before it)
 *                   (unsupported: the head unit refuses the tool, which stays idle)
 *                   (idle's reason: why a live stream shows no map; hide_bmw_guidance 0|1,
 *                    absent from a tool before the switch; guide_disabled 1: the cluster
 *                    renderer keeps BMW's turn box shown whatever the switch)
 *                   {"ev":"log"}, {"ev":"slots"}   ignored here
 *
 *   Patch daemon socket (PATCH_PATH; root helper, group tm_oap_app)
 *     app    -> daemon  {"cmd":"status"}
 *                       {"cmd":"apply","panel":"high","mode":"avb"}
 *                       {"cmd":"revert"}
 *                       {"cmd":"aa-apply"} | {"cmd":"aa-revert"}
 *                       {"cmd":"aa-density","value":n} | {"cmd":"aa-card","value":"shown"|"hidden"}
 *                       {"cmd":"reboot"}  activate a deferred change, CarPlay's or
 *                        Android Auto's, with a normal head-unit restart
 *     daemon -> app     {"ev":"status","state":"original"|"patched"|"unknown"|
 *                        "busy"|"error","panel":"high"|"mid"|null,"mode","res","safe",
 *                        "carplay","reason","pending","conflict","recovered","busy","restart","aa","wifi"}
 *                       res "WxH" and safe "WxH+X+Y": the advertised size and
 *                       its safe area (a daemon before the safe area omits safe)
 *                       carplay "deferred": the patch is written but takes effect
 *                       only at the next head-unit start (a restart is needed)
 *                       pending "apply" | "revert": which change waits for it
 *                       conflict: why the install left carplay as it was
 *                       recovered: boot recovery removed the patch (after that
 *                        many failed head-unit starts); also in aa
 *                       aa: Android Auto's patch, {"state":"original"|"patched"|
 *                        "unknown"|"absent"|"busy"|"error","reason","conflict",
 *                        "pending","aapd","dpi","diskCard","runDpi","runCard","density",
 *                        "card","densities":[min,max,step],"changeAt","error"}
 *                        (dpi and diskCard on disk; runDpi and runCard what runs
 *                        while a change is pending; changeAt: when a density or
 *                        card change is taken again; error: the last change failed)
 *                       busy: "carplay" | "aa" | "restart" while an operation runs
 *                       restart: {"state":"idle"|"restarting"|"failed","code",
 *                        "possibleAt"} (code refused | stayed | error; possibleAt:
 *                        when the restart limit takes a request again)
 *                       wifi: {"band":"5"|"2.4","iface","freq"} while CarPlay runs
 *                        over Wi-Fi and its band is known, else null (absent from
 *                        a daemon before it)
 *                       {"ev":"reboot-reply","ok":true|false,"reason"}
 *                       {"ev":"refused","cmd","reason"}  a command not taken
 *                       {"ev":"capture","capture":{"state","error"}}
 *                       {"ev":"log"}  ignored here
 *
 *   Pending markers (PENDING_PATH for CarPlay, AA_PENDING_PATH for Android
 *     Auto; world-readable JSON {"action":"apply"|"revert","boot"}) written with
 *     the change, so a patch the install wrote is shown before the next
 *     head-unit start starts the daemon. One with an earlier boot's id is
 *     done; one without a boot id (an older patchd's) counts.
 *
 * The render runs from boot and maps the map into the native central-map slot
 * whenever the CarPlay stream flows, and takes it down when the stream stops,
 * whether or not this app is open; its status says which. CarPlay's Patch button applies
 * the one patch configuration (high panel, the resolution SIZE the render
 * displays with its guidance safe area SAFE, the stock media-bus sink), and
 * Remove patch reverts it, which is what turns the map off. Installing the tool
 * writes the same patch, which takes effect at the next head-unit start. The
 * patch daemon is a new root service that may not be running until then, so
 * an unreachable patch socket is reported, not an error, and retried like the
 * render socket.
 */
'use strict';

const EventEmitter = require('events');
const fs = require('fs');
const net = require('net');
const { t } = require('./i18n.js');

const RENDER_PATH = '/dev/shm/waze-on-cluster/ctl.sock';
const PATCH_PATH = '/dev/shm/waze-on-cluster/patch.sock';
const PENDING_PATH = '/dev/shm/waze-on-cluster/activation-pending';
const AA_PENDING_PATH = '/dev/shm/waze-on-cluster/aa-pending';
const BOOT_ID_PATH = '/proc/sys/kernel/random/boot_id';
const RETRY_MS = 1000;
/* One Zoom in or Zoom out press, in dpi: the densities' own step. */
const ZOOM_STEP = 10;
/* Why a restart failed, by patchd's code (English keys of src/i18n.js). */
const RESTART_FAILED = { refused: 'not accepted', stayed: 'the head unit stayed on', error: 'command failed' };

/* The native central-map slot's size, which the patch advertises to the phone. */
const SIZE = '1088x480';
/* The safe area CarPlay's Patch button writes: where the phone keeps its guidance UI,
 * inside the part of the map the gauges leave visible. */
const SAFE = '504x368+286+72';
/* The HUD map's size: HUD_STEP_PCT steps from HUD_MIN_PCT to HUD_MAX_PCT (85 %, the widest the HUD shows
 * whole), HUD_SIZE_STEPS of them. Standard (0) counts as HUD_STANDARD_PCT, about what the fitted box is. */
const HUD_MIN_PCT = 30, HUD_MAX_PCT = 85, HUD_STEP_PCT = 5, HUD_STANDARD_PCT = 57;
const HUD_SIZE_STEPS = (HUD_MAX_PCT - HUD_MIN_PCT) / HUD_STEP_PCT;
/* Where the map can show, the choices in their order on the page. */
const SHOWN_ON = ['cluster', 'hud', 'both', 'off'];
/* The HUD map moved from its centre, HUD_MOVE_PX a press, within +-HUD_MOVE_MAX; the render holds it inside the
 * HUD and reports the offset as far as it went. An asked size or place shows until the render's status has it,
 * HUD_ASKED_MS has passed, the render reconnects or patchd fails to save the size; then the render's own. */
const HUD_MOVE_PX = 10, HUD_MOVE_MAX = 400, HUD_ASKED_MS = 300;
const MOVES = { left: [-1, 0], right: [1, 0], up: [0, -1], down: [0, 1] };

/* One newline-delimited-JSON stream socket, reconnecting while open.
 * link: 'down' | 'up' | 'absent' (ENOENT) | 'refused' (ECONNREFUSED) | 'failed'.
 * Emits 'connect' and 'close'; each parsed object goes to onMessage(msg). */
class LineSocket extends EventEmitter {
    constructor(options) {
        super();
        const o = options || {};
        this.path = o.path;
        this.retryMs = o.retryMs || RETRY_MS;
        this._log = o.log || { info() {}, warn() {} };
        this._onMessage = o.onMessage || function () {};
        this.link = 'down';
        this.linkCode = null;
        this._open = false;
        this._socket = null;
        this._buf = '';
        this._retryTimer = null;
    }

    open() {
        if (!this._open) {
            this._open = true;
            this._connect();
        }
    }

    close() {
        this._open = false;
        clearTimeout(this._retryTimer);
        this._retryTimer = null;
        if (this._socket) {
            /* forgotten here, so its late events cannot touch a new connection */
            this._socket.end();
            this._socket = null;
        }
        this.link = 'down';
    }

    connected() {
        return this.link === 'up';
    }

    send(obj) {
        if (this._socket && this.link === 'up') {
            this._socket.write(JSON.stringify(obj) + '\n');
        }
    }

    _connect() {
        if (!this._open || this._socket) {
            return;
        }
        const socket = net.connect(this.path);
        this._socket = socket;
        this._buf = '';
        socket.setEncoding('utf8');
        socket.on('connect', () => {
            if (this._socket === socket) {
                this._log.info(`connected to ${this.path}`);
                this.link = 'up';
                this.linkCode = null;
                this.emit('connect');
            }
        });
        socket.on('data', (chunk) => {
            if (this._socket === socket) {
                this._onData(chunk);
            }
        });
        socket.on('error', (err) => {
            if (this._socket !== socket) {
                return;
            }
            const code = (err && err.code) || 'ERROR';
            this.link = code === 'ENOENT' ? 'absent' : code === 'ECONNREFUSED' ? 'refused' : 'failed';
            this.linkCode = code;
            this._log.warn(`${this.path}: ${code}`);
        });
        socket.on('close', () => {
            if (this._socket !== socket) {
                return;
            }
            this._socket = null;
            if (this.link === 'up') {
                this._log.info(`${this.path}: connection closed`);
                this.link = 'refused';
            }
            this.emit('close');
            this._retryTimer = setTimeout(() => {
                this._retryTimer = null;
                this._connect();
            }, this.retryMs);
        });
    }

    _onData(chunk) {
        this._buf += chunk;
        let nl;
        while ((nl = this._buf.indexOf('\n')) >= 0) {
            const line = this._buf.slice(0, nl).trim();
            this._buf = this._buf.slice(nl + 1);
            if (!line) {
                continue;
            }
            let msg = null;
            try {
                msg = JSON.parse(line);
            } catch (e) {
                this._log.warn(`bad line from ${this.path}: ${line.slice(0, 80)}`);
            }
            if (msg) {
                this._onMessage(msg, line);
            }
        }
    }
}

/* patchd 0.8.0's Android Auto status for a command it turned away: busy or
 * an error, with the refusal's reason. */
function olderAaRefusal(msg) {
    const aa = msg.aa;
    return !!aa && (aa.state === 'busy' || aa.state === 'error') &&
        /^(changed moments ago|another operation|a restart is in progress|patchd is stopping|density must|card must)/.test(aa.reason || '');
}

/* A head unit that refuses every part of the tool (patchd's head-unit
 * conflict): not supported whatever is on disk, and no Patch button. */
function headUnitRefused(conflict) {
    return /^not supported on this car \(head unit:/.test(conflict || '');
}

/* A patch state the conflict keeps from being applied, in the words both
 * sections use. */
function conflictText(conflict) {
    return /^not supported|^not patchable|^no Android Auto/.test(conflict) ? t('Not supported on this car')
        : /unreadable/.test(conflict) ? t('Unavailable')
        : /^untested/.test(conflict) ? t('Untested version')
        : t('Changed by another tool');
}

/* Where a render status shows the map: 'cluster', 'hud', 'both', 'off'
 * (neither), or null while it has not said. */
function shownOnOf(rs) {
    const said = (v) => v === 0 || v === 1;
    if (!said(rs.cluster) || !said(rs.hud)) {
        return null;
    }
    const cluster = rs.cluster === 1, hud = rs.hud === 1;
    return cluster && hud ? 'both' : cluster ? 'cluster' : hud ? 'hud' : 'off';
}

/* After a zoom or turn-card value that waits for the next head-unit start. */
function afterRestart(waits) {
    return waits ? ` ${t('(after restart)')}` : '';
}

class WazeClusterControl extends EventEmitter {
    /* options: renderPath, patchPath, pendingPath, aaPendingPath, bootIdPath,
     * retryMs, log ({info, warn}). Emits 'change' whenever a status text or
     * a can*() flag may have changed. */
    constructor(options) {
        super();
        const o = options || {};
        this._log = o.log || { info() {}, warn() {} };
        this._pendingPath = o.pendingPath || PENDING_PATH;
        this._aaPendingPath = o.aaPendingPath || AA_PENDING_PATH;
        this._bootIdPath = o.bootIdPath || BOOT_ID_PATH;
        this._lastPatchCmd = 'apply';
        this._restartRequested = false;   /* a restart was asked for; waiting for the unit to go down */
        this._wakeTimer = null;
        this._forget();
        this.captureState = { state: 'unavailable' };
        this.renderStatus = null;   /* the render tool's last status while connected */

        this.render = new LineSocket({
            path: o.renderPath || RENDER_PATH,
            retryMs: o.retryMs,
            log: this._log,
            onMessage: (msg) => this._onRender(msg)
        });
        this.patch = new LineSocket({
            path: o.patchPath || PATCH_PATH,
            retryMs: o.retryMs,
            log: this._log,
            onMessage: (msg) => this._onPatch(msg)
        });

        /* a new connection's status decides: an asked change it never answered is dropped */
        const reset = () => {
            this.renderStatus = null;
            this._askedShownOn = null;
            this._clearAskedHud();
            this.emit('change');
        };
        this.render.on('connect', reset);
        this.render.on('close', reset);
        this.patch.on('connect', () => { this._forget(); this.emit('change'); });
        this.patch.on('close', () => { this._forget(); this._restartRequested = false; this.emit('change'); });
    }

    /* Open both sockets, each reconnecting while open. */
    open() {
        this.render.open();
        this.patch.open();
    }

    /* Close both; the tools keep running. */
    close() {
        clearTimeout(this._askedPosTimer);
        this._askedPos = null;
        this._clearAskedHud();
        this.render.close();
        this.patch.close();
        this.renderStatus = null;
        this._forget();
        clearTimeout(this._wakeTimer);
        this.emit('change');
    }

    connectedRender() {
        return this.render.connected();
    }

    connectedPatch() {
        return this.patch.connected();
    }

    /* True while patchd runs an operation, CarPlay's, Android Auto's or a
     * restart, or a restart was asked for: no other is taken. */
    busy() {
        const ps = this.patchStatus;
        return this._restartRequested || !!(ps && (ps.busy || ps.state === 'busy' || (ps.aa && ps.aa.state === 'busy') ||
            (ps.restart && ps.restart.state === 'restarting')));
    }

    /* ---- CarPlay ---- */
    enableMap() {
        if (this.canEnable()) {
            this._lastPatchCmd = 'apply';
            /* The stock media-bus sink: the capture path the render joins. */
            this.patch.send({ cmd: 'apply', panel: 'high', mode: 'avb' });
        }
    }

    disableMap() {
        if (this.canDisable()) {
            this._lastPatchCmd = 'revert';
            this.patch.send({ cmd: 'revert' });
        }
    }

    /* ---- where the map shows: the cluster, the HUD (experimental), both or neither (Off) ---- */
    /* 'cluster', 'hud', 'both' or 'off': as just asked for until the render
     * reports it, else as the render last said; null while it has not. */
    shownOn() {
        return this._askedShownOn || (this.renderStatus && shownOnOf(this.renderStatus));
    }

    /* Whether the map shows on the cluster / the HUD; null while the render has not said. */
    clusterOn() {
        return this._shows('cluster');
    }

    hudOn() {
        return this._shows('hud');
    }

    _shows(display) {
        const rs = this.renderStatus;
        if (!this._askedShownOn && !(rs && (rs[display] === 0 || rs[display] === 1))) {
            return null;
        }
        const on = this.shownOn();
        return on === 'both' || on === display;
    }

    /* A choice goes to the render and is saved by patchd: both connected. */
    canChooseShownOn() {
        return this.render.connected() && this.connectedPatch() && this.clusterOn() !== null && this.hudOn() !== null;
    }

    /* Shows the map on the cluster, the HUD, both or neither ('off': BMW's
     * own content on both): the render at once, patchd saves it. A display
     * turned on goes first, so a switch from one display to the other never
     * passes through neither. */
    showOn(choice) {
        if (!SHOWN_ON.includes(choice) || !this.canChooseShownOn() || choice === this.shownOn()) {
            return;
        }
        const want = { cluster: choice === 'cluster' || choice === 'both', hud: choice === 'hud' || choice === 'both' };
        const changes = ['cluster', 'hud'].filter((d) => this._shows(d) !== want[d]).sort((a, b) => want[b] - want[a]);
        this._askedShownOn = choice;
        changes.forEach((d) => {
            this.render.send({ cmd: d, on: want[d] ? '1' : '0' });
            this.patch.send({ cmd: d, on: want[d] ? 1 : 0 });
        });
        this.emit('change');
    }

    /* True while cluster-render's mapping guard keeps the HUD off. */
    hudDisabled() {
        const rs = this.renderStatus;
        return !!(rs && rs.hud_state === 'disabled');
    }

    hudText() {
        return t('HUD map: {0}', this._hudState());
    }

    _hudState() {
        const on = this.hudOn();
        if (this.render.link !== 'up' || on === null) {
            return t('Unavailable');
        }
        if (!on) {
            return t('Off');
        }
        if (this.hudDisabled()) {
            return t('Disabled after errors');
        }
        switch (this.renderStatus.hud_state) {
        case 'showing':
            return t('Showing');
        case 'starting':
            return t('Starting…');
        case 'error':
            return t('Error');
        case 'frozen':
            return t('Frozen');
        case 'off':
            return t('Off');
        default:
            return t('Waiting for the phone');
        }
    }

    /* ---- the HUD map's size ---- */
    hudWidth() {
        if (this._askedHud != null) {
            return this._askedHud;
        }
        const v = this.renderStatus && this.renderStatus.hud_width_pct;
        return typeof v === 'number' ? v : null;
    }

    hudSizeText() {
        const w = this.hudWidth();
        return t('Size: {0}', w == null ? '—' : w === 0 ? t('Standard') : t('{0} %', w));
    }

    /* The size and place change only while the HUD is on and not disabled. */
    canChangeHudSize() {
        return this.canChooseShownOn() && this.hudOn() === true && !this.hudDisabled();
    }

    /* The size as a slider position, 0 (HUD_MIN_PCT) to HUD_SIZE_STEPS
     * (HUD_MAX_PCT); Standard sits at the step at or below its ~57 %. Null
     * while the render has not said. */
    hudSizeStep() {
        const w = this.hudWidth();
        if (w == null) {
            return null;
        }
        const pct = Math.max(HUD_MIN_PCT, Math.min(HUD_MAX_PCT, w || HUD_STANDARD_PCT));
        return Math.floor((pct - HUD_MIN_PCT) / HUD_STEP_PCT);
    }

    /* The HUD map at a slider position: the render at once, patchd saves it. */
    setHudSizeStep(step) {
        if (!Number.isInteger(step) || step < 0 || step > HUD_SIZE_STEPS || !this.canChangeHudSize() || step === this.hudSizeStep()) {
            return;
        }
        const pct = HUD_MIN_PCT + step * HUD_STEP_PCT;
        this._askedHud = pct;
        clearTimeout(this._askedHudTimer);
        this._askedHudTimer = setTimeout(() => {
            this._askedHud = null;
            this.emit('change');
        }, HUD_ASKED_MS);
        this.render.send({ cmd: 'hud', hud_width_pct: String(pct) });
        this.patch.send({ cmd: 'hud-size', hud_width_pct: pct });
        this.emit('change');
    }

    _clearAskedHud() {
        clearTimeout(this._askedHudTimer);
        this._askedHud = null;
    }

    /* ---- where the HUD map is: moved from its centre, in HUD pixels ---- */
    hudOffset() {
        if (this._askedPos) {
            return this._askedPos;
        }
        const s = this.renderStatus;
        return s && typeof s.hud_x === 'number' ? [s.hud_x, s.hud_y] : null;
    }
    /* Moves it a step (left / right / up / down): the render moves it at once, patchd saves it. */
    moveHud(dir) {
        const d = MOVES[dir];
        const at = this.hudOffset();
        if (!d || !at || !this.canChangeHudSize()) {
            return;
        }
        const clamp = (v) => Math.max(-HUD_MOVE_MAX, Math.min(HUD_MOVE_MAX, v));
        this._setHudPos(clamp(at[0] + d[0] * HUD_MOVE_PX), clamp(at[1] + d[1] * HUD_MOVE_PX));
    }
    canCentreHud() {
        const at = this.hudOffset();
        return this.canChangeHudSize() && !!at && (at[0] !== 0 || at[1] !== 0);
    }
    centreHud() {
        if (this.canCentreHud()) {
            this._setHudPos(0, 0);
        }
    }
    _setHudPos(x, y) {
        this._askedPos = [x, y];
        clearTimeout(this._askedPosTimer);
        this._askedPosTimer = setTimeout(() => {
            this._askedPos = null;
            this.emit('change');
        }, HUD_ASKED_MS);
        this.render.send({ cmd: 'hud', hud_x: String(x), hud_y: String(y) });
        this.patch.send({ cmd: 'hud-pos', hud_x: x, hud_y: y });
        this.emit('change');
    }
    /* ---- the HUD map over BMW's pop-ups ---- */
    /* True while the HUD map stays shown over them, false while it gives way;
     * null while the render has not said. */
    hudKeepShown() {
        const v = this.renderStatus && this.renderStatus.hud_keep_shown;
        return v === 0 || v === 1 ? v === 1 : null;
    }
    /* Changes as the size and place do. */
    canKeepHud() {
        return this.canChangeHudSize() && this.hudKeepShown() !== null;
    }
    /* Flips it: the render at once, patchd saves it; the render's next status shows it. */
    toggleHudKeep() {
        if (this.canKeepHud()) {
            const keep = !this.hudKeepShown();
            this.render.send({ cmd: 'hud', hud_keep_shown: keep ? '1' : '0' });
            this.patch.send({ cmd: 'hud-keep-shown', on: keep ? 1 : 0 });
        }
    }

    hudPositionText() {
        const at = this.hudOffset();
        if (!at) {
            return t('Position: {0}', '—');
        }
        const [x, y] = at;
        if (!x && !y) {
            return t('Position: {0}', t('Centre'));
        }
        const h = x > 0 ? t('{0} px right', x) : x < 0 ? t('{0} px left', -x) : '';
        const v = y > 0 ? t('{0} px down', y) : y < 0 ? t('{0} px up', -y) : '';
        return t('Position: {0}', h && v ? t('{0}, {1}', h, v) : h || v);
    }

    /* CarPlay's Patch button is offered for an original (never patched, or reverted), an
     * unknown or failed state, or an outdated patch, with nothing pending. */
    canEnable() {
        const ps = this._idleStatus();
        return !!ps && !headUnitRefused(ps.conflict) &&
            (['original', 'unknown', 'error'].includes(ps.state) || (ps.state === 'patched' && this._outdated(ps)));
    }

    /* Remove patch is offered while something of ours is on disk, pending
     * or not (patchd's removable); a daemon that does not say, while
     * patched, partly patched or failed with nothing pending. */
    canDisable() {
        const ps = this.patchStatus;
        if (!this.connectedPatch() || !ps || this.busy()) {
            return false;
        }
        return ps.removable !== undefined ? ps.removable : !this.pending() && ['patched', 'unknown', 'error'].includes(ps.state);
    }

    /* The daemon's status while a CarPlay change can start, else null. */
    _idleStatus() {
        const ps = this.patchStatus;
        return this.connectedPatch() && ps && !this.busy() && !this.pending() ? ps : null;
    }

    /* The change waiting for the next head-unit start: "apply", "revert",
     * "unknown" (a daemon that does not say which), or null. From the daemon,
     * else from its marker, which the install writes before the daemon runs. */
    pending() {
        const ps = this.patchStatus;
        if (this.patch.link === 'up') {
            return ps && ps.pending ? ps.pending : ps && ps.carplay === 'deferred' ? 'unknown' : null;
        }
        return this._marker(this._pendingPath);
    }

    statusText() {
        return t('Status: {0}', this._carplayState());
    }

    /* CarPlay's patch, in the words both sections use. */
    _carplayState() {
        const p = this.patch.link;
        if (p === 'down') {
            return t('Connecting…');
        }
        const pending = this.pending(), ps = this.patchStatus;
        if (pending === 'apply' && ps && headUnitRefused(ps.conflict)) {
            return conflictText(ps.conflict);
        }
        if (pending) {
            return pending === 'revert' ? t('Removed after restart') : t('Patched after restart');
        }
        if (p !== 'up' || !ps) {
            return p === 'up' ? t('Connecting…') : t('Unavailable');
        }
        if (ps.state === 'busy' || ps.busy === 'carplay') {
            return this._lastPatchCmd === 'revert' ? t('Removing patch…') : t('Patching…');
        }
        if (ps.state === 'error') {
            return t('Failed');
        }
        if (ps.recovered && ps.state !== 'patched' && !headUnitRefused(ps.conflict)) {
            return t('Removed — head unit kept restarting');
        }
        if (ps.conflict && (ps.state !== 'patched' || headUnitRefused(ps.conflict))) {
            return conflictText(ps.conflict);
        }
        if (ps.state === 'patched') {
            return this._outdated(ps) ? t('Update needed') : t('Patched');
        }
        return ps.state === 'original' ? t('Not patched') : t('Partly patched');
    }

    /* The cluster map row: the shared render's map, while CarPlay's or
     * Android Auto's patch is in effect: on disk with nothing pending, as
     * the last status outside a change said (a running or failed change
     * keeps it). */
    mapText() {
        return t('Cluster map: {0}', this._mapState());
    }

    _mapState() {
        if ((!this._inEffect.carplay && !this._inEffect.androidAuto) || this.clusterOn() === false) {
            return t('Off');
        }
        if (this.render.link !== 'up') {
            return t('Unavailable');
        }
        const rs = this.renderStatus;
        switch (rs && rs.state) {
        case 'starting':
            return t('Starting…');
        case 'showing':
            return t('Showing {0}', rs.page || t('Waze map')) + (rs.reason ? ` — ${rs.reason}` : '');
        case 'error':
            return t('Error');
        case 'frozen':
            return t('Frozen');
        case 'unsupported':
            return t('Unavailable');
        default:
            return (rs && rs.reason) || t('Waiting for the phone');
        }
    }

    /* A patch other than the one CarPlay's Patch button writes: another capture sink,
     * panel, size or safe area, from an older app. */
    _outdated(ps) {
        return !!((ps.mode && ps.mode !== 'avb') || (ps.panel && ps.panel !== 'high') ||
            (ps.res && ps.res !== SIZE) || (ps.safe && ps.safe !== SAFE));
    }

    /* ---- BMW's turn box ---- */
    /* Whether BMW's turn box is hidden while the map shows, as the render
     * last said; null while it has not (not connected, or a tool without
     * the switch). */
    hideGuidance() {
        const rs = this.renderStatus;
        return rs && (rs.hide_bmw_guidance === 0 || rs.hide_bmw_guidance === 1) ? rs.hide_bmw_guidance === 1 : null;
    }

    /* True while the cluster renderer keeps BMW's turn box shown whatever
     * the switch says. */
    guidanceUnavailable() {
        const rs = this.renderStatus;
        return !!(rs && rs.guide_disabled === 1);
    }

    /* Flips the turn box's setting; the render's next status shows it.
     * Nothing while it is unavailable. */
    toggleGuidance() {
        const hide = this.hideGuidance();
        if (hide !== null && !this.guidanceUnavailable()) {
            this.render.send({ cmd: 'guide', hide: hide ? 0 : 1 });
        }
    }

    /* ---- Android Auto ---- */
    aa() {
        const ps = this.patchStatus;
        return this.connectedPatch() && ps && ps.aa ? ps.aa : null;
    }

    _aaIdle() {
        const aa = this.aa();
        return !!aa && !this.busy() && !aa.pending;
    }

    /* A density or card change is refused until aa.changeAt. */
    _aaSpaced() {
        const aa = this.aa();
        return !!aa && aa.changeAt > Date.now();
    }

    /* A conflict patchd refuses Patch, Zoom and the turn card for: any but
     * an untested aapd build, which only Patch patches. */
    _aaBlocked() {
        const aa = this.aa();
        return !!aa && !!aa.conflict && !/^untested/.test(aa.conflict);
    }

    canEnableAa() {
        const aa = this.aa();
        return this._aaIdle() && !this._aaBlocked() && ['original', 'unknown', 'error'].includes(aa.state);
    }

    canDisableAa() {
        const aa = this.aa();
        if (!aa || this.busy()) {
            return false;
        }
        return aa.removable !== undefined ? aa.removable : this._aaIdle() && ['patched', 'unknown', 'error'].includes(aa.state);
    }

    /* Zoom out shows more map: a lower density, from the one on disk. */
    canZoom(step) {
        const aa = this.aa();
        if (!this._aaIdle() || this._aaSpaced() || this._aaBlocked() || aa.state !== 'patched' || !aa.densities || aa.dpi == null) {
            return false;
        }
        const next = aa.dpi + step;
        return next >= aa.densities[0] && next <= aa.densities[1];
    }

    canToggleCard() {
        const aa = this.aa();
        return this._aaIdle() && !this._aaSpaced() && !this._aaBlocked() && aa.state === 'patched' && !!aa.diskCard;
    }

    /* The density on disk, marked when the one aapd runs until the next
     * head-unit start differs; none without Android Auto's patch. */
    zoomText() {
        const aa = this.aa();
        if (!aa || aa.dpi == null) {
            return t('Zoom: {0}', '—');
        }
        return t('Zoom: {0}', t('{0} dpi', aa.dpi)) + afterRestart(aa.pending === 'apply' && aa.runDpi !== aa.dpi);
    }

    cardText() {
        const aa = this.aa();
        return t("Hide phone's turn card") + afterRestart(aa && aa.diskCard && aa.pending === 'apply' && aa.runCard !== aa.diskCard);
    }

    /* The turn card checkbox's tick: the card a change asked for while it
     * runs (the HMI ticks the box itself on a press), else the one on disk. */
    cardHidden() {
        const aa = this.aa();
        return this._cardAsked ? this._cardAsked === 'hidden' : !!aa && aa.diskCard === 'hidden';
    }

    enableAa() {
        if (this.canEnableAa()) {
            this.patch.send({ cmd: 'aa-apply' });
        }
    }

    disableAa() {
        if (this.canDisableAa()) {
            this.patch.send({ cmd: 'aa-revert' });
        }
    }

    zoom(step) {
        if (this.canZoom(step)) {
            this.patch.send({ cmd: 'aa-density', value: this.aa().dpi + step });
        }
    }

    toggleCard() {
        if (this.canToggleCard()) {
            this._cardAsked = this.aa().diskCard === 'hidden' ? 'shown' : 'hidden';
            this.patch.send({ cmd: 'aa-card', value: this._cardAsked });
        }
    }

    aaStatusText() {
        return t('Status: {0}', this._aaState());
    }

    /* Android Auto's patch, in the words both sections use. */
    _aaState() {
        const aa = this.aa();
        if (!aa) {
            if (this.patch.link === 'down') {
                return t('Connecting…');
            }
            const marker = this.connectedPatch() ? null : this._marker(this._aaPendingPath);
            return marker === 'apply' ? t('Patched after restart') : marker === 'revert' ? t('Removed after restart') : t('Unavailable');
        }
        if (aa.state === 'busy') {
            return aa.reason === 'reverting' ? t('Removing patch…') : t('Patching…');
        }
        if (aa.state === 'error' || aa.error) {
            return t('Failed');
        }
        if (aa.pending && !(aa.pending === 'apply' && headUnitRefused(aa.conflict))) {
            return aa.pending === 'revert' ? t('Removed after restart') : t('Patched after restart');
        }
        if (aa.recovered && aa.state !== 'patched' && !headUnitRefused(aa.conflict)) {
            return t('Removed — head unit kept restarting');
        }
        /* A conflict patchd refuses changes for is named also while patched,
         * since it greys Zoom and the turn card. */
        if (aa.state === 'absent' || (aa.conflict && (aa.state !== 'patched' || this._aaBlocked()))) {
            return conflictText(aa.conflict || 'no Android Auto');
        }
        return aa.state === 'patched' ? t('Patched') : aa.state === 'original' ? t('Not patched') : t('Partly patched');
    }

    /* ---- CarPlay's Wi-Fi band ---- */
    /* '5' or '2.4' while CarPlay runs over Wi-Fi and patchd knows its band, else null. */
    wifiBand() {
        const w = this.connectedPatch() && this.patchStatus && this.patchStatus.wifi;
        return w && (w.band === '5' || w.band === '2.4') ? w.band : null;
    }

    /* The band's row; empty while it is not known. */
    wifiText() {
        const band = this.wifiBand();
        return band ? t('CarPlay Wi-Fi: {0}', band === '5' ? t('5 GHz') : t('2.4 GHz')) : '';
    }

    /* The line under it on 2.4 GHz; empty otherwise. */
    wifiHint() {
        return this.wifiBand() === '2.4' ? t('Audio may cut out. On your iPhone, turn Wi-Fi off and on.') : '';
    }

    /* ---- restart ---- */
    /* True while the daemon reports a written change, CarPlay's or Android
     * Auto's, that takes effect only at the next head-unit start. On a unit
     * where the service restarts at once this never holds. */
    restartNeeded() {
        const ps = this.patchStatus;
        return !!(ps && (ps.carplay === 'deferred' || (ps.aa && (ps.aa.aapd === 'deferred' || ps.aa.pending))));
    }

    restartRequested() {
        return this._restartRequested;
    }

    /* Minutes until the restart limit takes a request again, or 0. */
    _restartWaitMin() {
        const r = this.patchStatus && this.patchStatus.restart;
        const ms = r && r.possibleAt ? r.possibleAt - Date.now() : 0;
        return ms > 0 ? Math.ceil(ms / 60000) : 0;
    }

    /* Restart head unit is taken: a change waits, nothing runs, the limit allows it. */
    canRestart() {
        return this.connectedPatch() && this.restartNeeded() && !this.busy() && !this._restartWaitMin();
    }

    /* The restart's own row: under way, failed, or held by the limit; empty
     * when there is nothing to say. */
    restartText() {
        const r = (this.patchStatus && this.patchStatus.restart) || {};
        if (this._restartRequested || r.state === 'restarting') {
            return t('Restart: {0}', t('In progress'));
        }
        const wait = this._restartWaitMin();
        const held = wait && this.restartNeeded();
        if (r.state === 'failed') {
            const failed = t('Failed, {0}', t(RESTART_FAILED[r.code] || RESTART_FAILED.error));
            return t('Restart: {0}', held ? t('{0} — possible in {1} min', failed, wait) : failed);
        }
        return held ? t('Restart: {0}', t('Possible in {0} min', wait)) : '';
    }

    requestRestart() {
        if (this.canRestart()) {
            this._restartRequested = true;
            this.patch.send({ cmd: 'reboot' });
            this.emit('change');
        }
    }

    /* ---- capture ---- */
    canCapture() {
        return this.connectedPatch() && !this._restartRequested && this.captureState.state !== 'closing';
    }

    capture() {
        if (this.canCapture()) {
            const active = ['opening', 'open'].includes(this.captureState.state);
            this.patch.send({ cmd: active ? 'capture-stop' : 'capture-start' });
        }
    }

    captureButtonText() {
        return ['opening', 'open'].includes(this.captureState.state) ? t('Stop capture') : t('Start capture');
    }

    captureText() {
        const s = this.captureState;
        const value = s.state === 'unavailable' ? t('Unavailable') : s.state === 'open' ? t('On') : s.state === 'opening' ? t('Starting…')
            : s.state === 'closing' ? t('Stopping…') : s.error ? t('Failed') : t('Off');
        return t('Capture: {0}', value);
    }

    /* ---- internals ---- */
    /* A pending marker's action, or null; one with an earlier boot's id is
     * done. /dev/shm starts empty at every boot, so one without a boot id is
     * this boot's. */
    _marker(file) {
        let marker = null;
        try {
            marker = JSON.parse(fs.readFileSync(file, 'utf8'));
        } catch (e) {
            return null;
        }
        if (!marker || (marker.action !== 'apply' && marker.action !== 'revert')) {
            return null;
        }
        const boot = this._readText(this._bootIdPath).trim();
        return marker.boot && boot && marker.boot !== boot ? null : marker.action;
    }

    _readText(file) {
        try {
            return fs.readFileSync(file, 'utf8');
        } catch (e) {
            return '';
        }
    }

    /* A change once a time a status named has passed: the zoom spacing's
     * end, or the next minute of the restart limit. */
    _wake() {
        clearTimeout(this._wakeTimer);
        const ps = this.patchStatus;
        const now = Date.now();
        const at = [ps && ps.aa && ps.aa.changeAt, ps && ps.restart && ps.restart.possibleAt]
            .filter((t) => t > now)
            .map((t) => (t - now > 60000 ? now + ((t - now) % 60000 || 60000) : t));
        if (at.length) {
            this._wakeTimer = setTimeout(() => {
                this.emit('change');
                this._wake();
            }, Math.min.apply(null, at) - now + 50);
        }
    }

    /* patchd 0.8.0, which has no busy, restart or refused: its bare busy
     * status while a restart runs or a command is turned away, and its error
     * status for a failed restart, are the restart's state, not CarPlay's;
     * an Android Auto command it turns away (as busy or error) leaves
     * Android Auto's state as it was. */
    _fromOlder(msg, prev) {
        if (msg.restart || !prev) {
            return msg;
        }
        if (prev.aa && olderAaRefusal(msg)) {
            return Object.assign({}, msg, { aa: prev.aa });
        }
        if (msg.state === 'busy' && !msg.aa && /restart|another operation|stopping/.test(msg.reason || '')) {
            return /restarting the head unit/.test(msg.reason) ? Object.assign({}, prev, { restart: { state: 'restarting' } }) : prev;
        }
        if (msg.state === 'error' && /^reboot failed/.test(msg.reason || '')) {
            return Object.assign({}, msg, { state: prev.state, reason: prev.reason, restart: { state: 'failed', code: 'error' } });
        }
        return msg;
    }

    /* No status from patchd: no patch known in effect, no turn card asked for. */
    _forget() {
        this.patchStatus = null;     /* the patch daemon's last status while connected */
        this._inEffect = { carplay: false, androidAuto: false };
        this._cardAsked = null;      /* the turn card an aa-card change asked for, while it runs */
    }

    /* From a status outside a change (neither busy nor an error): which
     * patch is in effect. */
    _noteInEffect() {
        const ps = this.patchStatus;
        if (ps.state !== 'busy' && ps.state !== 'error' && ps.busy !== 'carplay') {
            this._inEffect.carplay = ps.state === 'patched' && !this.pending();
        }
        const aa = ps.aa;
        if (aa && aa.state !== 'busy' && aa.state !== 'error') {
            this._inEffect.androidAuto = aa.state === 'patched' && !aa.pending;
        }
    }

    /* The asked turn card goes when its change ends: a status of Android
     * Auto that is not busy (the card on disk then shows, after a failure
     * too), or patchd 0.8.0's turn-away. */
    _noteCardAsked(msg) {
        if (msg.aa && (msg.aa.state !== 'busy' || olderAaRefusal(msg))) {
            this._cardAsked = null;
        }
    }

    _onRender(msg) {
        if (msg.ev === 'status') {
            this.renderStatus = msg;
            if (this._askedShownOn && this._askedShownOn === shownOnOf(msg)) {
                this._askedShownOn = null;
            }
            if (this._askedHud != null && msg.hud_width_pct === this._askedHud) {
                this._clearAskedHud();
            }
            if (this._askedPos && msg.hud_x === this._askedPos[0] && msg.hud_y === this._askedPos[1]) {
                this._askedPos = null;
            }
            this.emit('change');
        }
    }

    _onPatch(msg) {
        if (msg.ev === 'capture') {
            this.captureState = msg.capture;
            this.emit('change');
        } else if (msg.ev === 'status') {
            /* A map-only event (no aa) keeps the last Android Auto status. */
            const prev = this.patchStatus;
            const status = this._fromOlder(msg, prev);
            this.patchStatus = !status.aa && prev && prev.aa ? Object.assign({}, status, { aa: prev.aa }) : status;
            this._noteInEffect();
            this._noteCardAsked(msg);
            this._wake();
            this.emit('change');
        } else if (/^(cluster|hud|hud-size|hud-pos|hud-keep-shown)-reply$/.test(msg.ev)) {
            if (!msg.ok) {
                this._log.info(`patchd did not save the setting: ${msg.reason || 'refused'}`);
            }
        } else if (msg.ev === 'reboot-reply') {
            /* A refused or failed request clears the asked flag; the status
             * that follows says why. A taken one leaves it set until the unit
             * restarts. */
            if (!msg.ok) {
                this._restartRequested = false;
            }
            this.emit('change');
        } else if (msg.ev === 'refused') {
            this._log.info(`patchd refused ${msg.cmd}: ${msg.reason}`);
            if (msg.cmd === 'aa-card') {
                this._cardAsked = null;
                this.emit('change');
            }
        }
    }
}

module.exports = {
    WazeClusterControl, LineSocket,
    RENDER_PATH, PATCH_PATH, PENDING_PATH, AA_PENDING_PATH, BOOT_ID_PATH,
    SIZE, SAFE, ZOOM_STEP, RESTART_FAILED, SHOWN_ON, HUD_SIZE_STEPS
};
