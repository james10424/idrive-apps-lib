// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * profile_sync against the fake patch daemon (which keeps the map settings
 * per driver profile as patchd does) and the fake render: a profile is told
 * to patchd once two reads agree; one without its own settings gets the
 * shared ones; ticked, it keeps a copy that changes on its own; a switch
 * puts the other profile's settings on the render (through a short
 * connection while the page is closed, through the page's own while it is
 * open, a display turned on first); Off as a profile's own, the render on
 * neither for it and back on the cluster for another; the HUD map kept shown
 * over BMW's pop-ups as a profile's own, and not told to the render by a
 * patchd before it; unticked, the shared ones back; a
 * tick patchd refuses (another profile in effect) tells the profile again;
 * a read naming another profile greys the checkbox until that one counts;
 * no profile (no proxy or field): patchd told none, the checkbox greyed;
 * reads with nothing to go by change nothing; a patchd before profiles: the
 * checkbox greyed, asked again later; patchd not running: asked at the next
 * read. The SessionManager reader: the field's id; none for good without
 * the proxy or the field; a value that is not an id, a read that fails, and
 * a proxy slow to set up or failing to start rejected (nothing changes), the
 * proxy set up again later.
 *
 *   node tools/test_profiles.js
 */
const assert = require('assert');
const fs = require('fs');
const { ProfileSync, sessionReader, IFACE, INSTANCE, FIELD } = require('../src/profile_sync.js');
const { WazeClusterControl } = require('../src/waze_cluster_control.js');
const { FakeRender, FakePatch } = require('./fakes.js');

const RSOCK = `/tmp/woc-sr-${process.pid}.sock`;
const PSOCK = `/tmp/woc-sp-${process.pid}.sock`;
const POLL = 30;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
function until(what, check, ms) {
    const deadline = Date.now() + (ms || 2000);
    return new Promise((resolve, reject) => {
        const poll = () => {
            if (check()) resolve();
            else if (Date.now() > deadline) reject(new Error(`timed out waiting for ${what}`));
            else setTimeout(poll, 10);
        };
        poll();
    });
}
/* keep left out: a patchd before hud_keep_shown */
const settings = (cluster, hud, pct, x, y, keep) => Object.assign({ cluster, hud, hud_width_pct: pct, hud_offset_x: x, hud_offset_y: y },
    keep === undefined ? {} : { hud_keep_shown: keep });

let passed = 0;
async function check(name, fn) {
    await fn();
    passed++;
    console.log(`ok   ${name}`);
}

/* A fake typed proxy factory: the field's value from value(), or a proxy
 * shaped by opts (no field, a build that fails, a read that fails; the
 * first builds that never answer, inits that never answer or fail). */
function builders(value, opts) {
    const o = opts || {};
    const b = { built: [], reads: 0 };
    b.buildTypedProxy = (iface, instance) => {
        b.built.push(`${iface} ${instance}`);
        if (o.buildFails) return Promise.reject(new Error('no interface configuration'));
        if (b.built.length <= (o.buildHangs || 0)) return new Promise(() => {});
        const slow = b.built.length <= (o.initHangs || 0), fails = b.built.length <= (o.initFails || 0);
        const proxy = { init: () => (slow ? new Promise(() => {}) : fails ? Promise.reject(new Error('not offered')) : Promise.resolve()),
            waitUntilAvailable: () => Promise.resolve() };
        if (!o.noField) {
            proxy[FIELD] = { getValue: () => { b.reads++; return o.readFails ? Promise.reject(new Error('timeout')) : Promise.resolve(value()); } };
        }
        return Promise.resolve(proxy);
    };
    return b;
}
const quiet = () => {
    const l = { lines: [] };
    l.info = (m) => l.lines.push(`info: ${m}`);
    l.warn = (m) => l.lines.push(`warn: ${m}`);
    return l;
};

async function main() {
    [RSOCK, PSOCK].forEach((p) => { try { fs.unlinkSync(p); } catch (e) { /* none */ } });
    const render = new FakeRender(RSOCK);
    await render.listen();
    const patch = new FakePatch(PSOCK);
    await patch.listen();
    patch.map = settings(1, 0, 0, 0, 0, 0);

    /* ---- the reader ---- */
    await check('the reader: the field\'s id, read-only through the typed SessionManager proxy', async () => {
        let v = 'a1b2c3d4-0000-4000-8000-000000000001';
        const b = builders(() => v);
        const read = sessionReader(b, quiet());
        assert.strictEqual(await read(), v);
        v = 'de.bmw.infotainment.personalization.UserAccountManager.guest';
        assert.strictEqual(await read(), v);
        v = 'x'.repeat(128);
        assert.strictEqual(await read(), v);
        assert.deepStrictEqual(b.built, [`${IFACE} ${INSTANCE}`], 'one proxy, built once');
    });
    await check('the reader: a value that is not a profile id is no information (rejected), warned once', async () => {
        let v = '';
        const log = quiet();
        const read = sessionReader(builders(() => v), log);
        for (v of ['', 'x'.repeat(129), 'a/b', '../x', '__proto__', 42, null, { value: 'A' }, ['A']]) {
            await assert.rejects(read(), /no profile id read/, JSON.stringify(v));
        }
        assert.strictEqual(log.lines.filter((l) => /^warn: driver profiles/.test(l)).length, 1, log.lines.join('\n'));
    });
    await check('the reader: no proxy, or no field: none for good', async () => {
        for (const opts of [{ buildFails: true }, { noField: true }]) {
            const log = quiet();
            const b = builders(() => 'A', opts);
            const read = sessionReader(b, log);
            assert.strictEqual(await read(), null);
            assert.strictEqual(await read(), null);
            assert.strictEqual(b.built.length, 1, 'not built again');
            assert(log.lines.some((l) => /^warn: driver profiles: .*; the map settings are shared$/.test(l)), log.lines.join('\n'));
        }
    });
    await check('the reader: a read that fails is rejected (nothing changes)', async () => {
        const read = sessionReader(builders(() => 'A', { readFails: true }), quiet());
        await assert.rejects(read());
    });
    await check('the reader: a proxy slow to set up, or failing to start, is rejected and set up again later, with backoff', async () => {
        for (const opts of [{ buildHangs: 2 }, { initHangs: 2 }, { initFails: 2 }]) {
            const b = builders(() => 'driverA', opts);
            const read = sessionReader(b, quiet(), { readMs: 40, retryMs: 200, retryMaxMs: 2000 });
            await assert.rejects(read(), JSON.stringify(opts));
            await assert.rejects(read(), /set up again later/, 'not set up again before the backoff');
            assert.strictEqual(b.built.length, 1);
            await sleep(260);
            await assert.rejects(read(), JSON.stringify(opts));
            assert.strictEqual(b.built.length, 2);
            await sleep(100);
            await assert.rejects(read(), /set up again later/, 'the backoff doubled');
            await sleep(360);
            assert.strictEqual(await read(), 'driverA', `${JSON.stringify(opts)}: set up at the third try`);
            assert.strictEqual(b.built.length, 3);
        }
    });
    await check('the sync with a proxy slow to set up: nothing told meanwhile, then the profile', async () => {
        const told = patch.count('profile');
        const b = builders(() => 'driverS', { initHangs: 1 });
        const s = new ProfileSync({ readProfile: sessionReader(b, quiet(), { readMs: 40, retryMs: 80 }), patchPath: PSOCK, renderPath: RSOCK,
            pollMs: POLL, log: quiet() });
        s.start();
        await sleep(100);
        assert.strictEqual(patch.count('profile'), told, 'nothing told while it is set up');
        assert.strictEqual(s.canChooseOwn(), false);
        await until('told', () => s.profile === 'driverS');
        assert.strictEqual(patch.count('profile'), told + 1);
        s.stop();
    });

    /* ---- the sync, page closed ---- */
    let id = 'driverA';
    let reads = 0;
    const sync = new ProfileSync({ readProfile: () => { reads++; return Promise.resolve(id); }, patchPath: PSOCK, renderPath: RSOCK,
        pollMs: POLL, retryMs: 300, log: quiet() });
    let changes = 0;
    sync.on('change', () => changes++);
    assert.strictEqual(sync.canChooseOwn(), false, 'greyed before patchd has said');
    sync.start();
    await check('the first profile is told once two reads agree; the shared settings, the render left alone', async () => {
        await until('told', () => sync.profile === 'driverA');
        assert.deepStrictEqual(patch.received.filter((m) => m.cmd === 'profile' && m.id !== 'driverS'), [{ cmd: 'profile', id: 'driverA' }]);
        assert(reads >= 2, 'two reads first');
        assert.strictEqual(sync.own, false);
        assert(sync.canChooseOwn(), 'the checkbox acts');
        await sleep(POLL * 4);
        assert.strictEqual(patch.received.filter((m) => m.cmd === 'profile' && m.id === 'driverA').length, 1, 'told once while it stays');
        assert.deepStrictEqual(render.received, []);
    });
    await check('ticked: shown ticked at once; own settings, a copy of the ones in effect; nothing moves', async () => {
        const asked = sync.setOwn(true);
        assert.strictEqual(sync.ownShown(), true, 'ticked as pressed');
        await sync.setOwn(false);
        assert.strictEqual(patch.count('profile-own'), 0, 'a press while one is asked: nothing asked yet');
        await asked;
        assert.strictEqual(sync.own, true);
        assert.deepStrictEqual(patch.received.slice(-1), [{ cmd: 'profile-own', id: 'driverA', on: 1 }]);
        assert.deepStrictEqual(render.received, []);
        await sync.setOwn(true);
        assert.strictEqual(patch.count('profile-own'), 1, 'ticked again: nothing asked');
    });
    /* driver A picks the HUD alone at 60 %, moved left, kept shown over BMW's pop-ups (the page's commands, which patchd saves) */
    patch.map = settings(0, 1, 60, -40, 0, 1);
    id = 'driverB';
    await check('another profile, without its own: the shared settings on the render over a short connection, a display on first', async () => {
        await until('B in effect', () => sync.profile === 'driverB' && sync.own === false);
        await until('render moved', () => render.received.length === 3);
        assert.deepStrictEqual(render.received, [{ cmd: 'cluster', on: '1' }, { cmd: 'hud', on: '0' },
            { cmd: 'hud', hud_width_pct: '0', hud_x: '0', hud_y: '0', hud_keep_shown: '0' }]);
        await until('short connection closed', () => render._clients.size === 0);
    });
    render.received.length = 0;
    id = 'driverA';
    await check('back to A: its own settings on the render, the HUD turned on before the cluster off', async () => {
        await until('A in effect', () => sync.profile === 'driverA' && sync.own === true);
        await until('render moved', () => render.hud === 1 && render.cluster === 0 && render.hudWidth === 60 && render.hudX === -40 &&
            render.hudKeep === 1);
        assert.deepStrictEqual(render.received.map((m) => m.cmd + (m.on || '')), ['hud1', 'cluster0', 'hud']);
    });
    /* driver A picks Off (the page turns the render to neither, patchd saves it as A's own) */
    patch.map = settings(0, 0, 60, -40, 0, 1);
    render.hud = 0;
    render.received.length = 0;
    id = 'driverB';
    await check('Off as A\'s own: another profile without its own gets the shared settings, the map back on the cluster', async () => {
        await until('B in effect', () => sync.profile === 'driverB' && sync.own === false);
        await until('render moved', () => render.cluster === 1 && render.hud === 0 && render.hudWidth === 0 && render.hudKeep === 0);
        assert.deepStrictEqual(render.received.map((m) => m.cmd + (m.on || '')), ['cluster1', 'hud0', 'hud']);
    });
    render.received.length = 0;
    id = 'driverA';
    await check('back to A: Off again, the render on neither, its HUD size and place kept', async () => {
        await until('A in effect', () => sync.profile === 'driverA' && sync.own === true);
        await until('render moved', () => render.cluster === 0 && render.hud === 0 && render.hudWidth === 60 && render.hudX === -40);
        assert.deepStrictEqual(render.received.map((m) => m.cmd + (m.on || '')), ['cluster0', 'hud0', 'hud']);
    });

    await check('a tick patchd refuses (another profile in effect): nothing changed, the profile told again, patchd\'s state shown', async () => {
        patch.current = 'driverB';
        const told = patch.count('profile');
        await sync.setOwn(false);
        await until('told again', () => patch.count('profile') === told + 1);
        await until('state re-read', () => sync.profile === 'driverA' && sync.own === true && sync.ownShown() === true);
        assert.strictEqual(patch.own.driverA !== undefined, true, 'A keeps its own settings');
    });
    await check('reads with nothing to go by: nothing told, the profile in effect stays', async () => {
        const told = patch.count('profile');
        const saved = sync._read;
        sync._read = () => Promise.reject(new Error('no profile id read'));
        await sleep(POLL * 5);
        assert.strictEqual(patch.count('profile'), told);
        assert.strictEqual(sync.profile, 'driverA');
        assert(sync.canChooseOwn());
        sync._read = saved;
    });

    await check('a read naming another profile greys the checkbox at once, until it counts or the reads come back', async () => {
        assert(sync.canChooseOwn());
        const saved = sync._read;
        let release = null;
        let n = 0;
        sync._read = () => (n++ === 0 ? Promise.resolve('driverB') : new Promise((resolve) => { release = resolve; }));
        await until('greyed, the next read waiting', () => !sync.canChooseOwn() && release);
        assert.strictEqual(sync.profile, 'driverA', 'driverB not yet told');
        const asked = patch.count('profile-own');
        await sync.setOwn(false);
        assert.strictEqual(patch.count('profile-own'), asked, 'a tick meanwhile asks nothing');
        sync._read = saved;
        release('driverA');
        await until('enabled again', () => sync.canChooseOwn() && sync.profile === 'driverA');
    });

    /* ---- with the page open: its own render connection ---- */
    const c = new WazeClusterControl({ renderPath: RSOCK, patchPath: PSOCK, retryMs: 50 });
    await until('short connection gone', () => render._clients.size === 0);
    const sync2 = new ProfileSync({ control: c, readProfile: () => Promise.resolve(id), patchPath: PSOCK, renderPath: `/tmp/woc-nowhere-${process.pid}.sock`,
        pollMs: POLL, log: quiet() });
    sync.stop();
    c.open();
    await until('page connected', () => c.render.connected());
    render.received.length = 0;
    sync2.start();
    await until('A again', () => sync2.profile === 'driverA' && sync2.own === true);
    await check('unticked with the page open: the shared settings through the page\'s render connection', async () => {
        await sync2.setOwn(false);
        assert.strictEqual(sync2.own, false);
        await until('render moved', () => render.cluster === 1 && render.hud === 0 && render.hudWidth === 0);
        assert.strictEqual(render._clients.size, 1, 'only the page\'s connection');
        assert.deepStrictEqual(patch.own, {}, 'no profile with its own settings');
    });

    /* ---- no profile detected ---- */
    render.received.length = 0;
    id = null;
    await check('no profile (no proxy or field, for good): patchd told none, the shared settings, the checkbox greyed', async () => {
        await until('none', () => sync2.profile === null);
        assert.deepStrictEqual(patch.received.filter((m) => m.cmd === 'profile').slice(-1), [{ cmd: 'profile', id: null }]);
        assert.strictEqual(sync2.canChooseOwn(), false);
        const asked = patch.count('profile-own');
        await sync2.setOwn(true);
        assert.strictEqual(patch.count('profile-own'), asked, 'a greyed tick asks nothing');
    });
    await check('reads that keep changing are not told', async () => {
        const told = patch.count('profile');
        let n = 0;
        const flip = new ProfileSync({ readProfile: () => Promise.resolve(n++ % 2 ? 'driverA' : 'driverB'), patchPath: PSOCK, renderPath: RSOCK,
            pollMs: POLL, log: quiet() });
        flip.start();
        await sleep(POLL * 6);
        flip.stop();
        assert.strictEqual(patch.count('profile'), told);
    });
    await check('a patchd before hud_keep_shown: the render not told it', async () => {
        render.received.length = 0;
        sync2._toRender(settings(1, 0, 0, 0, 0));
        await until('render moved', () => render.received.length === 3);
        assert.deepStrictEqual(render.received[2], { cmd: 'hud', hud_width_pct: '0', hud_x: '0', hud_y: '0' });
    });
    sync2.stop();
    c.close();

    /* ---- a patchd before profiles; none running ---- */
    patch.profileMute = true;
    const log = quiet();
    const sync3 = new ProfileSync({ readProfile: () => Promise.resolve('driverA'), patchPath: PSOCK, renderPath: RSOCK, pollMs: POLL,
        replyMs: 100, retryMs: 400, log });
    sync3.start();
    await check('a patchd that does not answer: the checkbox greyed, asked again after the retry time, not each read', async () => {
        await until('asked', () => log.lines.some((l) => /patchd did not take profile: no answer/.test(l)));
        const asked = patch.count('profile');
        assert.strictEqual(sync3.canChooseOwn(), false);
        await sleep(200);
        assert.strictEqual(patch.count('profile'), asked, 'not asked again before the retry time');
        patch.profileMute = false;
        await until('asked again and taken', () => sync3.profile === 'driverA', 1500);
        assert(sync3.canChooseOwn());
    });
    sync3.stop();
    await patch.close();
    const sync4 = new ProfileSync({ readProfile: () => Promise.resolve('driverB'), patchPath: PSOCK, renderPath: RSOCK, pollMs: POLL,
        retryMs: 60000, log: quiet() });
    sync4.start();
    await sleep(POLL * 4);
    await check('patchd not running: asked at the next read once it runs', async () => {
        assert.strictEqual(sync4.profile, undefined);
        const p2 = new FakePatch(PSOCK);
        await p2.listen();
        await until('taken', () => sync4.profile === 'driverB');
        sync4.stop();
        await p2.close();
    });

    await render.close();
    [RSOCK, PSOCK].forEach((p) => { try { fs.unlinkSync(p); } catch (e) { /* gone */ } });
    console.log(`${passed}/${passed} passed`);
    assert(changes > 0);
}

main().then(() => process.exit(0), (e) => {
    console.log(`FAIL ${e && e.stack}`);
    process.exit(1);
});
