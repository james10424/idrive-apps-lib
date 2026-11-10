// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Where release.js publishes, chosen by MGU_PUBLISH (default: the GitHub
 * repository the "origin" remote names):
 *
 *   github:<owner>/<repo>  GitHub Releases of the repository: each version is
 *                          the release <id>@<version> with its artifact as
 *                          the one asset; the release "catalog" holds
 *                          catalog.json and the icons it names. Runs in GitHub Actions only, with the
 *                          workflow's GITHUB_TOKEN (GH_TOKEN). A repository
 *                          whose visibility is not MGU_PUBLISH_VISIBILITY
 *                          (default private; "any" takes either) is
 *                          refused; nothing here changes a repository's
 *                          visibility.
 *   dir:<path>             A local folder (relative paths: to the repository):
 *                          catalog.json, the icons it names and the
 *                          artifacts side by side, a test catalog for a
 *                          test unit.
 *
 * A catalog entry's artifact url (listedUrl): in a public repository the
 * asset's release download (https://github.com/<owner>/<repo>/releases/
 * download/<tag>/<name>, its browser_download_url), which a car without a
 * token fetches without GitHub's API and its rate limit; in a private one
 * the asset's API URL, which the car downloads with its token.
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const { parse } = require('./catalog.js');

const REPO = path.join(__dirname, '..');

function originRepo() {
    try {
        const url = cp.execFileSync('git', ['remote', 'get-url', 'origin'],
            { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
        const m = /github\.com[:/]([^/]+\/[^/]+?)(\.git)?$/.exec(url);
        return m ? `github:${m[1]}` : '';
    } catch (e) {
        return '';
    }
}

const TARGET = process.env.MGU_PUBLISH || originRepo();
const KIND = TARGET.slice(0, TARGET.indexOf(':'));
const WHERE = KIND === 'dir' ? path.resolve(REPO, TARGET.slice(TARGET.indexOf(':') + 1)) : TARGET.slice(TARGET.indexOf(':') + 1);
const CATALOG_TAG = 'catalog';
const VISIBILITY = process.env.MGU_PUBLISH_VISIBILITY || 'private';   /* private, public or any */

function gh(args, opts = {}) {
    return cp.execFileSync('gh', args, Object.assign({ encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }, opts));
}

/* The published catalog, or null when none is published. Whatever is there
 * must be a well-formed catalog: its entries carry over into the next one. */
function published() {
    const text = chosen().fetchCatalogText();
    if (text === null) {
        return null;
    }
    try {
        return parse(text);
    } catch (e) {
        throw new Error(`the published catalog at ${TARGET} is unusable (${e.message}); ` +
            'find out who changed it before publishing over it');
    }
}

/* sha256 of a release's one asset, as GitHub serves it. */
function assetSha256(tag) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-asset-'));
    try {
        gh(['release', 'download', tag, '-R', WHERE, '-D', dir]);
        const files = fs.readdirSync(dir);
        return files.length === 1 ? crypto.createHash('sha256').update(fs.readFileSync(path.join(dir, files[0]))).digest('hex') : null;
    } catch (e) {
        return null; // an asset GitHub cannot serve is uploaded again
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

/* Publishing to GitHub runs in GitHub Actions (.github/workflows/release.yml),
 * and goes only to a repository of the visibility asked for. */
function githubPreflight(dryRun) {
    if (!dryRun && process.env.GITHUB_ACTIONS !== 'true') {
        throw new Error('publishing runs in GitHub Actions: run the Release workflow (release/README.md)');
    }
    assertVisibility();
}

let visibility = null;
function assertVisibility() {
    const repo = JSON.parse(gh(['api', `repos/${WHERE}`]));
    visibility = repo.private === false ? 'public' : 'private';
    if (VISIBILITY !== 'any' && (repo.visibility !== VISIBILITY || repo.private !== (VISIBILITY !== 'public'))) {
        throw new Error(`refusing to publish: ${WHERE} is ${repo.visibility}, not ${VISIBILITY}`);
    }
}

/* Whether the repository is public, as the last visibility check found it. */
function isPublic() {
    if (visibility === null) {
        assertVisibility();
    }
    return visibility === 'public';
}

/* The url a catalog lists a release asset under (above). */
function assetUrl(asset) {
    return isPublic() ? asset.browser_download_url : asset.url;
}

/* The release <id>@<version> of a published entry, or null. */
function releaseOf(tag) {
    try {
        return JSON.parse(gh(['api', `repos/${WHERE}/releases/tags/${encodeURIComponent(tag)}`]));
    } catch (e) {
        return null;
    }
}

/* The release "catalog", created when it is not there. */
function catalogRelease() {
    assertVisibility();
    try {
        gh(['release', 'view', CATALOG_TAG, '-R', WHERE]);
    } catch (e) {
        gh(['release', 'create', CATALOG_TAG, '-R', WHERE, '--latest=false', '--title', 'Catalog',
            '--notes', 'catalog.json: the newest version of every component (release/README.md)']);
    }
}

const targets = {
    dir: {
        preflight() {},
        fetchCatalogText() {
            const file = path.join(WHERE, 'catalog.json');
            return fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
        },
        listedUrl(entry) {
            return entry.artifact.url;
        },
        uploadArtifact(entry, file) {
            fs.mkdirSync(WHERE, { recursive: true });
            fs.copyFileSync(file, path.join(WHERE, entry.artifact.name));
            return entry.artifact.name;
        },
        uploadCatalogAsset(file) {
            fs.mkdirSync(WHERE, { recursive: true });
            fs.copyFileSync(file, path.join(WHERE, path.basename(file)));
        },
        uploadCatalog(catalogFile) {
            fs.mkdirSync(WHERE, { recursive: true });
            fs.copyFileSync(catalogFile, path.join(WHERE, 'catalog.json.tmp'));
            fs.renameSync(path.join(WHERE, 'catalog.json.tmp'), path.join(WHERE, 'catalog.json'));
        }
    },
    github: {
        preflight: githubPreflight,
        fetchCatalogText() {
            const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mgu-catalog-'));
            try {
                gh(['release', 'download', CATALOG_TAG, '-R', WHERE, '-p', 'catalog.json', '-D', dir]);
                return fs.readFileSync(path.join(dir, 'catalog.json'), 'utf8');
            } catch (e) {
                if (/release not found|no assets/i.test(String(e.stderr))) {
                    return null;
                }
                throw new Error(`cannot read the published catalog from ${WHERE}: ${String(e.stderr).trim()}`);
            } finally {
                fs.rmSync(dir, { recursive: true, force: true });
            }
        },
        /* A published entry's artifact url as this repository lists it
         * (assetUrl), from its release's one asset. */
        listedUrl(entry) {
            const url = entry.artifact.url || '';
            const download = /^https:\/\/github\.com\/[^/]+\/[^/]+\/releases\/download\//.test(url);
            if (download === isPublic()) {
                return url;
            }
            const release = releaseOf(`${entry.id}@${entry.version}`);
            const asset = release && release.assets.length === 1 && release.assets[0];
            if (!asset || asset.size !== entry.artifact.size) {
                throw new Error(`${entry.id}@${entry.version}: its release's asset is missing or not ${entry.artifact.size} bytes`);
            }
            return assetUrl(asset);
        },
        /* Returns the url the catalog lists the asset under (assetUrl). A
         * release left by an interrupted publish is completed rather than
         * created again. The release's one asset is the artifact, whatever
         * name GitHub gave it. Each new release is marked Latest, so a run's
         * last one is the repository's Latest. */
        uploadArtifact(entry, file) {
            const tag = `${entry.id}@${entry.version}`;
            const view = () => releaseOf(tag);
            let release = view();
            if (!release) {
                const commit = cp.execFileSync('git', ['rev-parse', 'HEAD'], { cwd: REPO, encoding: 'utf8' }).trim();
                gh(['release', 'create', tag, file, '-R', WHERE, '--target', commit, '--latest',
                    '--title', `${entry.title} ${entry.version}`, '--notes', entry.description || tag]);
                release = view();
            } else if (release.assets.length !== 1 || assetSha256(tag) !== entry.artifact.sha256) {
                release.assets.forEach((a) => gh(['api', '-X', 'DELETE', `repos/${WHERE}/releases/assets/${a.id}`]));
                gh(['release', 'upload', tag, file, '-R', WHERE]);
                release = view();
            }
            const asset = release && release.assets.length === 1 && release.assets[0];
            if (!asset || asset.size !== entry.artifact.size) {
                throw new Error(`${tag}: the release's asset is missing or not ${entry.artifact.size} bytes`);
            }
            return assetUrl(asset);
        },
        /* A file beside catalog.json in the release "catalog", under its name. */
        uploadCatalogAsset(file) {
            catalogRelease();
            gh(['release', 'upload', CATALOG_TAG, '-R', WHERE, '--clobber', file]);
        },
        uploadCatalog(catalogFile) {
            catalogRelease();
            gh(['release', 'upload', CATALOG_TAG, '-R', WHERE, '--clobber', catalogFile]);
        }
    }
};

/* Asked for only when publishing: building needs no target. */
function chosen() {
    if (!targets[KIND]) {
        throw new Error(`MGU_PUBLISH=${TARGET}: use github:<owner>/<repo> or dir:<path>` +
            (TARGET ? '' : ' (no GitHub "origin" remote to default to)'));
    }
    return targets[KIND];
}

module.exports = {
    describe: () => TARGET,
    published,
    preflight: (dryRun) => chosen().preflight(dryRun),
    listedUrl: (entry) => chosen().listedUrl(entry),
    uploadArtifact: (entry, file) => chosen().uploadArtifact(entry, file),
    uploadCatalogAsset: (file) => chosen().uploadCatalogAsset(file),
    uploadCatalog: (catalogFile) => chosen().uploadCatalog(catalogFile)
};
