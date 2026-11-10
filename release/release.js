#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Versions, builds and publishes everything the head unit installs and
 * updates from the catalog: each app with an apps/<id>/release.json and each
 * root tool with a native/<id>/component.json. release/README.md describes
 * the versioning rules, the catalog and the publish target.
 *
 *   node release/release.js list                      components, versions, publish state
 *   node release/release.js build [<id>...]           reproducible build of HEAD into release/out/
 *   node release/release.js check [<id>...]           build twice, compare the artifacts
 *   node release/release.js versions <base>           a pull request's checks against its base commit
 *   node release/release.js stamp [<id>...] [--dry-run]
 *                                                     next versions into the tree, the release commit
 *   node release/release.js publish [<id>...|--stamped] [--dry-run]
 *                                                     upload new versions, then the catalog
 *
 * Builds run on a clean `git archive HEAD` export, never on the working tree,
 * with SOURCE_DATE_EPOCH set to the last commit that touched what the
 * component ships (an app's staged tree, a tool's folder): the same tree
 * always gives the same bytes, so one version has exactly one artifact.
 *
 * Versions are set by the release, never by a pull request: `stamp` gives
 * each component changed since its last release (its <id>@<version> tag) the
 * next version, writes it into the tree with the CHANGELOG's Unreleased
 * section under it and each "requires" of "next" resolved, and commits
 * that. A pull request's labels release:minor and release:major raise the
 * step from a patch.
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');

const REPO = path.resolve(__dirname, '..');
const OUT = path.join(__dirname, 'out');
const PACKER = path.join('release', 'tool_artifact.js');
const publishTarget = require('./publish');
const { compareVersions, textsOf, SEMVER } = require('./catalog.js');

function readJson(file) {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function isDir(p) {
    return fs.existsSync(p) && fs.statSync(p).isDirectory();
}

function sha256(file) {
    return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function git(args, opts = {}) {
    return cp.execFileSync('git', args, Object.assign({ cwd: REPO, encoding: 'utf8' }, opts)).trim();
}

/* The release title of an app is the en_GB text of its first entry point. */
function appTitle(stagedDir) {
    const app = readJson(path.join(stagedDir, 'rhmiData', 'application.json')).application;
    const textId = app.entryPoints && app.entryPoints[0] && app.entryPoints[0].textId;
    const strings = path.join(stagedDir, 'rhmiData', 'variantGroups', 'Common', 'texts', 'en_GB.json');
    if (textId && fs.existsSync(strings)) {
        const text = readJson(strings)[textId];
        if (text) {
            return text;
        }
    }
    return app.name;
}

/* An app's id is also the name provisioning knows it by, and provisioning's
 * own section for an id wins over the fallback. New app ids carry this
 * prefix. A repository's release/repo.json, when it has one, lists
 * "unprefixedAppIds" that predate the rule. */
const APP_ID_PREFIX = 'robertn_';
const APP_TREE = 'package';
const REPO_RULES = fs.existsSync(path.join(__dirname, 'repo.json')) ? readJson(path.join(__dirname, 'repo.json')) : {};
const UNPREFIXED_APP_IDS = REPO_RULES.unprefixedAppIds || [];

/* An app's release.json or a tool's component.json "requires" (catalog
 * schema 2): component ids, of any catalog the unit reads, to their minimum
 * x.y.z versions, or to "next": the version this repository's component of
 * that id gets at the next release, which `stamp` writes in. */
const NEXT = 'next';
function requiresOf(release, where) {
    if (release.requires === undefined) {
        return undefined;
    }
    const r = release.requires;
    if (!r || typeof r !== 'object' || Array.isArray(r) || !Object.keys(r).every((id) => SEMVER.test(r[id]) || r[id] === NEXT)) {
        throw new Error(`${where}: "requires" maps component ids to x.y.z versions or "${NEXT}"`);
    }
    return r;
}

/* "uninstallable" (an app's release.json, a tool's component.json): the
 * Store offers Remove. */
function uninstallableOf(c, where) {
    if (c.uninstallable !== undefined && c.uninstallable !== true) {
        throw new Error(`${where}: "uninstallable" is true or absent`);
    }
    return c.uninstallable;
}

/* A tool's component.json or an app's release.json "mguHmi": the head units
 * it is for. */
function hmiOf(c, where) {
    if (c.mguHmi === undefined) {
        return undefined;
    }
    const h = c.mguHmi;
    if (!Array.isArray(h) || !h.length || !h.every((x) => x === 'ID7' || x === 'ID8')) {
        throw new Error(`${where}: "mguHmi" lists "ID7" and/or "ID8"`);
    }
    return h;
}

/* An app's icon, the one the car's app list shows: the PNG its staged
 * tree's rhmiData/application.json names on its first entry point, in the
 * first variant group's resourcesPath that has it, else resources/, as
 * mgu-store's updater looks for it (native/mgu-updater/js/sources.js
 * entryIcons); { file, sha256 }, or undefined without a PNG of at most
 * 128 KiB, the most the Store draws. */
const ICON_BYTES = 128 << 10;
function appIcon(stagedDir) {
    const rhmi = path.join(stagedDir, 'rhmiData');
    const app = readJson(path.join(rhmi, 'application.json')).application;
    const name = app && Array.isArray(app.entryPoints) && app.entryPoints[0] && app.entryPoints[0].icon;
    if (typeof name !== 'string' || !/^[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}$/.test(name)) {
        return undefined;
    }
    const dirs = (Array.isArray(app.variantGroups) ? app.variantGroups : [])
        .map((g) => (g && typeof g.resourcesPath === 'string' ? g.resourcesPath.replace(/^\.\//, '').replace(/\/+$/, '') : ''))
        .filter((d) => d && d[0] !== '/' && d.split('/').every((x) => x && x !== '.' && x !== '..'))
        .concat(['resources']);
    const file = dirs.map((d) => path.join(rhmi, d, name)).find((f) => fs.existsSync(f) && fs.statSync(f).isFile());
    const b = file ? fs.readFileSync(file) : null;
    if (!b || b.length > ICON_BYTES || b.slice(0, 8).toString('hex') !== '89504e470d0a1a0a') {
        return undefined;
    }
    return { file, sha256: crypto.createHash('sha256').update(b).digest('hex') };
}

/* Every releasable component in a repo tree (the working tree or an export). */
function components(root = REPO) {
    const out = [];
    const apps = path.join(root, 'apps');
    fs.readdirSync(apps).sort().forEach((id) => {
        const dir = path.join(apps, id);
        const releaseFile = path.join(dir, 'release.json');
        if (!fs.existsSync(releaseFile)) {
            return;
        }
        // with mgu-app.json the artifact is the source package, built on the unit
        const manifestFile = path.join(dir, 'mgu-app.json');
        const manifest = fs.existsSync(manifestFile) ? readJson(manifestFile) : null;
        // the staged tree: the folder mgu-app.json names, else package/; the
        // build names it <id>@<version> (stageTree)
        const tree = manifest ? manifest.tree : APP_TREE;
        if (typeof tree !== 'string' || !/^[A-Za-z0-9._-]+$/.test(tree) || tree.indexOf('@') >= 0 || !isDir(path.join(dir, tree))) {
            throw new Error(`apps/${id}: the staged tree is a folder without a version in its name (mgu-app.json "tree", or ${APP_TREE}/)`);
        }
        const stagedDir = path.join(dir, tree);
        const pkg = readJson(path.join(stagedDir, 'package.json'));
        const version = pkg.version;
        if (!SEMVER.test(version || '') || pkg.name !== id) {
            throw new Error(`apps/${id}: ${tree}/package.json is ${pkg.name}@${pkg.version}, not ${id}@<x.y.z>`);
        }
        // the deploy tool 0.1.x refuses an app without it (mgu-store: native/app-privacy)
        if (pkg.mguLocalOnly !== true) {
            throw new Error(`apps/${id}: ${tree}/package.json lacks "mguLocalOnly": true`);
        }
        if (!id.startsWith(APP_ID_PREFIX) && UNPREFIXED_APP_IDS.indexOf(id) < 0) {
            throw new Error(`apps/${id}: a new app id starts with ${APP_ID_PREFIX} (release/README.md)`);
        }
        const release = readJson(releaseFile);
        if (manifest && (manifest.id !== id || manifest.version !== version)) {
            throw new Error(`apps/${id}/mgu-app.json names ${manifest.id}@${manifest.version}, not ${id}@${version} as ${tree}/package.json`);
        }
        out.push({
            id,
            kind: 'app',
            version,
            dir: `apps/${id}`,
            title: appTitle(stagedDir),
            description: pkg.description || '',
            icon: appIcon(stagedDir),
            release,
            build: manifest ? 'unit' : null,
            requires: requiresOf(release, `apps/${id}/release.json`),
            uninstallable: uninstallableOf(release, `apps/${id}/release.json`),
            mguHmi: hmiOf(release, `apps/${id}/release.json`),
            tree,
            manifest: !!manifest,
            changelog: `apps/${id}/${tree}/CHANGELOG.md`,
            shipped: [`apps/${id}/${tree}`].concat(manifest ?
                [`apps/${id}/mgu-app.json`].concat(manifest.modules.map((m) => `apps/${id}/${m}`)) : []),
            // what a release looks at beside it: the catalog entry's fields
            listed: [`apps/${id}/release.json`],
            artifact: `apps/${id}/${id}@${version}${manifest ? '.src' : ''}.tgz`
        });
    });
    const tool = (dir, id, shipped) => {
        const file = path.join(root, dir, 'component.json');
        if (!fs.existsSync(file)) {
            return;
        }
        const c = readJson(file);
        if (c.id !== id || c.kind !== 'tool' || !SEMVER.test(c.version || '')) {
            throw new Error(`${dir}/component.json: needs id "${id}", kind "tool" and a x.y.z version`);
        }
        out.push({
            id,
            kind: 'tool',
            version: c.version,
            dir,
            title: c.title || id,
            description: c.description || '',
            // source files that carry the version too, as VERSION = 'x.y.z'
            versionFiles: c.versionFiles || [],
            requires: requiresOf(c, `${dir}/component.json`),
            uninstallable: uninstallableOf(c, `${dir}/component.json`),
            mguHmi: hmiOf(c, `${dir}/component.json`),
            changelog: `${dir}/CHANGELOG.md`,
            shipped,
            listed: [],
            artifact: null
        });
    };
    fs.readdirSync(path.join(root, 'native')).sort().forEach((id) => {
        const dir = `native/${id}`;
        tool(dir, id, [dir, `:(exclude,glob)${dir}/*.md`, `:(exclude)${dir}/test`]);
    });
    const texts = catalogTexts(out.map((c) => c.id), root);
    out.forEach((c) => {
        if (texts[c.id]) {
            c.texts = texts[c.id];
        }
        Object.keys(c.requires || {}).forEach((id) => {
            if (c.requires[id] === NEXT && !out.some((x) => x.id === id)) {
                throw new Error(`${c.dir}: requires ${id}: "${NEXT}" names a component of this repository only`);
            }
        });
    });
    return out;
}

/* The components' titles and descriptions in other languages, from
 * release/texts/<code>.json ({ "<id>": { "title", "description" } }): by id,
 * the catalog entry's "texts" (the Store shows them in the head unit's
 * language). */
function catalogTexts(ids, root) {
    const dir = path.join(root, 'release', 'texts');
    const out = {};
    (fs.existsSync(dir) ? fs.readdirSync(dir) : []).filter((n) => /\.json$/.test(n)).sort().forEach((n) => {
        const code = n.slice(0, -5);
        const file = readJson(path.join(dir, n));
        Object.keys(file).forEach((id) => {
            const one = textsOf({ [code]: file[id] });
            const given = file[id] && typeof file[id] === 'object' ? file[id] : {};
            const same = one && Object.keys(given).every((k) => one[code][k] === given[k]);
            if (ids.indexOf(id) < 0 || !same) {
                throw new Error(`release/texts/${n}: "${id}" is not a component's { "title", "description" }`);
            }
            out[id] = Object.assign(out[id] || {}, one);
        });
    });
    return out;
}

function select(all, ids) {
    if (!ids.length) {
        return all;
    }
    return ids.map((id) => {
        const c = all.find((x) => x.id === id);
        if (!c) {
            throw new Error(`no component "${id}" (have: ${all.map((x) => x.id).join(', ')})`);
        }
        return c;
    });
}

/* Commit time of the last commit that touched what the component ships: an
 * app's staged tree, a tool's folder without its tests and notes. A change
 * outside that gives the same bytes; one inside it changes them, and
 * publishing then asks for a new version. */
function sourceEpoch(c) {
    return git(['log', '-1', '--format=%ct', 'HEAD', '--'].concat(c.shipped));
}

/* A commit's tree (HEAD's), with each submodule's tree at the commit it
 * records (its checkout must hold that commit). */
function exportHead(repo = REPO, commit = 'HEAD') {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'release-export-'));
    const archive = (cwd, commit, into) => {
        const where = path.relative(repo, cwd) || '.';
        const tar = cp.spawnSync('git', ['archive', commit], { cwd, maxBuffer: 1 << 30 });
        if (tar.status !== 0) {
            throw new Error(`git archive ${commit} in ${where} failed: ${String(tar.stderr || tar.error).trim()}`);
        }
        const x = cp.spawnSync('tar', ['-x', '-C', into], { input: tar.stdout });
        if (x.status !== 0) {
            throw new Error(`unpacking ${commit} of ${where} failed: ${String(x.stderr || x.error).trim()}`);
        }
    };
    archive(repo, commit, dir);
    git(['ls-tree', '-r', commit], { cwd: repo }).split('\n').filter((l) => /^160000 commit /.test(l)).forEach((l) => {
        const [meta, sub] = l.split('\t');
        archive(path.join(repo, sub), meta.split(' ')[2], path.join(dir, sub));
    });
    return dir;
}

/* Inputs git does not carry (e.g. a game's data file), copied from the
 * working tree: paths in an app's release.json "local" are relative to its
 * staged tree. */
function copyLocalInputs(c, exportDir) {
    (c.release && c.release.local || []).forEach((rel) => {
        const staged = c.tree;
        const tree = path.join(REPO, c.dir, staged, rel);
        // in CI they come from the folder MGU_LOCAL_INPUTS names, by file name
        const inputs = process.env.MGU_LOCAL_INPUTS && path.join(process.env.MGU_LOCAL_INPUTS, path.basename(rel));
        const from = fs.existsSync(tree) || !inputs ? tree : inputs;
        const to = path.join(exportDir, c.dir, staged, rel);
        if (!fs.existsSync(from)) {
            throw new Error(`${c.id}: local input ${path.relative(REPO, tree)} is missing` +
                (inputs ? ` (looked for ${path.basename(rel)} in MGU_LOCAL_INPUTS too)` : ` (see ${c.dir}/README.md)`));
        }
        fs.mkdirSync(path.dirname(to), { recursive: true });
        fs.copyFileSync(from, to);
    });
}

/* An app's staged tree in the build export, named <id>@<version> as the
 * package carries it (mgu-app.json "tree" too). */
function stageTree(c, exportDir) {
    const staged = `${c.id}@${c.version}`;
    fs.renameSync(path.join(exportDir, c.dir, c.tree), path.join(exportDir, c.dir, staged));
    if (c.manifest) {
        const file = path.join(exportDir, c.dir, 'mgu-app.json');
        const m = readJson(file);
        m.tree = staged;
        fs.writeFileSync(file, JSON.stringify(m, null, 2) + '\n');
    }
}

function buildOne(c, exportDir) {
    // A private TMPDIR: app harnesses keep scratch files there by uid, which
    // concurrent builds and harness runs would otherwise share. Short, as
    // harnesses put Unix sockets in it (104 bytes on macOS).
    const tmp = fs.mkdtempSync('/tmp/mgu-rel-');
    const env = Object.assign({}, process.env, { SOURCE_DATE_EPOCH: sourceEpoch(c), TMPDIR: tmp });
    try {
        return buildIn(c, exportDir, env);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
}

function buildIn(c, exportDir, env) {
    let artifact;
    if (c.kind === 'app') {
        copyLocalInputs(c, exportDir);
        stageTree(c, exportDir);
        const r = cp.spawnSync(c.release.build, { cwd: path.join(exportDir, c.dir), env, shell: true,
            stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 64 << 20 });
        if (r.status !== 0) {
            process.stderr.write(String(r.stdout).slice(-4000) + String(r.stderr).slice(-4000));
            throw new Error(`${c.id}: build "${c.release.build}" failed (${r.error ? r.error.message : r.signal || `exit ${r.status}`})`);
        }
        artifact = path.join(exportDir, c.artifact);
    } else {
        // Tools are packed by HEAD's packer, in the export.
        const r = cp.spawnSync(process.execPath, [PACKER, c.dir, '--out', path.join(exportDir, 'release-out')],
            { cwd: exportDir, env, stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 64 << 20 });
        if (r.status !== 0) {
            process.stderr.write(String(r.stdout).slice(-4000) + String(r.stderr).slice(-4000));
            throw new Error(`${c.id}: ${PACKER} ${c.dir} failed`);
        }
        artifact = path.join(exportDir, 'release-out', `${c.id}@${c.version}.tgz`);
    }
    if (!fs.existsSync(artifact)) {
        throw new Error(`${c.id}: the build did not produce ${path.relative(exportDir, artifact)}`);
    }
    return artifact;
}

/* Sets each published entry's "texts" to its component's (none: removed);
 * returns the entries that changed. */
function retext(entries, all) {
    return entries.filter((e) => {
        const c = all.find((x) => x.id === e.id);
        const want = (c && c.texts) || null;
        if (JSON.stringify(want) === JSON.stringify(e.texts || null)) {
            return false;
        }
        if (want) {
            e.texts = want;
        } else {
            delete e.texts;
        }
        return true;
    });
}

/* A catalog entry (schema 2): the artifact is always <id>@<version>.tgz;
 * "build": "unit" marks a source package. */
function catalogEntry(c, file) {
    const entry = {
        id: c.id,
        kind: c.kind,
        title: c.title,
        description: c.description,
        version: c.version,
        artifact: { name: path.basename(file), size: fs.statSync(file).size, sha256: sha256(file) }
    };
    if (c.build) {
        entry.build = c.build;
    }
    if (c.requires) {
        entry.requires = c.requires;
    }
    if (c.mguHmi) {
        entry.mguHmi = c.mguHmi;
    }
    if (c.uninstallable) {
        entry.uninstallable = true;
    }
    if (c.texts) {
        entry.texts = c.texts;
    }
    // an app's icon, published beside the catalog as <id>.png
    if (c.icon) {
        entry.icon = `${c.id}.png`;
        entry.iconSha256 = c.icon.sha256;
    }
    return entry;
}

/* Builds the selected components of HEAD into outDir; returns their catalog
 * entries. With `failed` (an array), a component that does not build is
 * recorded there and the others are built. */
function build(ids, outDir, failed) {
    const exportDir = exportHead();
    let ok = false;
    try {
        const list = select(components(exportDir), ids);
        fs.mkdirSync(outDir, { recursive: true });
        const entries = [];
        list.forEach((c) => {
            process.stdout.write(`building ${c.id}@${c.version} ... `);
            // the icon beside the artifact, before the build stages the tree it is in
            if (c.icon) {
                fs.copyFileSync(c.icon.file, path.join(outDir, `${c.id}.png`));
            }
            let file;
            try {
                file = buildOne(c, exportDir);
            } catch (e) {
                if (!failed) {
                    throw e;
                }
                console.log('failed');
                failed.push(`${c.id}@${c.version} does not build: ${e.message}`);
                return;
            }
            const dest = path.join(outDir, `${c.id}@${c.version}.tgz`);
            fs.copyFileSync(file, dest);
            const entry = catalogEntry(c, dest);
            console.log(`${entry.artifact.sha256.slice(0, 12)} ${entry.artifact.size} bytes`);
            entries.push(entry);
        });
        ok = !failed || !failed.length;
        return entries;
    } finally {
        if (ok) {
            fs.rmSync(exportDir, { recursive: true, force: true });
        } else {
            console.error(`the build tree is kept for inspection: ${exportDir}`);
        }
    }
}

function writeCatalog(entries, dir) {
    const catalog = {
        schema: 2,
        commit: git(['rev-parse', 'HEAD']),
        components: entries.slice().sort((a, b) => a.id.localeCompare(b.id))
    };
    const file = path.join(dir, 'catalog.json');
    fs.writeFileSync(file, JSON.stringify(catalog, null, 2) + '\n');
    return file;
}

function warnDirty() {
    if (git(['status', '--porcelain', '--untracked-files=no'])) {
        console.log('note: uncommitted changes are not built; builds use HEAD');
    }
}

function cmdList() {
    const published = publishTarget.published();
    const byId = new Map((published ? published.components : []).map((e) => [e.id, e]));
    const rows = components().map((c) => {
        const p = byId.get(c.id);
        let state = 'not published';
        if (p) {
            const cmp = compareVersions(c.version, p.version);
            state = cmp === 0 ? 'published' : cmp > 0 ? `newer than published ${p.version}` : `OLDER than published ${p.version}`;
        }
        return [c.id, c.kind, c.version, state, c.title];
    });
    const widths = [0, 1, 2, 3].map((i) => Math.max(...rows.map((r) => r[i].length)));
    rows.forEach((r) => console.log(r.map((v, i) => (i < 4 ? v.padEnd(widths[i]) : v)).join('  ')));
    if (!published) {
        console.log(`(no published catalog at ${publishTarget.describe()})`);
    }
}

function cmdBuild(ids) {
    warnDirty();
    const entries = build(ids, OUT);
    console.log(`wrote ${path.relative(REPO, writeCatalog(entries, OUT))}`);
}

function cmdCheck(ids) {
    warnDirty();
    const a = build(ids, fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-check-a-')));
    const b = build(ids, fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-check-b-')));
    let ok = true;
    a.forEach((e, i) => {
        const same = e.artifact.sha256 === b[i].artifact.sha256;
        ok = ok && same;
        console.log(`${same ? 'reproducible' : 'DIFFERS'}  ${e.id}@${e.version}`);
    });
    if (!ok) {
        process.exitCode = 1;
    }
}

/* ---- versions -------------------------------------------------------------------- */

function bumpVersion(version, level) {
    const [major, minor, patch] = version.split('.').map(Number);
    return level === 'major' ? `${major + 1}.0.0` : level === 'minor' ? `${major}.${minor + 1}.0` : `${major}.${minor}.${patch + 1}`;
}

function editJson(file, edit) {
    const j = readJson(file);
    edit(j);
    fs.writeFileSync(file, JSON.stringify(j, null, 2) + '\n');
}

/* Writes a component's version into the tree at root: a tool's
 * component.json (and its versionFiles' VERSION = 'x.y.z'); an app's
 * mgu-app.json and its staged tree's package.json, npm-shrinkwrap.json and
 * rhmiData/application.json. */
function writeVersion(root, c, version) {
    const dir = path.join(root, c.dir);
    if (c.kind === 'tool') {
        const edits = [['component.json', `"version": "${c.version}"`, `"version": "${version}"`]]
            .concat(c.versionFiles.map((rel) => [rel, `VERSION = '${c.version}'`, `VERSION = '${version}'`]));
        // every file names the old version once, or none is written
        const texts = edits.map(([rel, from]) => {
            const text = fs.readFileSync(path.join(dir, rel), 'utf8');
            if (text.split(from).length !== 2) {
                throw new Error(`${c.dir}/${rel}: expected ${from} once`);
            }
            return text;
        });
        edits.forEach(([rel, from, to], i) => fs.writeFileSync(path.join(dir, rel), texts[i].replace(from, to)));
        return;
    }
    if (c.manifest) {
        editJson(path.join(dir, 'mgu-app.json'), (j) => { j.version = version; });
    }
    [['package.json', (j) => { j.version = version; }],
        ['npm-shrinkwrap.json', (j) => { j.version = version; }],
        [path.join('rhmiData', 'application.json'), (j) => { j.application.version = version; }]
    ].forEach(([rel, edit]) => {
        const p = path.join(dir, c.tree, rel);
        if (fs.existsSync(p)) {
            editJson(p, edit);
        }
    });
}

/* Writes resolved "requires" (id -> x.y.z) over the "next" ones of a
 * component's release.json or component.json. */
function writeRequires(root, c, resolved) {
    const file = path.join(root, c.dir, c.kind === 'app' ? 'release.json' : 'component.json');
    let text = fs.readFileSync(file, 'utf8');
    Object.keys(resolved).forEach((id) => {
        const from = new RegExp(`("${id}"\\s*:\\s*)"${NEXT}"`);
        if (!from.test(text)) {
            throw new Error(`${path.relative(root, file)}: no "${id}": "${NEXT}"`);
        }
        text = text.replace(from, `$1"${resolved[id]}"`);
    });
    fs.writeFileSync(file, text);
}

/* A CHANGELOG's "## Unreleased" section: { start, end, body } (body
 * trimmed), or null. Headings repeated right below it (two pull requests
 * that each added one) count as one. */
const UNRELEASED = /^## Unreleased[ \t]*$/m;
function unreleasedOf(text) {
    const m = UNRELEASED.exec(text);
    if (!m) {
        return null;
    }
    let after = m.index + m[0].length;
    let body = '';
    for (;;) {
        const next = text.slice(after).search(/^## /m);
        const end = next < 0 ? text.length : after + next;
        body += text.slice(after, end);
        const again = UNRELEASED.exec(text.slice(end));
        if (!again || again.index !== 0) {
            return { start: m.index, end, body: body.split('\n').map((l) => l.trimEnd()).join('\n').replace(/\n{3,}/g, '\n\n').trim() };
        }
        after = end + again[0].length;
    }
}

/* The CHANGELOG with its Unreleased text under a heading for version and an
 * empty "## Unreleased" above it for the next pull requests; when the
 * newest section is already that version (a release stamped and not yet
 * published), the text goes on top of it. Null when there is no text. */
function releaseChangelog(text, version) {
    const u = unreleasedOf(text);
    if (!u || !u.body) {
        return null;
    }
    let rest = text.slice(u.end);
    const heading = `## ${version}`;
    if (rest.startsWith(`${heading}\n`)) {
        rest = rest.slice(heading.length + 1).replace(/^\n+/, '');
    }
    return `${text.slice(0, u.start)}## Unreleased\n\n${heading}\n\n${u.body}\n\n${rest}`;
}

function tagExists(tag, repo) {
    return cp.spawnSync('git', ['rev-parse', '-q', '--verify', `refs/tags/${tag}^{commit}`], { cwd: repo }).status === 0;
}

/* The GitHub repository whose pull requests' labels set a release's step. */
function labelsRepo(repo) {
    if (process.env.GITHUB_REPOSITORY) {
        return process.env.GITHUB_REPOSITORY;
    }
    const m = /github\.com[:/]([^/]+\/[^/]+?)(\.git)?$/.exec(git(['remote', 'get-url', 'origin'], { cwd: repo }));
    if (!m) {
        throw new Error('no GitHub repository to read pull request labels from (set GITHUB_REPOSITORY)');
    }
    return m[1];
}

/* The step from a release to the next: "major" or "minor" when a pull
 * request of one of the commits carries release:major or release:minor,
 * else "patch". MGU_RELEASE_LABELS=off reads no labels: a patch, but for
 * the commits prLabels already holds (tests). */
const LEVELS = ['patch', 'minor', 'major'];
const prLabels = new Map();
function levelOf(commits, repo) {
    let level = 0;
    commits.forEach((sha) => {
        if (!prLabels.has(sha) && process.env.MGU_RELEASE_LABELS !== 'off') {
            const out = cp.execFileSync('gh', ['api', `repos/${labelsRepo(repo)}/commits/${sha}/pulls`,
                '--jq', '.[] | .labels[].name'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
            prLabels.set(sha, out.split('\n').filter(Boolean));
        }
        (prLabels.get(sha) || []).forEach((name) => {
            const m = /^release:(minor|major)$/.exec(name);
            if (m) {
                level = Math.max(level, LEVELS.indexOf(m[1]));
            }
        });
    });
    return LEVELS[level];
}

/* What a release of the selected components (none: all) does: for each
 * component changed since its tag <id>@<version>, the next version; a
 * component whose version has no tag yet (stamped, not published; or new)
 * keeps it. A component that requires another one "next" brings that one
 * along. Returns [{ c, version, level, resolved }] for the components to
 * release. */
function plan(all, ids, repo = REPO) {
    const byId = new Map(all.map((c) => [c.id, c]));
    const chosen = new Set(select(all, ids).map((c) => c.id));
    const out = new Map();
    const consider = (c) => {
        const tag = `${c.id}@${c.version}`;
        const paths = c.shipped.concat(c.listed);
        if (!tagExists(tag, repo)) {
            out.set(c.id, { c, version: c.version, level: 'new' });
            return;
        }
        if (cp.spawnSync('git', ['diff', '--quiet', tag, 'HEAD', '--'].concat(paths), { cwd: repo }).status === 0) {
            return;
        }
        const commits = git(['log', '--format=%H', `${tag}..HEAD`, '--'].concat(paths), { cwd: repo }).split('\n').filter(Boolean);
        const level = levelOf(commits, repo);
        out.set(c.id, { c, version: bumpVersion(c.version, level), level });
    };
    all.filter((c) => chosen.has(c.id)).forEach(consider);
    // what a chosen component requires "next" is released with it
    let grew = true;
    while (grew) {
        grew = false;
        [...out.values()].forEach((p) => Object.keys(p.c.requires || {}).forEach((id) => {
            if (p.c.requires[id] === NEXT && !chosen.has(id)) {
                chosen.add(id);
                consider(byId.get(id));
                grew = true;
            }
        }));
    }
    out.forEach((p) => {
        p.resolved = {};
        Object.keys(p.c.requires || {}).forEach((id) => {
            if (p.c.requires[id] === NEXT) {
                p.resolved[id] = out.has(id) ? out.get(id).version : byId.get(id).version;
            }
        });
    });
    return all.filter((c) => out.has(c.id)).map((c) => out.get(c.id));
}

/* Writes a plan into the tree at root: versions, "requires", CHANGELOGs. */
function writePlan(root, steps) {
    steps.forEach((p) => {
        const changelog = path.join(root, p.c.changelog);
        if (fs.existsSync(changelog)) {
            const text = fs.readFileSync(changelog, 'utf8');
            const released = releaseChangelog(text, p.version);
            if (released === null && !text.includes(`\n## ${p.version}\n`)) {
                throw new Error(`${p.c.changelog}: no "## Unreleased" section with what ${p.c.id} ${p.version} changes`);
            }
            if (released !== null) {
                fs.writeFileSync(changelog, released);
            }
        }
        if (p.version !== p.c.version) {
            writeVersion(root, p.c, p.version);
        }
        if (Object.keys(p.resolved).length) {
            writeRequires(root, p.c, p.resolved);
        }
    });
}

/* The ids the last stamp chose, for publish (release/out/stamped). */
const STAMPED = path.join(OUT, 'stamped');

function cmdStamp(ids, dryRun) {
    if (git(['status', '--porcelain', '--untracked-files=no'])) {
        throw new Error('commit first: stamp writes the release commit');
    }
    const steps = plan(components(), ids);
    steps.forEach((p) => {
        const req = Object.keys(p.resolved).map((id) => `, requires ${id} ${p.resolved[id]}`).join('');
        const how = p.level === 'new' ? 'not released yet' : `${p.level} from ${p.c.version}`;
        console.log(`${dryRun ? 'would release' : 'releasing'} ${p.c.id}@${p.version} (${how}${req})`);
    });
    if (!steps.length) {
        console.log('nothing changed since the last release');
    }
    if (dryRun) {
        return;
    }
    fs.mkdirSync(OUT, { recursive: true });
    fs.writeFileSync(STAMPED, steps.map((p) => p.c.id).join(' ') + '\n');
    writePlan(REPO, steps);
    git(['add', '-u']);
    if (!git(['status', '--porcelain', '--untracked-files=no'])) {
        return;
    }
    git(['commit', '-q', '-m', `release: ${steps.map((p) => `${p.c.id}@${p.version}`).join(', ')}`]);
    console.log(`committed ${git(['rev-parse', '--short', 'HEAD'])}`);
}

/* The components' versions in a tree, by id, as each one's own file has
 * it: a tool's component.json, an app's mgu-app.json (else its staged
 * tree's package.json). */
function versionsAt(root) {
    const out = {};
    const at = (file) => (fs.existsSync(file) ? readJson(file).version : undefined);
    fs.readdirSync(path.join(root, 'native')).forEach((id) => {
        out[id] = at(path.join(root, 'native', id, 'component.json'));
    });
    fs.readdirSync(path.join(root, 'apps')).forEach((id) => {
        out[id] = at(path.join(root, 'apps', id, 'mgu-app.json')) || at(path.join(root, 'apps', id, APP_TREE, 'package.json'));
    });
    return out;
}

/* A pull request's checks against its base commit: versions stay as the
 * base has them (the release sets them), and a changed component with a
 * CHANGELOG says what changed under "## Unreleased". */
function cmdVersions(base) {
    if (!base) {
        throw new Error('usage: versions <base commit>');
    }
    const baseDir = exportHead(REPO, base);
    const problems = [];
    try {
        const before = versionsAt(baseDir);
        components().forEach((c) => {
            const was = before[c.id];
            if (was && was !== c.version) {
                problems.push(`${c.id}: the release sets versions; keep ${was}`);
            }
            const changed = !was || cp.spawnSync('git', ['diff', '--quiet', base, 'HEAD', '--'].concat(c.shipped, c.listed),
                { cwd: REPO }).status !== 0;
            const log = path.join(REPO, c.changelog);
            if (changed && fs.existsSync(log)) {
                const u = unreleasedOf(fs.readFileSync(log, 'utf8'));
                if (!u || !u.body) {
                    problems.push(`${c.id}: add what changes under "## Unreleased" at the top of ${c.changelog}`);
                }
            }
        });
    } finally {
        fs.rmSync(baseDir, { recursive: true, force: true });
    }
    problems.forEach((m) => console.log(m));
    if (problems.length) {
        process.exitCode = 1;
    } else {
        console.log('versions: as the base has them');
    }
}

function cmdPublish(ids, dryRun, stamped) {
    if (git(['status', '--porcelain', '--untracked-files=no'])) {
        throw new Error('commit first: publish builds HEAD and records its commit in the catalog');
    }
    // before anything is built or uploaded: the target is private, and
    // publishing to it runs in GitHub Actions
    publishTarget.preflight(dryRun);
    const published = publishTarget.published() || { components: [] };
    // a run from a commit older than the published catalog's (a re-run of an
    // old tag) would take back what later commits published
    if (published.commit && cp.spawnSync('git', ['merge-base', '--is-ancestor', published.commit, 'HEAD'],
        { cwd: REPO }).status !== 0) {
        throw new Error(`HEAD does not contain ${published.commit}, the published catalog's commit: ` +
            'publish from that commit or a later one');
    }
    const current = new Map(published.components.map((e) => [e.id, e]));
    // the catalog lists what the repository has: a component removed from it
    // (or renamed) leaves the catalog too
    const inRepo = new Set(components().map((c) => c.id));
    const removed = [...current.keys()].filter((id) => !inRepo.has(id));
    removed.forEach((id) => current.delete(id));
    if (stamped) {
        // what the last stamp chose, and any component whose version is
        // tagged but not in the catalog (a publish that stopped part-way)
        const lagging = components().filter((c) => tagExists(`${c.id}@${c.version}`, REPO) &&
            (!current.has(c.id) || current.get(c.id).version !== c.version));
        ids = [...new Set(fs.readFileSync(STAMPED, 'utf8').split(/\s+/).filter(Boolean).concat(lagging.map((c) => c.id)))];
    }
    const staging = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-publish-'));
    const skipped = [];
    // built twice: a version publishes only when both builds give the same
    // bytes; with --stamped and nothing to publish, nothing is built
    const none = stamped && !ids.length;
    const built = none ? [] : build(ids, staging, skipped);
    const again = built.length ? build(built.map((e) => e.id), fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-publish-b-')), skipped) : [];
    const uploads = [];
    const all = components();
    built.forEach((e) => {
        const p = current.get(e.id);
        const b = again.find((x) => x.id === e.id);
        const next = Object.keys(e.requires || {}).filter((id) => e.requires[id] === NEXT);
        if (!b || b.artifact.sha256 !== e.artifact.sha256) {
            skipped.push(`${e.id}@${e.version} builds differently each time`);
        } else if (next.length) {
            skipped.push(`${e.id}@${e.version} requires ${next.join(', ')} "${NEXT}": stamp first`);
        } else if (p && p.version === e.version && p.artifact.sha256 !== e.artifact.sha256) {
            skipped.push(`${e.id}@${e.version} is published with other content: stamp gives it its next version`);
        } else if (p && compareVersions(e.version, p.version) < 0) {
            skipped.push(`${e.id}@${e.version} is older than the published ${p.version}`);
        } else if (!p || p.version !== e.version) {
            uploads.push(e);
        }
    });
    skipped.forEach((m) => console.log(`skipped: ${m}`));
    if (skipped.length) {
        process.exitCode = 1;
    }
    // the entries carried over list their artifact as the target does (publish.js listedUrl)
    const relisted = [...current.values()].filter((e) => !uploads.some((x) => x.id === e.id)).filter((e) => {
        const url = publishTarget.listedUrl(e);
        if (url === e.artifact.url) {
            return false;
        }
        e.artifact.url = url;
        return true;
    });
    // the entries carried over list their titles and descriptions in other languages as release/texts has them
    const retexted = retext([...current.values()].filter((e) => !uploads.some((x) => x.id === e.id)), all);
    if (!uploads.length && !removed.length && !relisted.length && !retexted.length) {
        console.log('nothing new to publish');
        return;
    }
    removed.forEach((id) => console.log(`${dryRun ? 'would remove' : 'removing'} ${id}: not in the repository`));
    relisted.forEach((e) => console.log(`${dryRun ? 'would list' : 'listing'} ${e.id}@${e.version} at ${e.artifact.url}`));
    retexted.forEach((e) => console.log(`${dryRun ? 'would list' : 'listing'} ${e.id}@${e.version} with its texts`));
    uploads.forEach((e) => console.log(`${dryRun ? 'would publish' : 'publishing'} ${e.id}@${e.version}`));
    if (dryRun) {
        return;
    }
    uploads.forEach((e) => {
        e.artifact.url = publishTarget.uploadArtifact(e, path.join(staging, e.artifact.name));
        current.set(e.id, e);
    });
    // the icons before the catalog that names them
    uploads.filter((e) => e.icon).forEach((e) => publishTarget.uploadCatalogAsset(path.join(staging, e.icon)));
    const catalogFile = writeCatalog([...current.values()], staging);
    publishTarget.uploadCatalog(catalogFile);
    console.log(`catalog published to ${publishTarget.describe()}`);
}

function main(argv) {
    const [cmd, ...rest] = argv;
    const flags = rest.filter((a) => a.startsWith('--'));
    const args = rest.filter((a) => !a.startsWith('--'));
    switch (cmd) {
    case 'list': return cmdList();
    case 'build': return cmdBuild(args);
    case 'check': return cmdCheck(args);
    case 'versions': return cmdVersions(args[0]);
    case 'stamp': return cmdStamp(args, flags.includes('--dry-run'));
    case 'publish': return cmdPublish(args, flags.includes('--dry-run'), flags.includes('--stamped'));
    default:
        process.stdout.write(fs.readFileSync(__filename, 'utf8').split('/**')[1].split('*/')[0].split('\n').slice(1, 14)
            .map((l) => l.replace(/^ \* ?/, '')).join('\n') + '\n');
        process.exitCode = cmd ? 2 : 0;
    }
    return undefined;
}

if (require.main === module) {
    try {
        main(process.argv.slice(2));
    } catch (e) {
        console.error(`release: ${e.message}`);
        process.exitCode = 1;
    }
}

module.exports = { components, catalogTexts, retext, compareVersions, catalogEntry, exportHead, bumpVersion, releaseChangelog,
    unreleasedOf, plan, writePlan, prLabels };
