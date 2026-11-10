// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/**
 * robertn_waze_cluster - the control panel for the native
 * waze-on-cluster tool, which puts the CarPlay cluster map on the instrument
 * cluster. It drives the tool entirely from the HMI, so no shell is needed.
 *
 * The main page, one LT_State_Wide list in four sections, each under a
 * header row: General, HUD, CarPlay, Android Auto, then the restart's two
 * rows. General: Show on (a drop-down of Cluster, HUD, both and Off), the Own
 * settings for this driver checkbox (profile_sync.js; greyed while no driver
 * profile is detected), Cluster map (the shared render's map), the Hide
 * BMW's turn box checkbox (the render's setting; a press asks the render to
 * flip it), Logs. HUD: HUD map (the map
 * on the HUD), the Keep HUD map over BMW's pop-ups checkbox, the Size
 * slider, Position with Move left, Move right, Move up, Move down and
 * Centre; the checkbox, the slider and the position buttons are greyed
 * while the HUD is off or disabled after errors. CarPlay and Android Auto
 * have the same rows: Status, Patch, Remove patch; CarPlay adds CarPlay
 * Wi-Fi (the band, 5 GHz or 2.4 GHz) and, on 2.4 GHz, the line under it;
 * Android Auto adds Zoom with Zoom out and Zoom in, and the Hide phone's
 * turn card checkbox. Info rows read "Name: Value"; every on/off setting is
 * a checkbox. One that cannot act now is greyed, and while patchd runs any
 * operation every operation is. Every row stays on the page but four, the
 * only rows shown and hidden: CarPlay Wi-Fi while CarPlay runs over Wi-Fi
 * and patchd knows the band, and its line on 2.4 GHz, CarPlay's last two
 * (only Android Auto's rows move); Restart head unit while a written change
 * waits for the next head-unit start, and the Restart row while it has
 * something to say, the page's last two (no row moves). Once patched, the
 * render puts the map on the cluster whenever the phone streams it, with no
 * button.
 *
 * The Logs page (logs_control.js): Start capture and the Capture row, Save
 * logs and the Saved logs row, the QR code of the address a phone on the
 * car's hotspot downloads the saved archives from, that address, and the
 * archives. The app holds the logs socket while the page is visible; the QR
 * code is a PNG in QR_DIR (RAM), replaced when the address changes.
 *
 * CarPlay's Patch button is enabled for an unpatched or unknown state and Remove
 * patch for a patch, both only with no change pending (waze_cluster_control.js
 * canEnable/canDisable); capture is enabled while the patch daemon is
 * reachable. The app connects both sockets while one of its pages is visible
 * and closes them when it is hidden or the app shuts down; the tools keep
 * running. Moving between its pages hides one before the other shows, so a
 * hide counts only after HIDE_SETTLE_MS. waze_cluster_control.js holds the
 * protocol.
 *
 * Every word on both pages is in the HMI's display language (src/i18n.js),
 * read from HMIContext at start and on each change, which writes every row
 * again; English until then, and without it. The tile's name and the Logs
 * page's title are string ids the HMI shows itself (the RHMI text files).
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const OnlineApp = require('oap-sdk/src/core/OnlineApp.js');
const logging = require('oap-sdk/src/logging/logging');
const someipBuilders = require('oap-sdk/src/someip/someipBuilders');
const i18n = require('./i18n.js');
const { WazeClusterControl, ZOOM_STEP, SHOWN_ON } = require('./waze_cluster_control.js');
const { LogsControl, ROWS: LOGS_ROWS } = require('./logs_control.js');
const { qrPng } = require('./qr.js');
const { ProfileSync, sessionReader } = require('./profile_sync.js');

const logger = logging.getSDKLogger();

const STATE_NAME = 'ST_OnlineApps_WazeCluster';
const STATUS_NAME = 'wocStatus';
const MAP_NAME = 'wocMap';
const ENABLE_NAME = 'wocEnable';
const DISABLE_NAME = 'wocDisable';
const RESTART_NAME = 'wocRestart';
const RESTART_STATUS_NAME = 'wocRestartStatus';
const WIFI_NAME = 'wocWifi';
const WIFI_HINT_NAME = 'wocWifiHint';
const AA_STATUS_NAME = 'wocAaStatus';
const AA_ENABLE_NAME = 'wocAaEnable';
const AA_DISABLE_NAME = 'wocAaDisable';
const AA_ZOOM_NAME = 'wocAaZoom';
const AA_ZOOM_OUT_NAME = 'wocAaZoomOut';
const AA_ZOOM_IN_NAME = 'wocAaZoomIn';
const AA_CARD_NAME = 'wocAaCard';
const GUIDE_NAME = 'wocGuide';
const HEAD_GENERAL_NAME = 'wocHeadGeneral';
const HEAD_HUD_NAME = 'wocHeadHud';
const HUD_NAME = 'wocHud';
const HUD_KEEP_NAME = 'wocHudKeep';
const SHOW_ON_NAME = 'wocShowOn';
const PROFILE_NAME = 'wocProfile';
const HUD_SIZE_NAME = 'wocHudSize';
const HUD_POS_NAME = 'wocHudPos';
const HUD_MOVE_NAMES = { left: 'wocHudLeft', right: 'wocHudRight', up: 'wocHudUp', down: 'wocHudDown' };
const HUD_CENTRE_NAME = 'wocHudCentre';
const CAPTURE_NAME = 'wocCapture';
const CAPTURE_STATUS = 'wocCaptureStatus';
const LOGS_NAME = 'wocLogs';
const LOGS_STATE = 'ST_OnlineApps_WazeClusterLogs';
const LOGS_SAVE = 'wocLogsSave';
const LOGS_STATUS = 'wocLogsStatus';
const LOGS_QR = 'wocLogsQr';
const LOGS_URL = 'wocLogsUrl';
const LOGS_ROW_NAMES = Array.from({ length: LOGS_ROWS }, (_, i) => `wocLogsRow${i + 1}`);
const ROW_CHARS = 90;
const PRESS_DEDUPE_MS = 400;
const HIDE_SETTLE_MS = 800;
const HMI_CONTEXT = ['de.bmw.infotainment.hmi.hmicontext.HMIContext', 'HMI_CID'];
const { t } = i18n;
const QR_DIR = path.join(fs.existsSync('/dev/shm') ? '/dev/shm' : os.tmpdir(),
    `robertn_waze_cluster_qr_${typeof process.getuid === 'function' ? process.getuid() : 'x'}`);

/* A line as one row: no line breaks, at most ROW_CHARS characters. */
function oneRow(line) {
    const flat = line.replace(/\s+/g, ' ');
    return flat.length > ROW_CHARS ? `${flat.slice(0, ROW_CHARS - 1)}…` : flat;
}

/* RHMI event values arrive as booleans, or as 1/0 on some HMIs. */
function isTrue(raw) {
    return raw === true || raw === 'true' || raw === 1 || raw === '1';
}

class WazeClusterApp extends OnlineApp {
    constructor() {
        super();
        this._state = null;
        this._components = {};
        this._actionById = {};
        this._lastPress = {};
        this._written = {};
        this._visible = false;
        this._pages = { main: false, logs: false };
        this._shownPage = 'main';   /* the page last shown, which shows again when the app does */
        this._hideTimer = null;
        this._qr = { url: undefined, file: null, chain: Promise.resolve() };
        this._sawForeground = false;
        this._shutdownPromise = null;
        this._control = new WazeClusterControl({ log: logger });
        this._control.on('change', () => this._render());
        this._logs = new LogsControl({ log: logger });
        this._logs.on('change', () => this._renderLogs());
        /* the map settings per driver profile, page shown or not */
        this._profiles = new ProfileSync({ control: this._control, log: logger, readProfile: sessionReader(someipBuilders, logger) });
        this._profiles.on('change', () => this._render());

        this.on('initialized', () => {
            this._appInitialized()
                .catch((err) => logger.error(`robertn_waze_cluster init failed: ${err && err.message}`));
        });
        this.on('shutdown', (reason) => this._handleShutdown(reason));
        this.on('error', (err) => logger.warn(`OnlineApp error: ${err && err.message}`));
        this.on('entryPointExecute', (uuid) => this._onEntryPointExecute(uuid));
        this.on('changeVisibility', (data) => {
            if (data && data.visible === false) {
                this._setVisible(false);
            }
        });
    }

    _appInitialized() {
        logger.info(`robertn_waze_cluster ${this.getAppId()} v${this._packageJson.version} starting`);
        this._profiles.start();
        if (!this.rhmiApplication) {
            logger.info('no RhmiApplication for this start reason; initializing without a screen');
            return this.initialized();
        }
        const foregroundStart = this._startedForeground();
        return this._setupRhmi()
            .then(() => this._setupLanguage())
            .then(() => this._registerEntryPoints())
            .then(() => {
                // A foreground start declares itself ACTIVE before initialized()
                // reports the first app status, so the manager keeps it running.
                if (foregroundStart) {
                    this.registerActive();
                }
                return this.initialized();
            })
            .then(() => {
                if (foregroundStart) {
                    this._sawForeground = true;
                    this._setVisible(true);
                }
                const lastUuid = this.getLastExecutedEntryPointUuid &&
                    this.getLastExecutedEntryPointUuid();
                return lastUuid ? this._onEntryPointExecute(lastUuid) : undefined;
            });
    }

    _startedForeground() {
        const FOREGROUND_REASONS = ['ENTRY_POINT', 'LUM', 'BOOKMARK',
            'START_WIDGET', 'TRIGGER_HMI', 'SPEECH_COMMAND'];
        const data = this.getVisibilityData && this.getVisibilityData();
        const reason = (data && data.reason) || this.startReason;
        return FOREGROUND_REASONS.includes(reason);
    }

    /* ---- RHMI ---- */
    _setupRhmi() {
        const rhmi = this.rhmiApplication;
        this._state = rhmi.getByName(STATE_NAME);
        this._logsState = rhmi.getByName(LOGS_STATE);
        if (!this._state || !this._logsState) {
            return Promise.reject(new Error(`uiDescription has no ${STATE_NAME} or ${LOGS_STATE}`));
        }
        const names = [STATUS_NAME, MAP_NAME, RESTART_STATUS_NAME, ENABLE_NAME, DISABLE_NAME, WIFI_NAME, WIFI_HINT_NAME, AA_STATUS_NAME,
            AA_ENABLE_NAME, AA_DISABLE_NAME, AA_ZOOM_NAME, AA_ZOOM_OUT_NAME, AA_ZOOM_IN_NAME, AA_CARD_NAME, RESTART_NAME, GUIDE_NAME,
            HEAD_GENERAL_NAME, HEAD_HUD_NAME, SHOW_ON_NAME, PROFILE_NAME, HUD_NAME, HUD_KEEP_NAME, HUD_SIZE_NAME, HUD_POS_NAME]
            .concat(Object.values(HUD_MOVE_NAMES), [HUD_CENTRE_NAME, CAPTURE_NAME, CAPTURE_STATUS,
                LOGS_NAME, LOGS_SAVE, LOGS_STATUS, LOGS_QR, LOGS_URL], LOGS_ROW_NAMES);
        names.forEach((name) => { this._components[name] = rhmi.getByName(name); });
        const missing = names.filter((name) => !this._components[name]);
        if (missing.length) {
            return Promise.reject(new Error(`uiDescription lacks ${missing.join(', ')}`));
        }
        this._bind(ENABLE_NAME, () => this._control.enableMap());
        this._bind(DISABLE_NAME, () => this._control.disableMap());
        this._bind(AA_ENABLE_NAME, () => this._control.enableAa());
        this._bind(AA_DISABLE_NAME, () => this._control.disableAa());
        this._bind(AA_ZOOM_OUT_NAME, () => this._control.zoom(-ZOOM_STEP));
        this._bind(AA_ZOOM_IN_NAME, () => this._control.zoom(ZOOM_STEP));
        this._bind(AA_CARD_NAME, () => {
            /* as for the turn box: patchd's answer decides the tick */
            delete this._written[`selected:${AA_CARD_NAME}`];
            this._control.toggleCard();
        });
        this._bind(RESTART_NAME, () => this._control.requestRestart());
        this._bind(GUIDE_NAME, () => {
            /* the HMI ticks or unticks the box itself; the render's answer
             * decides what it shows */
            delete this._written[`selected:${GUIDE_NAME}`];
            this._control.toggleGuidance();
        });
        this._bind(HUD_KEEP_NAME, () => {
            /* as for the turn box: the render's answer decides the tick */
            delete this._written[`selected:${HUD_KEEP_NAME}`];
            this._control.toggleHudKeep();
            this._render();
        });
        this._bind(PROFILE_NAME, () => {
            /* as for the turn box: patchd's answer decides the tick */
            delete this._written[`selected:${PROFILE_NAME}`];
            this._profiles.setOwn(!this._profiles.own);
            this._render();
        });
        /* The HMI moves the drop-down's choice and the slider itself; the
         * control's state, as asked and then as the render says, decides
         * what they show. */
        this._components[SHOW_ON_NAME].on('action', (ev) => this._press(SHOW_ON_NAME, () => {
            delete this._written[`cell:${SHOW_ON_NAME}:SelectedItem`];
            this._control.showOn(SHOWN_ON[ev && ev.row]);
            this._render();
        }));
        /* the SDK writes the slider's new step after telling it: the step it
         * shows is written again after that */
        this._components[HUD_SIZE_NAME].on('changed', (step) => {
            if (step !== this._written[`cell:${HUD_SIZE_NAME}:0:Value`]) {
                logger.info(`SLIDER ${HUD_SIZE_NAME} ${step}`);
                delete this._written[`cell:${HUD_SIZE_NAME}:0:Value`];
                this._control.setHudSizeStep(step);
                setImmediate(() => this._render());
            }
        });
        Object.keys(HUD_MOVE_NAMES).forEach((dir) => this._bind(HUD_MOVE_NAMES[dir], () => this._control.moveHud(dir)));
        this._bind(HUD_CENTRE_NAME, () => this._control.centreHud());
        this._bind(CAPTURE_NAME, () => this._control.capture());
        this._bind(LOGS_NAME, () => Promise.resolve(rhmi.openState(LOGS_STATE))
            .catch((err) => logger.warn(`openState ${LOGS_STATE} failed: ${err && err.message}`)));
        this._bind(LOGS_SAVE, () => this._logs.save());
        // Presses also arrive as the application's raw 'userEvent'; both paths
        // land in _press, which keeps a press delivered twice to one.
        rhmi.on('userEvent', (ev) => {
            const entry = ev && ev.eventType === 'ACTION' && this._actionById[Number(ev.sourceId)];
            if (entry) {
                this._press(entry.name, entry.handler);
            }
        });
        rhmi.on('applicationStackChanged', (stack) => {
            // The app holds the foreground while the stack has a ROOT_APP slot;
            // the empty stack seen during startup is ignored.
            const keys = stack && typeof stack === 'object' ? Object.keys(stack) : [];
            if (!keys.length) {
                return;
            }
            if (Object.prototype.hasOwnProperty.call(stack, 'ROOT_APP')) {
                this._sawForeground = true;
                this._setVisible(true);
            } else if (this._sawForeground) {
                this._setVisible(false);
            }
        });
        rhmi.on('restoreHmi', () => {
            logger.info('RESTORE_HMI: re-pushing page content');
            this._written = {};
            this._qr.url = undefined;
            this._render();
            this._renderLogs();
        });
        this._state.on('visible', (event) => this._pageVisible('main', isTrue(event && event.visible)));
        this._logsState.on('visible', (event) => this._pageVisible('logs', isTrue(event && event.visible)));
        this._removeStaleQr();
        return Promise.all([this._state.updateResources(), this._logsState.updateResources()]).then(() => {
            logger.info(`page ready: ${names.length} components found`);
            this._render();
            this._renderLogs();
        });
    }

    /* The words follow the HMI's display language: read at start and on
     * each change; English until then, and without it. Never holds up the
     * start. */
    _setupLanguage() {
        logger.info(`languages: en ${i18n.codes().join(' ')}`);
        const [iface, instance] = HMI_CONTEXT;
        let proxy;
        Promise.resolve()
            .then(() => someipBuilders.buildTypedProxy(iface, instance))
            .then((p) => { proxy = p; return proxy.init(); })
            .then(() => proxy.waitUntilAvailable())
            .then(() => proxy.currentDisplayLanguage.subscribe((v) => this._displayLanguage(v)))
            .then(() => proxy.currentDisplayLanguage.getValue())
            .then((v) => this._displayLanguage(v))
            .catch((err) => logger.warn(`display language unavailable from ${iface}: ${err && err.message}`));
    }

    _displayLanguage(value) {
        if (i18n.setHmiLanguage(value)) {
            logger.info(`display language ${value}: ${i18n.language()}`);
            this._render();
            this._renderLogs();
        }
    }

    _bind(name, handler) {
        const comp = this._components[name];
        let id = null;
        try {
            id = comp.getId();
        } catch (e) {
            id = null;
        }
        if (id !== null) {
            this._actionById[Number(id)] = { name, handler };
        }
        logger.info(`BIND OK: ${name} (id ${id})`);
        comp.on('action', () => this._press(name, handler));
    }

    _press(name, handler) {
        const now = Date.now();
        if (now - (this._lastPress[name] || 0) < PRESS_DEDUPE_MS) {
            return;
        }
        this._lastPress[name] = now;
        logger.info(`BUTTON ACTION ${name}`);
        try {
            handler();
        } catch (err) {
            logger.warn(`${name} failed: ${err && err.message}`);
        }
    }

    /* The app manager inserts the public tile from application.json, with its
     * name and icon from the manager's own bundles, and hands it to the app at
     * start; the SDK emits 'entryPointExecute' on a tap. The app leaves the
     * name and icon alone: its own bundles end with its process, and a tile
     * pointing at them shows no name once the manager stops the app. */
    _registerEntryPoints() {
        const descriptions = (this.appDescription &&
            this.appDescription.application.entryPoints) || [];
        const ours = this.getEntryPoints().find(
            (ep) => descriptions.some((d) => d.uuid === ep.getDescription().uuid));
        if (!ours) {
            return Promise.reject(new Error('no entry point handed over by the manager'));
        }
        const description = ours.getDescription();
        return ours.init()
            .then(() => this.rhmiApplication.mapEntryState(description.uuid, this._state))
            .then(() => ours.setAvailability('ENABLED'))
            .then(() => logger.info(`entry point ${description.name} ENABLED`));
    }

    _onEntryPointExecute(uuid) {
        if (this._shutdownPromise || !this.rhmiApplication || !uuid) {
            return Promise.resolve();
        }
        this._sawForeground = true;
        /* the tile opens the main page, whichever page the app was left on;
         * its stack change may have come first and opened the Logs page's socket */
        this._shownPage = 'main';
        this._pages.logs = false;
        this._logs.close();
        this._setVisible(true);
        return Promise.resolve(this.rhmiApplication.openEntryState(uuid))
            .catch((err) => logger.warn(`openEntryState failed: ${err && err.message}`));
    }

    /* ---- visibility ---- */
    /* One page shown or hidden. The logs socket follows the Logs page; the
     * app counts as hidden once neither page has shown for HIDE_SETTLE_MS. */
    _pageVisible(page, visible) {
        if (this._shutdownPromise) {
            return;
        }
        this._pages[page] = visible;
        if (visible) {
            this._shownPage = page;
        }
        if (page === 'logs') {
            if (visible) {
                this._logs.open();
            } else {
                this._logs.close();
            }
        }
        clearTimeout(this._hideTimer);
        this._hideTimer = null;
        if (visible) {
            this._sawForeground = true;
            this._setVisible(true);
        } else if (!this._pages.main && !this._pages.logs) {
            this._hideTimer = setTimeout(() => {
                this._hideTimer = null;
                this._setVisible(false);
            }, HIDE_SETTLE_MS);
        }
    }

    _setVisible(visible) {
        if (!visible) {
            this._pages = { main: false, logs: false };
            this._logs.close();
        }
        if (this._shutdownPromise || this._visible === visible) {
            return;
        }
        this._visible = visible;
        logger.info(`page ${visible ? 'visible' : 'hidden'}`);
        this._reportVisibilityStatus(visible);
        if (visible) {
            this._control.open();
            this._profiles.refresh();
            /* back from an overlay or another app on the Logs page, which
             * says nothing of its own */
            if (this._shownPage === 'logs') {
                this._pages.logs = true;
                this._logs.open();
            }
        } else {
            this._control.close();
        }
    }

    _reportVisibilityStatus(visible) {
        if (!this._initialized || !this._appStatusService) {
            return;
        }
        const reports = visible ?
            [this.registerActive(), this.registerHmiVisible()] :
            [this.unregisterHmiVisible(), this.registerInactive()];
        reports.forEach((report) => Promise.resolve(report).catch((err) =>
            logger.warn(`app status report failed: ${err && err.message}`)));
    }

    /* ---- rendering ---- */
    /* Writes text into a component's row unless it already shows it. */
    _write(name, text) {
        if (this._written[name] !== text) {
            this._written[name] = text;
            Promise.resolve(this._components[name].setValue(0, 'Text', text, 'Text'))
                .catch((err) => logger.warn(`${name} render failed: ${err && err.message}`));
        }
    }

    /* Writes a cell unless it already holds that value: a cell of the
     * component's (row null) or of its row. The slider's value goes through
     * the slider's own setStepValue, which keeps its step in step with it. */
    _writeCell(name, cell, value, type, row) {
        const key = `cell:${name}:${row == null ? '' : `${row}:`}${cell}`;
        if (this._written[key] !== value) {
            this._written[key] = value;
            const comp = this._components[name];
            Promise.resolve(cell === 'Value' && comp.setStepValue ? comp.setStepValue(value)
                : row == null ? comp.setValue(cell, value, type) : comp.setValue(row, cell, value, type))
                .catch((err) => logger.warn(`${name} ${cell} failed: ${err && err.message}`));
        }
    }

    /* Enables a component unless it already holds that state (the
     * drop-down and the slider have no setEnabled, only the property). */
    _setEnabled(name, enabled) {
        const key = `enabled:${name}`;
        if (this._written[key] !== enabled) {
            this._written[key] = enabled;
            const comp = this._components[name];
            Promise.resolve(comp.setEnabled ? comp.setEnabled(enabled) : comp.setProperty('Enabled', enabled ? 1 : 0, 'UInt'))
                .catch((err) => logger.warn(`${name} enable failed: ${err && err.message}`));
        }
    }

    /* Shows or hides a component unless it already holds that state. */
    _setVisibleComp(name, visible) {
        const key = `visible:${name}`;
        if (this._written[key] !== visible) {
            this._written[key] = visible;
            Promise.resolve(this._components[name].setVisible(visible))
                .catch((err) => logger.warn(`${name} visibility failed: ${err && err.message}`));
        }
    }

    /* Ticks or unticks a checkbox unless it already holds that state. */
    _setSelected(name, selected) {
        const key = `selected:${name}`;
        if (this._written[key] !== selected) {
            this._written[key] = selected;
            Promise.resolve(this._components[name].setValue(0, 'eParam_RowSelected', selected, 'Bool'))
                .catch((err) => logger.warn(`${name} select failed: ${err && err.message}`));
        }
    }

    _render() {
        if (!this._state || this._shutdownPromise) {
            return;
        }
        const c = this._control;
        [[ENABLE_NAME, t('Patch')], [DISABLE_NAME, t('Remove patch')], [AA_ENABLE_NAME, t('Patch')], [AA_DISABLE_NAME, t('Remove patch')],
            [AA_ZOOM_OUT_NAME, t('Zoom out')], [AA_ZOOM_IN_NAME, t('Zoom in')], [HEAD_GENERAL_NAME, t('General')], [LOGS_NAME, t('Logs')],
            [HEAD_HUD_NAME, t('HUD')],
            [HUD_MOVE_NAMES.left, t('Move left')], [HUD_MOVE_NAMES.right, t('Move right')], [HUD_MOVE_NAMES.up, t('Move up')],
            [HUD_MOVE_NAMES.down, t('Move down')], [HUD_CENTRE_NAME, t('Centre')],
            [RESTART_NAME, t('Restart head unit')]].forEach(([name, text]) => this._write(name, text));
        this._write(STATUS_NAME, c.statusText());
        this._write(MAP_NAME, c.mapText());
        this._write(AA_STATUS_NAME, c.aaStatusText());
        this._write(AA_ZOOM_NAME, c.zoomText());
        this._write(AA_CARD_NAME, c.cardText());
        this._setSelected(AA_CARD_NAME, c.cardHidden());
        this._write(CAPTURE_NAME, c.captureButtonText());
        this._write(CAPTURE_STATUS, c.captureText());
        [[ENABLE_NAME, c.canEnable()], [DISABLE_NAME, c.canDisable()],
            [AA_ENABLE_NAME, c.canEnableAa()], [AA_DISABLE_NAME, c.canDisableAa()],
            [AA_ZOOM_OUT_NAME, c.canZoom(-ZOOM_STEP)], [AA_ZOOM_IN_NAME, c.canZoom(ZOOM_STEP)],
            [AA_CARD_NAME, c.canToggleCard()], [CAPTURE_NAME, c.canCapture()], [RESTART_NAME, c.canRestart()]]
            .forEach(([name, enabled]) => this._setEnabled(name, enabled));
        const restartText = c.restartText();
        this._setVisibleComp(RESTART_NAME, c.restartNeeded() || c.restartRequested());
        this._write(RESTART_STATUS_NAME, restartText);
        this._setVisibleComp(RESTART_STATUS_NAME, restartText !== '');
        const wifi = c.wifiText(), hint = c.wifiHint();
        this._write(WIFI_NAME, wifi);
        this._setVisibleComp(WIFI_NAME, wifi !== '');
        this._write(WIFI_HINT_NAME, hint);
        this._setVisibleComp(WIFI_HINT_NAME, hint !== '');
        const hide = c.hideGuidance();
        const unavailable = c.guidanceUnavailable();
        /* where the map shows: the choice, '—' in its place until the render has said */
        const shownOn = SHOWN_ON.indexOf(c.shownOn());
        this._writeCell(SHOW_ON_NAME, 'HeaderText', t('Show on'), 'Text');
        [t('Cluster'), t('HUD (experimental)'), t('Cluster and HUD (experimental)'), t('Off')].forEach((text, row) =>
            this._writeCell(SHOW_ON_NAME, 'ItemText', row === 0 && shownOn < 0 ? '—' : text, 'Text', row));
        this._writeCell(SHOW_ON_NAME, 'SelectedItem', Math.max(shownOn, 0), 'UInt');
        this._setEnabled(SHOW_ON_NAME, c.canChooseShownOn());
        const profiles = this._profiles;
        this._write(PROFILE_NAME, t('Own settings for this driver'));
        this._setSelected(PROFILE_NAME, profiles.ownShown());
        this._setEnabled(PROFILE_NAME, profiles.canChooseOwn());
        /* the turn box's hold follows the map on the cluster */
        this._setEnabled(GUIDE_NAME, hide !== null && !unavailable && c.clusterOn() !== false);
        this._write(GUIDE_NAME, t("Hide BMW's turn box"));
        this._setSelected(GUIDE_NAME, !!hide && !unavailable);
        this._write(HUD_NAME, c.hudText());
        this._write(HUD_KEEP_NAME, t("Keep HUD map over BMW's pop-ups"));
        this._setSelected(HUD_KEEP_NAME, c.hudKeepShown() === true);
        this._setEnabled(HUD_KEEP_NAME, c.canKeepHud());
        /* the size slider and the place buttons: greyed while the HUD is off
         * or disabled (the HMI greys no label) */
        const adjust = c.canChangeHudSize();
        this._writeCell(HUD_SIZE_NAME, 'TextDescription', c.hudSizeText(), 'Text', 0);
        if (c.hudSizeStep() !== null) {
            this._writeCell(HUD_SIZE_NAME, 'Value', c.hudSizeStep(), 'SInt', 0);
        }
        this._setEnabled(HUD_SIZE_NAME, adjust);
        this._write(HUD_POS_NAME, c.hudPositionText());
        Object.values(HUD_MOVE_NAMES).forEach((n) => this._setEnabled(n, adjust));
        this._setEnabled(HUD_CENTRE_NAME, c.canCentreHud());
    }

    _renderLogs() {
        if (!this._logsState || this._shutdownPromise) {
            return;
        }
        this._write(LOGS_SAVE, t('Save logs'));
        this._write(LOGS_STATUS, this._logs.statusText());
        this._write(LOGS_URL, this._logs.urlText());
        this._setEnabled(LOGS_SAVE, this._logs.canSave());
        const rows = this._logs.rows();
        LOGS_ROW_NAMES.forEach((name, i) => this._write(name, oneRow(rows[i])));
        this._showQr(this._logs.url());
    }

    /* Writes url's QR code (none: a blank square) as a new file and shows it,
     * then removes the one it replaced. */
    _showQr(url) {
        const rhmi = this.rhmiApplication;
        if (this._qr.url === url || !rhmi) {
            return this._qr.chain;
        }
        this._qr.url = url;
        this._qr.chain = this._qr.chain.then(() => {
            /* the HMI reads the picture: the folder and the file world-readable */
            try {
                fs.mkdirSync(QR_DIR);
            } catch (e) {
                if (e.code !== 'EEXIST') {
                    throw e;
                }
            }
            fs.chmodSync(QR_DIR, 0o755);
            const file = path.join(QR_DIR, `qr_${Date.now()}.png`);
            fs.writeFileSync(file, qrPng(url).png);
            fs.chmodSync(file, 0o644);
            const bundle = rhmi.getResourceBundle();
            const old = this._qr.file;
            return Promise.resolve(bundle.createResourceHandles([file]))
                .then(() => this._components[LOGS_QR].setValue(0, 'Graphic', file, 'ResourceId'))
                .then(() => rhmi.flush())
                .then(() => {
                    this._qr.file = file;
                    return old ? this._retireQr(bundle, old) : undefined;
                });
        }).catch((err) => {
            this._qr.url = undefined;
            logger.warn(`QR code failed: ${err && err.message}`);
        });
        return this._qr.chain;
    }

    _retireQr(bundle, file) {
        return Promise.resolve()
            .then(() => bundle.destroyResourceHandles([file]))
            .catch((err) => logger.warn(`QR handle ${file}: ${err && err.message}`))
            .then(() => {
                try {
                    fs.unlinkSync(file);
                } catch (e) {
                    /* gone already */
                }
            });
    }

    _removeStaleQr() {
        try {
            fs.readdirSync(QR_DIR).filter((n) => /^qr_\d+\.png$/.test(n))
                .forEach((n) => fs.unlinkSync(path.join(QR_DIR, n)));
        } catch (e) {
            /* no folder yet */
        }
    }

    /* ---- shutdown ---- */
    _handleShutdown(reason) {
        if (this._shutdownPromise) {
            return this._shutdownPromise;
        }
        logger.info(`robertn_waze_cluster shutdown: ${JSON.stringify(reason)}`);
        clearTimeout(this._hideTimer);
        this._control.close();
        this._logs.close();
        this._profiles.stop();
        if (this._qr.file) {
            try {
                fs.unlinkSync(this._qr.file);
            } catch (e) {
                /* gone already */
            }
        }
        this._shutdownPromise = Promise.resolve()
            .then(() => this.destroyBundles().catch((err) =>
                logger.info(`destroyBundles: ${err && err.message}`)))
            .then(() => process.exit(0));
        return this._shutdownPromise;
    }
}

const app = new WazeClusterApp();
app.start().catch((err) => {
    /* Stays resident so the manager can stop or restart it. */
    logger.error(`robertn_waze_cluster failed to start: ${err && err.message}`);
});
