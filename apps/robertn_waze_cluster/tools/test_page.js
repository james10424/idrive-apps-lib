// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Runs the app's source modules (mgu-app.json, the entry first) against
 * a fake SDK and HMI built from the shipped uiDescription.json, with its two
 * sockets pointed at tools/fakes.js.
 *
 *   node tools/build.js && node tools/test_page.js
 *
 * Covers: the main page's four sections in order (General, HUD, CarPlay,
 * Android Auto), each under an unfocusable header row, the same in
 * application.xml; no log rows on it; its labels; every row stays on the
 * page through every state (CarPlay showing and waiting, patched and not,
 * Android Auto on, off and not supported, a patch pending), greyed where it
 * cannot act, with the only rows shown and hidden CarPlay Wi-Fi and its
 * line, CarPlay's last, and the restart's two rows, the page's last;
 * CarPlay Wi-Fi: 5 GHz, and 2.4 GHz with the line to turn the iPhone's Wi-Fi
 * off and on, neither while patchd does not know the band; CarPlay's
 * Patch button
 * enabled only while unpatched and CarPlay's Remove patch button only while patched;
 * pressing every button sends the render nothing; Android Auto's Status,
 * Zoom and the Hide phone's turn card checkbox; info rows as Name: Value;
 * the Cluster map row in General; Hide BMW's turn box ticked as the render
 * reports, each press one guide command to the render, greyed and unticked,
 * its label kept, while the cluster renderer holds the box shown; Show on,
 * a drop-down of Cluster, HUD, both and Off in General, Cluster by default,
 * each pick through the render and patchd, the display turned on first;
 * Off from the cluster and from both, Cluster map and HUD map Off, the HUD's
 * rows and the turn box greyed, Off also for a render on neither; the HUD
 * off by default, its size slider and position
 * buttons greyed (a greyed slider turned goes back), the slider a 5 % step
 * a turn, Disabled after errors with the slider and position buttons
 * greyed; Keep HUD map over BMW's pop-ups, a checkbox under HUD map,
 * unticked by default, each press one command to the render and one save,
 * ticked as the render reports, greyed with the size slider and for a render
 * without the setting; Own settings for this driver, a checkbox under Show on, greyed
 * while no driver profile is detected, ticked for a profile with its own
 * map settings, a switch of profile moving the render to that profile's
 * (or the shared) settings, unticked the shared ones back; no label greyed (the HMI's labels have no setEnabled, its
 * drop-downs and sliders only the property);
 * hiding the page closes both sockets; a page opened again reads the
 * render's current state; a pending patch, from the daemon and, before the
 * daemon runs, from its marker; a failed restart on the restart's own line,
 * CarPlay's line unchanged; the restart press restarts once, a greyed
 * button pressed sends nothing. The Logs
 * page: Start capture and the Capture row on it; Logs opens it (a child of the
 * main page), moving there keeps the app shown and its sockets open, it
 * holds the logs socket while shown; Save logs saves once and lists the
 * archive; the address shows as text and as a QR code, a new code replacing
 * the old one when the address changes and a blank one without an address;
 * back from an overlay on the Logs page, the logs socket opens again; a tile
 * launch after leaving on the Logs page opens none. The display language,
 * from HMIContext: a change writes the rows of both pages again in its
 * words, English for a word it lacks, and English again after.
 */
const assert = require('assert');
const EventEmitter = require('events');
const fs = require('fs');
const net = require('net');
const path = require('path');
const { FakeRender, FakePatch, FakeLogs, RENDER_PAGE, RENDER_FPS } = require('./fakes.js');
const { RENDER_PATH, PATCH_PATH, PENDING_PATH, SIZE, SAFE, HUD_SIZE_STEPS } = require('../src/waze_cluster_control.js');
const { LOGS_PATH } = require('../src/logs_control.js');

const MANIFEST = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'mgu-app.json'), 'utf8'));
const PKG = path.join(__dirname, '..', MANIFEST.tree);
const UI_DIR = path.join(PKG, 'rhmiData', 'uiDescriptions', 'ID7');
const UI = JSON.parse(fs.readFileSync(path.join(UI_DIR, 'uiDescription.json'), 'utf8'));
const XML = fs.readFileSync(path.join(UI_DIR, 'application.xml'), 'utf8');
const APP_JSON = JSON.parse(fs.readFileSync(path.join(PKG, 'rhmiData', 'application.json'), 'utf8'));

/* short: macOS limits a socket path to 104 bytes */
const RSOCK = `/tmp/woc-pr-${process.pid}.sock`;
const PSOCK = `/tmp/woc-pp-${process.pid}.sock`;
const PENDING = `/tmp/woc-ppend-${process.pid}.json`;
const LSOCK = `/tmp/woc-pl-${process.pid}.sock`;
const SOCKETS = { [RENDER_PATH]: RSOCK, [PATCH_PATH]: PSOCK, [LOGS_PATH]: LSOCK };
const realConnect = net.connect;
const exit = process.exit.bind(process);
net.connect = (p, ...rest) => realConnect.call(net, SOCKETS[p] || p, ...rest);
const realRead = fs.readFileSync;
fs.readFileSync = (p, ...rest) => realRead.call(fs, p === PENDING_PATH ? PENDING : p, ...rest);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
function until(what, check) {
    const deadline = Date.now() + 2000;
    return new Promise((resolve, reject) => {
        const poll = () => {
            if (check()) resolve();
            else if (Date.now() > deadline) reject(new Error(`timed out waiting for ${what}`));
            else setTimeout(poll, 10);
        };
        poll();
    });
}

/* ---- fake HMI, from the shipped uiDescription ----------------------------- */

class FakeComponent extends EventEmitter {
    constructor(id, desc) {
        super();
        this.id = Number(id);
        this.desc = desc;
        this.text = null;
        this.cells = {};
        this.enabled = null;
        this.visible = (desc && desc.properties && desc.properties.Visible) ? desc.properties.Visible.value === 1 : null;
        this.enabledSeen = new Set();
        this.ticks = [];
        const type = desc && desc.type;
        if (type === 'label') {
            this.setEnabled = undefined;   /* the HMI greys no label */
        }
        /* the oap-sdk's DropDown and Slider: no setEnabled or setVisible, only
         * setProperty; the HMI moves their value itself, the SDK writing it */
        if (type === 'dropDownMenu' || type === 'slider') {
            this.setEnabled = this.setVisible = undefined;
        }
        if (type === 'dropDownMenu') {
            this.on('action', ({ row }) => { this.cells.SelectedItem = row; });
        }
        if (type === 'slider') {
            this._step = 0;
            this.on('action', ({ subType }) => this.setStepValue(this.cells['0:Value'] + (subType === 4 ? 1 : -1)));
        }
    }
    getId() { return this.id; }
    setValue(...args) {
        const [row, cell, value] = args.length === 3 ? [null, ...args] : args;
        this.cells[row == null ? cell : `${row}:${cell}`] = value;
        if (cell === 'eParam_RowSelected') {
            this.selected = value;
            this.ticks.push(value);
        } else if (this.desc.type !== 'dropDownMenu' && (this.desc.type !== 'slider' || cell === 'TextDescription')) {
            this.text = value;
        }
        return Promise.resolve();
    }
    /* the SDK's Slider: held between its ends, 'changed' when it moves */
    setStepValue(v) {
        const row = this.desc.rows[0];
        const value = Math.max(row.MinValue.value, Math.min(row.MaxValue.value, v));
        if (value !== this._step) {
            this._step = value;
            this.emit('changed', value);
        }
        return this.setValue(0, 'Value', value, 'SInt');
    }
    setProperty(name, value) {
        assert.strictEqual(name, 'Enabled');
        this.enabled = value === 1;
        this.enabledSeen.add(this.enabled);
        return Promise.resolve();
    }
    /* A drop-down as it shows: its header and its chosen item. */
    shown() {
        return `${this.cells.HeaderText}: ${this.cells[`${this.cells.SelectedItem}:ItemText`]}`;
    }
    /* A press, as the HMI makes it: a checkbox ticks or unticks itself first. */
    press() {
        if (this.desc.type === 'checkBox') {
            this.selected = !this.selected;
            this.ticks.push(this.selected);
        }
        this.emit('action');
    }
    /* An item of the drop-down picked; the slider turned a step (Increase 4, Decrease 5). */
    pick(row) { this.emit('action', { row }); }
    turn(dir) { this.emit('action', { row: 0, subType: dir > 0 ? 4 : 5 }); }
    setEnabled(enabled) { this.enabled = enabled; this.enabledSeen.add(enabled); return Promise.resolve(); }
    setVisible(visible) { this.visible = visible; shownOrHidden.add(this.desc.name); return Promise.resolve(); }
    updateResources() { return Promise.resolve(); }
}

const shownOrHidden = new Set();
const byName = {};
Object.entries(UI.containers).forEach(([cid, cont]) => {
    byName[cont.name] = new FakeComponent(cid, cont);
    Object.entries(cont.components).forEach(([id, desc]) => {
        byName[desc.name] = new FakeComponent(id, desc);
    });
});
const rhmi = new EventEmitter();
rhmi.getByName = (name) => byName[name] || null;
rhmi.mapEntryState = () => Promise.resolve();
rhmi.openEntryState = () => Promise.resolve();
rhmi.opened = [];
rhmi.openState = (name) => { rhmi.opened.push(name); return Promise.resolve(); };
rhmi.flush = () => Promise.resolve();
rhmi.handles = new Set();
rhmi.getResourceBundle = () => ({
    createResourceHandles: (files) => { files.forEach((f) => rhmi.handles.add(f)); return Promise.resolve(); },
    destroyResourceHandles: (files) => { files.forEach((f) => rhmi.handles.delete(f)); return Promise.resolve(); }
});

/* ---- fake SDK ----------------------------------------------------------------- */

const logs = [];
let inactiveReports = 0;
const tileCalls = [];                   // the app's calls on its tile
const logger = {};
['error', 'warn', 'info', 'verbose', 'debug'].forEach((level) => {
    logger[level] = (msg) => logs.push(`${level}: ${msg}`);
});

class OnlineApp extends EventEmitter {
    constructor() {
        super();
        this._packageJson = JSON.parse(fs.readFileSync(path.join(PKG, 'package.json'), 'utf8'));
        this.appDescription = APP_JSON;
        this.rhmiApplication = rhmi;
        this.startReason = 'ENTRY_POINT';
        OnlineApp.instance = this;
        this._initialized = false;
        this._appStatusService = null;
        const description = APP_JSON.application.entryPoints[0];
        const call = (name) => (v) => { tileCalls.push(v === undefined ? name : `${name} ${v}`); return Promise.resolve(); };
        this._entryPoints = [{
            entryPoint: { setText: call('text'), setIcon: call('icon') },
            getDescription: () => description,
            init: call('init'),
            setAvailability: call('availability')
        }];
    }
    start() { setImmediate(() => this.emit('initialized')); return Promise.resolve(); }
    getAppId() { return 'robertn_waze_cluster'; }
    getEntryPoints() { return this._entryPoints; }
    getLastExecutedEntryPointUuid() { return null; }
    initialized() { this._initialized = true; this._appStatusService = {}; return Promise.resolve(); }
    registerActive() { return Promise.resolve(); }
    registerInactive() { inactiveReports++; return Promise.resolve(); }
    registerHmiVisible() { return Promise.resolve(); }
    unregisterHmiVisible() { return Promise.resolve(); }
    destroyBundles() { return Promise.resolve(); }
}

/* HMIContext: its display language, English at first; displayLanguage(v) is a change.
 * SessionManager: the driver profile in effect, sessionUser ('' while none). */
const hmiContexts = [];
let displayLanguage = null;
let sessionUser = '';
const someipBuilders = {
    buildTypedProxy: (iface, instance) => Promise.resolve(iface === 'de.bmw.infotainment.personalization.SessionManager' ? {
        init: () => Promise.resolve(),
        waitUntilAvailable: () => Promise.resolve(),
        currentUserAccountReference: { getValue: () => Promise.resolve(sessionUser) }
    } : {
        init: () => Promise.resolve(),
        waitUntilAvailable: () => Promise.resolve(),
        currentDisplayLanguage: {
            subscribe: (cb) => { displayLanguage = cb; hmiContexts.push(`${iface} ${instance}`); return Promise.resolve(); },
            getValue: () => Promise.resolve('ENGLISCH__UK')
        }
    })
};

const FAKE_MODULES = {
    'oap-sdk/src/core/OnlineApp.js': OnlineApp,
    'oap-sdk/src/someip/someipBuilders': someipBuilders,
    'oap-sdk/src/logging/logging': { getSDKLogger: () => logger }
};

/* Loads the app's entry module (mgu-app.json's first) with the SDK's names
 * answered by the fakes, as the unit's link of the source package would. */
function runSource() {
    const Module = require('module');
    const load = Module._load;
    Module._load = function (request, parent, isMain) {
        return FAKE_MODULES[request] || load.call(this, request, parent, isMain);
    };
    require(path.join(__dirname, '..', MANIFEST.modules[0]));
}

/* ---- scenario ----------------------------------------------------------------- */

async function main() {
    const components = UI.containers['1'].components;
    const xmlIds = (cid) => [...XML.split('<container id="').find((c) => c.startsWith(`${cid}"`))
        .matchAll(/<component id="(\d+)" type="(\w+)"(?: widget="(\w+)")?/g)].map((m) => [m[1], m[2], m[3]]);
    /* application.xml orders the rows; uiDescription holds them by id */
    const shipped = (d) => (d.type === 'dropDownMenu' ? `${d.cells.HeaderText.value}: ${d.rows[d.cells.SelectedItem.value].ItemText.value}`
        : d.type === 'slider' ? d.rows[0].TextDescription.value : d.rows[0].Text.value);
    const main = xmlIds('1').map(([id]) => [id, components[id].type, components[id].name, shipped(components[id])]);
    assert.deepStrictEqual(main.map(([id, type, name, text]) => (type === 'title' ? name : `${name} ${text}`)), [
        'wocTitle',
        'wocHeadGeneral General', 'wocShowOn Show on: Cluster', 'wocProfile Own settings for this driver', 'wocMap Cluster map: Off', "wocGuide Hide BMW's turn box", 'wocLogs Logs',
        'wocHeadHud HUD', 'wocHud HUD map: \u2014', "wocHudKeep Keep HUD map over BMW's pop-ups", 'wocHudSize Size: \u2014', 'wocHudPos Position: \u2014', 'wocHudLeft Move left',
        'wocHudRight Move right', 'wocHudUp Move up', 'wocHudDown Move down', 'wocHudCentre Centre',
        'wocHeadCarPlay CarPlay', 'wocStatus Status: Connecting…', 'wocEnable Patch', 'wocDisable Remove patch', 'wocWifi ', 'wocWifiHint ',
        'wocHeadAa Android Auto', 'wocAaStatus Status: Connecting…', 'wocAaEnable Patch', 'wocAaDisable Remove patch',
        'wocAaZoom Zoom: —', 'wocAaZoomOut Zoom out', 'wocAaZoomIn Zoom in', "wocAaCard Hide phone's turn card",
        'wocRestart Restart head unit', 'wocRestartStatus '],
        'the main page: four sections, in order');
    assert.deepStrictEqual(xmlIds('1').map(([id, type]) => `${id} ${type}`).sort(),
        Object.entries(components).map(([id, d]) => `${id} ${d.type}`).sort(), 'application.xml and uiDescription agree on the main page');
    const widgets = {};
    xmlIds('1').forEach(([id, , widget]) => { widgets[components[id].name] = widget; });
    assert.deepStrictEqual(['wocHeadGeneral', 'wocHeadHud', 'wocHeadCarPlay', 'wocHeadAa'].map((n) => widgets[n]), Array(4).fill('LT_Label_Header_1Row'),
        'the section headers are header rows');
    /* application.xml's property 3: 0 a row the controller passes over, 3 one it stops on */
    const focus = Object.fromEntries([...XML.split('<container id="2"')[0]
        .matchAll(/<component id="(\d+)"[^>]*>\s*<property id="2" value="\d"\/>\s*<property id="3" value="(\d)"\/>/g)]
        .map((m) => [components[m[1]].name, m[2]]));
    assert.deepStrictEqual(['wocHeadGeneral', 'wocHeadHud', 'wocHeadCarPlay', 'wocHeadAa'].map((n) => [focus[n], components[Object.keys(components)
        .find((id) => components[id].name === n)].properties.Focusable.value]), Array(4).fill(['0', 0]), 'the section headers take no focus');
    assert.strictEqual(main.find(([, , name]) => focus[name] === '3')[2], 'wocShowOn', 'the first row the controller stops on: Show on');
    assert(!main.some(([, , name]) => /^wocRow|^wocSetup|^wocCapture/.test(name)), 'no log rows, setup line or capture on the main page');
    assert.deepStrictEqual(['wocAaCard', 'wocGuide', 'wocProfile', 'wocHudKeep'].map((n) => widgets[n]), Array(4).fill('LT_CheckBox_1Row'),
        'the on/off settings are checkboxes');
    assert.deepStrictEqual([widgets.wocShowOn, widgets.wocHudSize], ['LT_DropDownMenu_1Row_2Col3070', 'LT_Slider_LeftJustified_Fill_Steps'],
        'where the map shows: a drop-down of four; the HUD\'s size: a slider');
    assert.deepStrictEqual(components['62'].rows.map((r) => r.ItemText.value), ['Cluster', 'HUD (experimental)', 'Cluster and HUD (experimental)', 'Off']);
    assert.deepStrictEqual(['MinValue', 'MaxValue', 'NumberOfSteps'].map((k) => components['52'].rows[0][k].value), [0, HUD_SIZE_STEPS, HUD_SIZE_STEPS],
        'the slider: one step for each 5 %');
    /* every info row reads "Name: Value" */
    assert(main.filter(([, type, name]) => type === 'label' && !/^wocHead|^(wocRestartStatus|wocWifi|wocWifiHint)$/.test(name))
        .every(([, , , text]) => /^[A-Z][A-Za-z ]+: [A-Z\u2014]/.test(text)), 'info rows as Name: Value');
    const hidden = Object.values(components).filter((d) => d.properties.Visible.value !== 1).map((d) => d.name);
    assert.deepStrictEqual(hidden, ['wocWifi', 'wocWifiHint', 'wocRestart', 'wocRestartStatus'],
        'every row ships shown but CarPlay Wi-Fi, its line and the restart\'s two');
    assert(/<component id="34" type="button" widget="LT_Button_1Row">\s*<property id="2" value="0"\/>/.test(XML), 'application.xml: the restart, hidden');
    ['29', '30'].forEach((id) => assert(new RegExp(`<component id="${id}" type="label" widget="LT_Label_1Row">\\s*<property id="2" value="0"\\/>\\s*` +
        '<property id="3" value="0"\\/>').test(XML), `application.xml: ${components[id].name}, hidden and unfocusable`));
    console.log('ok   the main page: General, HUD, CarPlay and Android Auto under unfocusable header rows, Show on the first stop; no log rows; ' +
        'CarPlay Wi-Fi and its line CarPlay\'s last, the restart\'s rows the page\'s last, all four hidden');
    const logsPage = UI.containers['2'];
    assert.strictEqual(logsPage.name, 'ST_OnlineApps_WazeClusterLogs');
    assert.deepStrictEqual(logsPage.properties.ParentState, { value: 1, type: 'UInt' }, 'the Logs page is a child of the main page');
    assert(/<container id="2" type="state" widget="LT_State_Wide">\s*<property id="9" value="1"\/>/.test(XML));
    assert.deepStrictEqual(xmlIds('2').map(([id, type]) => `${id} ${type}`).sort(),
        Object.entries(logsPage.components).map(([id, d]) => `${id} ${d.type}`).sort(), 'application.xml and uiDescription agree on the Logs page');
    assert.deepStrictEqual(xmlIds('2').map(([id]) => logsPage.components[id].name), ['wocLogsTitle', 'wocCapture', 'wocCaptureStatus',
        'wocLogsSave', 'wocLogsStatus', 'wocLogsQr', 'wocLogsUrl', 'wocLogsRow1', 'wocLogsRow2', 'wocLogsRow3', 'wocLogsRow4', 'wocLogsRow5']);
    assert(/<component id="83" type="label" widget="LT_Label_1Graphic_Scalable">/.test(XML), 'the QR code is a graphic');
    assert.strictEqual(APP_JSON.application.entryPoints.length, 1);
    assert.deepStrictEqual(xmlIds('2').slice(1).map(([id]) => logsPage.components[id].rows[0].Text && logsPage.components[id].rows[0].Text.value)
        .slice(0, 4), ['Start capture', 'Capture: Unavailable', 'Save logs', 'Saved logs: Connecting…']);
    console.log('ok   the Logs page: Start capture and its row, Save logs and its row, QR code, address, five archives; a child of the main page');

    const render = new FakeRender(RSOCK);
    let patch = new FakePatch(PSOCK);
    await render.listen();
    await patch.listen();
    patch.res = SIZE;
    patch.safe = SAFE;
    patch.announce('patched', 'high', null);
    render.stream(true);
    await until('fake showing', () => render.state === 'showing');

    try {
        process.exit = (code) => { throw new Error(`process.exit(${code})`); };
        runSource();
        OnlineApp.instance._profiles._pollMs = 20;
        const status = byName.wocStatus;
        await until('page shows the map', () => byName.wocMap.text === `Cluster map: Showing ${RENDER_PAGE} — ${RENDER_FPS}` && status.text === 'Status: Patched');
        assert(!logs.some((l) => /^error/.test(l)), logs.filter((l) => /^error/.test(l)).join('\n'));
        const bound = logs.filter((l) => l.startsWith('info: BIND OK: ')).map((l) => l.split(' ')[3]).sort();
        assert.deepStrictEqual(bound, ['wocAaCard', 'wocAaDisable', 'wocAaEnable', 'wocAaZoomIn', 'wocAaZoomOut', 'wocCapture', 'wocDisable', 'wocEnable',
            'wocGuide', 'wocHudCentre', 'wocHudDown', 'wocHudKeep', 'wocHudLeft', 'wocHudRight', 'wocHudUp', 'wocLogs', 'wocLogsSave', 'wocProfile',
            'wocRestart'],
            'bound buttons and checkbox');
        assert.strictEqual(byName.wocRestart.visible, false, 'restart button hidden while the map shows');
        console.log('ok   a page opened while the map is up shows it');
        assert.deepStrictEqual(tileCalls, ['init', 'availability ENABLED'],
            'the tile: enabled, its name and icon left to the app manager');
        console.log('ok   the tile keeps the name and icon the app manager gave it');

        await until('patched: CarPlay\'s Remove patch button enabled, CarPlay\'s Patch button greyed', () => byName.wocDisable.enabled === true &&
            byName.wocEnable.enabled === false);
        byName.wocDisable.emit('action');
        await until('reverted', () => patch.count('revert') === 1 && !patch.busy && patch.state === 'original');
        await until('original: CarPlay\'s Patch button enabled, CarPlay\'s Remove patch button greyed', () => byName.wocEnable.enabled === true &&
            byName.wocDisable.enabled === false);
        ['wocEnable', 'wocCapture'].forEach((name) => byName[name].emit('action'));
        await until('apply and capture sent', () => patch.count('apply') === 1 && patch.count('capture-start') === 1 && !patch.busy);
        assert.deepStrictEqual(render.received, [], 'no command to the render');
        console.log('ok   CarPlay\'s Remove patch button enabled while patched, CarPlay\'s Patch button while not; presses go to the patch daemon, none to the render');

        /* Android Auto next to CarPlay: its line, Patch, then Remove patch, the zoom and the card. */
        const aaRow = () => ['wocAaEnable', 'wocAaDisable', 'wocAaZoomOut', 'wocAaZoomIn', 'wocAaCard'].map((n) => byName[n].enabled);
        await until('Android Auto off, Patch enabled', () => byName.wocAaStatus.text === 'Status: Not patched' &&
            JSON.stringify(aaRow()) === JSON.stringify([true, false, false, false, false]) && byName.wocAaZoom.text === 'Zoom: —' &&
            byName.wocAaCard.text === "Hide phone's turn card" && byName.wocAaCard.selected === false);
        byName.wocAaEnable.emit('action');
        await until('Android Auto on', () => byName.wocAaStatus.text === 'Status: Patched' &&
            JSON.stringify(aaRow()) === JSON.stringify([false, true, true, true, true]));
        byName.wocAaZoomOut.emit('action');
        await until('zoomed out', () => patch.received.some((m) => m.cmd === 'aa-density' && m.value === 170) &&
            byName.wocAaZoom.text === 'Zoom: 170 dpi');
        assert(status.text !== '' && !/Android Auto/.test(status.text), 'CarPlay\'s line is its own');
        await until('the card shown', () => byName.wocAaCard.enabled === true && byName.wocAaCard.selected === false);
        const card = byName.wocAaCard;
        card.ticks.length = 0;
        card.press();
        await until('card hidden', () => patch.received.some((m) => m.cmd === 'aa-card' && m.value === 'hidden') &&
            patch.aa.state === 'patched' && patch.aa.diskCard === 'hidden' && card.selected === true &&
            card.text === "Hide phone's turn card");
        await sleep(100);
        assert(card.ticks.every((t) => t === true), `ticked from the press on, never unticked meanwhile: ${JSON.stringify(card.ticks)}`);
        patch.aa = Object.assign({}, patch.aa, { state: 'unknown', pending: 'apply', aapd: 'deferred', dpi: 160, density: 160, runDpi: 170, runCard: 'shown' });
        patch.announce('patched', 'high', null);
        await until('waiting for a restart', () => byName.wocAaZoom.text === 'Zoom: 160 dpi (after restart)' &&
            byName.wocAaCard.text === "Hide phone's turn card (after restart)" && byName.wocAaCard.selected === true &&
            byName.wocAaStatus.text === 'Status: Patched after restart' && byName.wocRestart.visible === true);
        patch.aa = { state: 'original', density: 160, densities: [150, 400, 10], conflict: 'not supported on this car (KI_KOMBI_VARIANT 9)' };
        patch.announce('patched', 'high', null);
        await until('not supported', () => byName.wocAaStatus.text === 'Status: Not supported on this car' &&
            JSON.stringify(aaRow()) === JSON.stringify([false, false, false, false, false]) && byName.wocRestart.visible === false);
        patch.aa = { state: 'patched', dpi: 160, density: 160, densities: [150, 400, 10] };
        patch.announce('patched', 'high', null);
        console.log('ok   Android Auto: its own line, Patch, Remove patch, the zoom and the Hide phone\'s turn card checkbox with their values, marked while waiting; ' +
            'not supported on this car: every button greyed');

        const guide = byName.wocGuide;
        await until('the switch enabled, ticked', () => guide.enabled === true && guide.selected === true);
        guide.press();
        await until('unticked by the render', () => render.hide === 0 && guide.selected === false);
        assert.deepStrictEqual(render.received, [{ cmd: 'guide', hide: 0 }], 'one guide command');
        await sleep(450);
        guide.press();
        await until('ticked again', () => render.hide === 1 && guide.selected === true);
        assert.deepStrictEqual(render.received[1], { cmd: 'guide', hide: 1 });
        render.received.length = 0;
        console.log('ok   Hide BMW\'s turn box: ticked as the render says, each press flips it through the render');

        render.guideDisabled = 1;
        render.announce(render.state, render.reason);
        await until('unavailable', () => guide.enabled === false && guide.selected === false &&
            guide.text === "Hide BMW's turn box");
        await sleep(450);
        guide.press();
        await sleep(100);
        assert.deepStrictEqual(render.received, [], 'no command while unavailable');
        render.guideDisabled = 0;
        render.announce(render.state, render.reason);
        await until('available again', () => guide.enabled === true && guide.selected === true &&
            guide.text === "Hide BMW's turn box");
        console.log('ok   held off by the cluster renderer: greyed and unticked, its label kept, presses send nothing');

        /* ---- where the map shows, and the HUD ---- */
        const showOn = byName.wocShowOn;
        const size = byName.wocHudSize;
        const hudRows = () => ['wocHudKeep', 'wocHudSize', 'wocHudLeft', 'wocHudRight', 'wocHudUp', 'wocHudDown'].map((n) => byName[n].enabled);
        await until('the cluster, the HUD off', () => showOn.shown() === 'Show on: Cluster' && showOn.enabled === true &&
            byName.wocHud.text === 'HUD map: Off' && size.text === 'Size: Standard' && size.cells['0:Value'] === 5 &&
            byName.wocHudPos.text === 'Position: Centre');
        assert(hudRows().every((e) => e === false) && byName.wocHudCentre.enabled === false, 'the size and position greyed while off');
        render.received.length = 0;
        size.turn(1);
        await until('a greyed slider turned: back where it was', () => size.cells['0:Value'] === 5);
        assert.deepStrictEqual(render.received, [], 'nothing sent');
        showOn.pick(1);
        await until('the HUD alone', () => render.hud === 1 && render.cluster === 0 && showOn.shown() === 'Show on: HUD (experimental)' &&
            byName.wocHud.text === 'HUD map: Showing' && byName.wocMap.text === 'Cluster map: Off' && byName.wocGuide.enabled === false);
        await until('the size and position enabled', () => hudRows().every((e) => e === true));
        assert.deepStrictEqual(render.received, [{ cmd: 'hud', on: '1' }, { cmd: 'cluster', on: '0' }], 'the HUD on, then the cluster off');
        await until('saved', () => patch.count('hud') === 1 && patch.count('cluster') === 1);
        size.turn(1);
        await until('larger', () => size.text === 'Size: 60 %' && render.hudWidth === 60 && size.cells['0:Value'] === 6);
        await until('saved', () => patch.received.some((m) => m.cmd === 'hud-size' && m.hud_width_pct === 60));
        size.turn(-1);
        size.turn(-1);
        await until('smaller', () => size.text === 'Size: 50 %' && render.hudWidth === 50);
        byName.wocHudLeft.emit('action');
        await until('moved', () => byName.wocHudPos.text === 'Position: 10 px left' && byName.wocHudCentre.enabled === true);
        /* the HUD map over BMW's pop-ups: gives way by default; a press keeps it shown, the render's answer ticks it, patchd saves it */
        const keep = byName.wocHudKeep;
        await until('the move at the render', () => render.hudX === -10);
        assert(keep.enabled === true && keep.selected === false && keep.text === "Keep HUD map over BMW's pop-ups", 'gives way by default');
        render.received.length = 0;
        keep.press();
        await until('kept shown', () => render.hudKeep === 1 && keep.selected === true);
        assert.deepStrictEqual(render.received, [{ cmd: 'hud', hud_keep_shown: '1' }], 'one command to the render');
        await until('saved', () => patch.received.some((m) => m.cmd === 'hud-keep-shown' && m.on === 1) && patch.map.hud_keep_shown === 1);
        await sleep(450);
        keep.press();
        await until('gives way again', () => render.hudKeep === 0 && keep.selected === false && patch.map.hud_keep_shown === 0);
        render.hudKeep = null;   /* a render without the setting */
        render.announce(render.state, render.reason);
        await until('a render without it: greyed, unticked', () => keep.enabled === false && keep.selected === false);
        render.received.length = 0;
        await sleep(450);
        keep.press();
        await until('a greyed checkbox pressed: unticked again', () => keep.selected === false);
        await sleep(100);
        assert.deepStrictEqual(render.received, [], 'nothing sent');
        render.hudKeep = 0;
        render.announce(render.state, render.reason);
        await until('enabled again', () => keep.enabled === true);
        await sleep(450);
        showOn.pick(2);
        await until('both', () => render.cluster === 1 && showOn.shown() === 'Show on: Cluster and HUD (experimental)' &&
            byName.wocGuide.enabled === true);
        render.hudState = 'disabled';
        render.announce(render.state, render.reason);
        await until('disabled after errors', () => byName.wocHud.text === 'HUD map: Disabled after errors' &&
            showOn.shown() === 'Show on: Cluster and HUD (experimental)' && hudRows().every((e) => e === false) && byName.wocHudCentre.enabled === false);
        render.hudState = null;
        await sleep(450);
        render.received.length = 0;
        showOn.pick(2);
        await sleep(100);
        assert.deepStrictEqual(render.received, [], 'the choice it has: nothing sent');
        await sleep(450);
        showOn.pick(0);
        await until('the cluster again', () => render.hud === 0 && showOn.shown() === 'Show on: Cluster' && byName.wocHud.text === 'HUD map: Off');
        render.received.length = 0;
        /* Off: the map on neither, both rows Off, the HUD's rows and the turn box greyed, saved; both again, Off from
         * both, Cluster again */
        const offShown = () => showOn.shown() === 'Show on: Off' && byName.wocMap.text === 'Cluster map: Off' &&
            byName.wocHud.text === 'HUD map: Off' && byName.wocGuide.enabled === false && hudRows().every((e) => e === false) &&
            byName.wocHudCentre.enabled === false && showOn.enabled === true;
        const saves = () => patch.received.filter((m) => /^(cluster|hud)$/.test(m.cmd)).length;
        let saved = saves();
        await sleep(450);
        showOn.pick(3);
        await until('off from the cluster', () => render.cluster === 0 && render.hud === 0 && offShown());
        assert.deepStrictEqual(render.received, [{ cmd: 'cluster', on: '0' }], 'only the cluster off');
        await until('off saved', () => saves() === saved + 1);
        assert.deepStrictEqual(patch.received.slice(-1), [{ cmd: 'cluster', on: 0 }]);
        render.received.length = 0;
        size.turn(1);
        byName.wocHudLeft.emit('action');
        await sleep(100);
        assert.deepStrictEqual(render.received, [], 'greyed while off: nothing sent');
        await sleep(450);
        showOn.pick(2);
        await until('both from off', () => render.cluster === 1 && render.hud === 1 &&
            showOn.shown() === 'Show on: Cluster and HUD (experimental)' && hudRows().every((e) => e === true) && byName.wocGuide.enabled === true);
        render.received.length = 0;
        saved = saves();
        await sleep(450);
        showOn.pick(3);
        await until('off from both', () => render.cluster === 0 && render.hud === 0 && offShown());
        assert.deepStrictEqual(render.received, [{ cmd: 'cluster', on: '0' }, { cmd: 'hud', on: '0' }], 'both off');
        await until('off from both saved', () => saves() === saved + 2);
        await sleep(450);
        showOn.pick(0);
        await until('the cluster from off', () => render.cluster === 1 && render.hud === 0 && showOn.shown() === 'Show on: Cluster' &&
            byName.wocGuide.enabled === true && /^Cluster map: Showing/.test(byName.wocMap.text));
        render.received.length = 0;
        /* the render on neither (its own configuration): Off shown, a choice to make */
        render.cluster = 0;
        render.announce(render.state, render.reason);
        await until('neither', () => showOn.shown() === 'Show on: Off' && showOn.enabled === true &&
            showOn.cells['1:ItemText'] === 'HUD (experimental)');
        await sleep(450);
        showOn.pick(0);
        await until('the cluster', () => render.cluster === 1 && showOn.shown() === 'Show on: Cluster');
        render.received.length = 0;
        console.log('ok   where the map shows: a drop-down, Cluster by default, each choice through the render and patchd, the HUD on first, ' +
            'Off for neither, the HUD\'s rows and the turn box greyed then; the HUD: off by default with its size and position greyed; ' +
            'the size slider, Position and Keep HUD map over BMW\'s pop-ups change it, the checkbox greyed for a render without it; ' +
            'Disabled after errors greys them');

        /* ---- own settings per driver profile ---- */
        const own = byName.wocProfile;
        /* no profile read yet (an empty value): greyed, nothing told, a press asks nothing */
        await sleep(100);
        assert(own.enabled === false && own.selected === false && own.text === 'Own settings for this driver', 'greyed, unticked');
        assert.strictEqual(patch.count('profile'), 0, 'an empty value tells patchd nothing');
        own.press();
        await until('a greyed checkbox pressed: unticked again', () => own.selected === false);
        await sleep(100);
        assert.strictEqual(patch.count('profile-own'), 0, 'a greyed checkbox pressed asks nothing');
        sessionUser = 'a1b2c3d4-0000-4000-8000-000000000001';
        await until('driver A: enabled, unticked', () => own.enabled === true && own.selected === false);
        await sleep(450);
        own.ticks.length = 0;
        own.press();
        await until('ticked', () => patch.received.some((m) => m.cmd === 'profile-own' && m.id === sessionUser && m.on === 1) &&
            own.selected === true && own.enabled === true);
        await sleep(100);
        assert(own.ticks.every((v) => v === true), `ticked from the press on, never unticked meanwhile: ${JSON.stringify(own.ticks)}`);
        await sleep(450);
        showOn.pick(1);
        await until('A: the HUD alone, saved', () => render.hud === 1 && render.cluster === 0 && showOn.shown() === 'Show on: HUD (experimental)' &&
            patch.map.hud === 1 && patch.map.cluster === 0);
        sessionUser = 'a1b2c3d4-0000-4000-8000-000000000002';
        await until('B, without its own: the shared settings, the cluster', () => render.cluster === 1 && render.hud === 0 &&
            showOn.shown() === 'Show on: Cluster' && own.selected === false && own.enabled === true);
        sessionUser = 'a1b2c3d4-0000-4000-8000-000000000001';
        await until('A again: its HUD, ticked', () => render.hud === 1 && render.cluster === 0 && showOn.shown() === 'Show on: HUD (experimental)' &&
            own.selected === true);
        await sleep(450);
        own.press();
        await until('unticked: the shared settings back', () => own.selected === false && own.enabled === true && render.cluster === 1 &&
            render.hud === 0 && showOn.shown() === 'Show on: Cluster');
        const told = patch.count('profile');
        sessionUser = '';
        await sleep(150);
        assert(own.enabled === true && patch.count('profile') === told && showOn.shown() === 'Show on: Cluster', 'an empty read changes nothing');
        sessionUser = 'a1b2c3d4-0000-4000-8000-000000000001';
        render.received.length = 0;
        console.log('ok   Own settings for this driver: greyed until a profile is read; ticked, the profile keeps its own map settings; ' +
            'a switch moves the render to the other profile\'s or the shared settings; unticked, the shared ones back; an empty read changes nothing');

        byName.ST_OnlineApps_WazeCluster.emit('visible', { visible: false });
        await until('sockets closed', () => render._clients.size === 0 && patch._clients.size === 0);
        console.log('ok   hiding the page closes both sockets');

        render.stream(false);
        patch.announce('patched', 'high', null);
        byName.ST_OnlineApps_WazeCluster.emit('visible', { visible: true });
        await until('page reads the current state', () => status.text === 'Status: Patched' && byName.wocMap.text === 'Cluster map: Waiting for the phone');
        console.log('ok   a page opened again reads the render\'s current state');

        /* The patch the install wrote, from the daemon: its line, Patch
         * greyed, Remove patch and the restart offered. */
        patch.pending = 'apply';
        patch.announce('unknown', 'high', 'patch written; takes effect at the next head-unit start', 'deferred');
        await until('pending on screen', () => status.text === 'Status: Patched after restart' && byName.wocMap.text === 'Cluster map: Waiting for the phone' &&
            byName.wocEnable.enabled === false && byName.wocDisable.enabled === true &&
            byName.wocRestart.visible === true && byName.wocRestart.enabled === true);
        console.log('ok   a pending patch from the daemon: its line, Patch greyed, Remove patch and the restart offered');

        /* Before the head-unit start that starts the daemon: from the marker. */
        byName.ST_OnlineApps_WazeCluster.emit('visible', { visible: false });
        await until('closed', () => patch._clients.size === 0);
        await patch.close();
        fs.writeFileSync(PENDING, JSON.stringify({ action: 'apply' }) + '\n');
        byName.ST_OnlineApps_WazeCluster.emit('visible', { visible: true });
        await until('pending from the marker', () => status.text === 'Status: Patched after restart' &&
            byName.wocEnable.enabled === false && byName.wocDisable.enabled === false && byName.wocRestart.visible === false &&
            byName.wocAaStatus.text === 'Status: Unavailable');
        console.log('ok   installed, the daemon not yet running: the marker\'s pending patch, every button greyed');

        /* The daemon up with the pending patch: the restart press reboots once. */
        patch = new FakePatch(PSOCK);
        await patch.listen();
        patch.pending = 'apply';
        patch.announce('unknown', 'high', 'patch written; takes effect at the next head-unit start', 'deferred');
        await until('restart offered', () => byName.wocRestart.visible === true && byName.wocRestart.enabled === true &&
            byName.wocRestartStatus.visible === false);
        /* NSM does not take it: the reason on the restart's line, CarPlay's line as it was */
        patch.rebootFails = 'refused';
        byName.wocRestart.emit('action');
        await until('failed on screen', () => byName.wocRestartStatus.text === 'Restart: Failed, not accepted' &&
            byName.wocRestartStatus.visible === true && status.text === 'Status: Patched after restart' && byName.wocRestart.enabled === true);
        console.log('ok   a failed restart: its reason on the restart\'s own line, CarPlay\'s line unchanged, the button back');
        patch.rebootFails = null;
        await sleep(450);
        byName.wocRestart.emit('action');
        await until('rebooted once more', () => patch.reboots === 2);
        await until('restarting on screen', () => byName.wocRestartStatus.text === 'Restart: In progress' && byName.wocRestart.enabled === false &&
            status.text === 'Status: Patched after restart');
        await sleep(450);
        byName.wocRestart.emit('action');
        await sleep(100);
        assert.strictEqual(patch.count('reboot'), 2, 'a greyed button pressed sends nothing');
        console.log('ok   pressing the restart button restarts the unit once; pressed again while greyed, nothing');

        /* CarPlay's Wi-Fi band: its row while patchd knows it, the line under
         * it on 2.4 GHz only, neither without patchd. */
        const wifi = byName.wocWifi, hint = byName.wocWifiHint;
        assert.deepStrictEqual([wifi.visible, hint.visible], [false, false], 'no band: neither row');
        patch.setWifi({ band: '5', iface: 'p2p-wlan0-0', freq: 5785 });
        await until('5 GHz', () => wifi.visible === true && wifi.text === 'CarPlay Wi-Fi: 5 GHz' && hint.visible === false);
        assert(/^[A-Z][A-Za-z -]+: [0-9]/.test(wifi.text), 'CarPlay Wi-Fi as Name: Value');
        patch.setWifi({ band: '2.4', iface: 'p2p-wlan0-1', freq: 2462 });
        await until('2.4 GHz', () => wifi.text === 'CarPlay Wi-Fi: 2.4 GHz' && wifi.visible === true && hint.visible === true &&
            hint.text === 'Audio may cut out. On your iPhone, turn Wi-Fi off and on.');
        patch.setWifi({ band: '6', iface: 'p2p-wlan0-1', freq: 5955 });
        await until('a band the app does not know', () => wifi.visible === false && hint.visible === false);
        patch.setWifi({ band: '2.4', iface: 'p2p-wlan0-1', freq: 2462 });
        await until('2.4 GHz again', () => wifi.visible === true && hint.visible === true);
        patch.setWifi(null);
        await until('unknown', () => wifi.visible === false && hint.visible === false);
        console.log('ok   CarPlay Wi-Fi: 5 GHz alone, 2.4 GHz with the line under it; neither while the band is unknown');

        /* Through every state above, no row came or went but CarPlay Wi-Fi,
         * its line and the restart's, and each button was greyed and enabled
         * in place. */
        assert.deepStrictEqual([...shownOrHidden].sort(), ['wocRestart', 'wocRestartStatus', 'wocWifi', 'wocWifiHint'],
            'only CarPlay Wi-Fi, its line and the restart\'s rows are shown and hidden');
        Object.values(components).filter((d) => !/^(wocRestart|wocWifi)/.test(d.name) && d.type !== 'title')
            .forEach((d) => assert.strictEqual(byName[d.name].visible, true, `${d.name} stays on the page`));
        ['wocEnable', 'wocDisable', 'wocAaEnable', 'wocAaDisable', 'wocAaZoomOut', 'wocAaZoomIn', 'wocAaCard', 'wocGuide', 'wocShowOn', 'wocProfile', 'wocHudSize',
            'wocHudLeft', 'wocHudCentre', 'wocHudKeep']
            .forEach((n) => assert.deepStrictEqual([...byName[n].enabledSeen].sort(), [false, true], `${n} greyed and enabled in place`));
        const valueRow = /^[A-Z][A-Za-z ]+: [A-Z0-9\u2014]/;
        ['wocStatus', 'wocAaStatus', 'wocAaZoom', 'wocMap', 'wocHud', 'wocHudSize', 'wocHudPos', 'wocRestartStatus']
            .forEach((n) => assert(valueRow.test(byName[n].text), `${n}: "${byName[n].text}" as Name: Value`));
        console.log('ok   no row came or went but CarPlay Wi-Fi, its line and the restart\'s; the buttons were greyed and enabled in place; ' +
            'info rows as Name: Value');

        /* ---- the Logs page ---- */
        const lserver = new FakeLogs(LSOCK);
        await lserver.listen();
        const main = byName.ST_OnlineApps_WazeCluster;
        const logsState = byName.ST_OnlineApps_WazeClusterLogs;
        await until('the Logs button', () => byName.wocLogs.visible === true);
        byName.wocLogs.emit('action');
        await until('Logs opened', () => rhmi.opened.indexOf('ST_OnlineApps_WazeClusterLogs') >= 0);
        await until('capture on the Logs page', () => byName.wocCapture.text === 'Start capture' &&
            byName.wocCaptureStatus.text === 'Capture: Unavailable');
        const inactiveBefore = inactiveReports;
        main.emit('visible', { visible: false });
        await sleep(100);
        logsState.emit('visible', { visible: true });
        await until('logs socket open', () => lserver._clients.size === 1);
        await sleep(900);
        assert.strictEqual(inactiveReports, inactiveBefore, 'moving to the Logs page keeps the app shown');
        assert.strictEqual(patch._clients.size, 1, 'and its sockets open');
        await until('logs state shown', () => byName.wocLogsStatus.text === 'Saved logs: None' &&
            byName.wocLogsUrl.text === `Address: ${lserver.url}` && byName.wocLogsSave.enabled === true);
        const qr1 = byName.wocLogsQr.text;
        assert(qr1 && rhmi.handles.has(qr1), 'the QR code shown through a resource handle');
        assert.deepStrictEqual(fs.readFileSync(qr1).slice(0, 8), Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
        assert.strictEqual(fs.statSync(qr1).mode & 0o777, 0o644, 'the HMI reads it');
        console.log('ok   Logs opens the Logs page; the app stays shown; the page shows the address and its QR code');

        byName.wocLogsSave.emit('action');
        ['wocCaptureStatus', 'wocLogsStatus', 'wocLogsUrl'].forEach((n) => assert(/^[A-Z][a-z ]+: [A-Z0-9h]/.test(byName[n].text), byName[n].text));
        await until('saved', () => byName.wocLogsStatus.text === 'Saved logs: 1' &&
            /^woc-logs-\d{8}T\d{6}Z\.tar\.gz · 412 KB$/.test(byName.wocLogsRow1.text));
        assert.strictEqual(lserver.count('save'), 1);
        assert.strictEqual(byName.wocLogsRow2.text, '');
        console.log('ok   Save logs saves once and lists the archive');

        lserver.url = 'http://172.16.222.97:8475/fedcba9876543210fedcba9876543210/';
        lserver.announce();
        await until('a new QR code', () => byName.wocLogsUrl.text === `Address: ${lserver.url}` && byName.wocLogsQr.text !== qr1);
        await until('the old one gone', () => !fs.existsSync(qr1) && !rhmi.handles.has(qr1));
        lserver.url = null;
        lserver.announce();
        await until('no address', () => byName.wocLogsUrl.text === 'Address: Wi-Fi hotspot off');
        const blank = byName.wocLogsQr.text;
        await until('blank code', () => rhmi.handles.has(blank) && fs.existsSync(blank));
        console.log('ok   a new address gets a new QR code, the old one removed; no address: a blank one');

        /* an overlay takes the screen (ROOT_APP gone) and gives it back; the
         * Logs page says nothing of its own */
        lserver.url = 'http://172.16.222.97:8475/0123456789abcdef0123456789abcdef/';
        lserver.announce();
        rhmi.emit('applicationStackChanged', { OVERLAY: 1 });
        await until('hidden: logs socket closed', () => lserver._clients.size === 0);
        rhmi.emit('applicationStackChanged', { ROOT_APP: 1 });
        await until('back: logs socket open', () => lserver._clients.size === 1 && byName.wocLogsUrl.text === `Address: ${lserver.url}` &&
            byName.wocLogsStatus.text !== 'Saved logs: Connecting…');
        console.log('ok   back from an overlay on the Logs page, the logs socket opens again');

        logsState.emit('visible', { visible: false });
        main.emit('visible', { visible: true });
        await until('logs socket closed', () => lserver._clients.size === 0);
        rhmi.emit('applicationStackChanged', { OVERLAY: 1 });
        await sleep(50);
        rhmi.emit('applicationStackChanged', { ROOT_APP: 1 });
        await sleep(200);
        assert.strictEqual(lserver._clients.size, 0, 'back on the main page: no logs socket');

        /* left from the Logs page, launched again from the tile: the main
         * page, no logs socket (which would start the page server) */
        byName.wocLogs.emit('action');
        main.emit('visible', { visible: false });
        logsState.emit('visible', { visible: true });
        await until('on the Logs page', () => lserver._clients.size === 1);
        rhmi.emit('applicationStackChanged', { OTHER_APP: 1 });
        await until('left: logs socket closed', () => lserver._clients.size === 0);
        rhmi.emit('applicationStackChanged', { ROOT_APP: 1 });
        OnlineApp.instance.emit('entryPointExecute', APP_JSON.application.entryPoints[0].uuid);
        await sleep(400);
        assert.strictEqual(lserver._clients.size, 0, 'a tile launch opens no logs socket');
        assert.strictEqual(patch._clients.size, 1, 'the main page\'s sockets are open');
        console.log('ok   launched from the tile after leaving on the Logs page: the main page, no logs socket');
        assert.strictEqual(patch._clients.size, 1, 'the main page keeps its sockets');
        console.log('ok   leaving the Logs page closes the logs socket');

        /* the display language: every row in its words, English for a word it lacks */
        assert.deepStrictEqual(hmiContexts, ['de.bmw.infotainment.hmi.hmicontext.HMIContext HMI_CID'], 'HMIContext HMI_CID followed');
        const de = require('../src/locales.js').find((l) => l.code === 'de');
        const deWords = de.words;
        de.words = { Patch: 'Patchen', 'Remove patch': 'Patch entfernen', General: 'Allgemein', 'Save logs': 'Logs speichern',
            'Status: {0}': 'Zustand: {0}', 'Saved logs: {0}': 'Gespeicherte Logs: {0}', "Hide BMW's turn box": 'BMW-Abbiegebox ausblenden',
            'HUD map: {0}': 'Karte im HUD: {0}', 'Show on': 'Anzeigen auf', 'HUD (experimental)': 'HUD (experimentell)', 'Move left': 'Nach links' };
        const before = ['wocStatus', 'wocAaStatus', 'wocMap', 'wocLogsStatus', 'wocHud'].map((n) => byName[n].text);
        displayLanguage('DEUTSCH');
        await until('German', () => byName.wocEnable.text === 'Patchen' && byName.wocAaDisable.text === 'Patch entfernen' &&
            byName.wocHeadGeneral.text === 'Allgemein' && byName.wocLogsSave.text === 'Logs speichern' &&
            byName.wocGuide.text === 'BMW-Abbiegebox ausblenden' && byName.wocStatus.text === before[0].replace(/^Status:/, 'Zustand:') &&
            byName.wocAaStatus.text === before[1].replace(/^Status:/, 'Zustand:') && byName.wocLogsStatus.text.startsWith('Gespeicherte Logs: ') &&
            byName.wocHud.text === before[4].replace(/^HUD map:/, 'Karte im HUD:') && byName.wocShowOn.cells.HeaderText === 'Anzeigen auf' &&
            byName.wocShowOn.cells['1:ItemText'] === 'HUD (experimentell)' && byName.wocShowOn.cells['0:ItemText'] === 'Cluster' &&
            byName.wocHudLeft.text === 'Nach links');
        assert.strictEqual(byName.wocMap.text, before[2], 'a word German lacks: its English');
        assert.strictEqual(byName.wocAaZoomIn.text, 'Zoom in', 'a word German lacks: its English');
        displayLanguage('ENGLISCH__US');
        await until('English again', () => byName.wocEnable.text === 'Patch' && byName.wocHeadGeneral.text === 'General' &&
            byName.wocLogsSave.text === 'Save logs' && byName.wocGuide.text === "Hide BMW's turn box" && byName.wocStatus.text === before[0] &&
            byName.wocHud.text === before[4] && byName.wocHudLeft.text === 'Move left');
        de.words = deWords;
        console.log('ok   a display language change writes every row again in its words, English for a word it lacks');
        await lserver.close();
    } finally {
        await render.close();
        await patch.close();
        [RSOCK, PSOCK, PENDING, LSOCK].forEach((p) => { try { fs.unlinkSync(p); } catch (e) { /* gone */ } });
    }
}

/* The app's sockets keep retrying, so the run ends here. */
main().then(() => {
    console.log('all passed');
    exit(0);
}, (err) => {
    console.log(`FAIL ${err && err.stack}`);
    exit(1);
});
