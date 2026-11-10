#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * The release tooling's tool fields: a tool's component.json `requires`,
 * `mguHmi` and `uninstallable` reach its catalog entry, which the catalog
 * check accepts; its
 * artifact's component.json keeps what the deploy tool reads (`activate`,
 * `deactivate`, `mguHmi`, `keepRunning`, ...) and leaves `requires` and
 * `uninstallable` to the catalog, as mgu-store's `deploy/hu artifact`; a malformed
 * `requires` or `mguHmi` is refused; the build tree takes each submodule at
 * the commit HEAD records, and refuses one its checkout lacks; a unit's
 * `runsInSafeMode` reaches the artifact; and this
 * repository's Waze chain is
 * declared: the app (for iDrive 7) requires waze-on-cluster, which requires
 * cluster-render and a deploy tool that runs install hooks, and has both
 * hooks; the app, the tool and the renderer are removable. Titles and
 * descriptions in other languages: release/texts/<code>.json by id into
 * each entry's `texts`, as mgu-store's updater reads them, in any key
 * order; a file naming an id that is not a component, or anything but a
 * title and a description, is refused; a
 * published entry carried over takes its component's texts. Versions: the
 * release gives each component changed since its <id>@<version> tag the next
 * version (a patch; minor or major by its pull requests' labels), once for
 * any number of changes, keeps a version not released yet, resolves
 * "requires" of "next" and turns the CHANGELOG's Unreleased section into the
 * version's; an app's staged tree has no version in its folder name.
 * Icons: an app's catalog entry names <id>.png with the sha256 of the PNG
 * its entry point shows; a tool's names none.
 *
 *   node release/test/release_test.js
 */
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const cp = require('child_process');
const { buildArtifact } = require('../tool_artifact.js');
const { components, catalogEntry, catalogTexts, retext, exportHead, bumpVersion, releaseChangelog, plan, writePlan,
    prLabels } = require('../release.js');
const catalog = require('../catalog.js');

/* The members of a tgz the packer wrote (ustar), by name. */
function untgz(file) {
    const tar = zlib.gunzipSync(fs.readFileSync(file));
    const out = {};
    for (let off = 0; off + 512 <= tar.length;) {
        const name = tar.toString('utf8', off, off + 100).replace(/\0.*$/, '');
        if (!name) {
            break;
        }
        const size = parseInt(tar.toString('utf8', off + 124, off + 136).replace(/\0.*$/, '').trim(), 8);
        out[name] = tar.slice(off + 512, off + 512 + size);
        off += 512 + Math.ceil(size / 512) * 512;
    }
    return out;
}

const tests = [];
function test(name, fn) {
    tests.push({ name, fn });
}

function fixtureTool(root, c) {
    const dir = path.join(root, 'native', c.id);
    fs.mkdirSync(path.join(dir, 'systemd'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'hook.js'), 'console.log("{}");\n');
    fs.writeFileSync(path.join(dir, 'systemd', `${c.id}.service`), '[Service]\nExecStart=/bin/true\n');
    fs.writeFileSync(path.join(dir, 'component.json'), JSON.stringify(c, null, 2));
    return dir;
}

const FIXTURE = {
    id: 'fixture-tool',
    kind: 'tool',
    version: '1.2.3',
    title: 'Fixture',
    requires: { 'cluster-render': '0.1.0', 'mgu-deploy': '0.7.0' },
    mguHmi: ['ID7'],
    uninstallable: true,
    keepRunning: true,
    activate: ['node', 'hook.js', 'activate'],
    deactivate: ['node', 'hook.js', 'deactivate'],
    files: [{ path: 'hook.js', mode: '0644' }],
    units: [{ name: 'fixture-tool.service', from: 'systemd/fixture-tool.service' }],
    restart: ['fixture-tool.service']
};

test('a tool artifact keeps the fields the deploy tool reads', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    const dir = fixtureTool(root, FIXTURE);
    const out = buildArtifact(dir, path.join(root, 'out'));
    const m = JSON.parse(untgz(out)['component.json'].toString('utf8'));
    assert.deepStrictEqual(m.activate, FIXTURE.activate);
    assert.deepStrictEqual(m.deactivate, FIXTURE.deactivate);
    assert.deepStrictEqual(m.mguHmi, ['ID7']);
    assert.strictEqual(m.keepRunning, true);
    assert.strictEqual(m.requires, undefined, 'the catalog\'s');
    assert.strictEqual(m.uninstallable, undefined, 'the catalog\'s');
    assert.strictEqual(m.mguProvides, undefined, 'only fields the tool sets');
    assert.deepStrictEqual(m.files.map((f) => f.path), ['hook.js']);
});

test('a tool artifact keeps runsInSafeMode on the units that set it, and nothing on the others', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    const dir = fixtureTool(root, Object.assign({}, FIXTURE, { units: [
        { name: 'fixture-tool.service', from: 'systemd/fixture-tool.service' },
        { name: 'fixture-tool-boot.service', from: 'systemd/fixture-tool.service', runsInSafeMode: true }] }));
    const m = JSON.parse(untgz(buildArtifact(dir, path.join(root, 'out')))['component.json'].toString('utf8'));
    assert.deepStrictEqual(m.units, [{ name: 'fixture-tool.service' }, { name: 'fixture-tool-boot.service', runsInSafeMode: true }]);
});

test('waze-on-cluster\'s units that put CarPlay and aapd back run in safe mode; the rest do not', () => {
    const c = JSON.parse(fs.readFileSync(path.join(__dirname, '..', '..', 'native', 'waze-on-cluster', 'component.json'), 'utf8'));
    assert.deepStrictEqual(c.units.filter((u) => u.runsInSafeMode === true).map((u) => u.name),
        ['waze-on-cluster-boot.service', 'waze-on-cluster-aa-boot.service', 'waze-on-cluster-recovery.service']);
    assert.ok(c.units.every((u) => u.runsInSafeMode === undefined || u.runsInSafeMode === true));
});

test('a tool catalog entry carries requires, mguHmi and uninstallable; the catalog check accepts it', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    fs.mkdirSync(path.join(root, 'apps'));
    fixtureTool(root, FIXTURE);
    const c = components(root).find((x) => x.id === 'fixture-tool');
    const artifact = buildArtifact(path.join(root, 'native', 'fixture-tool'), path.join(root, 'out'));
    const entry = catalogEntry(c, artifact);
    assert.deepStrictEqual(entry.requires, FIXTURE.requires);
    assert.deepStrictEqual(entry.mguHmi, ['ID7']);
    assert.strictEqual(entry.uninstallable, true);
    const parsed = catalog.parse(JSON.stringify({ schema: 2, components: [entry] }));
    assert.deepStrictEqual(parsed.components[0].requires, FIXTURE.requires);
});

test('a malformed requires, mguHmi or uninstallable is refused', () => {
    [{ requires: { 'cluster-render': '0.1' } }, { requires: ['cluster-render'] }, { mguHmi: ['ID9'] }, { mguHmi: [] }, { uninstallable: 'yes' }]
        .forEach((bad) => {
            const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
            fs.mkdirSync(path.join(root, 'apps'));
            fixtureTool(root, Object.assign({}, FIXTURE, bad));
            assert.throws(() => components(root), /requires|mguHmi|uninstallable/, JSON.stringify(bad));
        });
});

test('the build tree holds a submodule at its recorded commit; one its checkout lacks is an error', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    const g = (cwd, args) => cp.execFileSync('git', ['-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
        .concat(args), { cwd, encoding: 'utf8' }).trim();
    const sub = path.join(root, 'sub');
    fs.mkdirSync(sub);
    g(sub, ['init', '-q']);
    fs.writeFileSync(path.join(sub, 'lib.txt'), 'one\n');
    g(sub, ['add', 'lib.txt']);
    g(sub, ['commit', '-q', '-m', 'one']);
    const one = g(sub, ['rev-parse', 'HEAD']);
    const main = path.join(root, 'main');
    fs.mkdirSync(main);
    g(main, ['init', '-q']);
    fs.writeFileSync(path.join(main, 'a.txt'), 'a\n');
    g(main, ['add', 'a.txt']);
    g(main, ['update-index', '--add', '--cacheinfo', `160000,${one},third_party/lib`]);
    g(main, ['commit', '-q', '-m', 'main']);
    fs.mkdirSync(path.join(main, 'third_party'));
    fs.renameSync(sub, path.join(main, 'third_party', 'lib'));
    // the checkout moves on: HEAD's commit is what is built
    fs.writeFileSync(path.join(main, 'third_party', 'lib', 'lib.txt'), 'two\n');
    g(path.join(main, 'third_party', 'lib'), ['commit', '-q', '-am', 'two']);
    const dir = exportHead(main);
    assert.strictEqual(fs.readFileSync(path.join(dir, 'a.txt'), 'utf8'), 'a\n');
    assert.strictEqual(fs.readFileSync(path.join(dir, 'third_party', 'lib', 'lib.txt'), 'utf8'), 'one\n');
    g(main, ['update-index', '--cacheinfo', `160000,${'1'.repeat(40)},third_party/lib`]);
    g(main, ['commit', '-q', '-m', 'a commit the checkout lacks']);
    assert.throws(() => exportHead(main), /git archive 1{40} in third_party\/lib failed/);
});

test('the Waze chain: app -> waze-on-cluster -> cluster-render and a hook-running deploy tool', () => {
    const all = components();
    const byId = (id) => all.find((c) => c.id === id);
    const app = byId('robertn_waze_cluster');
    const tool = byId('waze-on-cluster');
    const renderer = byId('cluster-render');
    assert.ok(app && tool && renderer, 'all three are releasable');
    assert.deepStrictEqual(app.mguHmi, ['ID7']);
    assert.deepStrictEqual(Object.keys(app.requires), ['waze-on-cluster']);
    assert.ok(catalog.compareVersions(tool.version, app.requires['waze-on-cluster']) >= 0,
        'the tool meets the app\'s requirement');
    assert.deepStrictEqual(Object.keys(tool.requires), ['mgu-deploy', 'cluster-render'], 'the deploy tool installs first');
    assert.strictEqual(tool.requires['mgu-deploy'], '0.7.11', 'hooks, and recmgr and crashNames names on the lists');
    const needs = tool.requires['cluster-render'];
    assert.ok(needs === 'next' || (catalog.SEMVER.test(needs) && catalog.compareVersions(renderer.version, needs) >= 0),
        'the renderer released beside it meets the tool\'s minimum, or is the one released with it');
    assert.deepStrictEqual(renderer.requires, { 'mgu-deploy': '0.4.0' });
    assert.deepStrictEqual(tool.mguHmi, ['ID7']);
    const c = JSON.parse(fs.readFileSync(path.join(__dirname, '..', '..', 'native', 'waze-on-cluster', 'component.json'), 'utf8'));
    assert.deepStrictEqual(c.activate, ['node', 'patchd.js', 'activate']);
    assert.deepStrictEqual(c.deactivate, ['node', 'patchd.js', 'deactivate']);
    assert.ok(c.files.some((f) => f.path === 'patchd.js'), 'the hook ships');
    // recmgr: a CarPlay crash and the boot step's timeout do not reboot the unit;
    // neither is started or restarted by the deploy tool
    assert.ok(c.recmgr.includes('carplay') && c.recmgr.includes('waze-on-cluster-boot'));
    assert.ok(!c.restart.some((u) => /^(carplay|waze-on-cluster-boot)\.service$/.test(u)));
    // the shutdown record's drop-in and carplay's, removed with the tool;
    // patchd restarts carplay only once systemd loaded the latter where the
    // deploy tool writes it
    assert.deepStrictEqual(c.dropins.map((d) => [d.unit, d.name]),
        [['var-opt-ro.service', '50-waze-on-cluster.conf'], ['carplay.service', '50-waze-on-cluster.conf']]);
    const woc = path.join(__dirname, '..', '..', 'native', 'waze-on-cluster');
    c.dropins.forEach((d) => assert.ok(fs.existsSync(path.join(woc, d.from)), d.from));
    assert.ok(/^SuccessExitStatus=SIGKILL$/m.test(fs.readFileSync(path.join(woc, c.dropins[1].from), 'utf8')));
    assert.ok(fs.readFileSync(path.join(woc, 'patch', 'patchd.js'), 'utf8')
        .includes(`'/etc/systemd/system/${c.dropins[1].unit}.d/${c.dropins[1].name}'`));
    assert.deepStrictEqual(renderer.mguHmi, ['ID7', 'ID8'], 'the renderer serves iDrive 8 too');
    // the Store removes the app, then the tool, then the renderer
    assert.strictEqual(app.uninstallable, true);
    assert.strictEqual(tool.uninstallable, true);
    assert.strictEqual(renderer.uninstallable, true);
});

function fixtureTexts(files) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    fs.mkdirSync(path.join(root, 'apps'));
    fixtureTool(root, FIXTURE);
    fs.mkdirSync(path.join(root, 'release', 'texts'), { recursive: true });
    Object.keys(files).forEach((n) => fs.writeFileSync(path.join(root, 'release', 'texts', n), JSON.stringify(files[n])));
    return root;
}

test('texts: release/texts/<code>.json by id into the catalog entry, which the catalog check accepts', () => {
    const root = fixtureTexts({
        'de.json': { 'fixture-tool': { title: 'Vorrichtung', description: 'Eine Vorrichtung' } },
        'zh-TW.json': { 'fixture-tool': { title: '夾具' } },
        'fr.json': { 'fixture-tool': { description: 'Un outil', title: 'Outil' } },
        'pt-BR.json': {}
    });
    const c = components(root).find((x) => x.id === 'fixture-tool');
    const want = { de: { title: 'Vorrichtung', description: 'Eine Vorrichtung' }, fr: { title: 'Outil', description: 'Un outil' },
        'zh-TW': { title: '夾具' } };
    assert.deepStrictEqual(c.texts, want);
    const entry = catalogEntry(c, buildArtifact(path.join(root, 'native', 'fixture-tool'), path.join(root, 'out')));
    assert.deepStrictEqual(entry.texts, want);
    assert.deepStrictEqual(catalog.textsOf(entry.texts), want, 'as the updater reads it');
    assert.deepStrictEqual(catalog.parse(JSON.stringify({ schema: 2, components: [entry] })).components[0].texts, want);
    // without release/texts: no texts
    const bare = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    fs.mkdirSync(path.join(bare, 'apps'));
    fixtureTool(bare, FIXTURE);
    const plain = components(bare).find((x) => x.id === 'fixture-tool');
    assert.strictEqual(plain.texts, undefined);
    assert.strictEqual(catalogEntry(plain, buildArtifact(path.join(bare, 'native', 'fixture-tool'), path.join(bare, 'out'))).texts,
        undefined, 'no texts field');
});

test('texts: a file naming an id that is not a component, a bad code or anything but a title and a description is refused', () => {
    [['de.json', { 'no-such-tool': { title: 'X' } }],
        ['de.json', { 'fixture-tool': { title: '' } }],
        ['de.json', { 'fixture-tool': { title: 'X', subtitle: 'Y' } }],
        ['de.json', { 'fixture-tool': { title: '  X  ' } }],
        ['de.json', { 'fixture-tool': { title: 7 } }],
        ['de.json', { 'fixture-tool': 'X' }],
        ['de.json', { 'fixture-tool': { title: 'x'.repeat(81) } }],
        ['German.json', { 'fixture-tool': { title: 'X' } }],
        ['de-DE-x.json', { 'fixture-tool': { title: 'X' } }]].forEach(([n, o]) => {
        const root = fixtureTexts({ [n]: o });
        assert.throws(() => catalogTexts(['fixture-tool'], root), new RegExp(`release/texts/${n.replace(/\./g, '\\.')}`), `${n} ${JSON.stringify(o)}`);
    });
});

test('texts: a published entry carried over takes its component\'s texts, and loses them with the file', () => {
    const de = { de: { title: 'Vorrichtung' } };
    const entries = [{ id: 'a', version: '1.0.0' }, { id: 'b', version: '1.0.0', texts: de }, { id: 'c', version: '1.0.0', texts: de },
        { id: 'd', version: '1.0.0', texts: de }];
    const all = [{ id: 'a', texts: de }, { id: 'b', texts: de }, { id: 'c', texts: { de: { title: 'Neu' } } }, { id: 'd' }];
    const changed = retext(entries, all);
    assert.deepStrictEqual(changed.map((e) => e.id), ['a', 'c', 'd'], 'added, changed, removed; the same texts left alone');
    assert.deepStrictEqual(entries.map((e) => e.texts), [de, de, { de: { title: 'Neu' } }, undefined]);
    assert.ok(!Object.prototype.hasOwnProperty.call(entries[3], 'texts'), 'no texts field left behind');
    assert.deepStrictEqual(retext(entries, all), [], 'once listed: nothing to change');
});

test('this repository\'s release/texts name its components, every language each', () => {
    const all = components();
    const codes = fs.readdirSync(path.join(__dirname, '..', 'texts')).filter((n) => /\.json$/.test(n)).map((n) => n.slice(0, -5)).sort();
    assert.ok(codes.length > 0, 'release/texts has languages');
    all.forEach((c) => assert.deepStrictEqual(Object.keys(c.texts || {}).sort(), codes, c.id));
});

/* A git repository with tools a (1.0.0, with a CHANGELOG) and b (2.0.0,
 * requiring a 1.0.0), each released (tagged). */
function versionRepo() {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-ver-'));
    fs.mkdirSync(path.join(root, 'apps'));
    const g = (args) => cp.execFileSync('git', ['-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
        .concat(args), { cwd: root, encoding: 'utf8' }).trim();
    const a = fixtureTool(root, Object.assign({}, FIXTURE, { id: 'a', version: '1.0.0', requires: undefined }));
    fs.writeFileSync(path.join(a, 'CHANGELOG.md'), '# Changelog\n\n## 1.0.0\n\nFirst.\n');
    fixtureTool(root, Object.assign({}, FIXTURE, { id: 'b', version: '2.0.0', requires: { a: '1.0.0' } }));
    g(['init', '-q']);
    g(['add', '.']);
    g(['commit', '-q', '-m', 'one']);
    g(['tag', 'a@1.0.0']);
    g(['tag', 'b@2.0.0']);
    const change = (rel, text, log) => {
        fs.writeFileSync(path.join(root, rel), text);
        if (log) {
            const file = path.join(root, 'native', 'a', 'CHANGELOG.md');
            const t = fs.readFileSync(file, 'utf8');
            const u = t.includes('## Unreleased\n') ? t.replace('## Unreleased\n\n', `## Unreleased\n\n${log}\n`)
                : t.replace('# Changelog\n\n', `# Changelog\n\n## Unreleased\n\n${log}\n\n`);
            fs.writeFileSync(file, u);
        }
        g(['add', '.']);
        g(['commit', '-q', '-m', rel]);
        return g(['rev-parse', 'HEAD']);
    };
    const stamp = (ids) => {
        const steps = plan(components(root), ids || [], root);
        writePlan(root, steps);
        if (steps.length) {
            g(['commit', '-q', '-am', 'release']);
        }
        steps.forEach((p) => g(['tag', `${p.c.id}@${p.version}`]));
        return steps.map((p) => `${p.c.id}@${p.version}`);
    };
    const json = (id) => JSON.parse(fs.readFileSync(path.join(root, 'native', id, 'component.json'), 'utf8'));
    return { root, change, stamp, json, log: () => fs.readFileSync(path.join(root, 'native', 'a', 'CHANGELOG.md'), 'utf8') };
}

test('versions: the next version, a patch or what a label asks for', () => {
    assert.strictEqual(bumpVersion('0.6.2', 'patch'), '0.6.3');
    assert.strictEqual(bumpVersion('0.6.2', 'minor'), '0.7.0');
    assert.strictEqual(bumpVersion('0.6.2', 'major'), '1.0.0');
    process.env.MGU_RELEASE_LABELS = 'off';
    const r = versionRepo();
    assert.deepStrictEqual(plan(components(r.root), [], r.root), [], 'nothing changed since the tags');
    r.change('native/a/hook.js', 'console.log(1);\n', 'Fix one.');
    assert.deepStrictEqual(r.stamp(), ['a@1.0.1'], 'a patch; b unchanged');
    assert.strictEqual(r.json('a').version, '1.0.1');
    assert.ok(r.log().startsWith('# Changelog\n\n## Unreleased\n\n## 1.0.1\n\nFix one.\n\n## 1.0.0\n'), r.log());
    const sha = r.change('native/a/hook.js', 'console.log(2);\n', 'New thing.');
    prLabels.set(sha, ['enhancement', 'release:minor']);
    assert.deepStrictEqual(r.stamp(), ['a@1.1.0'], 'release:minor');
    const major = r.change('native/a/hook.js', 'console.log(3);\n', 'Breaks.');
    prLabels.set(major, ['release:minor', 'release:major']);
    assert.deepStrictEqual(r.stamp(), ['a@2.0.0'], 'the highest label wins');
});

test('versions: two changes in a row give one version, with both CHANGELOG entries', () => {
    const r = versionRepo();
    r.change('native/a/hook.js', 'console.log(1);\n', 'First fix.');
    r.change('native/a/hook.js', 'console.log(2);\n', 'Second fix.');
    assert.deepStrictEqual(r.stamp(), ['a@1.0.1']);
    assert.ok(r.log().includes('## 1.0.1\n\nSecond fix.\nFirst fix.\n\n## 1.0.0'), r.log());
    assert.deepStrictEqual(r.stamp(), [], 'released: nothing left');
});

test('versions: "requires" of "next" is the version released with it, or the one released before', () => {
    const r = versionRepo();
    r.change('native/a/hook.js', 'console.log(1);\n', 'Fix.');
    r.change('native/b/component.json', JSON.stringify(Object.assign(r.json('b'), { requires: { a: 'next' } }), null, 2));
    assert.deepStrictEqual(r.stamp(), ['a@1.0.1', 'b@2.0.1']);
    assert.deepStrictEqual(r.json('b').requires, { a: '1.0.1' });
    // a released first, on its own: b's "next" is a's version
    r.change('native/a/hook.js', 'console.log(2);\n', 'Fix again.');
    assert.deepStrictEqual(r.stamp(['a']), ['a@1.0.2']);
    r.change('native/b/component.json', JSON.stringify(Object.assign(r.json('b'), { requires: { a: 'next' } }), null, 2));
    assert.deepStrictEqual(r.stamp(['b']), ['b@2.0.2']);
    assert.deepStrictEqual(r.json('b').requires, { a: '1.0.2' });
    // b chosen alone brings a's unreleased change along
    r.change('native/a/hook.js', 'console.log(3);\n', 'Fix a third time.');
    r.change('native/b/component.json', JSON.stringify(Object.assign(r.json('b'), { requires: { a: 'next' } }), null, 2));
    assert.deepStrictEqual(r.stamp(['b']), ['a@1.0.3', 'b@2.0.3']);
    assert.deepStrictEqual(r.json('b').requires, { a: '1.0.3' });
});

test('versions: a version stamped and not released keeps its number; later entries join its section', () => {
    const r = versionRepo();
    r.change('native/a/hook.js', 'console.log(1);\n', 'Fix.');
    const steps = plan(components(r.root), [], r.root);
    writePlan(r.root, steps);   // stamped, never tagged: the publish did not happen
    r.change('native/a/hook.js', 'console.log(2);\n', 'Another fix.');
    assert.deepStrictEqual(r.stamp(), ['a@1.0.1'], 'no second bump');
    assert.ok(r.log().startsWith('# Changelog\n\n## Unreleased\n\n## 1.0.1\n\nAnother fix.\n\nFix.\n\n## 1.0.0\n'), r.log());
    assert.strictEqual(releaseChangelog('# C\n\n## 1.0.0\n\nx\n', '1.0.1'), null, 'no Unreleased section');
    assert.strictEqual(releaseChangelog('# C\n\n## Unreleased\n\n## 1.0.0\n\nx\n', '1.0.1'), null, 'an empty one');
    // two pull requests that each added the heading, merged with both sides' lines (merge=union)
    assert.strictEqual(releaseChangelog('# C\n\n## Unreleased\n\nOne.\n\n## Unreleased\n\nTwo.\n\n## 1.0.0\n\nx\n', '1.0.1'),
        '# C\n\n## Unreleased\n\n## 1.0.1\n\nOne.\n\nTwo.\n\n## 1.0.0\n\nx\n');
});

test('versions: a CHANGELOG without what changed stops the release; "next" names this repository only', () => {
    const r = versionRepo();
    r.change('native/a/hook.js', 'console.log(1);\n');
    assert.throws(() => writePlan(r.root, plan(components(r.root), [], r.root)), /Unreleased/);
    r.change('native/b/component.json', JSON.stringify(Object.assign(r.json('b'), { requires: { 'mgu-deploy': 'next' } }), null, 2));
    assert.throws(() => components(r.root), /names a component of this repository only/);
});

test('icons: the app\'s entry names <id>.png with the sha256 of the PNG its entry point shows; a tool\'s none', () => {
    const app = components().find((c) => c.id === 'robertn_waze_cluster');
    const png = path.join(__dirname, '..', '..', 'apps', 'robertn_waze_cluster', 'package', 'rhmiData', 'variantGroups', 'Common',
        'images', 'robertn_waze_cluster_icon.png');
    const sha = require('crypto').createHash('sha256').update(fs.readFileSync(png)).digest('hex');
    const out = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-icon-'));
    fs.writeFileSync(path.join(out, 'a.tgz'), 'x');
    const entry = catalogEntry(app, path.join(out, 'a.tgz'));
    assert.strictEqual(entry.icon, 'robertn_waze_cluster.png');
    assert.strictEqual(entry.iconSha256, sha);
    components().filter((c) => c.kind === 'tool').forEach((c) => assert.strictEqual(catalogEntry(c, path.join(out, 'a.tgz')).icon, undefined));
});

test('an app\'s staged tree has no version in its folder name', () => {
    const app = components().find((c) => c.kind === 'app');
    assert.ok(!app.tree.includes('@'), app.tree);
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-rel-test-'));
    const dir = path.join(root, 'apps', 'robertn_x');
    fs.mkdirSync(path.join(dir, 'robertn_x@1.0.0'), { recursive: true });
    fs.mkdirSync(path.join(root, 'native'));
    fs.writeFileSync(path.join(dir, 'release.json'), '{ "build": "true" }');
    fs.writeFileSync(path.join(dir, 'robertn_x@1.0.0', 'package.json'), '{ "name": "robertn_x", "version": "1.0.0", "mguLocalOnly": true }');
    fs.writeFileSync(path.join(dir, 'mgu-app.json'), '{ "schema": 1, "id": "robertn_x", "version": "1.0.0", "tree": "robertn_x@1.0.0" }');
    assert.throws(() => components(root), /without a version in its name/);
});

let failed = 0;
tests.forEach((t) => {
    try {
        t.fn();
        console.log(`ok   ${t.name}`);
    } catch (e) {
        failed++;
        console.log(`FAIL ${t.name}\n     ${e && e.stack}`);
    }
});
console.log(`${tests.length - failed}/${tests.length} passed`);
process.exitCode = failed ? 1 : 0;
