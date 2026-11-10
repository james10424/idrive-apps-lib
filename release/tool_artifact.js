#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Packs the tool in a folder holding its component.json (native/<id>) into
 * <out>/<id>@<version>.tgz, the artifact mgu-store's deploy tool installs:
 * the artifact's component.json (sha256 per file, units by name and
 * runsInSafeMode, drop-ins by name), files/<path>, units/<name> and dropins/<unit>.d/<name>, sorted,
 * owner 0:0, one mtime
 * (SOURCE_DATE_EPOCH, else oap_pack's fixed one). A component.json `build`
 * command runs in the folder first.
 *
 *   node release/tool_artifact.js <dir> --out <dir>
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const cp = require('child_process');
const { tgz, MTIME } = require('../tools/oap_pack.js');

const SEMVER = /^\d+\.\d+\.\d+$/;

function sha256(buf) {
    return crypto.createHash('sha256').update(buf).digest('hex');
}

function safeRel(p) {
    return typeof p === 'string' && p !== '' && !p.startsWith('/') &&
        !p.split('/').includes('..') && !p.includes('\\');
}

function mtime() {
    const epoch = process.env.SOURCE_DATE_EPOCH;
    if (epoch === undefined || epoch === '') {
        return MTIME;
    }
    if (!/^\d+$/.test(epoch)) {
        throw new Error(`SOURCE_DATE_EPOCH is not a number: ${epoch}`);
    }
    return Number(epoch);
}

function byName(a, b) {
    return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
}

function buildArtifact(compDir, outDir) {
    const dir = path.resolve(compDir);
    const rel = path.relative(process.cwd(), dir) || '.';
    const file = path.join(dir, 'component.json');
    if (!fs.existsSync(file)) {
        throw new Error(`${rel}: no component.json`);
    }
    const c = JSON.parse(fs.readFileSync(file, 'utf8'));
    const id = c.id;
    if (!/^[a-z0-9][a-z0-9-]*$/.test(id || '') || c.kind !== 'tool' || !SEMVER.test(c.version || '')) {
        throw new Error(`${rel}/component.json: needs an id of a-z, 0-9 and -, kind "tool" and a x.y.z version`);
    }
    const buildCmd = Array.isArray(c.build) && c.build.length ? c.build : null;
    if (buildCmd) {
        process.stderr.write(`${id}: ${buildCmd.join(' ')} (in ${rel})\n`);
        const r = cp.spawnSync(buildCmd[0], buildCmd.slice(1), { cwd: dir, stdio: ['ignore', 2, 2] });
        if (r.status !== 0) {
            throw new Error(`${id}: build "${buildCmd.join(' ')}" in ${rel} failed (${r.error ? r.error.message : r.signal || `exit ${r.status}`})`);
        }
    }
    const entries = [];
    const files = (c.files || []).map((f) => {
        if (!safeRel(f.path) || !/^[0-7]{3,4}$/.test(f.mode || '')) {
            throw new Error(`${rel}/component.json: bad files[] entry ${JSON.stringify(f)}`);
        }
        const from = path.join(dir, f.from || f.path);
        if (!fs.existsSync(from) || !fs.statSync(from).isFile()) {
            throw new Error(`${id}: ${path.relative(dir, from)} is missing` +
                (buildCmd ? ` (built by "${buildCmd.join(' ')}" in ${rel})` : ''));
        }
        const data = fs.readFileSync(from);
        entries.push({ name: `files/${f.path}`, data, mode: parseInt(f.mode, 8) });
        return { path: f.path, mode: f.mode, sha256: sha256(data) };
    });
    const units = (c.units || []).map((u) => {
        if (!safeRel(u.name) || u.name.includes('/')) {
            throw new Error(`${rel}/component.json: bad unit name ${u.name}`);
        }
        const from = path.join(dir, u.from || u.name);
        if (!fs.existsSync(from)) {
            throw new Error(`${id}: unit ${path.relative(dir, from)} is missing`);
        }
        entries.push({ name: `units/${u.name}`, data: fs.readFileSync(from), mode: 0o644 });
        return u.runsInSafeMode === true ? { name: u.name, runsInSafeMode: true } : { name: u.name };
    });
    const dropins = (c.dropins || []).map((d) => {
        if (!d || !/^[A-Za-z0-9_.@-]{1,100}\.(service|socket|target|mount|path|timer)$/.test(d.unit || '') ||
            !/^[A-Za-z0-9_.@-]{1,100}\.conf$/.test(d.name || '')) {
            throw new Error(`${rel}/component.json: bad dropins entry ${JSON.stringify(d)}`);
        }
        const from = path.join(dir, d.from || d.name);
        if (!fs.existsSync(from)) {
            throw new Error(`${id}: drop-in ${path.relative(dir, from)} is missing`);
        }
        entries.push({ name: `dropins/${d.unit}.d/${d.name}`, data: fs.readFileSync(from), mode: 0o644 });
        return { unit: d.unit, name: d.name };
    });
    const manifest = {
        id: c.id,
        kind: c.kind,
        version: c.version,
        title: c.title || c.id,
        description: c.description || '',
        files,
        units,
        enable: c.enable || [],
        recmgr: c.recmgr || [],
        restart: c.restart || []
    };
    if (dropins.length) {
        manifest.dropins = dropins;
    }
    if (c.internet !== undefined) {
        manifest.internet = c.internet;
        manifest.uids = c.uids;
    }
    // what the deploy tool reads (mgu-store deploy/README.md), as mgu-store's
    // `deploy/hu artifact` packs it; requires and uninstallable are the
    // catalog's (release.js)
    ['mguHmi', 'mguProvides', 'keepRunning', 'crashNames', 'postInstall', 'check', 'activate', 'deactivate'].forEach((k) => {
        if (c[k] !== undefined) {
            manifest[k] = c[k];
        }
    });
    entries.push({ name: 'component.json', data: Buffer.from(JSON.stringify(manifest, null, 2) + '\n'), mode: 0o644 });
    entries.sort(byName);
    const data = tgz(entries, mtime());
    fs.mkdirSync(outDir, { recursive: true });
    const out = path.join(outDir, `${c.id}@${c.version}.tgz`);
    fs.writeFileSync(out, data);
    return out;
}

if (require.main === module) {
    const args = process.argv.slice(2);
    if (args.length !== 3 || args[1] !== '--out') {
        console.error('usage: node release/tool_artifact.js <dir> --out <dir>');
        process.exitCode = 2;
    } else {
        try {
            console.log(buildArtifact(args[0], args[2]));
        } catch (e) {
            console.error(`tool_artifact: ${e.message}`);
            process.exitCode = 1;
        }
    }
}

module.exports = { buildArtifact };
