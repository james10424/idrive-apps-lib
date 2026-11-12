#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Mutation test of carplay_locate.js on the reference build (CARPLAY_BIN,
 * carplay 22.28.3, checked by SHA-256): each case perturbs a copy of the
 * build in memory the way another build could differ (code at other
 * addresses, a function or string duplicated, a check byte changed, a decoy
 * switch, a third geometry builder, inlining, a changed field layout, a
 * relocation or a merged string over an edit site) and the locator must
 * either find every site where it really is or refuse the build. A located
 * build with any site elsewhere is a failure. The map URL edit, which a build
 * may lack, is likewise found where it really is or not found, in each of
 * those and in perturbations of its own sites. Nothing is written to disk.
 *
 *   CARPLAY_BIN=<copy of /usr/bin/carplay> node mutation_test.js
 */
'use strict';

var fs = require('fs');
var crypto = require('crypto');
var locator = require('../carplay_locate.js');
var mutations = require('./carplay_mutations.js');

var REF_SHA = 'ddb33e33161387c6479c391684b3d019b0de54ecfcee8d6877eca94aa62f3e50';
var SRC = process.env.CARPLAY_BIN || '';
if (!fs.existsSync(SRC)) { process.stdout.write('SKIP: set CARPLAY_BIN to a copy of the unit\'s /usr/bin/carplay (carplay 22.28.3)\n'); process.exit(0); }
var REF = fs.readFileSync(SRC);
if (crypto.createHash('sha256').update(REF).digest('hex') !== REF_SHA) { process.stdout.write('SKIP: CARPLAY_BIN is not carplay 22.28.3\n'); process.exit(0); }

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}
function json(x) { return JSON.stringify(x); }

var M = mutations(REF), CASES = M.CASES, EXPECT = M.EXPECT, sites = M.sites, MAP = M.LOC.mapurl;
ok('reference build located, mutation anchors found', M.LOC.ok);

var tally = { correct: 0, refused: 0, WRONG: 0 };
CASES.forEach(function (t) {
    var b = Buffer.from(REF), out;
    try { out = t[1](b) || b; } catch (e) { ok(t[0] + ': mutation built', false, e.message); return; }
    var loc = locator.locate(out), want = t[3] || EXPECT, got = loc.ok ? sites(loc) : null;
    var outcome = !loc.ok ? 'refused' : json(got) === json(want) ? 'correct' : 'WRONG';
    tally[outcome]++;
    ok(t[0] + ': ' + outcome + (outcome === 'refused' ? ' (' + loc.reason + ')' : ''), t[2].indexOf(outcome) >= 0,
        'allowed ' + t[2].join('/') + (got ? '; got ' + json(got) + ', want ' + json(want) : ''));
    /* code and data moved: the map URL sites move with them */
    var d = want.slot - EXPECT.slot, m = loc.mapurl;
    ok(t[0] + ': the map URL edit ' + (m.ok ? 'found where it is' : 'not found (' + m.reason + ')'), !m.ok || (m.slot === MAP.slot + d && m.lea === MAP.lea + d),
        json([m.slot, m.lea]));
});

var mapTally = { correct: 0, refused: 0, WRONG: 0 };
M.MAP_CASES.forEach(function (t) {
    var b = Buffer.from(REF), out;
    try { out = t[1](b) || b; } catch (e) { ok('map URL, ' + t[0] + ': mutation built', false, e.message); return; }
    var loc = locator.locate(out), m = loc.mapurl;
    var outcome = !m.ok ? 'refused' : m.slot === MAP.slot && m.lea === MAP.lea && m.slotPatched === t[3].slotPatched && m.leaPatched === t[3].leaPatched ? 'correct' : 'WRONG';
    mapTally[outcome]++;
    ok('map URL, ' + t[0] + ': ' + outcome + (outcome === 'refused' ? ' (' + m.reason + ')' : '') + ', the other edits found', t[2].indexOf(outcome) >= 0 && loc.ok,
        'allowed ' + t[2].join('/') + '; ' + json([loc.reason, m]));
});
process.stdout.write('\n' + M.MAP_CASES.length + ' map URL mutations: ' + mapTally.correct + ' located correctly, ' + mapTally.refused + ' refused, ' + mapTally.WRONG + ' located wrongly\n');
process.stdout.write('\n' + CASES.length + ' mutations: ' + tally.correct + ' located correctly, ' + tally.refused + ' refused, ' + tally.WRONG + ' located wrongly\n');
process.stdout.write(fails ? ('\n' + fails + ' check(s) failed\n') : '\nall checks passed\n');
process.exit(fails ? 1 : 0);
