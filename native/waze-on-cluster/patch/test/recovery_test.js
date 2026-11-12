// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Host tests for boot recovery (patch/recovery.js: the boot step and the
 * watcher), its patch record (patch/patch_record.js), shutdown_mark.sh and
 * the shared remount lock (patch/remount_lock.js). Each world is a folder
 * of real files: the targets, their backups and records, the state files,
 * the shutdown marks shutdown_mark.sh writes, and a mount that flips a fake
 * /proc/mounts. Only systemd (the service's runs, the system's state, a
 * start) is faked, so the decisions run on what the unit would have on
 * disk. With CARPLAY_BIN and AAPD_BIN the patchers write real patches of
 * copies and the real boot step restores them.
 *
 *   node patch/test/recovery_test.js
 */
const assert = require('assert');
const headUnit = require('./head_unit_env.js');
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');

/* The mount, /proc/mounts, /proc and the remount lock every restore in this
 * process uses. */
const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-rec-'));
const MOUNTS = path.join(ROOT, 'mounts');
const MOUNT_LOG = path.join(ROOT, 'mount.log');
const ROMOUNT = 'echo "$*" >> "' + MOUNT_LOG + '"\ncase "$*" in *remount,rw*) echo "/dev/root / ext4 rw 0 0" > "' + MOUNTS +
    '";; *remount,ro*) echo "/dev/root / ext4 ro 0 0" > "' + MOUNTS + '";; esac\n';
fs.writeFileSync(path.join(ROOT, 'mount'), '#!/bin/sh\n' + ROMOUNT, { mode: 0o755 });
fs.writeFileSync(MOUNTS, '/dev/root / ext4 ro 0 0\n');
fs.mkdirSync(path.join(ROOT, 'proc'));
Object.assign(process.env, {
    WOC_MOUNT: path.join(ROOT, 'mount'), WOC_PROC_MOUNTS: MOUNTS, WOC_PROC: path.join(ROOT, 'proc'),
    WOC_REMOUNT_LOCK: path.join(ROOT, 'remount.lock'),
    /* No btime unless a test writes one: the host's own /proc/stat is no boot of the worlds'. */
    WOC_PROC_STAT: path.join(ROOT, 'no-proc-stat'),
});

const recovery = require('../recovery.js');
const record = require('../patch_record.js');
const { Recovery, THRESHOLD, RESTORE_BOUND_MS, BOOT_BOUND_MS } = recovery;

const PATCH = path.join(__dirname, '..');
const RECOVERY_PATH = path.join(PATCH, 'recovery.js');
const MARK_SH = path.join(PATCH, 'shutdown_mark.sh');
const CARPLAY_PATCH = path.join(PATCH, 'carplay_patch.js');
const AAPD_PATCH = path.join(PATCH, 'aapd_patch.js');
const CARPLAY_BIN = process.env.CARPLAY_BIN || '';
const AAPD_BIN = process.env.AAPD_BIN || '';
const AAPD_CONF = process.env.AAPD_CONF || '';
const sha = (buf) => crypto.createHash('sha256').update(buf).digest('hex');
const ro = () => /\bro\b/.test(fs.readFileSync(MOUNTS, 'utf8'));

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

function clock() {
    let t = 1000000;
    const fn = () => t;
    fn.advance = (ms) => { t += ms; };
    return fn;
}

/* A synthetic binary (no ELF, so no build-id): its original, and a patch of
 * a few bytes, the size of one of ours. */
function synthetic(seed, size) {
    const original = crypto.createHash('sha512').update(seed).digest();
    const out = Buffer.alloc(size || 65536);
    for (let i = 0; i < out.length; i++) out[i] = original[i % original.length] ^ (i & 0xff);
    const patched = Buffer.from(out);
    [100, 2000, 30000].forEach((o) => { patched[o] ^= 0xff; });
    return { original: out, patched };
}
const CONF = { original: Buffer.from('{"SCREENS":{"KOMBI_DISPLAY":{"DENSITY":160}}}\n'), patched: Buffer.from('{"SCREENS":{"KOMBI_DISPLAY":{"DENSITY":180}}}\n') };

/* A backup as the patchers write it: the original and its record. */
function backup(file, buf) {
    fs.writeFileSync(file + '.woc-orig', buf, { mode: 0o600 });
    fs.writeFileSync(file + '.woc-orig.json', JSON.stringify({ target: file, sha256_original: sha(buf) }));
}

/* One head unit's files and its services' runs. */
function makeWorld(opts) {
    opts = opts || {};
    const dir = fs.mkdtempSync(path.join(ROOT, 'w-'));
    ['var', 'shm', 'sin', 'sin/recmgr', 'sin/crashhandler'].forEach((d) => fs.mkdirSync(path.join(dir, d)));
    const T = recovery.targets({ state: path.join(dir, 'var', 'boot-recovery.json'), sock: path.join(dir, 'shm', 'patch.sock'),
        carplay: path.join(dir, 'carplay'), aapd: path.join(dir, 'aapd'), aapdConf: path.join(dir, 'aapd.conf'), sin: path.join(dir, 'sin') });
    const w = { dir, T, system: 'running', recmgr: !!opts.recmgr, lockHeld: false, now: clock(), events: [], logs: [], svc: {}, bin: {} };
    let nextPid = 100;
    ['carplay', 'aapd'].forEach((name) => {
        const s = w.svc[name] = { run: '', pid: 0, state: 'inactive', result: 'success', alive: new Set() };
        s.starts = () => {
            const pid = nextPid++;
            Object.assign(s, { run: name + '-run-' + pid, pid, state: 'active', result: 'success', code: '', status: '' });
            s.alive.add(pid);
            w.events.push(name + '-start:' + w.holds(name));
        };
        s.crashes = () => {
            s.alive.delete(s.pid);
            Object.assign(s, { pid: 0, state: 'failed', result: 'core-dump', code: '3', status: '11' });
            w.events.push(name + '-crash');
        };
        s.ends = (code, status) => {
            s.alive.delete(s.pid);
            Object.assign(s, { pid: 0, state: 'inactive', result: 'success', code, status });
        };
        s.down = () => { s.alive.clear(); Object.assign(s, { run: '', pid: 0, state: 'inactive', result: 'success' }); };
    });
    /* What the service's file holds now: the original or the patch. */
    w.holds = (name) => {
        let cur = null;
        try { cur = fs.readFileSync(T[name].target); } catch (e) { return 'missing'; }
        return cur.equals(w.bin[name].original) ? 'original' : cur.equals(w.bin[name].patched) ? 'patched' : 'other';
    };
    /* The service's file as the patcher leaves it: kind 'proven' |
     * 'unproven' | 'trial' (patched; trial: the first patch of a proven
     * build, unproven until it runs healthy), 'original' (Disable used),
     * 'unknown' (patched, no record), 'none' (stock, no backup). */
    w.install = (name, kind) => {
        const t = T[name], b = w.bin[name] = synthetic(name + kind);
        /* The modes the firmware ships, whatever the umask the tests run under. */
        fs.writeFileSync(t.target, kind === 'original' || kind === 'none' ? b.original : b.patched);
        fs.chmodSync(t.target, 0o755);
        if (kind !== 'none') backup(t.target, b.original);
        if (name === 'aapd') {
            fs.writeFileSync(t.files[1], kind === 'original' || kind === 'none' ? CONF.original : CONF.patched);
            fs.chmodSync(t.files[1], 0o644);
            if (kind !== 'none') backup(t.files[1], CONF.original);
        }
        if (kind === 'proven' || kind === 'unproven' || kind === 'original') record.write(t.target, { patched: kind !== 'original', proven: kind === 'proven' });
        if (kind === 'trial') record.write(t.target, { patched: true, proven: false, trial: true });
    };
    w.recovery = (name, bootId, extra) => new Recovery(Object.assign({
        target: T[name],
        bootId,
        now: w.now,
        probe: (cb) => cb(Object.assign({}, w.svc[name])),
        systemState: (cb) => cb(w.system),
        start: (cb) => { w.svc[name].starts(); cb(null); },
        alive: (pid) => w.svc[name].alive.has(pid),
        recmgrLoaded: () => w.recmgr,
        lock: { tryAcquire: () => !w.lockHeld, release: () => {} },
        /* Whether the service runs its target's own file (w.runs). */
        runsTarget: () => !!w.runs,
        log: (l) => w.logs.push(l),
    }, extra || {}));
    w.state = (name) => readJson(T[name].state);
    return w;
}

function readJson(p) {
    try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch (e) { return null; }
}

const startBoot = (rec) => new Promise((resolve) => rec.startBoot(resolve));
const tick = (rec) => new Promise((resolve) => rec.tick(resolve));
const shutdown = (rec) => new Promise((resolve) => rec.onShutdown(resolve));
const starts = (w, name) => w.events.filter((e) => e.startsWith(name + '-start')).length;

/* One boot as the units order it: each boot step (its restore links a
 * running binary only, and waits for the lock, as `recovery.js boot`
 * does), the services' start, then the watcher's own start and a tick. */
async function bootUp(w, bootId, names) {
    names = names || ['carplay'];
    const steps = {}, watchers = {};
    for (const n of names) {
        steps[n] = await startBoot(w.recovery(n, bootId, { restore: (fails, cb) => recovery.restoreTarget(w.T[n], true, fails, cb, 1000) }));
        w.events.push(n + '-boot-step:' + steps[n].action);
    }
    for (const n of names) w.svc[n].starts();
    for (const n of names) {
        watchers[n] = w.recovery(n, bootId);
        await startBoot(watchers[n]);
        await tick(watchers[n]);
    }
    return { steps, watchers, step: steps[names[0]], watcher: watchers[names[0]] };
}

/* shutdown_mark.sh as var-opt-ro.service runs it at an NSM shutdown of boot:
 * each service's state as systemctl shows it (running | failed | stopped |
 * null, unreadable), and, when crash, the restart recmgr asked for. */
function nsmShutdown(w, boot, states, crash, up) {
    const started = '\\nExecMainStartTimestampMonotonic=1234567';
    const d = w.dir, show = { running: 'ActiveState=active\\nResult=success' + started, failed: 'ActiveState=failed\\nResult=core-dump\\nExecMainCode=3\\nExecMainStatus=11' + started,
        stopped: 'ActiveState=inactive\\nResult=success\\nExecMainStartTimestampMonotonic=0',
        killed: 'ActiveState=failed\\nResult=signal\\nExecMainCode=2\\nExecMainStatus=9' + started };
    fs.writeFileSync(path.join(d, 'boot_id'), boot + '\n');
    fs.writeFileSync(path.join(d, 'uptime'), (up || 20) + '.00 50.00\n');
    fs.writeFileSync(path.join(d, 'stat'), 'cpu 1 2 3\nbtime ' + Math.floor(Date.now() / 1000 - (up || 20)) + '\n');
    const unit = (u, s) => u + ') ' + (s ? 'printf "' + show[s] + '\\n"' : 'exit 1') + ' ;;';
    fs.writeFileSync(path.join(d, 'systemctl'), '#!/bin/sh\ncase "$2" in\n' + unit('carplay.service', states.carplay) + '\n' + unit('aapd.service', states.aapd) + '\nesac\n', { mode: 0o755 });
    const info = path.join(d, 'sin', 'recmgr', 'restartInfo.xml');
    if (crash) fs.writeFileSync(info, '<restartInfo/>\n'); else try { fs.unlinkSync(info); } catch (e) {}
    const r = cp.spawnSync('sh', [MARK_SH], { env: Object.assign({}, process.env, { WOC_STATE_DIR: path.join(d, 'var'), WOC_BOOT_ID: path.join(d, 'boot_id'),
        WOC_SYSTEMCTL: path.join(d, 'systemctl'), WOC_MARK_WAIT: '2', WOC_SIN_DIR: path.join(d, 'sin'), WOC_UPTIME: path.join(d, 'uptime'),
        WOC_PROC_STAT: path.join(d, 'stat') }), stdio: 'ignore' });
    assert.strictEqual(r.status, 0);
    /* recmgr deletes its restart record when it starts at the next boot. */
    try { fs.unlinkSync(info); } catch (e) {}
    Object.keys(w.svc).forEach((n) => w.svc[n].down());
}

/* The services as systemctl shows them now. */
function shown(w) {
    const one = (s) => s.state === 'failed' ? 'failed' : s.state === 'active' ? 'running' : 'stopped';
    return { carplay: one(w.svc.carplay), aapd: one(w.svc.aapd) };
}

/* How a boot ends. */
const END = {
    /* The patched service crashes at start; its OnFailure= and crashhandler
     * restart the head unit before its watcher looks. */
    crashAtStart: (w, boot, name) => { w.svc[name].crashes(); nsmShutdown(w, boot, shown(w), true); },
    /* Another process crashes; recmgr restarts the head unit. */
    otherCrash: (w, boot) => nsmShutdown(w, boot, shown(w), true),
    /* An engine stop or switch-off after the healthy window. */
    switchOff: (w, boot) => nsmShutdown(w, boot, shown(w), false, 300),
    /* An NSM shutdown within the healthy window with no restart record: a
     * switch-off, or a crash restart whose record was missed. */
    shortOff: (w, boot) => nsmShutdown(w, boot, shown(w), false, 20),
    /* Power lost, or a reset with nothing recorded. */
    powerCut: (w) => Object.keys(w.svc).forEach((n) => w.svc[n].down()),
};

/* A loop: each boot ends as end does; the boot index at which the original
 * is restored, or 0 within max boots. */
async function loop(w, name, kind, end, max, names) {
    w.install(name, kind);
    for (let i = 1; i <= max; i++) {
        const { steps } = await bootUp(w, 'boot-' + i, names || [name]);
        if (steps[name].action === 'restored') return i;
        end(w, 'boot-' + i, name);
    }
    return 0;
}

/* ---- thresholds ---- */

for (const name of ['carplay', 'aapd']) {
    for (const [kind, at] of [['unproven', 2], ['proven', 3], ['unknown', 3]]) {
        test(`${name}, ${kind} patch crashing at start: restored before ${name} starts, at boot ${at}`, async () => {
            const w = makeWorld();
            assert.strictEqual(await loop(w, name, kind, END.crashAtStart, 5), at);
            assert.strictEqual(w.events.filter((e) => e === name + '-start:patched').length, at - 1, 'the patch ran ' + (at - 1) + ' times');
            assert.strictEqual(w.events[w.events.length - 1], name + '-start:original', 'the boot that restored starts the original');
            assert.strictEqual(w.holds(name), 'original', 'byte for byte');
            if (name === 'aapd') assert(fs.readFileSync(w.T.aapd.files[1]).equals(CONF.original), 'aapd.conf restored too');
            assert(ro(), '/ read-only again');
            const rec = record.current(w.T[name].target);
            assert.deepStrictEqual([rec.patched, rec.by, rec.fails], [false, 'recovery', THRESHOLD[kind]], 'the record names the restore');
            assert.deepStrictEqual([w.state(name).fails, w.state(name).arm], [0, null]);
            assert.strictEqual((await bootUp(w, 'boot-after', [name])).steps[name].action, 'idle', 'nothing armed after');
        });
    }
}

test('a process runs a file by its exe, or by mapping it (a translator such as Rosetta); a link to it is held', () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-inuse-'));
    try {
        const file = path.join(dir, 'carplay'), other = path.join(dir, 'translator'), link = path.join(dir, 'carplay.woc-running');
        fs.writeFileSync(file, 'binary');
        fs.writeFileSync(other, 'translator');
        fs.mkdirSync(path.join(dir, 'proc', '101'), { recursive: true });
        fs.symlinkSync(other, path.join(dir, 'proc', '101', 'exe'));
        process.env.WOC_PROC = path.join(dir, 'proc');
        delete require.cache[require.resolve('../remount_lock.js')];
        const lock = require('../remount_lock.js');
        const st = { dev: 0xfd01, ino: 4242 };
        fs.writeFileSync(path.join(dir, 'proc', '101', 'maps'), '7f0000000000-7f0000001000 r-xp 00000000 fd:01 4243      /other\n' +
            '7f0000001000-7f0000002000 r-xp 00000000 fd:02 4242      /elsewhere\n');
        assert.strictEqual(lock.runs(st), false, 'another inode, or the inode number on another device, mapped: not running it');
        fs.appendFileSync(path.join(dir, 'proc', '101', 'maps'), `555555554000-555555c31000 r-xp 00000000 fd:01 4242      ${file}\n`);
        assert.strictEqual(lock.runs(st), true, 'the inode mapped, its exe the translator: running it');
        assert.strictEqual(lock.inUse(file), false, 'neither exe nor maps name the file');
        fs.linkSync(file, link);
        fs.unlinkSync(path.join(dir, 'proc', '101', 'exe'));
        fs.symlinkSync(link, path.join(dir, 'proc', '101', 'exe'));
        assert.strictEqual(lock.inUse(file), true, 'its exe is the file: running it');
        fs.writeFileSync(file + '.new', 'patched');
        fs.renameSync(file + '.new', file);
        assert.strictEqual(lock.holds(link, file), true, 'the link to the running inode, replaced on disk, is held');
    } finally {
        delete process.env.WOC_PROC;
        delete require.cache[require.resolve('../remount_lock.js')];
        cp.execFileSync('rm', ['-rf', dir]);
    }
});

test('a failure another process causes counts: unproven restored at boot 2, proven at boot 3', async () => {
    for (const [kind, at] of [['unproven', 2], ['proven', 3]]) {
        const w = makeWorld();
        assert.strictEqual(await loop(w, 'carplay', kind, END.otherCrash, 5), at, kind);
        assert(!w.events.includes('carplay-crash'), 'carplay itself never crashed');
    }
});

test('a crash of the patched service counts even when its watcher saw it and the system then stopped in order', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    for (const boot of ['A', 'B']) {
        const { watcher } = await bootUp(w, boot);
        w.svc.carplay.crashes();
        w.now.advance(5000);
        assert.strictEqual((await tick(watcher)).action, 'crash');
        w.system = 'stopping';
        await shutdown(watcher);
        w.system = 'running';
        w.svc.carplay.down();
    }
    assert.strictEqual((await bootUp(w, 'C')).step.action, 'restored');
});

/* ---- a first patch on trial ---- */

const whenRestored = (rec) => new Promise((resolve) => rec.whenRestored(resolve));
const mountLog = () => fs.existsSync(MOUNT_LOG) ? fs.readFileSync(MOUNT_LOG, 'utf8') : '';
const recordOf = (w) => { const r = record.current(w.T.carplay.target); return r && { patched: r.patched, proven: r.proven, trial: r.trial }; };

test('a first patch on trial crashing at start: restored before carplay starts, at boot 2, as an unproven one', async () => {
    const w = makeWorld();
    w.runs = true;
    assert.strictEqual(await loop(w, 'carplay', 'trial', END.crashAtStart, 5), 2);
    assert.strictEqual(w.holds('carplay'), 'original');
});

test('a first patch on trial that fails before its healthy minute is restored after that one failed start', async () => {
    const w = makeWorld();
    w.runs = true;
    w.install('carplay', 'trial');
    const { watcher } = await bootUp(w, 'A');
    w.now.advance(59000);
    assert.strictEqual((await tick(watcher)).action, 'watch');
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: false, trial: true }, 'still on trial');
    END.crashAtStart(w, 'A', 'carplay');
    assert.strictEqual((await bootUp(w, 'B')).step.action, 'restored');
    assert.strictEqual(w.holds('carplay'), 'original');
});

test('a first patch on trial that runs healthy a minute is recorded proven; then it takes two failed starts', async () => {
    const w = makeWorld();
    w.runs = true;
    w.install('carplay', 'trial');
    const tgt = w.T.carplay.target, st0 = fs.statSync(tgt), sha0 = sha(fs.readFileSync(tgt));
    const { step, watcher } = await bootUp(w, 'A');
    assert.deepStrictEqual([step.action, step.patch], ['armed', 'unproven']);
    fs.writeFileSync(MOUNT_LOG, '');
    w.now.advance(61000);
    assert.strictEqual((await tick(watcher)).action, 'confirmed');
    await whenRestored(watcher);
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: true, trial: undefined }, 'recorded proven');
    const st1 = fs.statSync(tgt);
    assert.deepStrictEqual([st1.ino, st1.size, st1.mtime.getTime(), sha(fs.readFileSync(tgt))], [st0.ino, st0.size, st0.mtime.getTime(), sha0], 'carplay untouched');
    assert.deepStrictEqual(mountLog().trim().split('\n').map((l) => /remount,(rw|ro)/.exec(l)[1]), ['rw', 'ro'], 'one write window');
    assert(ro(), '/ read-only again');
    assert(w.logs.some((l) => /ran its patch healthy: the patch is recorded proven/.test(l)), w.logs.join('\n'));
    fs.writeFileSync(MOUNT_LOG, '');
    for (let i = 0; i < 30; i++) { w.now.advance(1000); await tick(watcher); }
    assert.strictEqual(mountLog(), '', 'settled: nothing more this run');
    END.switchOff(w, 'A');
    const b = await bootUp(w, 'B');
    assert.deepStrictEqual([b.step.action, b.step.patch, b.step.fails], ['armed', 'proven', 0], 'armed as a proven patch');
    END.crashAtStart(w, 'B', 'carplay');
    assert.strictEqual((await bootUp(w, 'C')).step.action, 'armed', 'one failed start does not restore it');
    END.crashAtStart(w, 'C', 'carplay');
    assert.strictEqual((await bootUp(w, 'D')).step.action, 'restored', 'two do');
});

test('a patch on trial is not recorded proven for a run of another file (a change waits for the next start)', async () => {
    const w = makeWorld();
    w.runs = false;
    w.install('carplay', 'trial');
    const { watcher } = await bootUp(w, 'A');
    fs.writeFileSync(MOUNT_LOG, '');
    w.now.advance(61000);
    assert.strictEqual((await tick(watcher)).action, 'confirmed');
    for (let i = 0; i < 5; i++) { w.now.advance(1000); await tick(watcher); }
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: false, trial: true });
    assert.strictEqual(mountLog(), '', 'no remount');
    /* The next run (patchd restarted carplay onto the file on disk) is. */
    w.svc.carplay.ends('2', '9');
    fs.writeFileSync(w.T.carplay.killed, w.svc.carplay.run + '\n');
    w.now.advance(5000);
    await tick(watcher);
    w.svc.carplay.starts();
    w.runs = true;
    w.now.advance(5000);
    await tick(watcher);
    w.now.advance(61000);
    assert.strictEqual((await tick(watcher)).action, 'watch', 'this boot was confirmed already');
    await whenRestored(watcher);
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: true, trial: undefined }, 'the new run\'s healthy minute records it');
    assert(ro());
});

test('a patch on trial whose record cannot be written now (patchd holds the remount lock) is tried again a minute later', async () => {
    const w = makeWorld();
    w.runs = true;
    w.install('carplay', 'trial');
    const { watcher } = await bootUp(w, 'A');
    fs.writeFileSync(process.env.WOC_REMOUNT_LOCK, 'patchd\n');
    try {
        fs.writeFileSync(MOUNT_LOG, '');
        w.now.advance(61000);
        assert.strictEqual((await tick(watcher)).action, 'confirmed');
        await whenRestored(watcher);
        assert.deepStrictEqual(recordOf(w), { patched: true, proven: false, trial: true });
        assert(w.logs.some((l) => /recording it proven failed: remount lock held; deferring; trying again in 60 s/.test(l)), w.logs.join('\n'));
        for (let i = 0; i < 50; i++) { w.now.advance(1000); await tick(watcher); }
        assert.strictEqual(mountLog(), '', 'not tried again within the minute, nothing remounted');
    } finally { fs.unlinkSync(process.env.WOC_REMOUNT_LOCK); }
    for (let i = 0; i < 11; i++) { w.now.advance(1000); await tick(watcher); await whenRestored(watcher); }
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: true, trial: undefined });
    assert(ro() && !fs.existsSync(process.env.WOC_REMOUNT_LOCK), '/ read-only, the lock released');
});

test('recording a trial proven checks again under the remount lock: a process runs the target, the record still on trial', async () => {
    const w = makeWorld();
    w.install('carplay', 'trial');
    const prove = (runs) => new Promise((resolve) => recovery.proveRecord(w.T.carplay, () => runs, (err, wrote) => resolve([err, wrote])));
    fs.writeFileSync(MOUNT_LOG, '');
    assert.deepStrictEqual(await prove(false), [null, false], 'another file runs (patchd changed it meanwhile)');
    assert.strictEqual(mountLog(), '', 'nothing remounted');
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: false, trial: true });
    assert(!fs.existsSync(process.env.WOC_REMOUNT_LOCK), 'the lock released');
    assert.deepStrictEqual(await prove(true), [null, true]);
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: true, trial: undefined });
    fs.writeFileSync(MOUNT_LOG, '');
    assert.deepStrictEqual(await prove(true), [null, false], 'no trial left');
    assert.strictEqual(mountLog(), '', 'nothing remounted');
    assert(ro() && !fs.existsSync(process.env.WOC_REMOUNT_LOCK));
});

test('a patch on trial that crashed in this boot is not recorded proven by a later healthy run, also when its restore failed', async () => {
    const w = makeWorld({ recmgr: true });
    w.runs = true;
    w.install('carplay', 'trial');
    const watcher = w.recovery('carplay', 'A', { restore: (fails, cb) => cb(new Error('remount lock held; deferring')) });
    await startBoot(watcher);
    w.svc.carplay.starts();
    await tick(watcher);
    w.svc.carplay.crashes();
    w.now.advance(1000);
    const crash = await tick(watcher);
    assert.deepStrictEqual([crash.action, crash.restore], ['crash', 'restore-failed'], 'restored at 1, and the restore failed');
    w.svc.carplay.starts();
    for (let i = 0; i < 3; i++) { w.now.advance(5000); await tick(watcher); }
    fs.writeFileSync(MOUNT_LOG, '');
    w.now.advance(61000);
    await tick(watcher);
    await whenRestored(watcher);
    for (let i = 0; i < 5; i++) { w.now.advance(1000); await tick(watcher); }
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: false, trial: true }, 'still on trial');
    assert.strictEqual(mountLog(), '', 'nothing remounted for it');
    assert(w.logs.some((l) => /this boot is not clean \(failed starts and crashes 1\): the patch stays on trial/.test(l)), w.logs.join('\n'));
});

test('a patch on trial with failed starts counted before this boot (its restore failed) is not recorded proven', async () => {
    const w = makeWorld();
    w.runs = true;
    w.install('carplay', 'trial');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, unknown: 0, arm: null }));
    const watcher = w.recovery('carplay', 'B', { restore: (fails, cb) => cb(new Error('remount lock held; deferring')) });
    assert.strictEqual((await startBoot(watcher)).action, 'restore-failed');
    w.svc.carplay.starts();
    await tick(watcher);
    w.now.advance(61000);
    await tick(watcher);
    await whenRestored(watcher);
    assert.deepStrictEqual(recordOf(w), { patched: true, proven: false, trial: true });
    assert(w.logs.some((l) => /not clean \(failed starts and crashes 1\)/.test(l)), w.logs.join('\n'));
});

test('a record written proven with / left read-write after it: said so, no try again', async () => {
    const w = makeWorld();
    w.runs = true;
    w.install('carplay', 'trial');
    const mount = process.env.WOC_MOUNT, saved = fs.readFileSync(mount);
    /* a mount whose read-only remount leaves / read-write */
    fs.writeFileSync(mount, '#!/bin/sh\necho "$*" >> "' + MOUNT_LOG + '"\ncase "$*" in *remount,rw*) echo "/dev/root / ext4 rw 0 0" > "' + MOUNTS + '";; esac\n');
    let proves = 0;
    try {
        const r = await new Promise((resolve) => recovery.proveRecord(w.T.carplay, () => true, (err, wrote, roErr) => resolve([err, wrote, roErr && roErr.message])));
        assert.deepStrictEqual(r, [null, true, '/ is still read-write after remount'], 'written; the read-only remount failed');
        assert.deepStrictEqual(recordOf(w), { patched: true, proven: true, trial: undefined });
        assert(!fs.existsSync(process.env.WOC_REMOUNT_LOCK), 'the lock released');
        w.install('carplay', 'trial');
        const watcher = w.recovery('carplay', 'A', { prove: (cb) => { proves++; recovery.proveRecord(w.T.carplay, () => true, cb); } });
        await startBoot(watcher);
        w.svc.carplay.starts();
        await tick(watcher);
        w.now.advance(61000);
        await tick(watcher);
        await whenRestored(watcher);
        for (let i = 0; i < 70; i++) { w.now.advance(1000); await tick(watcher); }
        assert.strictEqual(proves, 1, 'no try again');
        assert(w.logs.some((l) => /ran its patch healthy: the patch is recorded proven/.test(l)), w.logs.join('\n'));
        assert(w.logs.some((l) => /\/ is still read-write after remount: \/ stays read-write until the next head-unit start/.test(l)), w.logs.join('\n'));
        assert(!w.logs.some((l) => /recording it proven failed/.test(l)), w.logs.join('\n'));
    } finally {
        fs.writeFileSync(mount, saved);
        fs.writeFileSync(MOUNTS, '/dev/root / ext4 ro 0 0\n');
    }
});

test('a stop while a patch on trial is recorded proven waits for the write', async () => {
    const w = makeWorld();
    w.runs = true;
    w.install('carplay', 'trial');
    let finish = null;
    const watcher = w.recovery('carplay', 'A', { prove: (cb) => { finish = cb; } });
    await startBoot(watcher);
    w.svc.carplay.starts();
    await tick(watcher);
    w.now.advance(61000);
    await tick(watcher);
    assert(finish, 'the write started');
    let stopped = false;
    watcher.whenRestored(() => { stopped = true; });
    assert.strictEqual((await tick(watcher)).action, 'skip', 'nothing else meanwhile');
    assert.strictEqual(stopped, false, 'the stop waits');
    finish(null, true);
    assert.strictEqual(stopped, true, 'then goes');
});

test('a patch recorded proven (by the watcher, or by a release before trials) and the original are left as they are at the healthy minute', async () => {
    for (const kind of ['proven', 'unproven', 'original']) {
        const w = makeWorld();
        w.runs = true;
        w.install('carplay', kind);
        const before = fs.readFileSync(record.recordPath(w.T.carplay.target), 'utf8');
        const { watcher } = await bootUp(w, 'A');
        fs.writeFileSync(MOUNT_LOG, '');
        w.now.advance(61000);
        await tick(watcher);
        await whenRestored(watcher);
        assert.strictEqual(fs.readFileSync(record.recordPath(w.T.carplay.target), 'utf8'), before, kind + ': the record as it was');
        assert.strictEqual(mountLog(), '', kind + ': no remount');
    }
});

/* ---- no false restores ---- */

test('a switch-off after the healthy window, every boot, never restores, even unproven', async () => {
    const w = makeWorld();
    assert.strictEqual(await loop(w, 'carplay', 'unproven', END.switchOff, 6), 0);
    assert.strictEqual(w.state('carplay').fails, 0);
});

test('a shutdown within the healthy window with no restart record is a boot with no record: a loop whose records are missed is still caught', async () => {
    for (const [kind, at] of [['unproven', 3], ['proven', 5]]) {
        const w = makeWorld();
        assert.strictEqual(await loop(w, 'carplay', kind, END.shortOff, 6), at, kind);
        const x = makeWorld();
        x.install('carplay', kind);
        await bootUp(x, 'A');
        END.shortOff(x, 'A');
        assert.strictEqual((await bootUp(x, 'B')).step.action, 'armed', kind + ': one restores nothing');
        assert.deepStrictEqual([x.state('carplay').fails, x.state('carplay').unknown], [0, 1]);
    }
});

test('a state written before the counting rules (no unknown) starts its counts again', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, arm: null, starts: null, ended: null }));
    const r = await startBoot(w.recovery('carplay', 'A'));
    assert.deepStrictEqual([r.action, r.fails], ['armed', 0]);
    assert.strictEqual(w.holds('carplay'), 'patched');
});

test('proven: two failed boots with no healthy boot between restore; a healthy boot between does not', async () => {
    for (const healthyBetween of [false, true]) {
        const w = makeWorld();
        w.install('carplay', 'proven');
        await bootUp(w, 'A');
        END.otherCrash(w, 'A');
        const b = await bootUp(w, 'B');
        if (healthyBetween) { w.now.advance(61000); await tick(b.watcher); }
        END.switchOff(w, 'B');
        await bootUp(w, 'C');
        END.otherCrash(w, 'C');
        assert.strictEqual((await bootUp(w, 'D')).step.action, healthyBetween ? 'armed' : 'restored', 'healthy between: ' + healthyBetween);
    }
});

test('a boot the patched service never ran in counts nothing, whatever ended it', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    await startBoot(w.recovery('carplay', 'A'));   /* armed; the restart came before carplay started */
    nsmShutdown(w, 'A', { carplay: 'stopped', aapd: 'stopped' }, true);
    const r = await startBoot(w.recovery('carplay', 'B'));
    assert.deepStrictEqual([r.action, r.fails], ['armed', 0]);
    w.svc.carplay.starts();
    const watcher = w.recovery('carplay', 'B');
    await tick(watcher);   /* seen running, then stopped cleanly */
    w.svc.carplay.ends('1', '0');
    nsmShutdown(w, 'B', { carplay: 'stopped', aapd: 'stopped' }, true);
    assert.strictEqual((await startBoot(w.recovery('carplay', 'C', { restore: (f, cb) => recovery.restoreTarget(w.T.carplay, true, f, cb, 1000) }))).action, 'restored',
        'once it ran, the restart counts');
});

test('a restart the user asked for is not a failure: patchd\'s record, or the watcher stopped while the system stops', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    await bootUp(w, 'A');
    fs.writeFileSync(w.T.carplay.restart, 'A\n');   /* patchd's reboot command; the watcher never got a SIGTERM */
    END.powerCut(w);
    const { watcher } = await bootUp(w, 'B');
    assert.strictEqual(w.state('carplay').fails, 0, 'patchd recorded the restart');
    w.system = 'stopping';
    await shutdown(watcher);
    w.system = 'running';
    END.powerCut(w);
    assert.strictEqual((await bootUp(w, 'C')).step.action, 'armed', 'the watcher saw the orderly stop');
    assert.strictEqual(w.state('carplay').fails, 0);
});

test('a boot that ends with no record is unknown: one restores nothing, two in a row count one failure', async () => {
    for (const [kind, at] of [['unproven', 3], ['proven', 5]]) {
        const w = makeWorld();
        assert.strictEqual(await loop(w, 'carplay', kind, END.powerCut, 6), at, kind + ': a loop that leaves no record is still caught');
        const x = makeWorld();
        x.install('carplay', kind);
        await bootUp(x, 'A');
        END.powerCut(x);
        assert.strictEqual((await bootUp(x, 'B')).step.action, 'armed', kind + ': one power cut restores nothing');
        assert.deepStrictEqual([x.state('carplay').fails, x.state('carplay').unknown], [0, 1]);
        END.switchOff(x, 'B');
        await bootUp(x, 'C');
        END.powerCut(x);
        assert.strictEqual((await bootUp(x, 'D')).step.action, 'armed', kind + ': an orderly boot between breaks the row');
        assert.deepStrictEqual([x.state('carplay').fails, x.state('carplay').unknown], [0, 1]);
    }
});

test('a single unrelated crash restart within the window never removes a proven patch', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    for (const boot of ['A', 'B', 'C', 'D']) {
        const { step, watcher } = await bootUp(w, boot);
        assert.strictEqual(step.action, 'armed', boot);
        if (boot === 'A' || boot === 'C') { END.otherCrash(w, boot); continue; }   /* a BMW process's crash, once */
        w.now.advance(61000);
        assert.strictEqual((await tick(watcher)).action, 'confirmed');
        END.switchOff(w, boot);
    }
    assert.strictEqual(w.holds('carplay'), 'patched');
    await bootUp(w, 'E');
    assert.deepStrictEqual([w.holds('carplay'), w.state('carplay').fails], ['patched', 0], 'D ran healthy');
});

test('with no shutdown mark, a restart record written after this boot started, or ahead of the clock, is no evidence', async () => {
    for (const where of ['this boot', 'future']) {
        const w = makeWorld();
        w.install('carplay', 'unproven');
        const info = path.join(w.dir, 'sin', 'recmgr', 'restartInfo.xml'), stat = path.join(w.dir, 'stat');
        await bootUp(w, 'A');
        END.powerCut(w);
        fs.writeFileSync(info, '<x/>');
        if (where === 'future') { const t = Date.now() / 1000 + 600; fs.utimesSync(info, t, t); }
        fs.writeFileSync(stat, 'btime ' + Math.floor(Date.now() / 1000 - (where === 'future' ? 30 : 0) - 1) + '\n');
        const was = process.env.WOC_PROC_STAT;
        process.env.WOC_PROC_STAT = stat;
        try {
            if (where === 'this boot') { const t = Date.now() / 1000 + 0.5; fs.utimesSync(info, t, t); fs.writeFileSync(stat, 'btime ' + Math.floor(Date.now() / 1000 - 2) + '\n'); }
            const { step } = await bootUp(w, 'B');
            assert.deepStrictEqual([step.action, w.state('carplay').unknown], ['armed', 1], where);
        } finally { process.env.WOC_PROC_STAT = was; }
    }
});

test('with no shutdown mark, a restart record recmgr or crashhandler wrote after the boot was armed is the evidence', async () => {
    for (const who of ['recmgr', 'crashhandler']) {
        for (const fresh of [true, false]) {
            const w = makeWorld();
            w.install('carplay', 'unproven');
            const info = path.join(w.dir, 'sin', who, 'restartInfo.xml');
            if (!fresh) { fs.writeFileSync(info, '<x/>'); const t = Date.now() / 1000 - 3600; fs.utimesSync(info, t, t); }
            await bootUp(w, 'A');
            if (fresh) fs.writeFileSync(info, '<x/>');   /* the crash; var-opt-ro never ran */
            END.powerCut(w);
            /* the next boot starts after the record */
            fs.writeFileSync(path.join(ROOT, 'no-proc-stat'), 'cpu 1 2 3\nbtime ' + Math.ceil(Date.now() / 1000) + '\n');
            let step;
            try { step = (await bootUp(w, 'B')).step; } finally { fs.unlinkSync(path.join(ROOT, 'no-proc-stat')); }
            assert.strictEqual(step.action, fresh ? 'restored' : 'armed', who + (fresh ? ': written in that boot' : ': older than the boot'));
        }
    }
});

test('a healthy boot clears the counter; a crash after a healthy minute still counts', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    await bootUp(w, 'A');
    END.otherCrash(w, 'A');
    const { watcher } = await bootUp(w, 'B');
    assert.strictEqual(w.state('carplay').fails, 1);
    w.now.advance(30000);
    assert.strictEqual((await tick(watcher)).action, 'watch');
    w.now.advance(31000);
    assert.strictEqual((await tick(watcher)).action, 'confirmed');
    END.otherCrash(w, 'B');
    const c = await bootUp(w, 'C');
    assert.strictEqual(w.state('carplay').fails, 0, 'healthy, then a restart for another crash: cleared');
    w.now.advance(61000);
    assert.strictEqual((await tick(c.watcher)).action, 'confirmed');
    w.svc.carplay.crashes();
    await tick(c.watcher);
    END.powerCut(w);
    await bootUp(w, 'D');
    assert.strictEqual(w.state('carplay').fails, 1, 'its own crash counts');
});

test('the original (Disable used) or no backup of ours: nothing armed, counted or restored', async () => {
    for (const kind of ['original', 'none']) {
        const w = makeWorld();
        assert.strictEqual(await loop(w, 'carplay', kind, END.crashAtStart, 4), 0, kind);
        assert.strictEqual(w.state('carplay'), null, kind + ': no state written');
        assert.strictEqual(w.holds('carplay'), 'original');
    }
});

test('a missing or corrupt state file restores nothing on its own; the boot is armed afresh', async () => {
    for (const content of [null, '', '{"fails": ', 'null\n', '[1,2]']) {
        const w = makeWorld();
        w.install('carplay', 'unproven');
        if (content !== null) fs.writeFileSync(w.T.carplay.state, content);
        END.otherCrash(w, 'A');   /* a crash restart of a boot with no record of ours */
        const r = await startBoot(w.recovery('carplay', 'B'));
        assert.deepStrictEqual([r.action, r.fails], ['armed', 0], JSON.stringify(content));
        assert.strictEqual(w.holds('carplay'), 'patched');
    }
});

test('a corrupt or missing patch record: the patch counts as proven (restored at boot 3)', async () => {
    for (const rec of ['{oops', '{"patched":true,"proven":false}\n']) {
        const w = makeWorld();
        w.install('carplay', 'unknown');
        fs.writeFileSync(record.recordPath(w.T.carplay.target), rec);
        const r = await startBoot(w.recovery('carplay', 'A'));
        assert.deepStrictEqual([r.action, r.patch], ['armed', 'unknown'], rec);
        END.crashAtStart(w, 'A', 'carplay');
        assert.strictEqual((await startBoot(w.recovery('carplay', 'B'))).action, 'armed', rec);
        END.crashAtStart(w, 'B', 'carplay');
        assert.strictEqual((await startBoot(w.recovery('carplay', 'C', { restore: (f, cb) => recovery.restoreTarget(w.T.carplay, true, f, cb, 1000) }))).action, 'restored', rec);
    }
});

test('an unknown binary that is the original already: nothing written but the record', async () => {
    const w = makeWorld();
    w.install('carplay', 'unknown');
    fs.writeFileSync(w.T.carplay.target, w.bin.carplay.original);
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 2, unknown: 0, arm: null }));
    const r = await startBoot(w.recovery('carplay', 'A'));
    assert.strictEqual(r.action, 'idle');
    assert.deepStrictEqual([record.current(w.T.carplay.target).patched, record.current(w.T.carplay.target).by], [false, undefined], 'recorded, no restore reported');
    assert.strictEqual((await startBoot(w.recovery('carplay', 'B'))).action, 'idle', 'and not armed again');
});

/* ---- both services ---- */

test('both patched: a failure no record explains counts for both; a crash of one counts for it alone', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    w.install('aapd', 'proven');
    await bootUp(w, 'A', ['carplay', 'aapd']);
    END.otherCrash(w, 'A');
    const b = await bootUp(w, 'B', ['carplay', 'aapd']);
    assert.deepStrictEqual([b.steps.carplay.action, b.steps.aapd.action], ['restored', 'armed'], 'each at its own threshold');
    assert.strictEqual(w.state('aapd').fails, 1);
    w.events.length = 0;

    const x = makeWorld();
    x.install('carplay', 'proven');
    x.install('aapd', 'proven');
    await bootUp(x, 'A', ['carplay', 'aapd']);
    END.crashAtStart(x, 'A', 'aapd');
    await bootUp(x, 'B', ['carplay', 'aapd']);
    assert.deepStrictEqual([x.state('carplay').fails, x.state('aapd').fails], [0, 1], 'aapd\'s crash ended the boot');
    END.crashAtStart(x, 'B', 'aapd');
    const c = await bootUp(x, 'C', ['carplay', 'aapd']);
    assert.deepStrictEqual([c.steps.carplay.action, c.steps.aapd.action], ['armed', 'restored']);
});

test('a crash of the other service explains nothing once that service is not patched', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    w.install('aapd', 'original');
    for (const boot of ['A', 'B']) {
        await bootUp(w, boot, ['carplay', 'aapd']);
        END.crashAtStart(w, boot, 'aapd');
    }
    assert.strictEqual((await bootUp(w, 'C', ['carplay', 'aapd'])).steps.carplay.action, 'restored');
});

/* ---- the restore ---- */

test('a backup that does not match its record: refused, the service starts as it is, the next boot tries again', async () => {
    for (const damage of ['bytes', 'record']) {
        const w = makeWorld();
        w.install('carplay', 'unproven');
        if (damage === 'bytes') fs.appendFileSync(w.T.carplay.target + '.woc-orig', 'x');
        else fs.writeFileSync(w.T.carplay.target + '.woc-orig.json', '{}');
        await bootUp(w, 'A');
        END.crashAtStart(w, 'A', 'carplay');
        fs.writeFileSync(MOUNT_LOG, '');
        const { step } = await bootUp(w, 'B');
        assert.strictEqual(step.action, 'restore-failed', damage);
        assert(/refusing the backup/.test(step.error), step.error);
        assert(w.logs.some((l) => /restore failed: refusing the backup/.test(l)), damage + ': logged');
        assert.deepStrictEqual(w.events.slice(-1), ['carplay-start:patched'], damage + ': carplay starts');
        assert.strictEqual(fs.readFileSync(MOUNT_LOG, 'utf8'), '', damage + ': / never remounted');
        assert.strictEqual(w.state('carplay').fails, 1, damage + ': the count stays');
        END.crashAtStart(w, 'B', 'carplay');
        assert.strictEqual((await bootUp(w, 'C')).step.action, 'restore-failed', damage + ': tried again');
    }
});

test('another build than the backup\'s (a firmware update): not restored, the counter cleared', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    await bootUp(w, 'A');
    END.crashAtStart(w, 'A', 'carplay');
    fs.writeFileSync(w.T.carplay.target, Buffer.concat([w.bin.carplay.patched, Buffer.alloc(16)]));
    const { step } = await bootUp(w, 'B');
    assert.strictEqual(step.action, 'armed', 'not ours to vouch for: counted as proven');
    END.crashAtStart(w, 'B', 'carplay');
    const c = await bootUp(w, 'C');
    assert.strictEqual(c.step.action, 'idle');
    assert.strictEqual(w.holds('carplay'), 'other', 'the newer build stays');
    assert.strictEqual(w.state('carplay').fails, 0);
});

test('aapd.conf another file than its backup is left while aapd is restored', async () => {
    const w = makeWorld();
    w.install('aapd', 'unproven');
    fs.writeFileSync(w.T.aapd.files[1], '{"OTHER":1}\n');
    await bootUp(w, 'A', ['aapd']);
    END.crashAtStart(w, 'A', 'aapd');
    assert.strictEqual((await bootUp(w, 'B', ['aapd'])).step.action, 'restored');
    assert.strictEqual(w.holds('aapd'), 'original');
    assert.strictEqual(fs.readFileSync(w.T.aapd.files[1], 'utf8'), '{"OTHER":1}\n');
});

test('the restore sets carplay to 0755, leaves no temporary file, clears the pending marker and leaves / read-only', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    fs.chmodSync(w.T.carplay.target, 0o751);
    fs.writeFileSync(w.T.carplay.pending, '{"action":"apply"}\n');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, unknown: 0, arm: null }));
    let outcome = null;
    await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (err, o) => { assert.ifError(err); outcome = o; res(); }));
    assert.strictEqual(outcome, 'restored');
    assert.strictEqual(fs.statSync(w.T.carplay.target).mode & 0o777, 0o755);
    assert(!fs.existsSync(w.T.carplay.target + '.woc-tmp') && !fs.existsSync(w.T.carplay.pending) && !fs.existsSync(w.T.carplay.running));
    assert(ro());
});

test('recovery under umask 077 sets 0755 even with a restrictive temporary file and keeps ownership', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    const file = w.T.carplay.target, owner = fs.statSync(file);
    fs.chmodSync(file, 0o700);
    fs.writeFileSync(file + '.woc-tmp', 'interrupted restore', { mode: 0o600 });
    const mask = process.umask(0o077);
    try {
        await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (err, outcome) => {
            assert.ifError(err); assert.strictEqual(outcome, 'restored'); res();
        }));
    } finally { process.umask(mask); }
    const after = fs.statSync(file);
    assert.strictEqual(after.mode & 0o7777, 0o755);
    assert.strictEqual(after.uid, owner.uid);
    assert.strictEqual(after.gid, owner.gid);
    assert.strictEqual(w.holds('carplay'), 'original');
    assert(ro());
});

/* Permissions-only damage: an original carplay, aapd or aapd.conf that lacks
 * the access its service's user needs, as a write under umask 077 leaves it. */
async function underUmask077(fn) {
    const mask = process.umask(0o077);
    try { return await fn(); } finally { process.umask(mask); }
}

test('the boot step gives a verified original at 0700 its mode back at once, whatever the count; nothing else changes', async () => {
    for (const fails of [0, 1]) {
        const w = makeWorld();
        w.install('carplay', 'original');
        const file = w.T.carplay.target;
        fs.chmodSync(file, 0o700);
        fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails, unknown: 0, arm: null }));
        const before = fs.statSync(file), rec = record.current(file);
        const r = await underUmask077(() => startBoot(w.recovery('carplay', 'A', { repairModes: (cb) => recovery.repairModes(w.T.carplay, 1000, cb) })));
        assert.strictEqual(r.action, 'idle');
        assert.strictEqual(r.repaired, 1);
        const after = fs.statSync(file);
        assert.strictEqual(after.mode & 0o7777, 0o755);
        assert.strictEqual(after.uid, before.uid);
        assert.strictEqual(after.gid, before.gid);
        assert.strictEqual(after.ino, before.ino);
        assert.strictEqual(w.holds('carplay'), 'original');
        assert.deepStrictEqual(record.current(file), rec);
        assert(ro());
        const again = await startBoot(w.recovery('carplay', 'B'));
        assert.strictEqual(again.action, 'idle');
        assert.strictEqual(again.repaired, undefined);
    }
});

test('the watcher\'s start repairs it too; the original\'s recorded mode and owner are the ones set', async () => {
    const w = makeWorld();
    w.install('carplay', 'original');
    const file = w.T.carplay.target, st = fs.statSync(file);
    fs.writeFileSync(file + '.woc-orig.json', JSON.stringify({ target: file, sha256_original: sha(w.bin.carplay.original), mode: 0o775, uid: st.uid, gid: st.gid }));
    fs.chmodSync(file, 0o700);
    const r = await underUmask077(() => startBoot(w.recovery('carplay', 'A')));
    assert.strictEqual(r.repaired, 1);
    assert.strictEqual(fs.statSync(file).mode & 0o7777, 0o775);
});

test('aapd at 0700 and aapd.conf at 0600, each its verified original: 0755 and 0644 at the boot step', async () => {
    const w = makeWorld();
    w.install('aapd', 'original');
    fs.chmodSync(w.T.aapd.files[0], 0o700);
    fs.chmodSync(w.T.aapd.files[1], 0o600);
    const r = await underUmask077(() => startBoot(w.recovery('aapd', 'A')));
    assert.strictEqual(r.repaired, 2);
    assert.strictEqual(fs.statSync(w.T.aapd.files[0]).mode & 0o7777, 0o755);
    assert.strictEqual(fs.statSync(w.T.aapd.files[1]).mode & 0o7777, 0o644);
    assert(ro());
});

test('with no backup of ours, the proven build\'s shipped carplay at 0700 is repaired; any other file is not', async () => {
    for (const stock of [true, false]) {
        const w = makeWorld();
        w.install('carplay', 'none');
        w.T.carplay.stock = (i, cur) => stock && cur.equals(w.bin.carplay.original);
        fs.chmodSync(w.T.carplay.target, 0o700);
        const r = await startBoot(w.recovery('carplay', 'A'));
        assert.strictEqual(r.action, 'idle');
        assert.strictEqual(r.repaired, stock ? 1 : undefined);
        assert.strictEqual(fs.statSync(w.T.carplay.target).mode & 0o7777, stock ? 0o755 : 0o700);
        assert(ro());
    }
});

test('our patch at 0700 is not repaired in place: it counts, and the restore at the threshold writes the original at 0755', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    fs.chmodSync(w.T.carplay.target, 0o700);
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: THRESHOLD.proven, unknown: 0, arm: null }));
    const r = await underUmask077(() => startBoot(w.recovery('carplay', 'A', { restore: (fails, cb) => recovery.restoreTarget(w.T.carplay, true, fails, cb, 1000) })));
    assert.strictEqual(r.repaired, undefined);
    assert.strictEqual(r.action, 'restored');
    assert.strictEqual(w.holds('carplay'), 'original');
    assert.strictEqual(fs.statSync(w.T.carplay.target).mode & 0o7777, 0o755);
    assert(ro());
});

test('the aapd restore under umask 077 writes aapd 0755 and aapd.conf 0644 over a stale temporary file', async () => {
    const w = makeWorld();
    w.install('aapd', 'unproven');
    fs.chmodSync(w.T.aapd.files[0], 0o700);
    fs.chmodSync(w.T.aapd.files[1], 0o600);
    fs.writeFileSync(w.T.aapd.files[0] + '.woc-tmp', 'cut short', { mode: 0o600 });
    await underUmask077(() => new Promise((res) => recovery.restoreTarget(w.T.aapd, true, 1, (err, o) => { assert.ifError(err); assert.strictEqual(o, 'restored'); res(); })));
    assert.strictEqual(w.holds('aapd'), 'original');
    assert(fs.readFileSync(w.T.aapd.files[1]).equals(CONF.original));
    assert.strictEqual(fs.statSync(w.T.aapd.files[0]).mode & 0o7777, 0o755);
    assert.strictEqual(fs.statSync(w.T.aapd.files[1]).mode & 0o7777, 0o644);
    assert(!fs.existsSync(w.T.aapd.files[0] + '.woc-tmp'));
    assert(ro());
});

test('the real boot step under umask 077 repairs an original carplay at 0700 before carplay starts', () => {
    const w = makeWorld();
    w.install('carplay', 'original');
    fs.chmodSync(w.T.carplay.target, 0o700);
    const mask = process.umask(0o077);
    let r;
    try { r = runBootStep(w, 'carplay', 'A'); } finally { process.umask(mask); }
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    assert(/carplay boot check: idle, mode and owner set on 1 file/.test(r.stdout), r.stdout);
    assert.strictEqual(fs.statSync(w.T.carplay.target).mode & 0o7777, 0o755);
    assert(ro());
});

test('originals the service users can run and read (0555, 0444) are left alone: nothing remounted', async () => {
    for (const name of ['carplay', 'aapd']) {
        const w = makeWorld();
        w.install(name, 'original');
        fs.chmodSync(w.T[name].files[0], 0o555);
        if (name === 'aapd') fs.chmodSync(w.T.aapd.files[1], 0o444);
        const before = fs.existsSync(MOUNT_LOG) ? fs.readFileSync(MOUNT_LOG, 'utf8') : '';
        const r = await startBoot(w.recovery(name, 'A'));
        assert.strictEqual(r.repaired, undefined, name);
        assert.strictEqual(fs.statSync(w.T[name].files[0]).mode & 0o7777, 0o555);
        if (name === 'aapd') assert.strictEqual(fs.statSync(w.T.aapd.files[1]).mode & 0o7777, 0o444);
        assert.strictEqual(fs.existsSync(MOUNT_LOG) ? fs.readFileSync(MOUNT_LOG, 'utf8') : '', before, name + ': no remount');
    }
});

test('a stop during the boot step\'s mode repair waits for it: / read-only and the lock released before it exits', async () => {
    const w = makeWorld();
    w.install('carplay', 'original');
    fs.chmodSync(w.T.carplay.target, 0o700);
    const mounts = path.join(w.dir, 'mounts'), lockFile = path.join(w.dir, 'remount.lock'), mount = path.join(w.dir, 'mount');
    fs.writeFileSync(mounts, '/dev/root / ext4 ro 0 0\n');
    /* remount,rw takes / read-write and returns after 1.5 s */
    fs.writeFileSync(mount, '#!' + process.execPath + '\nvar fs = require("fs"), rw = process.argv.indexOf("remount,rw") > 0;\n' +
        'fs.writeFileSync(' + JSON.stringify(mounts) + ', "/dev/root / ext4 " + (rw ? "rw" : "ro") + " 0 0\\n");\n' +
        'if (rw) setTimeout(function () {}, 1500);\n');
    fs.chmodSync(mount, 0o755);
    cp.execFileSync(mount, ['first']);   /* macOS checks a new executable at its first start */
    fs.writeFileSync(mounts, '/dev/root / ext4 ro 0 0\n');
    fs.writeFileSync(path.join(w.dir, 'boot_id'), 'A\n');
    const child = cp.spawn(process.execPath, [RECOVERY_PATH, 'boot', 'carplay'], {
        env: Object.assign({}, process.env, { WOC_CARPLAY: w.T.carplay.target, WOC_AAPD: w.T.aapd.target, WOC_AAPD_CONF: w.T.aapd.files[1],
            WOC_RECOVERY_STATE: w.T.carplay.state, WOC_PATCH_SOCK: path.join(w.dir, 'shm', 'patch.sock'), WOC_BOOT_ID: path.join(w.dir, 'boot_id'),
            WOC_MOUNT: mount, WOC_PROC_MOUNTS: mounts, WOC_REMOUNT_LOCK: lockFile }),
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    child.stdout.on('data', (d) => { out += d; });
    const exited = new Promise((r) => child.on('exit', (code) => r(code)));
    for (let t = Date.now(); !/\brw\b/.test(fs.readFileSync(mounts, 'utf8')) && Date.now() - t < 5000;) await new Promise((r) => setTimeout(r, 10));
    assert(/\brw\b/.test(fs.readFileSync(mounts, 'utf8')), 'the repair took / read-write: ' + out);
    child.kill('SIGTERM');
    const code = await exited;
    assert.strictEqual(code, 0, out);
    assert(/\bro\b/.test(fs.readFileSync(mounts, 'utf8')), '/ read-only again: ' + out);
    assert(!fs.existsSync(lockFile), 'the remount lock released');
    assert.strictEqual(fs.statSync(w.T.carplay.target).mode & 0o7777, 0o755);
    assert(!/boot check: (idle|armed)/.test(out), 'nothing decided after the stop: ' + out);
});

test('permissions-only recovery leaves unknown originals and firmware replacements untouched', async () => {
    for (const other of [false, true]) {
        const w = makeWorld();
        w.install('carplay', 'original');
        const file = w.T.carplay.target;
        if (other) fs.writeFileSync(file, require('./synthetic_carplay.js').build().buf);
        fs.chmodSync(file, 0o700);
        const before = fs.readFileSync(file);
        let outcome;
        await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 2, (err, o) => {
            assert.ifError(err); outcome = o; res();
        }));
        assert.strictEqual(outcome, other ? 'other' : 'original');
        assert(fs.readFileSync(file).equals(before));
        assert.strictEqual(fs.statSync(file).mode & 0o7777, 0o700);
        assert(ro());
    }
});

test('the restore defers to patchd\'s remount lock, and the boot step waits for it a bounded time', async () => {
    const lock = require('../remount_lock.js');
    const w = makeWorld();
    w.install('carplay', 'unproven');
    assert(lock.tryAcquire());
    try {
        let err = null;
        await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e) => { err = e; res(); }));
        assert(err && /deferring/.test(err.message), String(err));
        const t0 = Date.now();
        await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e) => { err = e; res(); }, 300));
        assert(err && /held for 300 ms/.test(err.message) && Date.now() - t0 < 2000, String(err));
        assert.strictEqual(w.holds('carplay'), 'patched');
    } finally { lock.release(); }
    setTimeout(() => lock.release(), 200);
    assert(lock.tryAcquire());
    let outcome = null;
    await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e, o) => { assert.ifError(e); outcome = o; res(); }, 3000));
    assert.strictEqual(outcome, 'restored', 'taken once patchd let go');
});

/* A process runs file: a fake /proc/<pid>/exe holding its inode. */
function runs(pid, file) { fs.mkdirSync(path.join(ROOT, 'proc', pid)); fs.linkSync(file, path.join(ROOT, 'proc', pid, 'exe')); }
function noProcs() { fs.readdirSync(path.join(ROOT, 'proc')).forEach((p) => cp.execFileSync('rm', ['-rf', path.join(ROOT, 'proc', p)])); }

test('the patched binary a process runs stays linked so / can go read-only; the watcher links it always', async () => {
    for (const [onlyIfRunning, running] of [[true, true], [false, false], [true, false]]) {
        const w = makeWorld();
        w.install('carplay', 'unproven');
        const ino = fs.statSync(w.T.carplay.target).ino;
        if (running) runs('1', w.T.carplay.target);
        try {
            await new Promise((res) => recovery.restoreTarget(w.T.carplay, onlyIfRunning, 1, (e) => { assert.ifError(e); res(); }));
            const linked = fs.existsSync(w.T.carplay.running) ? fs.statSync(w.T.carplay.running).ino : null;
            assert.strictEqual(linked, running || !onlyIfRunning ? ino : null, JSON.stringify([onlyIfRunning, running]));
            assert.strictEqual(w.holds('carplay'), 'original');
        } finally { noProcs(); }
    }
});

test('with processes running both the target and its link, the restore waits for the next boot', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    fs.writeFileSync(w.T.carplay.running, 'an older binary\n');
    runs('1', w.T.carplay.running);
    runs('2', w.T.carplay.target);
    try {
        let err = null;
        await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e) => { err = e; res(); }));
        assert(err && /deferring/.test(err.message));
        assert.strictEqual(w.holds('carplay'), 'patched');
    } finally { noProcs(); }
});

test('a remount that leaves / writable is reported', async () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    const mount = path.join(ROOT, 'mount'), body = fs.readFileSync(mount, 'utf8');
    fs.writeFileSync(mount, '#!/bin/sh\ncase "$*" in *remount,rw*) echo "/dev/root / ext4 rw 0 0" > "' + MOUNTS + '";; esac\n');
    try {
        let err = null;
        await new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e) => { err = e; res(); }));
        assert(err && /read-write after remount/.test(err.message), String(err));
    } finally { fs.writeFileSync(mount, body); fs.writeFileSync(MOUNTS, '/dev/root / ext4 ro 0 0\n'); }
});

test('the boot step restores a patch at once when it cannot save the boot\'s record; the original it leaves alone', async () => {
    for (const kind of ['unproven', 'original']) {
        const w = makeWorld();
        w.install('carplay', kind);
        fs.mkdirSync(w.T.carplay.state + '.tmp');   /* the temporary file's name is taken */
        const r = await startBoot(w.recovery('carplay', 'A', { restore: (f, cb) => recovery.restoreTarget(w.T.carplay, true, f, cb, 1000) }));
        assert.strictEqual(r.action, kind === 'original' ? 'idle' : 'restored', kind);
        assert.strictEqual(w.holds('carplay'), 'original', kind);
    }
});

/* ---- the watcher ---- */

test('recmgr loaded carplay: an in-boot crash loop is restored and carplay started again, with no reboot', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'proven');
    const { watcher } = await bootUp(w, 'A');
    w.events.length = 0;
    w.svc.carplay.crashes();
    w.now.advance(5000);
    assert.strictEqual((await tick(watcher)).fails, 1);
    assert.strictEqual((await tick(watcher)).action, 'watch', 'the start waits its delay');
    w.now.advance(5000);
    assert.strictEqual((await tick(watcher)).action, 'started');
    await tick(watcher);
    w.svc.carplay.crashes();
    w.now.advance(5000);
    assert.strictEqual((await tick(watcher)).restore, 'restored');
    w.now.advance(15000);
    assert.strictEqual((await tick(watcher)).action, 'started');
    assert.deepStrictEqual(w.events, ['carplay-crash', 'carplay-start:patched', 'carplay-crash', 'carplay-start:original']);
    const st = w.state('carplay');
    assert.deepStrictEqual([st.fails, st.arm, st.starts], [0, null, { boot: 'A', n: 2 }]);
    assert(fs.existsSync(w.T.carplay.running), 'the watcher kept the binary that ran linked');
});

test('an unproven patch is restored at its first in-boot crash', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'unproven');
    const { watcher } = await bootUp(w, 'A');
    w.svc.carplay.crashes();
    w.now.advance(5000);
    assert.deepStrictEqual(await tick(watcher), { action: 'crash', fails: 1, restore: 'restored' });
});

test('a run that crashed before the watcher\'s first probe is a crash, counted once, also across a watcher restart', async () => {
    const w = makeWorld({ recmgr: false });
    w.install('carplay', 'proven');
    await startBoot(w.recovery('carplay', 'A'));
    w.svc.carplay.starts();
    w.svc.carplay.crashes();
    const watcher = w.recovery('carplay', 'A');
    await startBoot(watcher);
    assert.strictEqual((await tick(watcher)).action, 'crash');
    w.now.advance(5000);
    assert.strictEqual((await tick(watcher)).action, 'watch');
    const again = w.recovery('carplay', 'A');
    await startBoot(again);
    assert.strictEqual((await tick(again)).action, 'watch');
    assert.strictEqual(w.state('carplay').arm.crashes, 1);
    assert.strictEqual(starts(w, 'carplay'), 1, 'without recmgr\'s entry nothing is started');
});

test('after patchd\'s activation kill, a crash of the run it started is a crash', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'proven');
    const { watcher } = await bootUp(w, 'A');
    fs.writeFileSync(w.T.carplay.killed, w.svc.carplay.run + '\n');
    w.svc.carplay.alive.clear();
    Object.assign(w.svc.carplay, { pid: 0, state: 'failed', result: 'signal' });
    assert.strictEqual((await tick(watcher)).action, 'watch', 'patchd\'s kill is not a crash');
    w.svc.carplay.starts();
    w.svc.carplay.crashes();
    w.now.advance(5000);
    assert.strictEqual((await tick(watcher)).action, 'crash');
});

test('a SIGKILL that was not patchd\'s: no crash, started again where recmgr loaded the entry', async () => {
    for (const recmgr of [true, false]) {
        const w = makeWorld({ recmgr });
        w.install('carplay', 'proven');
        const { watcher } = await bootUp(w, 'A');
        w.svc.carplay.ends('2', '9');
        w.now.advance(5000);
        assert.strictEqual((await tick(watcher)).action, 'killed');
        w.now.advance(5000);
        assert.strictEqual((await tick(watcher)).action, recmgr ? 'started' : 'watch');
        assert.strictEqual(w.state('carplay').arm.crashes, 0);
    }
});

test('no crash: patchd\'s kill, a death while the system stops, a SIGTERM, a clean exit or stop', async () => {
    for (const how of ['patchd', 'stopping', 'sigterm', 'exit', 'stopped']) {
        const w = makeWorld({ recmgr: true });
        w.install('carplay', 'proven');
        const { watcher } = await bootUp(w, 'A');
        if (how === 'patchd') fs.writeFileSync(w.T.carplay.killed, w.svc.carplay.run + '\n');
        if (how === 'stopping') { w.system = 'stopping'; w.svc.carplay.crashes(); }
        else if (how === 'sigterm') w.svc.carplay.ends('2', '15');
        else if (how === 'exit') w.svc.carplay.ends('1', '0');
        else if (how === 'stopped') w.svc.carplay.ends('', '');
        else w.svc.carplay.ends('2', '9');
        w.now.advance(5000);
        assert.strictEqual((await tick(watcher)).action, 'watch', how);
        w.system = 'running';
        for (let i = 0; i < 3; i++) { w.now.advance(60000); await tick(watcher); }
        assert.strictEqual(starts(w, 'carplay'), 1, how);
        assert.strictEqual(w.state('carplay').arm.crashes, 0, how);
    }
});

test('recmgr loaded carplay: starts are bounded per boot, and none while patchd holds the lock or the system stops', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'original');
    const { watcher } = await bootUp(w, 'A');
    w.svc.carplay.crashes();
    w.now.advance(5000);
    await tick(watcher);
    w.now.advance(5000);
    w.lockHeld = true;
    assert.strictEqual((await tick(watcher)).action, 'watch', 'waits for patchd');
    w.lockHeld = false;
    w.system = 'stopping';
    assert.strictEqual((await tick(watcher)).action, 'start-skipped');
    w.system = 'running';
    w.svc.carplay.starts();   /* by hand */
    for (let i = 0; i < 5; i++) {
        w.svc.carplay.crashes();
        w.now.advance(5000);
        await tick(watcher);
        w.now.advance(60000);
        await tick(watcher);
        w.now.advance(5000);
        await tick(watcher);
    }
    assert.strictEqual(starts(w, 'carplay'), 2 + 3, 'three starts of its own');
});

test('an unwritable state file restores a patch at its first crash, and does not lift the start limit', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'proven');
    const { watcher } = await bootUp(w, 'A');
    fs.mkdirSync(w.T.carplay.state + '.tmp');
    w.svc.carplay.crashes();
    w.now.advance(5000);
    const r = await tick(watcher);
    assert.deepStrictEqual([r.fails, r.restore], [1, 'restored'], 'below the threshold, since the count is not kept');
    for (let i = 0; i < 5; i++) {
        w.now.advance(60000);
        await tick(watcher);
        w.svc.carplay.crashes();
        w.now.advance(5000);
        await tick(watcher);
    }
    assert.strictEqual(starts(w, 'carplay'), 1 + 3);
});

test('a crash the watcher sees once var-opt-ro made /var/opt read-only restores nothing; the next boot counts it', async () => {
    for (const armed of [true, false]) {
        const w = makeWorld({ recmgr: true });
        w.install('carplay', 'proven');
        const { watcher } = armed ? await bootUp(w, 'A') : { watcher: w.recovery('carplay', 'A') };
        if (!armed) { w.svc.carplay.starts(); await tick(watcher); }
        const open = fs.openSync;
        fs.openSync = function (p, flags) {
            if (String(p).indexOf(w.T.carplay.state) === 0 && flags === 'w') { const e = new Error('EROFS: read-only file system, open \'' + p + '\''); e.code = 'EROFS'; throw e; }
            return open.apply(fs, arguments);
        };
        try {
            w.svc.carplay.crashes();
            w.now.advance(5000);
            assert.strictEqual((await tick(watcher)).restore, undefined, 'armed ' + armed);
        } finally { fs.openSync = open; }
        assert.strictEqual(w.holds('carplay'), 'patched', 'armed ' + armed);
        nsmShutdown(w, 'A', shown(w), true);
        await bootUp(w, 'B');
        assert.strictEqual(w.state('carplay').fails, armed ? 1 : 0, 'armed ' + armed + ': counted from the shutdown mark when this boot was armed');
    }
});

test('a dying PID systemd has not reaped yet is no crash', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    await startBoot(w.recovery('carplay', 'A'));
    w.svc.carplay.starts();
    const watcher = w.recovery('carplay', 'A');
    await tick(watcher);
    w.svc.carplay.alive.clear();   /* gone from /proc, systemd still says active */
    assert.strictEqual((await tick(watcher)).action, 'watch');
    assert.strictEqual(w.state('carplay').arm.crashes, 0);
});

test('systemctl is asked every 60 s while the service\'s main PID is in /proc, every 5 s while it is down', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    let probes = 0;
    const watcher = w.recovery('carplay', 'A', { probe: (cb) => { probes++; cb(Object.assign({}, w.svc.carplay)); } });
    await startBoot(watcher);
    w.svc.carplay.starts();
    for (let i = 0; i < 120; i++) { await tick(watcher); w.now.advance(1000); }
    assert.strictEqual(probes, 2, 'carplay up');
    w.svc.carplay.crashes();
    probes = 0;
    assert.strictEqual((await tick(watcher)).action, 'crash', 'its PID gone: a crash seen at the next tick');
    assert.strictEqual(probes, 1);
    w.svc.carplay.ends('', '');
    probes = 0;
    for (let i = 0; i < 10; i++) { w.now.advance(1000); await tick(watcher); }
    assert.strictEqual(probes, 2, 'carplay down');
    w.svc.carplay.starts();
    probes = 0;
    for (let i = 0; i < 5; i++) { w.now.advance(1000); await tick(watcher); }
    assert.strictEqual(probes, 1, 'a new run found within 5 s');
});

test('the state is read for a run\'s healthy minute until it is settled, then not again in that run', async () => {
    const w = makeWorld();
    w.install('carplay', 'proven');
    let loads = 0, failSave = true;
    const watcher = w.recovery('carplay', 'A');
    const load = watcher._load, save = watcher._save;
    watcher._load = function () { loads++; return load.call(this); };
    watcher._save = function (st) { return failSave && st.arm && st.arm.confirmed ? false : save.call(this, st); };
    await startBoot(watcher);
    w.svc.carplay.starts();
    await tick(watcher);
    w.now.advance(61000);
    assert.strictEqual((await tick(watcher)).action, 'confirmed', 'the confirmation could not be saved');
    w.now.advance(1000);
    assert.strictEqual((await tick(watcher)).action, 'confirmed', 'so the next tick confirms again');
    failSave = false;
    w.now.advance(1000);
    assert.strictEqual((await tick(watcher)).action, 'confirmed');
    assert.strictEqual(w.state('carplay').arm.confirmed, true);
    loads = 0;
    for (let i = 0; i < 30; i++) { w.now.advance(1000); await tick(watcher); }
    assert.strictEqual(loads, 0, 'saved: no more reads this run');
    w.svc.carplay.crashes();
    await tick(watcher);
    w.svc.carplay.starts();
    w.now.advance(5000);
    await tick(watcher);
    w.now.advance(61000);
    loads = 0;
    await tick(watcher);
    assert(loads > 0, 'the next run reads it again at its healthy minute');

    const none = makeWorld();
    none.install('carplay', 'none');
    const idle = none.recovery('carplay', 'A');
    let idleLoads = 0;
    const idleLoad = idle._load;
    idle._load = function () { idleLoads++; return idleLoad.call(this); };
    await startBoot(idle);
    none.svc.carplay.starts();
    await tick(idle);
    none.now.advance(61000);
    idleLoads = 0;
    for (let i = 0; i < 30; i++) { await tick(idle); none.now.advance(1000); }
    assert.strictEqual(idleLoads, 1, 'nothing armed: read once');
});

test('a stop during a restore waits for it', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'proven');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, unknown: 0, arm: null }));
    let finishRestore = null;
    const watcher = w.recovery('carplay', 'A', { restore: (f, cb) => { finishRestore = cb; } });
    await startBoot(watcher);
    w.svc.carplay.starts();
    await tick(watcher);
    w.svc.carplay.crashes();
    w.now.advance(5000);
    watcher.tick();
    assert.strictEqual(watcher.restoring, true);
    let stopped = false;
    watcher.whenRestored(() => { stopped = true; });
    assert.strictEqual(stopped, false);
    finishRestore(null, 'restored');
    assert.strictEqual(stopped, true);
});

test('a save that fails leaves the previous state whole; a folder that cannot be synced is a saved state', () => {
    const w = makeWorld();
    const before = JSON.stringify({ fails: 1, unknown: 0, arm: { boot: 'A', patch: 'proven', crashes: 1, confirmed: false, clean: false } }) + '\n';
    fs.writeFileSync(w.T.carplay.state, before);
    fs.mkdirSync(w.T.carplay.state + '.tmp');
    assert.strictEqual(w.recovery('carplay', 'B')._save({ fails: 0, arm: null }), false);
    assert.strictEqual(fs.readFileSync(w.T.carplay.state, 'utf8'), before);
    fs.rmdirSync(w.T.carplay.state + '.tmp');
    fs.chmodSync(path.dirname(w.T.carplay.state), 0o300);
    try { assert.strictEqual(w.recovery('carplay', 'B')._save({ fails: 2, arm: null }), true); } finally { fs.chmodSync(path.dirname(w.T.carplay.state), 0o700); }
    assert.strictEqual(w.state('carplay').fails, 2);
});

test('the clock is monotonic: the default timer reads process.hrtime', () => {
    const w = makeWorld();
    const rec = new Recovery({ target: w.T.carplay, bootId: 'A' });
    const real = process.hrtime();
    assert(Math.abs(rec._now() - (real[0] * 1000 + real[1] / 1e6)) < 1000);
});

test('the system counts as stopping while shutdown.target is queued or active, also before the boot finished', () => {
    const w = makeWorld();
    const sc = path.join(w.dir, 'sc');
    /* show prints shutdown.target's block, and var-opt-ro.service's when asked for it */
    const state = (target, running, varOptRo) => {
        fs.writeFileSync(sc, '#!/bin/sh\ncase "$1" in show) printf "' + target + '\\n"; case "$*" in *var-opt-ro.service*) printf "\\n' + (varOptRo || 'ActiveState=inactive\\nJob=') +
            '\\n" ;; esac ;; is-system-running) echo ' + running + ' ;; esac\n', { mode: 0o755 });
        return cp.execFileSync(process.execPath, ['-e', 'require(process.argv[1]).realSystemState(function (s) { process.stdout.write(s); })', RECOVERY_PATH],
            { env: Object.assign({}, process.env, { WOC_SYSTEMCTL: sc }), encoding: 'utf8' });
    };
    assert.strictEqual(state('ActiveState=inactive\\nJob=', 'starting'), 'starting');
    assert.strictEqual(state('ActiveState=inactive\\nJob=', 'running'), 'running');
    assert.strictEqual(state('ActiveState=inactive\\nJob=1234', 'starting'), 'stopping', 'a reboot asked for before the boot finished');
    assert.strictEqual(state('ActiveState=active\\nJob=', 'stopping'), 'stopping');
    assert.strictEqual(state('ActiveState=inactive\\nJob=', 'running', 'ActiveState=activating\\nJob=88'), 'stopping', 'an NSM shutdown: var-opt-ro.service starting');
    assert.strictEqual(state('ActiveState=inactive\\nJob=', 'running', 'ActiveState=inactive\\nJob=91'), 'stopping', 'an NSM shutdown: var-opt-ro.service queued');
});

test('a crash the watcher sees once this boot\'s shutdown mark is written (an NSM shutdown under way) never counts', async () => {
    const w = makeWorld({ recmgr: true });
    w.install('carplay', 'proven');
    const { watcher } = await bootUp(w, 'A');
    fs.writeFileSync(w.T.carplay.mark, 'A running up 30\n');   /* shutdown_mark.sh ran */
    w.svc.carplay.crashes();
    w.now.advance(5000);
    assert.strictEqual((await tick(watcher)).action, 'watch');
    assert.strictEqual(w.state('carplay').arm.crashes, 0);
    await bootUp(w, 'B');
    assert.strictEqual(w.state('carplay').fails, 0, 'a switch-off');
});

/* ---- shutdown_mark.sh ---- */

function runMark(show, opts) {
    opts = opts || {};
    const dir = fs.mkdtempSync(path.join(ROOT, 'mark-'));
    fs.writeFileSync(path.join(dir, 'boot_id'), 'boot-X\n');
    fs.writeFileSync(path.join(dir, 'uptime'), (opts.uptime || '125.50') + ' 300.00\n');
    fs.writeFileSync(path.join(dir, 'stat'), 'cpu 1 2 3\nbtime ' + Math.floor(Date.now() / 1000 - parseFloat(opts.uptime || '125.50')) + '\n');
    const sc = path.join(dir, 'systemctl');
    fs.writeFileSync(sc, '#!/bin/sh\n' + (show === null ? 'exit 1\n' : show === 'hang' ? 'exec sleep 30\n' : 'printf "%s\\n" "' + show.join('" "') + '"\n'), { mode: 0o755 });
    const state = path.join(dir, 'state');
    if (!opts.noDir) fs.mkdirSync(state);
    const sin = path.join(dir, 'sin');
    fs.mkdirSync(path.join(sin, 'recmgr'), { recursive: true });
    fs.mkdirSync(path.join(sin, 'crashhandler'), { recursive: true });
    (opts.info || []).forEach((i) => {
        const f = path.join(sin, i.who, 'restartInfo.xml');
        fs.writeFileSync(f, '<x/>');
        if (i.ageS) { const t = Date.now() / 1000 - i.ageS; fs.utimesSync(f, t, t); }
    });
    const t0 = Date.now();
    const r = cp.spawnSync('sh', [MARK_SH], { env: Object.assign({}, process.env, { WOC_STATE_DIR: state, WOC_BOOT_ID: path.join(dir, 'boot_id'),
        WOC_SYSTEMCTL: sc, WOC_MARK_WAIT: '1', WOC_SIN_DIR: sin, WOC_UPTIME: path.join(dir, 'uptime'), WOC_PROC_STAT: path.join(dir, 'stat') }), stdio: 'ignore' });
    const read = (f) => { try { return fs.readFileSync(path.join(state, f), 'utf8'); } catch (e) { return null; } };
    if (opts.ms) opts.ms.v = Date.now() - t0;
    return { status: r.status, mark: read('shutdown'), aa: read('shutdown-aapd') };
}

test('shutdown_mark.sh records running, failed or stopped, and nothing it cannot read', () => {
    const up = ['ActiveState=active', 'Result=success'];
    assert.deepStrictEqual(runMark(up), { status: 0, mark: 'boot-X running up 125\n', aa: 'boot-X running up 125\n' });
    assert.strictEqual(runMark(['ActiveState=failed', 'Result=core-dump']).mark, 'boot-X failed up 125\n');
    assert.strictEqual(runMark(['ActiveState=deactivating', 'Result=watchdog']).mark, 'boot-X failed up 125\n');
    assert.strictEqual(runMark(['ActiveState=inactive', 'Result=success']).mark, 'boot-X stopped up 125\n');
    assert.strictEqual(runMark(['ActiveState=inactive', 'Result=success', 'ExecMainStartTimestampMonotonic=99']).mark, 'boot-X stopped ran up 125\n', 'started, then stopped');
    assert.strictEqual(runMark(['ActiveState=inactive', 'Result=success', 'ExecMainStartTimestampMonotonic=0']).mark, 'boot-X stopped up 125\n', 'never started');
    assert.deepStrictEqual(runMark(null), { status: 0, mark: null, aa: null });
    assert.deepStrictEqual(runMark(up, { noDir: true }), { status: 0, mark: null, aa: null });
    const ms = {};
    assert.strictEqual(runMark('hang', { ms }).mark, null, 'a hung systemctl writes nothing');
    assert(ms.v < 5000, 'and holds the remount up WAIT seconds at most: ' + ms.v);
});

test('shutdown_mark.sh adds crash for a restart recmgr or crashhandler asked for this boot, not for an older record', () => {
    const up = ['ActiveState=active', 'Result=success'];
    for (const who of ['recmgr', 'crashhandler']) {
        assert.deepStrictEqual(runMark(up, { info: [{ who }] }), { status: 0, mark: 'boot-X running crash up 125\n', aa: 'boot-X running crash up 125\n' }, who);
        assert.strictEqual(runMark(up, { info: [{ who, ageS: 200 }], uptime: '125.50' }).mark, 'boot-X running up 125\n', who + ': from before this boot');
        assert.strictEqual(runMark(up, { info: [{ who, ageS: 100 }], uptime: '125.50' }).mark, 'boot-X running crash up 125\n', who + ': from this boot');
        assert.strictEqual(runMark(up, { info: [{ who, ageS: -60 }], uptime: '125.50' }).mark, 'boot-X running up 125\n', who + ': from the future');
    }
    assert.strictEqual(runMark(['ActiveState=failed', 'Result=core-dump'], { info: [{ who: 'crashhandler' }] }).mark, 'boot-X failed crash up 125\n');
});

/* ---- the boot step as the unit runs it ---- */

function runBootStep(w, name, bootId, env) {
    fs.writeFileSync(path.join(w.dir, 'boot_id'), bootId + '\n');
    const t0 = Date.now();
    const r = cp.spawnSync(process.execPath, [RECOVERY_PATH, 'boot', name], {
        env: Object.assign({}, process.env, { WOC_CARPLAY: w.T.carplay.target, WOC_AAPD: w.T.aapd.target, WOC_AAPD_CONF: w.T.aapd.files[1],
            WOC_RECOVERY_STATE: w.T.carplay.state, WOC_PATCH_SOCK: path.join(w.dir, 'shm', 'patch.sock'), WOC_BOOT_ID: path.join(w.dir, 'boot_id') }, env || {}),
        encoding: 'utf8', timeout: 60000,
    });
    r.ms = Date.now() - t0;
    r.own = Number((/ in (\d+) ms,/.exec(r.stdout) || [])[1]);
    return r;
}

test('the boot step decides from stat and small files: an unreadable binary is armed or left alone, in milliseconds', () => {
    for (const kind of ['unproven', 'original', 'none']) {
        const w = makeWorld();
        w.install('carplay', kind);
        fs.chmodSync(w.T.carplay.target, 0o000);   /* reading it would fail */
        try {
            const r = runBootStep(w, 'carplay', 'A');
            assert.strictEqual(r.status, 0, r.stdout + r.stderr);
            assert(new RegExp('carplay boot check: ' + (kind === 'unproven' ? 'armed \\(unproven patch, 0 failed\\)' : 'idle')).test(r.stdout), kind + ': ' + r.stdout);
            assert(r.own < 100, kind + ': its own work took ' + r.own + ' ms');
            console.log(`     (${kind}: boot step ${r.ms} ms in all, ${r.own} ms after node started)`);
        } finally { fs.chmodSync(w.T.carplay.target, 0o755); }
    }
});

test('the boot step exits 0 whatever fails, so the service\'s start goes ahead', () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 5, unknown: 0, arm: { boot: 'old', crashes: 1 } }));
    const noMount = path.join(w.dir, 'no-mount');
    for (const env of [{ WOC_RECOVERY_STATE: path.join(w.T.carplay.target, 'nested.json'), WOC_MOUNT: noMount }, { WOC_MOUNT: noMount }]) {
        const r = runBootStep(w, 'carplay', 'B', env);
        assert.strictEqual(r.status, 0, JSON.stringify(env) + ': ' + r.stdout + r.stderr);
        assert(/boot check: restore-failed/.test(r.stdout), r.stdout);
        assert(r.ms < 10000);
    }
    assert.strictEqual(runBootStep(w, 'nothing', 'B').status, 0, 'an unknown service');
});

test('the carplay boot step keeps recmgr\'s and crashhandler\'s restart records by boot, the last 5 boots by count, scrubbed and cut; none, nothing', () => {
    const w = makeWorld();
    w.install('carplay', 'original');
    const sin = path.join(w.dir, 'sin'), keep = path.join(w.dir, 'var', 'restart-records');
    const env = { WOC_SIN_DIR: sin, WOC_RESTART_RECORDS: keep };
    const listed = () => fs.readdirSync(keep).sort();
    /* as root the step makes the folder group systemd-journal; here, the
     * test's own uid and group stand in */
    const groups = path.join(w.dir, 'group');
    fs.writeFileSync(groups, 'systemd-journal:x:' + process.getgid() + ':\n');
    const opts = { uid: process.getuid(), groupFile: groups };
    const keepNow = (id) => recovery.keepRestartRecords(w.T.carplay, keep, id, opts);
    /* no record: nothing written, not even the folder, by the boot step itself */
    let r = runBootStep(w, 'carplay', 'boot-0', env);
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    assert(!fs.existsSync(keep), 'no folder without a record');
    assert.strictEqual(keepNow('boot-0'), 0);
    assert(!fs.existsSync(keep));
    /* both records: one with a VIN, one over the cap with a VIN across it */
    const VIN = 'WBA8E9C50GK123456';
    fs.writeFileSync(path.join(sin, 'recmgr', 'restartInfo.xml'), '<restartInfo vin="' + VIN + '">recmgr</restartInfo>\n');
    const at = recovery.RECORD_MAX - 70;
    fs.writeFileSync(path.join(sin, 'crashhandler', 'restartInfo.xml'), 'z'.repeat(at) + ' ' + VIN + ' ' + 'z'.repeat(5000));
    fs.mkdirSync(keep);
    fs.writeFileSync(path.join(keep, '9-boot-x-recmgr.xml.tmp'), 'left by a cut-off write');
    assert.strictEqual(keepNow('boot-1'), 2);
    assert.deepStrictEqual(listed(), ['1-boot-1-crashhandler.xml', '1-boot-1-recmgr.xml'], 'a stray temporary file removed');
    assert.strictEqual(fs.readFileSync(path.join(keep, '1-boot-1-recmgr.xml'), 'utf8'), '<restartInfo vin="<VIN>">recmgr</restartInfo>\n');
    const big = fs.readFileSync(path.join(keep, '1-boot-1-crashhandler.xml'), 'utf8');
    assert(Buffer.byteLength(big) <= recovery.RECORD_MAX && big.indexOf('WBA8E9') < 0 && /\.\.\. \(cut at \d+ bytes\)\n$/.test(big), big.slice(-100));
    assert.strictEqual(fs.statSync(keep).mode & 0o7777, 0o2770, 'the folder as the kept journal\'s');
    listed().forEach((n) => assert.strictEqual(fs.statSync(path.join(keep, n)).mode & 0o777, 0o640, n));
    /* a second run in the same boot adds nothing */
    assert.strictEqual(keepNow('boot-1'), 0);
    /* six more boots with a record, the files kept so far dated a year ahead
     * (the clock unset at the boot step): those of the newest 5 boots stay */
    fs.unlinkSync(path.join(sin, 'crashhandler', 'restartInfo.xml'));
    for (let i = 2; i <= 7; i++) {
        const ahead = Date.now() / 1000 + 365 * 86400;
        listed().forEach((n) => fs.utimesSync(path.join(keep, n), ahead, ahead));
        keepNow('boot-' + i);
    }
    assert.deepStrictEqual(listed(), ['3-boot-3-recmgr.xml', '4-boot-4-recmgr.xml', '5-boot-5-recmgr.xml', '6-boot-6-recmgr.xml', '7-boot-7-recmgr.xml']);
    /* the boot step: only the carplay one keeps them, and a failure (here the
     * group, or a folder it cannot make) is a line in its log, the boot check as ever */
    assert(!/restart record/.test(runBootStep(w, 'aapd', 'boot-8', env).stdout));
    fs.writeFileSync(path.join(w.dir, 'not-a-folder'), '');
    r = runBootStep(w, 'carplay', 'boot-8', Object.assign({}, env, { WOC_RESTART_RECORDS: path.join(w.dir, 'not-a-folder', 'x') }));
    assert.strictEqual(r.status, 0);
    assert(/restart records not kept: /.test(r.stdout) && /carplay boot check: idle/.test(r.stdout), r.stdout);
});

test('a boot step out of time puts / back read-only and releases the lock before it exits', () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, unknown: 0, arm: null }));
    const mounts = path.join(w.dir, 'mounts'), lockFile = path.join(w.dir, 'remount.lock');
    fs.writeFileSync(mounts, '/dev/root / ext4 ro 0 0\n');
    /* remount,rw takes / read-write, then hangs past its kill (SIGTERM ignored) */
    fs.writeFileSync(path.join(w.dir, 'mount'), '#!' + process.execPath + '\nvar fs = require("fs"), rw = process.argv.indexOf("remount,rw") > 0;\n' +
        'fs.writeFileSync(' + JSON.stringify(mounts) + ', "/dev/root / ext4 " + (rw ? "rw" : "ro") + " 0 0\\n");\n' +
        'if (rw) { process.on("SIGTERM", function () {}); setTimeout(function () {}, 30000); }\n', { mode: 0o755 });
    /* run once first: macOS checks a new executable at its first start, which can outlast a 400 ms step */
    cp.execFileSync(path.join(w.dir, 'mount'), ['first']);
    const t0 = Date.now();
    const r = runBootStep(w, 'carplay', 'B', { WOC_RECOVERY_STEP_MS: '400', WOC_MOUNT: path.join(w.dir, 'mount'), WOC_PROC_MOUNTS: mounts, WOC_REMOUNT_LOCK: lockFile });
    const bound = 400 + 400 * 3 + 500 + 200 + 400 + 500;
    assert(/out of time/.test(r.stdout), r.stdout + r.stderr);
    assert(Date.now() - t0 < bound + 3000, 'exited by its bound: ' + (Date.now() - t0));
    assert(/\bro\b/.test(fs.readFileSync(mounts, 'utf8')), '/ read-only again');
    assert(!fs.existsSync(lockFile), 'the remount lock released');
});

test('a backup without its record: restored when it is the proven build\'s shipped file, refused otherwise', () => {
    const w = makeWorld();
    w.install('carplay', 'unproven');
    fs.unlinkSync(w.T.carplay.target + '.woc-orig.json');
    let err = null;
    const synthetic = new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e) => { err = e; res(); })).then(() => {
        assert(err && /no record/.test(err.message), 'a build of no table: ' + err);
    });
    if (!fs.existsSync(CARPLAY_BIN) || !fs.existsSync(AAPD_BIN) || !fs.existsSync(AAPD_CONF)) { console.log('     (the proven builds skipped: CARPLAY_BIN, AAPD_BIN, AAPD_CONF not set)'); return synthetic; }
    const x = makeWorld(), tgt = x.T.carplay.target;
    fs.copyFileSync(CARPLAY_BIN, tgt);
    cp.execFileSync(process.execPath, [CARPLAY_PATCH, 'apply', '--panel', 'high', '--yes', '--target', tgt], { stdio: 'ignore' });
    fs.unlinkSync(tgt + '.woc-orig.json');
    aapdPatcher(x)(['apply', '--dpi', '170', '--yes']);
    fs.unlinkSync(x.T.aapd.target + '.woc-orig.json');
    fs.unlinkSync(x.T.aapd.files[1] + '.woc-orig.json');
    const outcome = (t) => new Promise((res) => recovery.restoreTarget(t, true, 1, (e, o) => res(e ? e.message : o)));
    return synthetic.then(() => outcome(x.T.carplay)).then((o) => {
        assert.strictEqual(o, 'restored', 'carplay 22.28.3');
        assert(fs.readFileSync(tgt).equals(fs.readFileSync(CARPLAY_BIN)));
        return outcome(x.T.aapd);
    }).then((o) => {
        assert.strictEqual(o, 'restored', 'aapd 22.39.3');
        assert(fs.readFileSync(x.T.aapd.target).equals(fs.readFileSync(AAPD_BIN)) && fs.readFileSync(x.T.aapd.files[1]).equals(fs.readFileSync(AAPD_CONF)));
    });
});

test('a carplay backup whose record names another build-id is refused (the patcher\'s own check)', () => {
    if (!fs.existsSync(CARPLAY_BIN)) { console.log('     (skipped: CARPLAY_BIN not set)'); return; }
    const w = makeWorld(), tgt = w.T.carplay.target;
    fs.copyFileSync(CARPLAY_BIN, tgt);
    cp.execFileSync(process.execPath, [CARPLAY_PATCH, 'apply', '--panel', 'high', '--yes', '--target', tgt], { stdio: 'ignore' });
    const meta = JSON.parse(fs.readFileSync(tgt + '.woc-orig.json', 'utf8'));
    meta.buildId = '00' + meta.buildId.slice(2);
    fs.writeFileSync(tgt + '.woc-orig.json', JSON.stringify(meta));
    let err = null;
    return new Promise((res) => recovery.restoreTarget(w.T.carplay, true, 1, (e) => { err = e; res(); })).then(() => {
        assert(err && /build-id/.test(err.message), String(err));
    });
});

test('the units: before their service, never blocking it, timeouts covering the bound, whitelisted', () => {
    for (const [file, svc] of [['waze-on-cluster-boot.service', 'carplay'], ['waze-on-cluster-aa-boot.service', 'aapd']]) {
        const unit = fs.readFileSync(path.join(PATCH, '..', 'systemd', file), 'utf8');
        const val = (k) => (new RegExp('^' + k + '=(.*)$', 'm').exec(unit) || [])[1];
        assert.strictEqual(val('Before'), svc + '.service');
        assert(!/^(Requires|BindsTo|Requisite|RequiredBy)=/m.test(unit), svc + ' does not depend on it');
        assert.strictEqual(val('Type'), 'oneshot');
        assert.strictEqual(val('ExecStart'), '-/usr/bin/node /var/opt/waze-on-cluster/current/recovery.js boot ' + svc);
        assert(parseInt(val('TimeoutStartSec'), 10) * 1000 > recovery.BOOT_EXIT_MS + 5000, 'the step exits by its bound, inside the start timeout');
        assert(parseInt(val('TimeoutStopSec'), 10) * 1000 > recovery.BOOT_EXIT_MS, 'a stop mid-restore lets it finish');
    }
    const watcher = fs.readFileSync(path.join(PATCH, '..', 'systemd', 'waze-on-cluster-recovery.service'), 'utf8');
    assert(parseInt(/^TimeoutStopSec=(\d+)/m.exec(watcher)[1], 10) * 1000 > RESTORE_BOUND_MS);
    const c = JSON.parse(fs.readFileSync(path.join(PATCH, '..', 'component.json'), 'utf8'));
    ['waze-on-cluster-boot', 'waze-on-cluster-aa-boot', 'waze-on-cluster-recovery'].forEach((n) => assert(c.recmgr.includes(n), n));
    assert(!c.restart.some((u) => /boot/.test(u)), 'they run at boot, not at install');
    assert(c.dropins.some((d) => d.unit === 'var-opt-ro.service') && c.dropins.some((d) => d.unit === 'carplay.service'));
    ['shutdown_mark.sh', 'recmgr_list.js', 'patch_record.js', 'remount_lock.js'].forEach((f) => assert(c.files.some((x) => x.path === f), f));
});

/* ---- the patchers' record, and the real binaries ---- */

/* A copy of a carplay build with another build-id: outside the proven list. */
function unprovenCarplay(buf) {
    const out = Buffer.from(buf), id = require('../carplay_locate.js').buildId(buf), at = out.indexOf(Buffer.from(id, 'hex'));
    assert(at > 0, 'the build-id note');
    out[at] ^= 0xff;
    return out;
}

test('the carplay patcher records each write: on trial, proven once run healthy, unproven, reverted, a missing record repaired, dropped with the backup', async () => {
    if (!fs.existsSync(CARPLAY_BIN)) { console.log('     (skipped: CARPLAY_BIN not set)'); return; }
    const w = makeWorld();
    const tgt = w.T.carplay.target, patcher = (args) => cp.execFileSync(process.execPath, [CARPLAY_PATCH].concat(args, ['--target', tgt]), { encoding: 'utf8' });
    fs.copyFileSync(CARPLAY_BIN, tgt);
    patcher(['apply', '--panel', 'high', '--yes']);
    assert.deepStrictEqual([record.current(tgt).patched, record.current(tgt).proven, record.current(tgt).trial], [true, false, true], 'the first patch is on trial');
    w.runs = true;
    w.bin.carplay = { original: fs.readFileSync(CARPLAY_BIN), patched: fs.readFileSync(tgt) };
    const { watcher } = await bootUp(w, 'A');
    w.now.advance(61000);
    await tick(watcher);
    await whenRestored(watcher);
    assert.deepStrictEqual([record.current(tgt).patched, record.current(tgt).proven, record.current(tgt).trial], [true, true, undefined], 'proven once run healthy');
    assert(/nothing to do/.test(patcher(['apply', '--panel', 'high', '--yes'])));
    assert.strictEqual(record.current(tgt).proven, true, 'an apply keeps it proven');
    patcher(['revert', '--yes']);
    assert.deepStrictEqual([record.current(tgt).patched, record.current(tgt).by, record.current(tgt).healthy], [false, undefined, true]);
    patcher(['apply', '--panel', 'high', '--yes']);
    assert.strictEqual(record.current(tgt).proven, true, 'patched again: the same build ran healthy here');
    record.remove(tgt);
    assert(/nothing to do/.test(patcher(['apply', '--panel', 'high', '--yes'])));
    assert.strictEqual(record.current(tgt).patched, true, 'repaired by an apply with nothing to write');
    patcher(['revert', '--yes', '--drop-backup']);
    assert(!fs.existsSync(record.recordPath(tgt)), 'dropped with the backup');
    fs.writeFileSync(tgt, unprovenCarplay(fs.readFileSync(CARPLAY_BIN)));
    patcher(['apply', '--panel', 'high', '--unproven', '--yes']);
    assert.deepStrictEqual([record.current(tgt).patched, record.current(tgt).proven], [true, false]);
});

/* The aapd patcher on copies of aapd and aapd.conf in w, with a coding of
 * KI_KOMBI_VARIANT 7 and room on the disk. */
function aapdPatcher(w) {
    const coding = path.join(w.dir, 'coding'), c = Buffer.alloc(0x1030);
    c[0x28] = 0x09;
    c[0x2c + 0x75a] = 7 << 1;
    fs.writeFileSync(coding, c);
    fs.writeFileSync(path.join(w.dir, 'df'), '#!/bin/sh\necho "F 1 2 99999999 1% /"\n', { mode: 0o755 });
    fs.copyFileSync(AAPD_BIN, w.T.aapd.target);
    fs.copyFileSync(AAPD_CONF, w.T.aapd.files[1]);
    return (args) => cp.execFileSync(process.execPath, [AAPD_PATCH].concat(args, ['--target', w.T.aapd.target, '--conf', w.T.aapd.files[1], '--coding', coding]),
        { encoding: 'utf8', env: Object.assign({}, process.env, { WOC_DF: path.join(w.dir, 'df') }) });
}

test('the aapd patcher records each write (only its proven build), and a revert', () => {
    if (!fs.existsSync(AAPD_BIN) || !fs.existsSync(AAPD_CONF)) { console.log('     (skipped: AAPD_BIN, AAPD_CONF not set)'); return; }
    const w = makeWorld();
    const patcher = aapdPatcher(w);
    patcher(['apply', '--yes']);
    assert.deepStrictEqual([record.current(w.T.aapd.target).patched, record.current(w.T.aapd.target).proven], [true, true]);
    patcher(['revert', '--yes']);
    assert.strictEqual(record.current(w.T.aapd.target).patched, false);
});

test('the real boot step restores a patched carplay copy and aapd copy in about a second, byte for byte, / read-only', () => {
    if (!fs.existsSync(CARPLAY_BIN) || !fs.existsSync(AAPD_BIN) || !fs.existsSync(AAPD_CONF)) { console.log('     (skipped: CARPLAY_BIN, AAPD_BIN, AAPD_CONF not set)'); return; }
    const w = makeWorld();
    const cpt = w.T.carplay.target;
    fs.writeFileSync(cpt, unprovenCarplay(fs.readFileSync(CARPLAY_BIN)));
    fs.chmodSync(cpt, 0o755);
    const unpatched = fs.readFileSync(cpt);
    cp.execFileSync(process.execPath, [CARPLAY_PATCH, 'apply', '--panel', 'high', '--unproven', '--yes', '--target', cpt], { stdio: 'ignore' });
    aapdPatcher(w)(['apply', '--dpi', '170', '--yes']);
    assert(!fs.readFileSync(w.T.aapd.target).equals(fs.readFileSync(AAPD_BIN)) && !fs.readFileSync(w.T.aapd.files[1]).equals(fs.readFileSync(AAPD_CONF)), 'both patched');

    const armed = runBootStep(w, 'carplay', 'A');
    assert(/carplay boot check: armed \(unproven patch, 0 failed\)/.test(armed.stdout), armed.stdout);
    END.crashAtStart(w, 'A', 'carplay');
    const r = runBootStep(w, 'carplay', 'B');
    assert(/carplay boot check: restored/.test(r.stdout), r.stdout + r.stderr);
    assert(fs.readFileSync(cpt).equals(unpatched), 'the original, byte for byte');
    assert(ro());
    console.log(`     (carplay restore: boot step ${r.ms} ms in all, ${r.own} ms after node started)`);
    assert(r.own < 2000, r.stdout);

    fs.writeFileSync(w.T.aapd.state, JSON.stringify({ fails: 2, unknown: 0, arm: null }));
    const a = runBootStep(w, 'aapd', 'B');
    assert(/aapd boot check: restored/.test(a.stdout), a.stdout + a.stderr);
    assert(fs.readFileSync(w.T.aapd.target).equals(fs.readFileSync(AAPD_BIN)) && fs.readFileSync(w.T.aapd.files[1]).equals(fs.readFileSync(AAPD_CONF)));
    console.log(`     (aapd restore: boot step ${a.ms} ms in all, ${a.own} ms after node started)`);
    assert(a.own < 2000, a.stdout);
});

test('the boot step restores what an older release patched where the locator now refuses a site', () => {
    if (!fs.existsSync(CARPLAY_BIN) || sha(fs.readFileSync(CARPLAY_BIN)) !== 'ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50') {
        console.log('     (skipped: carplay 22.28.3 not found)'); return;
    }
    require('./carplay_mutations.js')(fs.readFileSync(CARPLAY_BIN)).oldRelease().forEach((f) => {
        const w = makeWorld();
        fs.writeFileSync(w.T.carplay.target, f.patched);
        fs.chmodSync(w.T.carplay.target, 0o755);
        backup(w.T.carplay.target, f.original);
        fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, unknown: 0, arm: { boot: 'A', patch: 'unknown', crashes: 1 } }));
        const r = runBootStep(w, 'carplay', 'B');
        assert(/carplay boot check: restored/.test(r.stdout), f.name + ': ' + r.stdout + r.stderr);
        assert(fs.readFileSync(w.T.carplay.target).equals(f.original), f.name + ': the original');
        assert(ro(), f.name);
    });
});

test('a service systemd started in that boot ran, even when it died before the watcher looked: a crash restart counts', async () => {
    const w = makeWorld();
    w.install('aapd', 'proven');
    await startBoot(w.recovery('aapd', 'A'));   /* armed; aapd starts, is SIGKILLed, recmgr restarts the unit */
    nsmShutdown(w, 'A', { carplay: 'running', aapd: 'killed' }, true);
    assert(/^A stopped crash ran up \d+\n$/.test(fs.readFileSync(w.T.aapd.mark, 'utf8')), fs.readFileSync(w.T.aapd.mark, 'utf8'));
    await startBoot(w.recovery('aapd', 'B'));
    assert.strictEqual(w.state('aapd').fails, 1);
});

/* ---- a head unit that refuses our parts ---- */

test('a refused head unit: a patch of ours is restored before its service starts, whatever the count, no patcher run', async () => {
    for (const [name, kind] of [['carplay', 'unproven'], ['carplay', 'proven'], ['carplay', 'unknown'], ['aapd', 'proven']]) {
        const w = makeWorld();
        w.install(name, kind);
        fs.writeFileSync(w.T[name].state, JSON.stringify({ fails: 0, unknown: 0, arm: { boot: 'A', at: Date.now(), patch: kind, crashes: 0, ran: true, confirmed: true, clean: false } }));
        headUnit.set(headUnit.NONE);
        try {
            const { steps } = await bootUp(w, 'B', [name]);
            assert.strictEqual(steps[name].action, 'restored', name + ' ' + kind);
        } finally { headUnit.set(headUnit.HIGH); }
        assert(w.logs.some((l) => l === headUnit.REFUSED + '; restoring the original ' + name), w.logs.join('\n'));
        assert.deepStrictEqual(w.events.slice(-1), [name + '-start:original'], 'restored before it starts');
        assert.deepStrictEqual([w.state(name).fails, w.state(name).arm], [0, null], 'nothing armed');
        assert.strictEqual(record.current(w.T[name].target).by, 'refused', 'not a restore after errors');
    }
});

test('a refused head unit: the original, no backup or an untrusted backup leave the binary and / alone; a deferred restore keeps the count', async () => {
    for (const how of ['original', 'none', 'untrusted']) {
        const w = makeWorld();
        w.install('carplay', how === 'untrusted' ? 'unproven' : how);
        if (how === 'untrusted') fs.writeFileSync(w.T.carplay.target + '.woc-orig.json', JSON.stringify({ sha256_original: '0'.repeat(64) }));
        const before = fs.readFileSync(w.T.carplay.target);
        fs.writeFileSync(MOUNT_LOG, '');
        headUnit.set(headUnit.NONE);
        let r;
        try { r = await startBoot(w.recovery('carplay', 'B', { restore: (f, cb) => recovery.restoreTarget(w.T.carplay, true, f, cb, 1000) })); } finally { headUnit.set(headUnit.HIGH); }
        assert.strictEqual(r.action, how === 'untrusted' ? 'restore-failed' : 'idle', how);
        assert(fs.readFileSync(w.T.carplay.target).equals(before), how + ': the binary as it was');
        assert.strictEqual(fs.readFileSync(MOUNT_LOG, 'utf8'), '', how + ': no remount');
    }
    const lock = require('../remount_lock.js');
    const w = makeWorld();
    w.install('carplay', 'proven');
    fs.writeFileSync(w.T.carplay.state, JSON.stringify({ fails: 1, unknown: 0, arm: null }));
    assert(lock.tryAcquire());
    headUnit.set(headUnit.NONE);
    try {
        const r = await startBoot(w.recovery('carplay', 'B', { restore: (f, cb) => recovery.restoreTarget(w.T.carplay, true, f, cb) }));
        assert.deepStrictEqual([r.action, w.state('carplay').fails], ['restore-failed', 1]);
    } finally { headUnit.set(headUnit.HIGH); lock.release(); }
});

test('a refused head unit: aapd original but aapd.conf still patched is restored too', async () => {
    const w = makeWorld();
    w.install('aapd', 'original');
    fs.writeFileSync(w.T.aapd.files[1], CONF.patched);
    headUnit.set(headUnit.NONE);
    let r;
    try { r = await startBoot(w.recovery('aapd', 'B', { restore: (f, cb) => recovery.restoreTarget(w.T.aapd, true, f, cb, 1000) })); } finally { headUnit.set(headUnit.HIGH); }
    assert.strictEqual(r.action, 'restored');
    assert(fs.readFileSync(w.T.aapd.files[1]).equals(CONF.original));
    assert(ro());
});

test('the boot step restores a patched carplay on a unit whose variant it cannot read and leaves it armed on HIGH, HIGH F and MID', () => {
    for (const [unit, restored] of [[headUnit.NONE, true], [headUnit.HIGH, false], [headUnit.HIGHF, false], [headUnit.MID, false]]) {
        const w = makeWorld();
        w.install('carplay', 'unproven');
        headUnit.set(unit);
        let r;
        try { r = runBootStep(w, 'carplay', 'B'); } finally { headUnit.set(headUnit.HIGH); }
        assert.strictEqual(r.status, 0, r.stdout + r.stderr);
        if (restored) {
            assert(r.stdout.indexOf(headUnit.REFUSED + '; restoring the original carplay') >= 0 && /boot check: restored/.test(r.stdout), r.stdout);
            assert.strictEqual(w.holds('carplay'), 'original');
            assert(ro());
        } else {
            assert(/boot check: armed/.test(r.stdout), r.stdout);
            assert.strictEqual(w.holds('carplay'), 'patched');
        }
    }
});

/* ---- the remount lock and the watcher service ---- */

test('remount lock: mutual exclusion and stale break', () => {
    const lockPath = path.join(ROOT, 'stale.lock');
    const env = { WOC_REMOUNT_LOCK: process.env.WOC_REMOUNT_LOCK, WOC_REMOUNT_LOCK_STALE_MS: process.env.WOC_REMOUNT_LOCK_STALE_MS };
    process.env.WOC_REMOUNT_LOCK = lockPath;
    process.env.WOC_REMOUNT_LOCK_STALE_MS = '50';
    const key = require.resolve('../remount_lock.js'), cached = require.cache[key];
    delete require.cache[key];
    try {
        const lock = require('../remount_lock.js');
        assert.strictEqual(lock.tryAcquire(), true, 'first acquire');
        assert.strictEqual(lock.tryAcquire(), false, 'held: second acquire fails');
        lock.release();
        assert.strictEqual(lock.tryAcquire(), true, 'reacquire after release');
        fs.writeFileSync(lockPath, '999999 someone\n');
        lock.release();
        assert(fs.existsSync(lockPath), 'another holder\'s lock is not released');
        const old = Date.now() / 1000 - 10;
        fs.utimesSync(lockPath, old, old);
        assert.strictEqual(lock.tryAcquire(), true, 'stale lock broken');
        lock.release();
    } finally {
        require.cache[key] = cached;
        Object.keys(env).forEach((k) => { if (env[k] === undefined) delete process.env[k]; else process.env[k] = env[k]; });
    }
});

test('the watcher service stays up without a patch to watch, and stops on SIGTERM', async () => {
    const w = makeWorld();
    w.install('carplay', 'none');
    fs.writeFileSync(path.join(w.dir, 'boot_id'), 'boot-idle\n');
    const calls = path.join(w.dir, 'calls'), sc = path.join(w.dir, 'sc');
    fs.writeFileSync(sc, '#!/bin/sh\necho "$*" >> "' + calls + '"\n', { mode: 0o755 });
    const child = cp.spawn(process.execPath, [RECOVERY_PATH], {
        env: Object.assign({}, process.env, { WOC_CARPLAY: w.T.carplay.target, WOC_AAPD: w.T.aapd.target, WOC_RECOVERY_STATE: w.T.carplay.state,
            WOC_BOOT_ID: path.join(w.dir, 'boot_id'), WOC_SYSTEMCTL: sc, WOC_RECOVERY_TICK_MS: '50' }),
        stdio: 'ignore',
    });
    try {
        let exit = null;
        const exited = new Promise((r) => child.on('exit', (code, signal) => { exit = { code, signal }; r(); }));
        const probed = () => fs.existsSync(calls) && /^show carplay\.service/m.test(fs.readFileSync(calls, 'utf8'));
        for (let t = Date.now(); !probed() && exit === null && Date.now() - t < 3000;) await new Promise((r) => setTimeout(r, 20));
        assert(probed(), 'the first tick ran: ' + JSON.stringify(exit));
        await new Promise((r) => setTimeout(r, 300));
        assert.strictEqual(exit, null, 'still running after its ticks');
        child.kill('SIGTERM');
        await Promise.race([exited, new Promise((r) => setTimeout(r, 2000))]);
        assert(exit && exit.code === 0, 'a SIGTERM ends it cleanly: ' + JSON.stringify(exit));
    } finally { if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL'); }
});

/* TEST_ONLY=<part of a name> runs only the tests it names. */
const run = tests.filter((t) => !process.env.TEST_ONLY || t.name.indexOf(process.env.TEST_ONLY) >= 0);

async function main() {
    let failed = 0;
    try {
        for (const t of run) {
            try { await t.fn(); console.log(`ok   ${t.name}`); }
            catch (err) { failed++; console.log(`FAIL ${t.name}\n     ${err && err.stack}`); }
        }
    } finally { cp.execFileSync('rm', ['-rf', ROOT]); }
    console.log(`${run.length - failed}/${run.length} passed`);
    process.exit(failed ? 1 : 0);
}

main();
