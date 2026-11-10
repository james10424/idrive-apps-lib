// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Drives src/waze_cluster_control.js against tools/fakes.js.
 *
 *   node tools/test_control.js
 *
 * Covers: both sockets unreachable (the patch daemon not yet running);
 * CarPlay's Patch button / CarPlay's Remove patch button applying the one patch configuration; the map
 * following the stream with no command from the app, and an idle status
 * showing the stream's reason; a page opened while the
 * map is up reading its state, also when opened again;
 * the combined status line across patch and render state, ignoring extra
 * status fields and reading an unrecognised patch state as unknown; the
 * Patch button's prompt for a unit on an older safe area, and none from a daemon that
 * omits it; the status line a page renders on the link-up change, before the
 * first patch status arrives; the pending patch or removal (from the daemon,
 * or from its marker before the daemon runs) with Patch and Remove patch greyed; which
 * states offer Patch and Remove patch; a conflict the install left alone.
 * Android Auto: its line per state, the zoom and the phone's turn card with
 * the values on disk, marked only when the running one differs, none
 * without the patch, greyed through the spacing between changes, and its
 * pending patch from its marker before the daemon runs; markers from an
 * earlier boot are done. While any operation runs every operation is greyed
 * and sends nothing; a refused command changes neither section. A failed
 * restart: its own line and reason, the limit greying the button, CarPlay's
 * state as it was. The cluster map follows either side's patch in effect,
 * kept through a running or failed change; the turn card checkbox shows
 * the asked card while its change runs and the one on disk once it fails
 * or is refused. The capture's texts. The turn box switch: hidden by default, each toggle one guide command to the
 * render and the state as the render reports it, nothing known or sent
 * while closed or with a render that has no switch, nothing sent while the
 * cluster renderer holds the box shown. Where the map shows (the cluster,
 * the HUD, both or Off, from the cluster and from both, the HUD's size and
 * place refused while Off) and the HUD's size slider, place, Keep HUD map
 * over BMW's pop-ups and guard; an asked HUD size the render does not show goes after a moment,
 * on a failed save and on a render reconnect. CarPlay Wi-Fi: the band patchd reports, 5 GHz or 2.4 GHz, the
 * line under it on 2.4 GHz only, nothing while the band is unknown, from a
 * patchd before it, or with patchd gone.
 * The Logs page's client (src/logs_control.js): the socket absent before the
 * logs service runs, Save logs sending one save and the list, the address
 * and the status following the service, its errors, and closing.
 */
const assert = require('assert');
const fs = require('fs');
const { WazeClusterControl, SIZE, SAFE, ZOOM_STEP, HUD_SIZE_STEPS } = require('../src/waze_cluster_control.js');
const { LogsControl } = require('../src/logs_control.js');
const { FakeRender, FakePatch, FakeLogs, RENDER_PAGE, RENDER_FPS } = require('./fakes.js');

/* short: macOS limits a socket path to 104 bytes */
const RSOCK = `/tmp/woc-r-${process.pid}.sock`;
const PSOCK = `/tmp/woc-p-${process.pid}.sock`;
const PENDING = `/tmp/woc-pending-${process.pid}.json`;
const LSOCK = `/tmp/woc-l-${process.pid}.sock`;

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/* Resolves once check() holds, polling every 10 ms. */
function until(what, check) {
    const deadline = Date.now() + 1000;
    return new Promise((resolve, reject) => {
        const poll = () => {
            if (check()) {
                resolve();
            } else if (Date.now() > deadline) {
                reject(new Error(`timed out waiting for ${what}`));
            } else {
                setTimeout(poll, 10);
            }
        };
        poll();
    });
}

function control() {
    return new WazeClusterControl({ renderPath: RSOCK, patchPath: PSOCK, pendingPath: PENDING, retryMs: 100 });
}

const tests = [];
function test(name, fn) {
    tests.push({ name, fn });
}

test('patch daemon unreachable: reported, not an error', async () => {
    /* Neither socket exists — the patch service has not come up yet. */
    const c = control();
    c.open();
    await until('patch absent', () => c.patch.link === 'absent');
    assert.strictEqual(c.statusText(), 'Status: Unavailable');
    assert.strictEqual(c.connectedPatch(), false);
    assert.strictEqual(c.connectedRender(), false);
    c.close();
});

test('explicit capture and early stop follow capture status without applying a patch', async (render, patch) => {
    const c=control();c.open();await until('connected',()=>c.connectedPatch());
    c.capture();await until('start sent',()=>patch.count('capture-start')===1);
    patch._broadcast({ev:'capture',capture:{state:'open'}});
    await until('capture ready',()=>c.captureButtonText()==='Stop capture');
    assert.strictEqual(c.captureText(), 'Capture: On');
    c.capture();await until('stop sent',()=>patch.count('capture-stop')===1);
    assert.strictEqual(patch.count('apply'),0);
    patch._broadcast({ev:'capture',capture:{state:'restore_failed',error:'daemon unavailable'}});
    await until('restore failure visible',()=>c.captureText()==='Capture: Failed');
    c.close();
});

test('connected, original: not patched', async (render, patch) => {
    const c = control();
    c.open();
    await until('original', () => c.statusText() === 'Status: Not patched');
    assert.strictEqual(c.connectedPatch(), true);
    assert.strictEqual(c.connectedRender(), true);
    c.close();
});

test('CarPlay\'s Patch button applies the patch; patched line once done', async (render, patch) => {
    const c = control();
    c.open();
    await until('original', () => c.statusText() === 'Status: Not patched');
    c.enableMap();
    await until('applying', () => c.statusText() === 'Status: Patching…');
    assert.deepStrictEqual(patch.received.filter((m) => m.cmd === 'apply'), [{ cmd: 'apply', panel: 'high', mode: 'avb' }]);
    await until('patched', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    c.close();
});

test('A unit on the older mid-panel patch is prompted to CarPlay\'s Patch button', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.announce('patched', 'mid', null);
    await until('update patch', () => c.statusText() === 'Status: Update needed');
    c.enableMap();
    await until('apply high', () => patch.count('apply') === 1);
    assert.deepStrictEqual(patch.received.filter((m) => m.cmd === 'apply'), [{ cmd: 'apply', panel: 'high', mode: 'avb' }]);
    await until('patched', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    c.close();
});

test('the map follows the stream; the app sends the render nothing for it', async (render, patch) => {
    const c = control();
    const seen = [];
    c.on('change', () => seen.push(c.mapText()));
    c.open();
    await until('connected', () => c.connectedRender());
    patch.announce('patched', 'high', null);
    await until('patched idle', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    render.stream(true);
    await until('showing', () => c.mapText() === `Cluster map: Showing ${RENDER_PAGE} — ${RENDER_FPS}`);
    assert(seen.includes('Cluster map: Starting…'), `map lines rendered on change: ${JSON.stringify(seen)}`);
    render.stream(false);
    await until('stream stopped', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    render.announce('idle', 'Stream lost');
    await until('idle with the stream\'s reason', () => c.mapText() === 'Cluster map: Stream lost' && c.statusText() === 'Status: Patched');
    assert.deepStrictEqual(render.received, [], 'no command to the render');
    assert.strictEqual(c.show, undefined);
    assert.strictEqual(c.stop, undefined);
    /* The DLT capture window is the Capture button's alone. */
    assert(!patch.received.some((m) => m.cmd === 'capture-start'), 'the stream does not arm capture');
    assert.strictEqual(SIZE, '1088x480');
    c.close();
});

test('the turn box switch: hidden by default, toggled through the render, shown as the render says', async (render, patch) => {
    const c = control();
    c.open();
    await until('render status', () => c.hideGuidance() === true);
    c.toggleGuidance();
    await until('shown', () => c.hideGuidance() === false);
    assert.deepStrictEqual(render.received, [{ cmd: 'guide', hide: 0 }]);
    c.toggleGuidance();
    await until('hidden again', () => c.hideGuidance() === true);
    assert.deepStrictEqual(render.received[1], { cmd: 'guide', hide: 1 });
    c.close();
    assert.strictEqual(c.hideGuidance(), null, 'closed: not known');
    c.toggleGuidance();
    await sleep(50);
    assert.strictEqual(render.received.length, 2, 'nothing sent while closed');
});

test('the turn box held off by the cluster renderer: unavailable, nothing sent', async (render, patch) => {
    render.guideDisabled = 1;
    const c = control();
    c.open();
    await until('render status', () => c.renderStatus);
    assert.strictEqual(c.guidanceUnavailable(), true);
    c.toggleGuidance();
    await sleep(50);
    assert.deepStrictEqual(render.received, []);
    render.guideDisabled = 0;
    render.announce('idle', null);
    await until('available again', () => c.guidanceUnavailable() === false);
    c.close();
});

test('a render without the switch: not known, nothing sent', async (render, patch) => {
    render.hide = null;
    const c = control();
    c.open();
    await until('render status', () => c.renderStatus);
    assert.strictEqual(c.hideGuidance(), null);
    c.toggleGuidance();
    await sleep(50);
    assert.deepStrictEqual(render.received, []);
    c.close();
});

test('the cluster map: off unless CarPlay or Android Auto is patched with nothing pending, either one', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa() && c.connectedRender());
    const D = [150, 400, 10];
    patch.aa = { state: 'original', density: 180, densities: D };
    patch.announce('original', null, null);
    await until('both off', () => c.statusText() === 'Status: Not patched' && c.aaStatusText() === 'Status: Not patched');
    assert.strictEqual(c.mapText(), 'Cluster map: Off');
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, densities: D };
    patch.announce('original', null, null);
    await until('Android Auto alone', () => c.aaStatusText() === 'Status: Patched');
    assert.strictEqual(c.mapText(), 'Cluster map: Waiting for the phone');
    render.stream(true);
    await until('showing', () => c.mapText() === `Cluster map: Showing ${RENDER_PAGE} — ${RENDER_FPS}`);
    patch.aa = { state: 'unknown', pending: 'apply', aapd: 'deferred', dpi: 180, diskCard: 'shown', density: 180, densities: D };
    patch.announce('original', null, null);
    await until('pending', () => c.aaStatusText() === 'Status: Patched after restart');
    assert.strictEqual(c.mapText(), 'Cluster map: Off', 'nothing in effect while its change waits');
    c.close();
});

test('the cluster map stays while a change runs or fails on the side in effect, the other unpatched', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa() && c.connectedRender());
    const D = [150, 400, 10];
    const aaOn = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: D };
    const send = (extra) => patch._broadcast(Object.assign(patch._status(), extra));
    /* Android Auto alone */
    send({ state: 'original', aa: aaOn });
    await until('Android Auto alone', () => c.mapText() === 'Cluster map: Waiting for the phone');
    send({ state: 'original', busy: 'aa', aa: Object.assign({}, aaOn, { state: 'busy', reason: 'applying' }) });
    await until('busy', () => c.aaStatusText() === 'Status: Patching…');
    assert.strictEqual(c.mapText(), 'Cluster map: Waiting for the phone', 'Android Auto busy');
    send({ state: 'original', aa: Object.assign({}, aaOn, { state: 'error', reason: 'Command failed' }) });
    await until('failed', () => c.aaStatusText() === 'Status: Failed');
    assert.strictEqual(c.mapText(), 'Cluster map: Waiting for the phone', 'Android Auto failed, no status after it');
    /* CarPlay alone */
    send({ state: 'patched', panel: 'high', aa: { state: 'original', density: 180, densities: D } });
    await until('CarPlay alone', () => c.aaStatusText() === 'Status: Not patched' && c.statusText() === 'Status: Patched');
    assert.strictEqual(c.mapText(), 'Cluster map: Waiting for the phone');
    send({ state: 'busy', busy: 'carplay', aa: { state: 'original', density: 180, densities: D } });
    await until('CarPlay busy', () => c.statusText() === 'Status: Patching…');
    assert.strictEqual(c.mapText(), 'Cluster map: Waiting for the phone', 'CarPlay busy');
    send({ state: 'error', reason: 'remount rw: EBUSY', aa: { state: 'original', density: 180, densities: D } });
    await until('CarPlay failed', () => c.statusText() === 'Status: Failed');
    assert.strictEqual(c.mapText(), 'Cluster map: Waiting for the phone', 'CarPlay failed');
    send({ state: 'original', aa: { state: 'original', density: 180, densities: D } });
    await until('both off', () => c.mapText() === 'Cluster map: Off');
    c.close();
});

test('the turn card checkbox: the asked card until its change ends; the one on disk after a failure or a refusal', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const D = [150, 400, 10];
    /* an earlier change failed: patchd keeps its error on every status, busy ones too */
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: D, error: 'saving the choice failed' };
    patch.holdAa = true;
    patch.announce('patched', 'high', null);
    await until('shown', () => c.canToggleCard() && c.cardHidden() === false);
    c.toggleCard();
    await until('busy', () => c.aa().state === 'busy');
    await sleep(50);
    assert.deepStrictEqual([c.aa().diskCard, c.cardHidden()], ['shown', true], 'the asked card while the change runs, the earlier error notwithstanding');
    patch.releaseAa();
    await until('done', () => c.aa().state === 'patched' && c.aa().diskCard === 'hidden');
    assert.strictEqual(c.cardHidden(), true, 'the card on disk once it ends');
    /* a change that fails: the card on disk again */
    c.toggleCard();
    await until('busy again', () => c.aa().state === 'busy');
    assert.strictEqual(c.cardHidden(), false, 'asked: shown');
    patch.releaseAa('writing aa-crop.env: EACCES');
    await until('failed', () => c.aa().state === 'error');
    assert.strictEqual(c.cardHidden(), true, 'failed: hidden, as on disk');
    patch.holdAa = false;
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'hidden', density: 180, card: 'hidden', densities: D };
    patch.announce('patched', 'high', null);
    await until('idle', () => c.canToggleCard() && !c.aa().error);
    /* a refusal: the card on disk */
    patch.busy = true;
    c.toggleCard();
    assert.strictEqual(c.cardHidden(), false);
    await until('refused', () => c.cardHidden() === true);
    patch.busy = false;
    /* patchd 0.8.0 turns it away with a busy Android Auto status */
    patch.mute = true;
    c.toggleCard();
    assert.strictEqual(c.cardHidden(), false);
    await sleep(50);
    patch._broadcast(Object.assign(patch._status(), { restart: undefined, busy: undefined,
        aa: { state: 'busy', reason: 'another operation is running', density: 180, densities: D } }));
    await until('0.8.0 refusal', () => c.cardHidden() === true);
    patch.mute = false;
    c.close();
});

test('where the map shows: the cluster by default; the HUD and both through the render, patchd saves each change; Off for neither', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender() && c.connectedPatch());
    patch.res = SIZE;
    patch.safe = SAFE;
    patch.announce('patched', 'high', null);
    render.stream(true);
    await until('the cluster', () => c.shownOn() === 'cluster' && /^Cluster map: Showing/.test(c.mapText()) && c.hudText() === 'HUD map: Off');
    assert(c.canChooseShownOn() && !c.canChangeHudSize() && !c.canCentreHud(), 'only the choice while the HUD is off');
    c.setHudSizeStep(6);
    c.moveHud('left');
    c.showOn('cluster');
    c.showOn('neither');
    assert.deepStrictEqual(render.received, [], 'nothing sent: the HUD off, the choice as it is, no such choice');
    c.showOn('hud');
    assert.deepStrictEqual([c.shownOn(), c.clusterOn(), c.hudOn()], ['hud', false, true], 'chosen at once, before the render answers');
    await until('the HUD alone', () => render.hud === 1 && render.cluster === 0 && c.renderStatus.hud === 1 &&
        c.hudText() === 'HUD map: Showing' && c.mapText() === 'Cluster map: Off');
    assert.deepStrictEqual(render.received, [{ cmd: 'hud', on: '1' }, { cmd: 'cluster', on: '0' }], 'the HUD on before the cluster off');
    assert(c.canChangeHudSize(), 'the size and place while on');
    c.showOn('both');
    await until('both', () => render.cluster === 1 && c.shownOn() === 'both' && /^Cluster map: Showing/.test(c.mapText()));
    c.showOn('cluster');
    await until('the cluster again', () => render.hud === 0 && c.shownOn() === 'cluster' && c.hudText() === 'HUD map: Off');
    assert.deepStrictEqual(render.received.slice(2), [{ cmd: 'cluster', on: '1' }, { cmd: 'hud', on: '0' }], 'only what changes');
    await until('each saved', () => patch.received.filter((m) => /^(cluster|hud)$/.test(m.cmd)).length === 4);
    assert.deepStrictEqual(patch.received.filter((m) => /^(cluster|hud)$/.test(m.cmd)),
        [{ cmd: 'hud', on: 1 }, { cmd: 'cluster', on: 0 }, { cmd: 'cluster', on: 1 }, { cmd: 'hud', on: 0 }]);
    /* Off: the cluster off, the HUD stays off, nothing else sent; the stock map back */
    render.received.length = 0;
    patch.received.length = 0;
    c.showOn('off');
    assert.deepStrictEqual([c.shownOn(), c.clusterOn(), c.hudOn()], ['off', false, false], 'Off at once, before the render answers');
    await until('off', () => render.cluster === 0 && render.hud === 0 && c.renderStatus.cluster === 0 && c.mapText() === 'Cluster map: Off');
    assert.deepStrictEqual(render.received, [{ cmd: 'cluster', on: '0' }], 'only the cluster off');
    await until('off saved', () => patch.received.some((m) => m.cmd === 'cluster'));
    assert.deepStrictEqual(patch.received.filter((m) => /^(cluster|hud)$/.test(m.cmd)), [{ cmd: 'cluster', on: 0 }]);
    assert(c.hudText() === 'HUD map: Off' && c.canChooseShownOn() && !c.canChangeHudSize() && !c.canCentreHud(), 'Off: the choice, not the HUD\'s size and place');
    c.showOn('off');
    c.setHudSizeStep(6);
    c.moveHud('left');
    await sleep(50);
    assert.deepStrictEqual(render.received, [{ cmd: 'cluster', on: '0' }], 'Off again, the HUD\'s size and place: nothing sent');
    /* both, then Off from both: each display off, both saved */
    c.showOn('both');
    await until('both from off', () => render.cluster === 1 && render.hud === 1 && c.shownOn() === 'both' && c.renderStatus.hud === 1);
    render.received.length = 0;
    patch.received.length = 0;
    c.showOn('off');
    await until('off from both', () => render.cluster === 0 && render.hud === 0 && c.renderStatus.cluster === 0 && c.renderStatus.hud === 0 &&
        c.shownOn() === 'off' && c.mapText() === 'Cluster map: Off' && c.hudText() === 'HUD map: Off');
    assert.deepStrictEqual(render.received, [{ cmd: 'cluster', on: '0' }, { cmd: 'hud', on: '0' }], 'both off');
    await until('both saved', () => patch.received.filter((m) => /^(cluster|hud)$/.test(m.cmd)).length === 2);
    assert.deepStrictEqual(patch.received.filter((m) => /^(cluster|hud)$/.test(m.cmd)), [{ cmd: 'cluster', on: 0 }, { cmd: 'hud', on: 0 }]);
    c.showOn('cluster');
    await until('the cluster from off', () => render.cluster === 1 && render.hud === 0 && c.shownOn() === 'cluster' &&
        /^Cluster map: Showing/.test(c.mapText()));
    /* a render on neither from its own configuration reads as Off, a choice still to make */
    render.cluster = 0;
    render.announce(render.state, render.reason);
    await until('neither', () => c.shownOn() === 'off' && c.canChooseShownOn());
    c.close();
    assert(!c.canChooseShownOn(), 'no choice while the render is not connected');
    assert.deepStrictEqual([c.shownOn(), c.hudText()], [null, 'HUD map: Unavailable']);
});

test('the HUD disabled by the cluster renderer: Disabled after errors, the size and place greyed, the choice kept', async (render, patch) => {
    render.hud = 1;
    render.hudState = 'disabled';
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender() && c.renderStatus);
    render.stream(true);
    await until('disabled', () => c.hudText() === 'HUD map: Disabled after errors');
    assert(c.shownOn() === 'both' && c.canChooseShownOn() && !c.canChangeHudSize() && !c.canCentreHud());
    c.setHudSizeStep(6);
    assert(!render.received.some((m) => m.cmd === 'hud'), 'no size sent while disabled');
    c.showOn('cluster');
    await until('the cluster', () => render.hud === 0 && c.shownOn() === 'cluster');
    c.showOn('both');
    await until('both again', () => render.received.filter((m) => m.cmd === 'hud').length === 2 && render.hud === 1);
    await until('still disabled', () => c.hudText() === 'HUD map: Disabled after errors');
    c.close();
});

test('the HUD map size as a slider: 5 % steps from 30 to 85 %, Standard at the step below its ~57 %', async (render, patch) => {
    render.hud = 1;
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender() && c.connectedPatch());
    render.stream(true);
    await until('on', () => c.renderStatus && c.canChangeHudSize());
    assert.deepStrictEqual([c.hudSizeText(), c.hudSizeStep()], ['Size: Standard', 5], 'Standard at 55 %');
    c.setHudSizeStep(6);
    assert.deepStrictEqual([c.hudSizeText(), c.hudSizeStep()], ['Size: 60 %', 6], 'shown at once');
    await until('60 %', () => render.hudWidth === 60 && c.renderStatus.hud_width_pct === 60);
    c.setHudSizeStep(HUD_SIZE_STEPS);
    await until('85 %', () => c.renderStatus.hud_width_pct === 85 && c.hudSizeStep() === HUD_SIZE_STEPS);
    c.setHudSizeStep(0);
    await until('30 %', () => c.renderStatus.hud_width_pct === 30 && c.hudSizeText() === 'Size: 30 %');
    const sent = render.received.length;
    [-1, HUD_SIZE_STEPS + 1, 2.5, '3', 0].forEach((step) => c.setHudSizeStep(step));
    assert.strictEqual(render.received.length, sent, 'nothing sent past either end, between steps or for the same step');
    await until('each one saved', () => patch.received.filter((m) => m.cmd === 'hud-size').map((m) => m.hud_width_pct).join() === '60,85,30');
    c.close();
    assert.deepStrictEqual([c.hudSizeText(), c.hudSizeStep()], ['Size: —', null], 'nothing known while closed');
});

test('the HUD map over BMW\'s pop-ups: gives way by default; a flip goes to the render and patchd, the render\'s status shows it; ' +
    'greyed while the HUD is off and for a render without it', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender() && c.connectedPatch());
    render.stream(true);
    await until('the HUD off', () => c.renderStatus && c.hudOn() === false);
    assert.deepStrictEqual([c.hudKeepShown(), c.canKeepHud()], [false, false], 'giving way, greyed while the HUD is off');
    c.toggleHudKeep();
    render.hud = 1;
    render.announce(render.state, render.reason);
    await until('on', () => c.canKeepHud());
    assert.strictEqual(render.received.filter((m) => m.hud_keep_shown !== undefined).length, 0, 'nothing sent while greyed');
    c.toggleHudKeep();
    await until('kept shown', () => c.hudKeepShown() === true && render.hudKeep === 1);
    await until('saved', () => patch.map.hud_keep_shown === 1);
    c.toggleHudKeep();
    await until('gives way again', () => c.hudKeepShown() === false && patch.map.hud_keep_shown === 0);
    assert.deepStrictEqual(render.received.filter((m) => m.hud_keep_shown !== undefined), [{ cmd: 'hud', hud_keep_shown: '1' }, { cmd: 'hud', hud_keep_shown: '0' }]);
    assert.deepStrictEqual(patch.received.filter((m) => m.cmd === 'hud-keep-shown'), [{ cmd: 'hud-keep-shown', on: 1 }, { cmd: 'hud-keep-shown', on: 0 }]);
    render.hudKeep = null;
    render.announce(render.state, render.reason);
    await until('a render without it', () => c.hudKeepShown() === null && !c.canKeepHud());
    c.close();
});

test('an asked HUD size the render does not show: the render\'s own after a moment, at once on a render reconnect; ' +
    'the same step taken again', async (render, patch) => {
    render.hud = 1;
    render.hudWidth = 50;
    const c = control();
    c.open();
    await until('on', () => c.renderStatus && c.canChangeHudSize() && c.hudSizeText() === 'Size: 50 %');
    /* a render that takes no size */
    const take = render._onCommand.bind(render);
    render._onCommand = (msg) => (msg.cmd === 'hud' && msg.hud_width_pct !== undefined ? render.received.push(msg) : take(msg));
    const asked = () => render.received.filter((m) => m.cmd === 'hud' && m.hud_width_pct !== undefined).length;
    c.setHudSizeStep(6);
    assert.strictEqual(c.hudSizeText(), 'Size: 60 %', 'shown at once');
    await until('the render\'s own again', () => c.hudSizeText() === 'Size: 50 %');
    c.setHudSizeStep(6);
    assert.strictEqual(c.hudSizeText(), 'Size: 60 %');
    await until('the same step taken again', () => asked() === 2);
    let t0 = Date.now();
    render.dropClients();
    await until('the render reconnecting: nothing asked kept', () => c.hudSizeText() === 'Size: —');
    assert(Date.now() - t0 < 250, 'at once, not after the wait');
    await until('the render\'s own after it reconnects', () => c.hudSizeText() === 'Size: 50 %');
    c.setHudSizeStep(6);
    await until('taken again after the reconnect', () => asked() === 3);
    c.close();
    assert.deepStrictEqual([c.hudSizeText(), c.hudSizeStep()], ['Size: —', null], 'nothing asked kept while closed');
});

test('the HUD map position: 10 px a press each way, Centre back; the render moves it and patchd saves it', async (render, patch) => {
    render.hud = 1;
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender() && c.connectedPatch());
    render.stream(true);
    await until('on', () => c.renderStatus && c.canChangeHudSize());
    assert.strictEqual(c.hudPositionText(), 'Position: Centre');
    assert(!c.canCentreHud(), 'Centre greyed while centred');
    c.moveHud('right'); c.moveHud('right'); c.moveHud('up');
    await until('moved', () => c.renderStatus.hud_x === 20 && c.renderStatus.hud_y === -10);
    assert.strictEqual(c.hudPositionText(), 'Position: 20 px right, 10 px up');
    assert(c.canCentreHud());
    c.moveHud('down'); c.moveHud('down'); c.moveHud('left');
    await until('moved back past the centre', () => c.renderStatus.hud_x === 10 && c.renderStatus.hud_y === 10);
    await until('saved', () => { const p = patch.received.filter((m) => m.cmd === 'hud-pos').slice(-1)[0]; return p && p.hud_x === 10 && p.hud_y === 10; });
    c.centreHud();
    await until('centred', () => c.renderStatus.hud_x === 0 && c.renderStatus.hud_y === 0);
    assert.strictEqual(c.hudPositionText(), 'Position: Centre');
    for (let i = 0; i < 50; i++) c.moveHud('left');
    await until('held at the limit', () => c.renderStatus.hud_x === -400);
    /* the render holds the map inside the HUD: past its edge, the place it reports */
    render.hudMaxX = 60;
    c.centreHud();
    await until('centred again', () => c.renderStatus.hud_x === 0);
    for (let i = 0; i < 9; i++) c.moveHud('right');
    assert.strictEqual(c.hudPositionText(), 'Position: 90 px right', 'as asked at first');
    await until('as far as it goes', () => c.hudPositionText() === 'Position: 60 px right');
    c.moveHud('left');
    await until('the next press moves it back at once', () => c.renderStatus.hud_x === 50);
    c.close();
});

test('a page opened while the map is up reads it, again after closing', async (render, patch) => {
    patch.res = SIZE;
    patch.safe = SAFE;
    patch.announce('patched', 'high', null);
    render.stream(true);
    await until('fake showing', () => render.state === 'showing');
    const c = control();
    c.open();
    await until('showing', () => c.mapText() === `Cluster map: Showing ${RENDER_PAGE} — ${RENDER_FPS}`);
    c.close();
    await until('closed', () => render._clients.size === 0);
    c.open();
    await until('showing again', () => c.mapText() === `Cluster map: Showing ${RENDER_PAGE} — ${RENDER_FPS}`);
    c.close();
});

test('Starting map while the render is mapping', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender());
    patch.announce('patched', 'high', null);
    render.announce('starting', null);
    await until('starting', () => c.mapText() === 'Cluster map: Starting…');
    c.close();
});

test('a frozen stream: the map line says so', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedRender());
    patch.announce('patched', 'high', null);
    render.announce('frozen', "the phone's cluster stream froze");
    await until('frozen', () => c.mapText() === 'Cluster map: Frozen');
    c.close();
});

test('CarPlay\'s Remove patch button reverts', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.announce('patched', 'high', null);
    await until('patched', () => c.statusText() === 'Status: Patched');
    c.disableMap();
    await until('reverting', () => c.statusText() === 'Status: Removing patch…');
    assert.strictEqual(patch.count('revert'), 1);
    await until('original', () => c.statusText() === 'Status: Not patched');
    c.close();
});

test('status line on the link-up change, before the first patch status', async (render, patch) => {
    const c = control();
    /* The page renders on every change. The link-up change fires with the last
     * patch status cleared and the new one not yet arrived. */
    const seen = [];
    c.on('change', () => seen.push(c.statusText()));
    c.open();
    await until('connected', () => c.connectedPatch());
    assert(seen.includes('Status: Connecting…') && !seen.some((l) => /enable|reboot/i.test(l)),
        `status lines rendered on change: ${JSON.stringify(seen)}`);
    c.close();
});

test('extra status fields are ignored; an unrecognised state is not shown as patched', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch() && c.connectedRender());
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', selfstart: 'patched', edits: [1, 2, 3, 4] });
    await until('patched', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    /* e.g. a binary patched before the self-start edit, reported under a new state name */
    patch._broadcast({ ev: 'status', state: 'partial', panel: 'high', mode: 'udp' });
    await until('needs enable', () => c.statusText() === 'Status: Partly patched');
    /* written but deferred to the next head-unit start: show the restart-needed
     * line with the daemon's reason, not "enable" */
    patch._broadcast({ ev: 'status', state: 'unknown', panel: 'high', mode: 'udp', carplay: 'deferred', reason: 'patch written; takes effect at the next head-unit start' });
    await until('restart needed', () => c.statusText() === 'Status: Patched after restart');
    /* patched onto another sink by an older app: the stream never reaches the
     * render, so CarPlay's Patch button is prompted instead of the render line */
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'udp' });
    await until('update capture', () => c.statusText() === 'Status: Update needed');
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', res: '720x320' });
    await until('update resolution', () => c.statusText() === 'Status: Update needed');
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', res: '1088x480' });
    await until('patched avb', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    c.close();
});

test('an older full-frame safe area prompts CarPlay\'s Patch button, which writes the default', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch() && c.connectedRender());
    /* 1088x480 with the whole frame as the safe area: the guidance banner
     * lands under the gauges, so CarPlay's Patch button is prompted. */
    patch.res = '1088x480';
    patch.safe = '1088x480+0+0';
    patch.announce('patched', 'high', null);
    await until('update safe area', () => c.statusText() === 'Status: Update needed');
    c.enableMap();
    await until('applied', () => patch.count('apply') === 1 && !patch.busy);
    assert.strictEqual(patch.safe, SAFE);
    await until('current', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    /* A daemon that omits safe (before the safe area) is taken as current. */
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', res: '1088x480' });
    await until('older daemon', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone' && c.patchStatus.safe === undefined);
    assert.strictEqual(SAFE, '504x368+286+72');
    c.close();
});

test('installed, before the head-unit start: the marker shows the pending patch, no buttons', async () => {
    /* The install wrote the patch; the daemon starts at the next head-unit
     * start, so only its marker says so. */
    fs.writeFileSync(PENDING, JSON.stringify({ action: 'apply', reason: 'patch written; takes effect at the next head-unit start' }) + '\n');
    const c = control();
    c.open();
    await until('patch absent', () => c.patch.link === 'absent');
    assert.strictEqual(c.statusText(), 'Status: Patched after restart');
    assert.strictEqual(c.canEnable(), false);
    assert.strictEqual(c.canDisable(), false);
    assert.strictEqual(c.restartNeeded(), false, 'no daemon to reboot through');
    fs.writeFileSync(PENDING, JSON.stringify({ action: 'revert' }) + '\n');
    assert.strictEqual(c.statusText(), 'Status: Removed after restart');
    fs.unlinkSync(PENDING);
    assert.strictEqual(c.statusText(), 'Status: Unavailable');
    c.close();
});

test('a pending apply from the daemon: its line, a restart, Patch greyed and Remove patch offered; a pending removal greys both', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.pending = 'apply';
    patch.announce('unknown', 'high', 'patch written; takes effect at the next head-unit start', 'deferred');
    await until('pending', () => c.statusText() === 'Status: Patched after restart');
    assert.strictEqual(c.canEnable(), false);
    assert.strictEqual(c.canDisable(), true);
    assert.strictEqual(c.restartNeeded(), true);
    patch.pending = 'revert';
    patch.announce('unknown', null, 'patch removed; the original carplay runs from the next head-unit start', 'deferred');
    await until('removal pending', () => c.statusText() === 'Status: Removed after restart');
    assert.strictEqual(c.canEnable(), false);
    assert.strictEqual(c.canDisable(), false);
    c.requestRestart();
    await until('restarting', () => c.restartText() === 'Restart: In progress');
    c.close();
});

test('CarPlay\'s Patch button only for original, unknown or outdated; CarPlay\'s Remove patch button only for a patch', async (render, patch) => {
    const c = control();
    c.open();
    await until('original', () => c.statusText() === 'Status: Not patched');
    assert.deepStrictEqual([c.canEnable(), c.canDisable()], [true, false], 'original');
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', res: SIZE, safe: SAFE });
    await until('patched', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    assert.deepStrictEqual([c.canEnable(), c.canDisable()], [false, true], 'patched');
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'mid', mode: 'avb', res: null, safe: null });
    await until('outdated', () => c.statusText() === 'Status: Update needed');
    assert.deepStrictEqual([c.canEnable(), c.canDisable()], [true, true], 'outdated patch');
    patch._broadcast({ ev: 'status', state: 'unknown', panel: 'high', reason: 'self-start edit missing (older patch); CarPlay\'s Patch button applies it' });
    await until('unknown', () => c.statusText() === 'Status: Partly patched');
    assert.deepStrictEqual([c.canEnable(), c.canDisable()], [true, true], 'unknown');
    patch._broadcast({ ev: 'status', state: 'busy', reason: 'applying' });
    await until('busy', () => c.statusText() === 'Status: Patching…');
    assert.deepStrictEqual([c.canEnable(), c.canDisable()], [false, false], 'busy');
    c.close();
});

test('a conflict the install left alone is shown, and CarPlay\'s Patch button stays the user\'s choice', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.conflict = 'carplay.service starts /bin/sh (/etc/systemd/system/carplay.service.d/60-other.conf)';
    patch.announce('original', null, null);
    await until('conflict', () => c.statusText() === 'Status: Changed by another tool');
    assert.strictEqual(c.canEnable(), true);
    patch.conflict = 'not patchable: self-start: onSuggestUi call not found';
    patch.announce('unknown', null, null);
    await until('unknown conflict', () => c.statusText() === 'Status: Not supported on this car');
    assert.strictEqual(c.canEnable(), true);
    c.close();
});

test('a head unit that refuses the tool: not supported in both sections, also patched or with an apply pending; no Patch, zoom or card; the map unavailable; the turn box and the HUD greyed', async (render, patch) => {
    const NOT = 'not supported on this car (head unit: unknown variant is not supported (no hwvariant= on the kernel command line))';
    const D = [150, 400, 10];
    render.hide = null;
    render.guideDisabled = 1;
    render.hud = null;
    const c = control();
    c.open();
    await until('connected', () => c.aa() && c.connectedRender());
    render.announce('unsupported', NOT);
    patch.conflict = NOT;
    patch.aa = { state: 'original', density: 180, densities: D, conflict: NOT };
    patch.announce('original', null, null);
    await until('not supported', () => c.statusText() === 'Status: Not supported on this car' && c.aaStatusText() === 'Status: Not supported on this car');
    assert.deepStrictEqual([c.canEnable(), c.canDisable(), c.canEnableAa(), c.canDisableAa()], [false, false, false, false]);
    assert.strictEqual(c.mapText(), 'Cluster map: Off');
    assert.strictEqual(c.hideGuidance(), null, 'the turn box row is greyed');
    /* A patch still on disk until boot recovery restores it: still not
     * supported, Remove patch offered, the map unavailable. */
    patch.res = SIZE;
    patch.safe = SAFE;
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, densities: D, conflict: NOT };
    patch.announce('patched', 'high', null);
    await until('patched, not supported', () => c.mapText() === 'Cluster map: Unavailable');
    assert.deepStrictEqual([c.statusText(), c.aaStatusText()], ['Status: Not supported on this car', 'Status: Not supported on this car']);
    assert.deepStrictEqual([c.canEnable(), c.canDisable(), c.canEnableAa(), c.canDisableAa()], [false, true, false, true]);
    assert.deepStrictEqual([c.canZoom(-ZOOM_STEP), c.canZoom(ZOOM_STEP), c.canToggleCard()], [false, false, false]);
    /* A patch written before, waiting for the start that boot recovery
     * removes it at: not supported, the restart still offered. */
    patch.pending = 'apply';
    patch.aa = { state: 'unknown', pending: 'apply', aapd: 'deferred', density: 180, densities: D, conflict: NOT };
    patch.announce('unknown', null, null, 'deferred');
    await until('pending, not supported', () => c.canRestart());
    assert.deepStrictEqual([c.statusText(), c.aaStatusText()], ['Status: Not supported on this car', 'Status: Not supported on this car']);
    patch.pending = null;
    assert.strictEqual(c.hudText(), 'HUD map: Unavailable');
    assert.deepStrictEqual([c.canChooseShownOn(), c.canChangeHudSize(), c.canCentreHud()], [false, false, false],
        'the choice and the HUD rows greyed');
    c.enableMap();
    c.toggleGuidance();
    c.showOn('hud');
    c.setHudSizeStep(6);
    c.moveHud('left');
    await sleep(50);
    assert.strictEqual(patch.count('apply'), 0);
    assert.strictEqual(patch.count('cluster') + patch.count('hud') + patch.count('hud-size') + patch.count('hud-pos'), 0, 'nothing saved');
    assert.deepStrictEqual(render.received, []);
    c.close();
});

test('a patch boot recovery removed says so until its Patch button applies it again', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.recovered = 2;
    patch.announce('original', null, null);
    await until('removed', () => c.statusText() === 'Status: Removed — head unit kept restarting');
    assert.deepStrictEqual([c.canEnable(), c.canDisable()], [true, false]);
    c.enableMap();
    await until('sent', () => patch.received.some((m) => m.cmd === 'apply'));
    patch.recovered = null;
    patch.announce('patched', 'high', null);
    await until('patched', () => c.statusText() === 'Status: Patched');
    patch.aa = { state: 'original', density: 180, densities: [150, 400, 10], recovered: 1 };
    patch.announce('patched', 'high', null);
    await until('aa removed', () => c.aaStatusText() === 'Status: Removed — head unit kept restarting');
    assert.strictEqual(c.canEnableAa(), true);
    patch.aa = { state: 'patched', dpi: 180, density: 180, densities: [150, 400, 10] };
    patch.announce('patched', 'high', null);
    await until('aa patched', () => c.aaStatusText() === 'Status: Patched');
    /* aapd restored, its aapd.conf left patched: still the restore */
    patch.aa = { state: 'unknown', density: 180, densities: [150, 400, 10], recovered: 1 };
    patch.announce('patched', 'high', null);
    await until('aa partly', () => c.aaStatusText() === 'Status: Removed — head unit kept restarting');
    assert.strictEqual(c.canEnableAa(), true);
    /* A head unit that refuses the tool, after an earlier restore: not supported first */
    const NOT = 'not supported on this car (head unit: unknown variant is not supported (no hwvariant= on the kernel command line))';
    patch.recovered = 2;
    patch.conflict = NOT;
    patch.aa = { state: 'original', density: 180, densities: [150, 400, 10], recovered: 1, conflict: NOT };
    patch.announce('original', null, null);
    await until('refused first', () => c.statusText() === 'Status: Not supported on this car' && c.aaStatusText() === 'Status: Not supported on this car');
    assert.deepStrictEqual([c.canEnable(), c.canEnableAa()], [false, false]);
    c.close();
});

test('a deferred patch offers a restart; one press reboots once', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    assert.strictEqual(c.restartNeeded(), false, 'no restart before a deferred patch');
    patch.announce('unknown', 'high', 'patch written; takes effect at the next head-unit start', 'deferred');
    await until('restart needed', () => c.restartNeeded());
    assert.strictEqual(c.statusText(), 'Status: Patched after restart');
    c.requestRestart();
    await until('reboot sent', () => patch.count('reboot') === 1);
    await until('restarting', () => c.restartText() === 'Restart: In progress');
    assert.strictEqual(c.restartRequested(), true);
    /* A second press does not send another reboot. */
    c.requestRestart();
    await sleep(50);
    assert.strictEqual(patch.count('reboot'), 1, 'the restart is sent once');
    assert.strictEqual(patch.reboots, 1, 'the daemon reboots once');
    c.close();
});

test('no restart button when carplay restarts at once (never deferred)', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    /* An apply that restarts carplay reports carplay "restarted", never
     * "deferred": the restart is not needed. */
    patch._broadcast({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', res: SIZE, safe: SAFE, carplay: 'restarted' });
    await until('patched', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Waiting for the phone');
    assert.strictEqual(c.restartNeeded(), false);
    c.requestRestart();
    await sleep(50);
    assert.strictEqual(patch.count('reboot'), 0, 'no reboot is sent when none is needed');
    c.close();
});

test('a refused restart reply clears the asked flag so the button returns', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.announce('unknown', 'high', 'patch written; takes effect at the next head-unit start', 'deferred');
    await until('restart needed', () => c.restartNeeded());
    /* The daemon refuses this request (a restart is already underway); its
     * reply carries no status, so the deferred state is unchanged. */
    patch._rebooting = true;
    c.requestRestart();
    await until('requested', () => c.restartRequested());
    await until('cleared', () => !c.restartRequested());
    assert.strictEqual(c.restartNeeded(), true, 'still deferred, so the button is offered again');
    assert.strictEqual(patch.reboots, 0, 'the refused request reboots nothing');
    c.close();
});

test('Android Auto: its line per state; Patch, Remove patch and zoom only where they apply', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const D = [150, 400, 10];
    const cases = [
        [{ state: 'original', density: 180, densities: D }, 'Status: Not patched', [true, false, false, false]],
        [{ state: 'patched', dpi: 180, diskCard: 'shown', density: 180, densities: D }, 'Status: Patched', [false, true, true, true]],
        [{ state: 'patched', dpi: 400, diskCard: 'shown', density: 400, densities: D }, 'Status: Patched', [false, true, true, false]],
        [{ state: 'unknown', density: 180, densities: D }, 'Status: Partly patched', [true, true, false, false]],
        [{ state: 'original', density: 180, conflict: 'not supported on this car (KI_KOMBI_VARIANT 9)' }, 'Status: Not supported on this car', [false, false, false, false]],
        [{ state: 'original', density: 180, conflict: 'untested aapd build' }, 'Status: Untested version', [true, false, false, false]],
        [{ state: 'original', density: 180, conflict: 'not patchable: gate: variant check not found' }, 'Status: Not supported on this car', [false, false, false, false]],
        [{ state: 'original', density: 180, conflict: 'aapd.conf not usable: no number SCREENS/KOMBI_DISPLAY/DIMENSIONS/12.3/SA_WIDTHPIXELS' }, 'Status: Changed by another tool', [false, false, false, false]],
        [{ state: 'original', density: 180, conflict: 'aapd.service starts /opt/other/aapd (/etc/systemd/system/aapd.service.d/x.conf)' }, 'Status: Changed by another tool', [false, false, false, false]],
        [{ state: 'absent', conflict: 'no Android Auto on this head unit' }, 'Status: Not supported on this car', [false, false, false, false]],
        [{ state: 'unknown', pending: 'apply', aapd: 'deferred', density: 180 }, 'Status: Patched after restart', [false, false, false, false]],
        [{ state: 'unknown', pending: 'revert', aapd: 'deferred', density: 180 }, 'Status: Removed after restart', [false, false, false, false]],
        [{ state: 'busy', reason: 'applying', density: 180 }, 'Status: Patching…', [false, false, false, false]],
        [{ state: 'busy', reason: 'reverting', density: 180 }, 'Status: Removing patch…', [false, false, false, false]],
        [{ state: 'error', reason: 'another change to / is running; try again', density: 180 }, 'Status: Failed', [true, true, false, false]],
        [{ state: 'patched', dpi: 180, diskCard: 'shown', density: 170, densities: D, error: 'Command failed' }, 'Status: Failed', [false, true, true, true]],
        [{ state: 'original', density: 180, conflict: 'aapd was modified by something else' }, 'Status: Changed by another tool', [false, false, false, false]],
        [{ state: 'unknown', density: 180, conflict: 'aapd status unreadable: Error: Command failed: …' }, 'Status: Unavailable', [false, true, false, false]],
    ];
    for (const [aa, line, can] of cases) {
        patch.aa = aa;
        patch.announce('patched', 'high', null);
        await until(`${line} ${JSON.stringify(aa)}`, () => JSON.stringify(c.aa()) === JSON.stringify(aa));
        assert.strictEqual(c.aaStatusText(), line);
        assert.deepStrictEqual([c.canEnableAa(), c.canDisableAa(), c.canZoom(-10), c.canZoom(10)], can, line);
    }
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: D };
    patch.announce('patched', 'high', null);
    await until('patched', () => c.canZoom(-10) && !c.aa().error);
    assert.strictEqual(c.zoomText(), 'Zoom: 180 dpi');
    assert.deepStrictEqual([c.cardText(), c.cardHidden()], ["Hide phone's turn card", false]);
    c.zoom(-10);
    await until('zoom sent', () => patch.count('aa-density') === 1);
    await until('zoom done', () => c.aa().state === 'patched' && c.aa().dpi === 170);
    c.toggleCard();
    await until('card sent', () => patch.count('aa-card') === 1);
    await until('card done', () => c.aa().state === 'patched' && c.aa().diskCard === 'hidden');
    c.disableAa();
    await until('revert sent', () => patch.count('aa-revert') === 1);
    await until('reverted', () => c.aa().state === 'original');
    c.enableAa();
    await until('apply sent', () => patch.count('aa-apply') === 1);
    assert.deepStrictEqual(patch.received.filter((m) => /^aa-/.test(m.cmd)),
        [{ cmd: 'aa-density', value: 170 }, { cmd: 'aa-card', value: 'hidden' }, { cmd: 'aa-revert' }, { cmd: 'aa-apply' }]);
    c.close();
    assert.deepStrictEqual([c.zoomText(), c.cardText(), c.cardHidden()], ['Zoom: —', "Hide phone's turn card", false], 'no value while closed');
});

test('Remove patch while something of ours is on disk, pending too; greyed otherwise and while any change runs', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const cases = [
        [{ state: 'unknown', pending: 'apply', carplay: 'deferred', removable: true }, true],
        [{ state: 'unknown', pending: 'revert', carplay: 'deferred', removable: false }, false],
        [{ state: 'patched', removable: true }, true],
        [{ state: 'unknown', removable: true }, true],
        [{ state: 'error', removable: true }, true],
        [{ state: 'original', conflict: 'carplay was modified by something else', removable: true }, true],
        [{ state: 'original', removable: false }, false],
        [{ state: 'original', removable: undefined }, false],
        [{ state: 'patched', removable: undefined }, true],
        [{ state: 'unknown', pending: 'apply', carplay: 'deferred', removable: undefined }, false],
    ];
    for (const [s, can] of cases) {
        patch.pending = s.pending || null;
        patch.conflict = s.conflict || null;
        patch.removable = s.removable;
        patch.announce(s.state, 'high', null, s.carplay);
        await until(JSON.stringify(s), () => c.patchStatus && c.patchStatus.state === s.state && c.patchStatus.removable === s.removable &&
            (c.patchStatus.pending || null) === (s.pending || null));
        assert.strictEqual(c.canDisable(), can, JSON.stringify(s));
    }
    const aaCases = [
        [{ state: 'unknown', pending: 'apply', aapd: 'deferred', density: 180, removable: true }, true],
        [{ state: 'unknown', pending: 'revert', aapd: 'deferred', density: 180, removable: false }, false],
        [{ state: 'patched', dpi: 180, diskCard: 'shown', density: 180, removable: true }, true],
        [{ state: 'original', density: 180, conflict: 'aapd was modified by something else', removable: true }, true],
        [{ state: 'original', density: 180, removable: false }, false],
        [{ state: 'busy', reason: 'applying', density: 180, removable: true }, false],
    ];
    patch.pending = null;
    patch.removable = undefined;
    for (const [aa, can] of aaCases) {
        patch.aa = aa;
        patch.announce('original', null, null);
        await until(JSON.stringify(aa), () => JSON.stringify(c.aa()) === JSON.stringify(aa));
        assert.strictEqual(c.canDisableAa(), can, JSON.stringify(aa));
    }
    patch.aa = { state: 'unknown', pending: 'apply', aapd: 'deferred', density: 180, removable: true };
    patch.announce('original', null, null);
    await until('pending', () => c.canDisableAa());
    c.disableAa();
    await until('revert sent', () => patch.count('aa-revert') === 1);
    c.close();
});

test('Android Auto patched with a conflict patchd refuses changes for: Zoom and the turn card greyed, the conflict named, Remove patch offered', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const D = [150, 400, 10];
    const patched = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: D, removable: true };
    const cases = [
        [null, true, 'Status: Patched', [false, true, true, true, true]],
        ['aapd.service starts /opt/other/aapd (/etc/systemd/system/aapd.service.d/x.conf)', true, 'Status: Changed by another tool', [false, true, false, false, false]],
        ['aapd is another build than our backup', false, 'Status: Changed by another tool', [false, false, false, false, false]],
        ['not supported on this car (KI_KOMBI_VARIANT 9)', true, 'Status: Not supported on this car', [false, true, false, false, false]],
        ['cluster coding unreadable', true, 'Status: Unavailable', [false, true, false, false, false]],
    ];
    for (const [conflict, removable, line, can] of cases) {
        const aa = Object.assign({}, patched, { removable }, conflict ? { conflict } : {});
        patch.aa = aa;
        patch.announce('patched', 'high', null);
        await until(`${conflict}`, () => JSON.stringify(c.aa()) === JSON.stringify(aa));
        assert.strictEqual(c.aaStatusText(), line, `${conflict}`);
        assert.deepStrictEqual([c.canEnableAa(), c.canDisableAa(), c.canZoom(-10), c.canZoom(10), c.canToggleCard()], can, `${conflict}`);
    }
    c.zoom(-10);
    c.toggleCard();
    await sleep(100);
    assert.strictEqual(patch.received.filter((m) => /^aa-(density|card)$/.test(m.cmd)).length, 0, 'a greyed Zoom or turn card sends nothing');
    c.close();
});

test('zoom and turn card: the value on disk, "(after restart)" only for the setting that waits, none without the patch', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const D = [150, 400, 10];
    const show = async (aa) => {
        patch.aa = aa;
        patch.announce('patched', 'high', null);
        await until(JSON.stringify(aa), () => JSON.stringify(c.aa()) === JSON.stringify(aa));
        return [c.zoomText(), c.cardText(), c.cardHidden()];
    };
    assert.deepStrictEqual(await show({ state: 'original', density: 200, card: 'hidden', densities: D }), ['Zoom: —', "Hide phone's turn card", false]);
    assert.deepStrictEqual([c.canZoom(-10), c.canToggleCard()], [false, false], 'greyed without the patch');
    assert.deepStrictEqual(await show({ state: 'unknown', pending: 'apply', aapd: 'deferred', dpi: 170, diskCard: 'shown', runDpi: 180,
        runCard: 'shown', density: 170, card: 'shown', densities: D }), ['Zoom: 170 dpi (after restart)', "Hide phone's turn card", false]);
    assert.deepStrictEqual(await show({ state: 'unknown', pending: 'apply', aapd: 'deferred', dpi: 180, diskCard: 'hidden', runDpi: 180,
        runCard: 'shown', density: 180, card: 'hidden', densities: D }), ['Zoom: 180 dpi', "Hide phone's turn card (after restart)", true]);
    assert.deepStrictEqual(await show({ state: 'unknown', pending: 'apply', aapd: 'deferred', dpi: 180, diskCard: 'shown', runDpi: null,
        runCard: null, density: 180, card: 'shown', densities: D }), ['Zoom: 180 dpi (after restart)', "Hide phone's turn card (after restart)", false]);
    assert.deepStrictEqual([c.canZoom(-10), c.canZoom(10), c.canToggleCard()], [false, false, false], 'greyed while waiting');
    /* chosen 170, the apply failed: the disk's 180 and the failure */
    assert.deepStrictEqual(await show({ state: 'patched', dpi: 180, diskCard: 'shown', density: 170, card: 'shown', densities: D, error: 'x' }),
        ['Zoom: 180 dpi', "Hide phone's turn card", false]);
    assert.strictEqual(c.aaStatusText(), 'Status: Failed');
    c.close();
});

test('the spacing between zoom and card changes greys both until it ends', async (render, patch) => {
    const c = control();
    let changes = 0;
    c.on('change', () => changes++);
    c.open();
    await until('connected', () => c.aa());
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: [150, 400, 10], changeAt: Date.now() + 300 };
    patch.announce('patched', 'high', null);
    await until('spaced', () => c.aa().changeAt && !c.canZoom(-10) && !c.canToggleCard());
    assert.strictEqual(c.canDisableAa(), true, 'only the zoom and the card wait');
    c.zoom(-10);
    c.toggleCard();
    const before = changes;
    await until('a change when the spacing ends, for the page to render', () => changes > before);
    assert.deepStrictEqual([c.canZoom(-10), c.canToggleCard()], [true, true], 'taken again');
    assert.strictEqual(patch.count('aa-density') + patch.count('aa-card'), 0, 'nothing sent while greyed');
    c.close();
});

test('while any operation runs, every operation is greyed in both sections and nothing is sent', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const D = [150, 400, 10];
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: D };
    patch.announce('original', null, null);
    await until('idle', () => c.canEnable() && c.canDisableAa());
    const all = () => [c.canEnable(), c.canDisable(), c.canEnableAa(), c.canDisableAa(), c.canZoom(-10), c.canZoom(10), c.canToggleCard()];
    for (const busy of ['carplay', 'aa', 'restart']) {
        patch._broadcast(Object.assign(patch._status(), { busy }));
        await until(busy, () => c.patchStatus.busy === busy);
        assert.deepStrictEqual(all(), [false, false, false, false, false, false, false], busy);
        c.enableMap();
        c.disableMap();
        c.enableAa();
        c.disableAa();
        c.zoom(-10);
        c.toggleCard();
        c.requestRestart();
    }
    await sleep(50);
    assert.deepStrictEqual(patch.received.filter((m) => m.cmd !== 'status'), [], 'nothing sent');
    assert.strictEqual(c.statusText(), 'Status: Not patched', 'CarPlay\'s line is its own while Android Auto runs');
    patch._broadcast(patch._status());
    await until('idle again', () => c.canEnable() && c.canDisableAa());
    c.close();
});

test('a refused command changes neither section', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    patch.aa = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, card: 'shown', densities: [150, 400, 10] };
    patch.announce('patched', 'high', null);
    await until('on', () => c.aaStatusText() === 'Status: Patched' && c.statusText() === 'Status: Patched');
    const lines = [c.statusText(), c.aaStatusText(), c.canEnable(), c.canDisableAa()];
    patch._broadcast({ ev: 'refused', cmd: 'aa-density', reason: 'changed moments ago; try again in a few seconds' });
    await sleep(50);
    assert.deepStrictEqual([c.statusText(), c.aaStatusText(), c.canEnable(), c.canDisableAa()], lines);
    c.close();
});

test('Android Auto before the daemon runs: its marker, else unavailable; a marker with an earlier boot\'s id is done', async () => {
    const aaPending = `${PENDING}.aa`;
    const boot = `${PENDING}.boot`;
    fs.writeFileSync(boot, 'boot-2\n');
    const c = new WazeClusterControl({ renderPath: RSOCK, patchPath: PSOCK, pendingPath: PENDING, aaPendingPath: aaPending,
        bootIdPath: boot, retryMs: 100 });
    assert.strictEqual(c.aaStatusText(), 'Status: Connecting…');
    c.open();
    await until('patch absent', () => c.patch.link === 'absent');
    assert.strictEqual(c.aaStatusText(), 'Status: Unavailable');
    fs.writeFileSync(aaPending, JSON.stringify({ action: 'apply', boot: 'boot-2' }) + '\n');
    assert.strictEqual(c.aaStatusText(), 'Status: Patched after restart');
    fs.writeFileSync(aaPending, JSON.stringify({ action: 'apply', boot: 'boot-1' }) + '\n');
    assert.strictEqual(c.aaStatusText(), 'Status: Unavailable', 'an earlier boot\'s marker');
    /* an older patchd's marker, no boot id, however old it looks */
    fs.writeFileSync(aaPending, JSON.stringify({ action: 'revert' }) + '\n');
    fs.writeFileSync(PENDING, JSON.stringify({ action: 'apply' }) + '\n');
    [aaPending, PENDING].forEach((f) => fs.utimesSync(f, Date.now() / 1000 - 86400, Date.now() / 1000 - 86400));
    assert.deepStrictEqual([c.aaStatusText(), c.statusText()], ['Status: Removed after restart', 'Status: Patched after restart']);
    [aaPending, boot].forEach((f) => fs.unlinkSync(f));
    c.close();
});

test('patchd 0.8.0: an Android Auto refusal and failure, the bare busy of its restart and its failed restart', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.aa());
    const D = [150, 400, 10];
    const on = { state: 'patched', dpi: 180, diskCard: 'shown', density: 180, densities: D };
    const old = (extra) => Object.assign({ ev: 'status', state: 'patched', panel: 'high', mode: 'avb', res: SIZE, safe: SAFE }, extra);
    patch._broadcast(old({ aa: on }));
    await until('on', () => c.aaStatusText() === 'Status: Patched' && c.canZoom(-10));
    /* turned away, as 0.8.0 does: an error or busy Android Auto status with
     * the refusal's reason, and no status after it */
    patch._broadcast(old({ aa: { state: 'error', reason: 'changed moments ago; try again in a few seconds', density: 170, densities: D } }));
    patch._broadcast(old({ aa: { state: 'busy', reason: 'another operation is running', density: 170, densities: D } }));
    await sleep(50);
    assert.deepStrictEqual([c.aaStatusText(), c.zoomText(), c.canZoom(-10), c.canDisableAa()], ['Status: Patched', 'Zoom: 180 dpi', true, true],
        'Android Auto as it was, no text, nothing greyed');
    patch._broadcast(old({ state: 'unknown', carplay: 'deferred', pending: 'apply', reason: 'patch written; takes effect at the next head-unit start', aa: on }));
    await until('pending', () => c.statusText() === 'Status: Patched after restart' && c.canRestart());
    patch._broadcast({ ev: 'status', state: 'busy', reason: 'restarting the head unit' });
    await until('restarting', () => c.restartText() === 'Restart: In progress');
    assert.strictEqual(c.statusText(), 'Status: Patched after restart', 'CarPlay\'s line as it was');
    patch._broadcast({ ev: 'status', state: 'busy', reason: 'another operation is running' });
    await sleep(50);
    assert.strictEqual(c.statusText(), 'Status: Patched after restart', 'a refusal leaves it too');
    patch._broadcast(old({ state: 'error', reason: 'reboot failed: Command failed', carplay: 'deferred', pending: 'apply', aa: on }));
    await until('failed', () => c.restartText() === 'Restart: Failed, command failed');
    assert.deepStrictEqual([c.statusText(), c.canRestart()], ['Status: Patched after restart', true]);
    /* an Android Auto change that failed, not a refusal */
    patch._broadcast(old({ aa: { state: 'error', reason: 'saving the choice failed: EROFS', density: 170, densities: D } }));
    await until('Android Auto failed', () => c.aaStatusText() === 'Status: Failed');
    c.close();
});

test('a failed restart: its own line with a short reason, the limit greys the button, CarPlay\'s state as it was', async (render, patch) => {
    const c = control();
    c.open();
    await until('connected', () => c.connectedPatch());
    patch.pending = 'apply';
    patch.announce('unknown', 'high', 'patch written; takes effect at the next head-unit start', 'deferred');
    await until('pending', () => c.canRestart());
    const carplay = [c.statusText(), c.canEnable(), c.canDisable()];
    patch.rebootFails = 'refused';
    c.requestRestart();
    await until('failed', () => c.restartText() === 'Restart: Failed, not accepted' && !c.restartRequested());
    assert.deepStrictEqual([c.statusText(), c.canEnable(), c.canDisable()], carplay, 'CarPlay untouched by the failure');
    assert.strictEqual(c.canRestart(), true, 'a refused restart can be asked again');
    patch.rebootFails = 'stayed';
    c.requestRestart();
    await until('stayed', () => c.restartText() === 'Restart: Failed, the head unit stayed on — possible in 10 min');
    assert.strictEqual(c.canRestart(), false, 'held by the limit');
    assert.deepStrictEqual([c.statusText(), c.canEnable(), c.canDisable()], carplay);
    c.requestRestart();
    await sleep(50);
    assert.strictEqual(patch.count('reboot'), 2, 'nothing sent while held');
    patch.restart = { state: 'idle', possibleAt: Date.now() + 90000 };
    patch._broadcast(patch._status());
    await until('limit line', () => c.restartText() === 'Restart: Possible in 2 min' && !c.canRestart());
    c.close();
});

test('capture texts; capture sends nothing while closing', async (render, patch) => {
    const c = control();
    const text = (capture) => { c.captureState = capture; return [c.captureButtonText(), c.captureText()]; };
    assert.deepStrictEqual(text({ state: 'unavailable' }), ['Start capture', 'Capture: Unavailable']);
    assert.deepStrictEqual(text({ state: 'closed' }), ['Start capture', 'Capture: Off']);
    assert.deepStrictEqual(text({ state: 'opening' }), ['Stop capture', 'Capture: Starting…']);
    assert.deepStrictEqual(text({ state: 'open' }), ['Stop capture', 'Capture: On']);
    assert.deepStrictEqual(text({ state: 'closing' }), ['Start capture', 'Capture: Stopping…']);
    assert.deepStrictEqual(text({ state: 'restore_failed', error: 'daemon unavailable' }), ['Start capture', 'Capture: Failed']);
    c.open();
    await until('connected', () => c.connectedPatch());
    c.captureState = { state: 'closing' };
    c.capture();
    await sleep(50);
    assert.strictEqual(patch.count('capture-start') + patch.count('capture-stop'), 0);
    c.close();
});

test('a map-only status event keeps the Android Auto line', async (render, patch) => {
    const c = control();
    c.open();
    patch.aa = { state: 'patched', dpi: 180, density: 180, densities: [150, 400, 10] };
    patch.announce('patched', 'high', null);
    await until('patched', () => c.aaStatusText() === 'Status: Patched');
    patch._broadcast({ ev: 'status', state: 'error', reason: 'another change to / is running; try again' });
    await until('map error', () => c.patchStatus.state === 'error');
    assert.strictEqual(c.aaStatusText(), 'Status: Patched');
    assert.strictEqual(c.canDisableAa(), true);
    c.close();
});

test('a pending Android Auto change alone offers the restart; CarPlay\'s line is unchanged', async (render, patch) => {
    const c = control();
    c.open();
    patch.announce('patched', 'high', null);
    await until('connected', () => c.aa());
    assert.strictEqual(c.restartNeeded(), false);
    patch.aa = { state: 'unknown', pending: 'apply', aapd: 'deferred', density: 180 };
    patch.res = SIZE;
    patch.safe = SAFE;
    patch.announce('patched', 'high', null);
    await until('restart needed', () => c.restartNeeded());
    assert.strictEqual(c.statusText(), 'Status: Patched', 'CarPlay has nothing pending');
    assert.strictEqual(c.canRestart(), true);
    c.requestRestart();
    await until('rebooted', () => patch.reboots === 1);
    c.close();
});

test('CarPlay Wi-Fi: the band patchd knows, the line to turn the iPhone\'s Wi-Fi off and on on 2.4 GHz only; nothing without either', async (render, patch) => {
    const c = control();
    c.open();
    await until('original', () => c.statusText() === 'Status: Not patched');
    assert.deepStrictEqual([c.wifiBand(), c.wifiText(), c.wifiHint()], [null, '', ''], 'no band');
    patch.setWifi({ band: '5', iface: 'p2p-wlan0-0', freq: 5785 });
    await until('5 GHz', () => c.wifiText() === 'CarPlay Wi-Fi: 5 GHz');
    assert.deepStrictEqual([c.wifiBand(), c.wifiHint()], ['5', '']);
    patch.setWifi({ band: '2.4', iface: 'p2p-wfd0-1', freq: 2437 });
    await until('2.4 GHz', () => c.wifiText() === 'CarPlay Wi-Fi: 2.4 GHz');
    assert.strictEqual(c.wifiHint(), 'Audio may cut out. On your iPhone, turn Wi-Fi off and on.');
    patch.setWifi({ band: '6', iface: 'p2p-wlan0-0', freq: 5955 });
    await until('a band the app does not know', () => c.wifiText() === '' && c.wifiHint() === '');
    patch.setWifi(null);
    await until('unknown', () => c.wifiBand() === null);
    patch.setWifi({ band: '2.4', iface: 'p2p-wlan0-1', freq: 2462 });
    await until('2.4 GHz again', () => c.wifiBand() === '2.4');
    patch.announce('patched', 'high', null);
    await until('a status of another change keeps it', () => c.statusText() === 'Status: Patched');
    assert.strictEqual(c.wifiBand(), '2.4');
    await patch.close();
    await until('patchd gone', () => !c.connectedPatch());
    assert.deepStrictEqual([c.wifiBand(), c.wifiText(), c.wifiHint()], [null, '', ''], 'patchd gone: no band');
    c.close();
});

test('CarPlay Wi-Fi from a patchd before it: none', async (render, patch) => {
    const c = control();
    c.open();
    await until('original', () => c.statusText() === 'Status: Not patched');
    const older = patch._status();
    delete older.wifi;
    patch._broadcast(older);
    await sleep(50);
    assert.deepStrictEqual([c.wifiBand(), c.wifiText(), c.wifiHint()], [null, '', '']);
    c.close();
});

test('render unreachable while patched', async (render, patch) => {
    /* Only the patch daemon is up (the render socket is absent in this case). */
    const c = new WazeClusterControl({ renderPath: `/tmp/woc-none-${process.pid}.sock`, patchPath: PSOCK, retryMs: 100 });
    c.open();
    await until('patch up', () => c.connectedPatch());
    patch.announce('patched', 'high', null);
    await until('map unavailable', () => c.statusText() === 'Status: Patched' && c.mapText() === 'Cluster map: Unavailable');
    assert.strictEqual(c.connectedRender(), false);
    c.close();
});

test('logs: before the logs service runs, unavailable and nothing to save', async () => {
    const l = new LogsControl({ path: LSOCK, retryMs: 50 });
    l.open();
    assert.strictEqual(l.statusText(), 'Saved logs: Connecting…');
    await until('absent', () => l.socket.link === 'absent');
    assert.strictEqual(l.statusText(), 'Saved logs: Unavailable');
    assert.strictEqual(l.canSave(), false);
    assert.strictEqual(l.url(), null);
    assert.strictEqual(l.urlText(), 'Address: —');
    assert.deepStrictEqual(l.rows(), ['', '', '', '', '']);
    l.close();
});

test('logs: Save logs saves once, then lists the archive; the address and QR text follow the service', async () => {
    const logs = new FakeLogs(LSOCK);
    await logs.listen();
    try {
        const l = new LogsControl({ path: LSOCK, retryMs: 50 });
        l.open();
        await until('state', () => l.state);
        assert.strictEqual(l.statusText(), 'Saved logs: None');
        assert.strictEqual(l.urlText(), `Address: ${logs.url}`);
        assert.strictEqual(l.canSave(), true);
        l.save();
        await until('saving', () => l.statusText() === 'Saved logs: Saving…');
        assert.strictEqual(l.canSave(), false, 'no second save while one runs');
        await until('saved', () => l.statusText() === 'Saved logs: 1');
        assert.strictEqual(logs.count('save'), 1);
        assert(/^woc-logs-\d{8}T\d{6}Z\.tar\.gz · 412 KB$/.test(l.rows()[0]), l.rows()[0]);
        assert.deepStrictEqual(l.rows().slice(1), ['', '', '', '']);
        logs.url = null;
        logs.announce();
        await until('hotspot off', () => l.urlText() === 'Address: Wi-Fi hotspot off');
        assert.strictEqual(l.url(), null);
        logs.failWith = 'no-space';
        l.save();
        await until('no space', () => l.statusText() === 'Saved logs: Not enough space');
        logs.failWith = 'too-large';
        l.save();
        await until('too large', () => l.statusText() === 'Saved logs: Too large to save');
        l.close();
        await until('closed', () => logs._clients.size === 0);
        assert.strictEqual(l.state, null);
        l.save();
        await sleep(50);
        assert.strictEqual(logs.count('save'), 3, 'nothing sent once closed');
    } finally {
        await logs.close();
        try { fs.unlinkSync(LSOCK); } catch (e) { /* gone */ }
    }
});

async function main() {
    let failed = 0;
    for (const t of tests) {
        const needsServers = t.fn.length > 0;
        const render = new FakeRender(RSOCK);
        const patch = new FakePatch(PSOCK);
        if (needsServers) {
            await render.listen();
            await patch.listen();
        }
        try {
            await t.fn(render, patch);
            console.log(`ok   ${t.name}`);
        } catch (err) {
            failed++;
            console.log(`FAIL ${t.name}\n     ${err && err.stack}`);
        } finally {
            if (needsServers) {
                await render.close();
                await patch.close();
            }
            [RSOCK, PSOCK, PENDING].forEach((p) => {
                try {
                    fs.unlinkSync(p);
                } catch (e) {
                    /* gone */
                }
            });
        }
    }
    console.log(`${tests.length - failed}/${tests.length} passed`);
    process.exit(failed ? 1 : 0);
}

main();
