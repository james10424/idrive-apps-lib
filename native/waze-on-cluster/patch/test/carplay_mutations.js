// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Perturbations of the reference build (carplay 22.28.3), in memory, the way
 * another build could differ, for mutation_test.js; and builds an older
 * release of this tool patched, for the undo tests: the reference perturbed
 * where that release's locator found a site the current one refuses, with the
 * bytes that release wrote. Nothing here holds BMW bytes; everything is
 * derived from the buffer passed in.
 *
 *   require('./carplay_mutations.js')(REF) -> { LOC, EXPECT, sites, CASES, MAP_CASES, oldRelease }
 *     CASES: [name, mutate(buf) -> buf' or nothing, allowed outcomes, expected sites]
 *     MAP_CASES: [name, mutate(buf), allowed outcomes of the map URL edit,
 *                 { slotPatched, leaPatched } expected when found]
 *     oldRelease(): [{ name, original, patched }]
 */
'use strict';

var locator = require('../carplay_locate.js');

module.exports = function (REF) {
function json(x) { return JSON.stringify(x); }

/* ---- ELF helpers (program headers, .eh_frame_hdr, rel32) ---------------- */

function phdrs(buf) {
    var phoff = buf.readUInt32LE(0x20), n = buf.readUInt16LE(0x38), out = [];
    for (var i = 0; i < n; i++) {
        var o = phoff + i * 56;
        out.push({ hdr: o, type: buf.readUInt32LE(o), off: buf.readUInt32LE(o + 8), va: buf.readUInt32LE(o + 16), size: buf.readUInt32LE(o + 32) });
    }
    return out;
}
function vaOf(buf, off) {
    var l = phdrs(buf).filter(function (p) { return p.type === 1 && off >= p.off && off < p.off + p.size; })[0];
    return l ? l.va + off - l.off : null;
}
function offOf(buf, va) {
    var l = phdrs(buf).filter(function (p) { return p.type === 1 && va >= p.va && va < p.va + p.size; })[0];
    return l ? l.off + va - l.va : null;
}
function starts(buf) {
    var eh = phdrs(buf).filter(function (p) { return p.type === 0x6474e550; })[0], n = buf.readUInt32LE(eh.off + 8), s = [];
    for (var i = 0; i < n; i++) s.push(offOf(buf, (eh.va + buf.readInt32LE(eh.off + 12 + i * 8)) >>> 0));
    return s.sort(function (a, b) { return a - b; });
}
var STARTS = starts(REF);
function func(off) {
    for (var i = STARTS.length - 1; i >= 0; i--) if (STARTS[i] <= off) return { off: STARTS[i], end: STARTS[i + 1] };
    return null;
}
/* The target of the rel32 at `pos` whose instruction ends at `next`. */
function relTo(buf, pos, next) { return offOf(buf, (vaOf(buf, next) + buf.readInt32LE(pos)) >>> 0); }
function setRel(buf, pos, next, to) { buf.writeInt32LE(vaOf(buf, to) - vaOf(buf, next), pos); }
function callsIn(buf, f) {
    var out = [];
    for (var p = f.off; p + 5 <= f.end; p++) if (buf[p] === 0xe8) out.push({ at: p, to: relTo(buf, p + 1, p + 5) });
    return out;
}
/* rip-relative lea r64 in f: [{ at (REX byte), to }] */
function leasIn(buf, f) {
    var out = [];
    for (var p = f.off + 1; p + 6 <= f.end; p++) {
        if (buf[p] === 0x8d && (buf[p - 1] & 0xfb) === 0x48 && (buf[p + 1] & 0xc7) === 0x05) out.push({ at: p - 1, to: relTo(buf, p + 2, p + 6) });
    }
    return out;
}
function cstr(buf, off) { return buf.toString('latin1', off, buf.indexOf(0, off)); }
function strOff(buf, s) { var i = buf.indexOf(Buffer.from(s + '\0', 'latin1')); return i; }

/* ---- the reference's sites ---------------------------------------------- */

var LOC = locator.locate(REF);
if (!LOC.ok) throw new Error('reference build not located: ' + LOC.reason);

function sites(loc) {
    return {
        slot: loc.variant.slot, high: loc.variant.high, sink: loc.sink.offset, call: loc.selfstart.offset,
        callPatched: loc.selfstart.patched, requestStream: loc.selfstart.requestStream,
        mm: loc.res.mm, px: loc.res.px, safe1: loc.res.safe1, safe2: loc.res.safe2,
    };
}
var EXPECT = sites(LOC);
function shifted(d) { var e = {}; Object.keys(EXPECT).forEach(function (k) { e[k] = /^(high|callPatched)$/.test(k) ? EXPECT[k] : EXPECT[k] + d; }); return e; }

var SS = LOC.selfstart.setStreamStatus, RS = LOC.selfstart.requestStream, OSU = LOC.selfstart.onSuggestUi;
var BUILDER = LOC.res.builder;

/* The converter switch (the jump through the table the variant site is in). */
var SWITCH = (function () {
    for (var p = 0; p + 24 <= REF.length; p++) {
        if (REF[p] === 0x80 && REF[p + 1] === 0x3f && REF[p + 3] === 0x77 && REF[p + 8] === 0x48 && REF[p + 9] === 0x8d && REF[p + 10] === 0x15 &&
            relTo(REF, p + 11, p + 15) === LOC.variant.table) return p;
    }
    return null;
})();
var CONV = func(SWITCH);
/* The ICSafeAreaProvider constructor: the caller of the IC_HIGH builder. */
var CTOR_CALL_HIGH = (function () {
    for (var p = 0; p + 5 <= REF.length; p++) if (REF[p] === 0xe8 && relTo(REF, p + 1, p + 5) === BUILDER) return p;
    return null;
})();
var CTOR = func(CTOR_CALL_HIGH);
var CTOR_CALLS = callsIn(REF, CTOR);
function isBuilder(to) { return to !== null && REF[to] === 0x48 && REF[to + 1] === 0xba; }
var MID_BUILDER = CTOR_CALLS.filter(function (c) { return isBuilder(c.to) && c.to !== BUILDER; })[0].to;
/* The two variant checks the constructor calls: 64-byte functions comparing [rdi]. */
function checkKind(f) {
    var fn = func(f); if (!fn || fn.off !== f || fn.end - fn.off > 128) return null;
    var b = REF.slice(fn.off, fn.end);
    return b.indexOf(Buffer.from('8b0785c00f94c0', 'hex')) >= 0 ? 'high' : b.indexOf(Buffer.from('833f010f94c0', 'hex')) >= 0 ? 'mid' : null;
}
var CHECK_HIGH = CTOR_CALLS.filter(function (c) { return checkKind(c.to) === 'high'; })[0];
var CHECK_MID = CTOR_CALLS.filter(function (c) { return checkKind(c.to) === 'mid'; })[0];

/* A function far from every site, overwritten to plant code (the locator
 * reads code; nothing runs). */
var VICTIM = (function () {
    for (var i = 0; i < STARTS.length - 1; i++) {
        var s = STARTS[i], e = STARTS[i + 1];
        if (s >= 0x100000 && s < 0x170000 && e - s >= 0x400) return { off: s, end: e };
    }
    return null;
})();
var VICTIM2 = (function () {
    for (var i = 0; i < STARTS.length - 1; i++) {
        var s = STARTS[i], e = STARTS[i + 1];
        if (s >= 0x170000 && s < 0x180000 && e - s >= 0x100) return { off: s, end: e };
    }
    return null;
})();
/* A short function, the size of a builder. */
var SHORT = (function () {
    for (var i = 0; i < STARTS.length - 1; i++) {
        var s = STARTS[i], e = STARTS[i + 1];
        if (s >= 0x100000 && s < 0x170000 && e - s >= 0x70 && e - s <= 0x80) return { off: s, end: e };
    }
    return null;
})();
if (!(SWITCH !== null && CONV && CTOR && MID_BUILDER && CHECK_HIGH && CHECK_MID && VICTIM && VICTIM2 && SHORT)) throw new Error('mutation anchors not found in the reference');

/* Copy [from, from+len) to `to`, fixing every call/rip-lea rel32 so it keeps
 * its target. Returns nothing; the caller fixes what should change. */
function plant(b, from, len, to) {
    REF.copy(b, to, from, from + len);
    var f = { off: from, end: from + len };
    callsIn(REF, f).forEach(function (c) { if (c.to !== null) setRel(b, c.at - from + to + 1, c.at - from + to + 5, c.to); });
    leasIn(REF, f).forEach(function (l) { if (l.to !== null) setRel(b, l.at - from + to + 3, l.at - from + to + 7, l.to); });
}

/* ---- the cases -------------------------------------------------------- */

/* Each: [name, mutate(b) -> b' (or nothing: b changed in place), allowed outcomes,
 *        expected sites when located (default the reference's)] */
var CASES = [
    ['code and data at other file offsets and addresses', function (b) {
        var pad = 0x1000, out = Buffer.concat([Buffer.alloc(pad), b]);
        b.copy(out, 0, 0, 0x40 + 56 * b.readUInt16LE(0x38));
        phdrs(out).forEach(function (p) {
            out.writeUInt32LE(p.off + pad, p.hdr + 8);
            if (p.type !== 6) { out.writeUInt32LE(p.va + 0x400000, p.hdr + 16); out.writeUInt32LE(p.va + 0x400000, p.hdr + 24); }
            /* the dynamic table's relocation-table addresses move with them */
            if (p.type === 2) for (var o = p.off + pad; o + 16 <= p.off + pad + p.size && out.readUInt32LE(o) !== 0; o += 16) {
                if (out.readUInt32LE(o) === 7 || out.readUInt32LE(o) === 23) out.writeUInt32LE(out.readUInt32LE(o + 8) + 0x400000, o + 8);
            }
        });
        return out;
    }, ['correct'], shifted(0x1000)],

    ['requestStream\'s name string duplicated', function (b) {
        return Buffer.concat([b, Buffer.from('\0' + cstr(b, strOff(b, 'void carplay::navi_handler::AlternateDisplays::requestStream()')) + '\0', 'latin1')]);
    }, ['refused']],

    ['the converter\'s name string duplicated', function (b) {
        var s = b.indexOf('codingToInternal(const CodingKOMBI_HUD::eKI_KOMBI_VARIANT&');
        while (b[s - 1] !== 0) s--;
        return Buffer.concat([b, Buffer.from('\0' + cstr(b, s) + '\0', 'latin1')]);
    }, ['correct', 'refused']],

    ['a copy of the converter sharing its table', function (b) {
        plant(b, CONV.off, CONV.end - CONV.off, VICTIM.off);
    }, ['correct', 'refused']],

    ['a decoy converter with its own table, the real one compiled differently', function (b) {
        var n = b[SWITCH + 2] + 1, table = LOC.variant.table, len = CONV.end - CONV.off;
        plant(b, CONV.off, len, VICTIM.off);
        var name = leasIn(b, { off: VICTIM.off, end: VICTIM.off + len }).filter(function (l) { return /codingToInternal/.test(cstr(b, l.to)); });
        name.forEach(function (l) { setRel(b, l.at + 3, l.at + 7, VICTIM2.off); });   /* the decoy logs no name */
        var copy = VICTIM.off + ((len + 15) & ~15);
        for (var i = 0; i < n; i++) b.writeInt32LE(vaOf(b, relTo(REF, table + i * 4, table)) - vaOf(b, copy), copy + i * 4);   /* entries rebased onto the copy */
        var sw = SWITCH - CONV.off + VICTIM.off;
        setRel(b, sw + 11, sw + 15, copy);
        b[SWITCH + 7] = 0x0f;   /* the real one: movzx ecx,byte [rdi] */
    }, ['refused']],

    ['IC_HIGH and IC_MID swapped in the converter', function (b) {
        var t = LOC.variant.table, s9 = b.readUInt32LE(t + 36);
        b.writeUInt32LE(b.readUInt32LE(t + 40), t + 36); b.writeUInt32LE(s9, t + 40);
    }, ['refused']],

    ['a new internal value for IC_HIGH', function (b) {
        var h = relTo(b, LOC.variant.table + 36, LOC.variant.table); b.writeUInt32LE(4, h + 2);
    }, ['refused']],

    ['the IC_HIGH check inverted (setne)', function (b) {
        var f = func(CHECK_HIGH.to), i = b.indexOf(Buffer.from('0f94c0', 'hex'), f.off); b[i + 1] = 0x95;
    }, ['refused']],

    ['the constructor\'s two builder calls swapped', function (b) {
        var mid = CTOR_CALLS.filter(function (c) { return c.to === MID_BUILDER; })[0];
        setRel(b, CTOR_CALL_HIGH + 1, CTOR_CALL_HIGH + 5, MID_BUILDER);
        setRel(b, mid.at + 1, mid.at + 5, BUILDER);
    }, ['correct', 'refused'], (function () {
        var e = JSON.parse(json(EXPECT)), d = MID_BUILDER - BUILDER;
        e.mm += d; e.px += d; e.safe1 += d; e.safe2 += d; return e;
    })()],

    ['the constructor\'s IC_HIGH and IC_MID check calls swapped', function (b) {
        setRel(b, CHECK_HIGH.at + 1, CHECK_HIGH.at + 5, CHECK_MID.to);
        setRel(b, CHECK_MID.at + 1, CHECK_MID.at + 5, CHECK_HIGH.to);
    }, ['correct', 'refused'], (function () {
        var e = JSON.parse(json(EXPECT)), d = MID_BUILDER - BUILDER;
        e.mm += d; e.px += d; e.safe1 += d; e.safe2 += d; return e;
    })()],

    ['a third geometry builder called by the constructor', function (b) {
        plant(b, BUILDER, 0x70, SHORT.off);
        var other = CTOR_CALLS.filter(function (c) { var f = func(c.to); return f && f.off === c.to && !isBuilder(c.to) && !checkKind(c.to) && f.end - f.off > 0x100; })[0];
        setRel(b, other.at + 1, other.at + 5, SHORT.off);
    }, ['refused']],

    ['the mm pair loaded from memory, 48 ba inside the displacement', function (b) {
        /* mov rdx,[rip+0xba48] ; nop3 in place of movabs rdx,imm64 */
        Buffer.from('488b1548ba00000f1f00', 'hex').copy(b, BUILDER);
    }, ['refused']],

    ['a getter reads another field', function (b) {
        var i = b.indexOf(Buffer.from('0fb7472c', 'hex'), BUILDER); b[i + 3] = 0x2a;
    }, ['refused']],

    ['AVAILABLE changed to another status', function (b) { b[LOC.selfstart.offset - 12] = 0x03; }, ['refused']],

    ['onSuggestUi calls setStreamStatus twice', function (b) {
        var c = callsIn(REF, { off: OSU, end: LOC.selfstart.offset }).filter(function (x) { return x.to !== null && x.to !== SS; });
        setRel(b, c[c.length - 1].at + 1, c[c.length - 1].at + 5, SS);
    }, ['refused']],

    ['requestStream inlined into its caller', function (b) {
        var caller = null;
        for (var p = 0; p + 5 <= 0x338000; p++) if (b[p] === 0xe8 && relTo(b, p + 1, p + 5) === RS) { caller = func(p); break; }
        var cl = leasIn(REF, caller), rl = leasIn(REF, { off: RS, end: func(RS).end });
        var rsName = strOff(REF, 'void carplay::navi_handler::AlternateDisplays::requestStream()');
        var named = cl.filter(function (l) { return / carplay::navi_handler::AlternateDisplays::/.test(cstr(REF, l.to)); });
        var others = cl.filter(function (l) { return named.indexOf(l) < 0; });
        /* the caller loads requestStream's name and the two check strings; requestStream loads the caller's name */
        named.forEach(function (l) { setRel(b, l.at + 3, l.at + 7, rsName); });
        rl.filter(function (l) { return l.to === rsName; }).forEach(function (l) { setRel(b, l.at + 3, l.at + 7, named[0].to); });
        setRel(b, others[0].at + 3, others[0].at + 7, b.indexOf('mCinemoCarPlay->ShowUI('));
        setRel(b, others[1].at + 3, others[1].at + 7, b.indexOf('mCinemoCarPlay->ForceKeyframe('));
        if (!callsIn(REF, caller).some(function (c) { return c.to === SS; })) throw new Error('caller does not call setStreamStatus');
    }, ['refused']],

    ['requestStream\'s name loaded only deep inside it (a split-off part)', function (b) {
        var rsName = strOff(REF, 'void carplay::navi_handler::AlternateDisplays::requestStream()');
        var l = leasIn(REF, { off: RS, end: func(RS).end }).filter(function (x) { return x.to === rsName; })[0];
        setRel(b, l.at + 3, l.at + 7, strOff(REF, 'void carplay::navi_handler::AlternateDisplays::stopStream()'));
    }, ['refused']],

    ['a second sink URL slot', function (b) {
        return Buffer.concat([b, Buffer.from('\0' + locator.SINK_STOCK + '\0\0\0\0\0mCarPlay->GetDisplaySource( src, uuid)\0', 'latin1')]);
    }, ['refused']],

    ['a string merged into the tail of the sink slot', function (b) {
        var l = leasIn(REF, VICTIM2)[0];
        setRel(b, l.at + 3, l.at + 7, LOC.sink.offset + locator.SINK_STOCK.indexOf('mime_type'));
    }, ['refused']],

    ['a dynamic relocation over the sink slot', function (b) {
        var shoff = b.readUInt32LE(0x28), n = b.readUInt16LE(0x3c);
        for (var i = 0; i < n; i++) {
            var o = shoff + i * 64;
            if (b.readUInt32LE(o + 4) === 4) { b.writeUInt32LE(vaOf(b, LOC.sink.offset), b.readUInt32LE(o + 24)); return; }
        }
        throw new Error('no .rela section');
    }, ['refused']],
];

/* ---- the map URL edit --------------------------------------------------- */

var MAP = LOC.mapurl;
if (!MAP.ok) throw new Error('map URL not located in the reference: ' + MAP.reason);
/* The first RELA entry, from the section headers. */
function firstRela(b) {
    var shoff = b.readUInt32LE(0x28), n = b.readUInt16LE(0x3c);
    for (var i = 0; i < n; i++) { var o = shoff + i * 64; if (b.readUInt32LE(o + 4) === 4) return b.readUInt32LE(o + 24); }
    throw new Error('no .rela section');
}
/* The log block's je: in the log function, the je rel32 landing within 0x40
 * bytes before the log string's lea. */
var LOG_FN = func(MAP.log), LOG_JE = null, GATE_CMP = null;
for (var q = LOG_FN.off; q + 6 <= LOG_FN.end; q++) {
    if (REF[q] === 0x0f && REF[q + 1] === 0x84) { var t = q + 6 + REF.readInt32LE(q + 2); if (t <= MAP.log && MAP.log - t < 0x40) LOG_JE = { at: q, to: t }; }
}
callsIn(REF, LOG_FN).forEach(function (c) {
    var g = func(c.to), i = g && REF.indexOf(Buffer.from('83fe010f96c0', 'hex'), g.off);
    if (g && i >= 0 && i < g.end) GATE_CMP = i;
});
var URL_G = (vaOf(REF, MAP.lea + 14) + REF.readInt32LE(MAP.lea + 10)) >>> 0;
if (!LOG_JE || GATE_CMP === null) throw new Error('map URL anchors not found in the reference');

var MAP_CASES = [
    ['the map URL edit written', function (b) {
        Buffer.from(MAP.slotBytes, 'hex').copy(b, MAP.slot); Buffer.from(MAP.leaBytes, 'hex').copy(b, MAP.lea);
    }, ['correct'], { slotPatched: true, leaPatched: true }],
    ['only the initial URL\'s lea written', function (b) { Buffer.from(MAP.leaBytes, 'hex').copy(b, MAP.lea); }, ['correct'], { slotPatched: false, leaPatched: true }],
    ['the log string duplicated', function (b) {
        b[VICTIM.off] = 0; Buffer.from('Head-Unit does not support Alternate Displays feature.\0', 'latin1').copy(b, VICTIM.off + 1);
    }, ['refused']],
    ['the log string read a second time', function (b) {
        var l = leasIn(REF, VICTIM2)[0]; setRel(b, l.at + 3, l.at + 7, MAP.slot);
    }, ['refused']],
    ['the gate says variant <= 2', function (b) { b[GATE_CMP + 2] = 2; }, ['refused']],
    ['the log block on the gate\'s true branch (jne)', function (b) { b[LOG_JE.at + 1] = 0x85; }, ['refused']],
    ['a jump into the log block from elsewhere', function (b) {
        b[VICTIM.off] = 0xe9; setRel(b, VICTIM.off + 1, VICTIM.off + 5, LOG_JE.to);
    }, ['refused']],
    ['the initial URL global built a second time', function (b) {
        REF.copy(b, VICTIM.off, MAP.lea, MAP.lea + 19);
        setRel(b, VICTIM.off + 3, VICTIM.off + 7, offOf(REF, (vaOf(REF, MAP.lea + 7) + REF.readInt32LE(MAP.lea + 3)) >>> 0));
        b.writeInt32LE((URL_G - vaOf(b, VICTIM.off + 14)) | 0, VICTIM.off + 10);
    }, ['refused']],
    ['the cluster display builder loads another global', function (b) {
        for (var p = MAP.builder; p < MAP.builder + 0x400; p++) {
            if ((b[p] & 0xfb) === 0x48 && b[p + 1] === 0x8d && (b[p + 2] & 0xc7) === 5 && ((vaOf(b, p + 7) + b.readInt32LE(p + 3)) >>> 0) === URL_G) {
                b.writeInt32LE(b.readInt32LE(p + 3) + 0x400, p + 3); return;
            }
        }
        throw new Error('the builder\'s lea not found');
    }, ['refused']],
    ['the initial URL built from another string', function (b) { setRel(b, MAP.lea + 3, MAP.lea + 7, MAP.slot + 4); }, ['refused']],
    ['a data pointer into the log string', function (b) { b.writeUInt32LE(vaOf(b, MAP.slot) + 4, firstRela(b) + 16); b.writeUInt32LE(0, firstRela(b) + 20); }, ['refused']],
    ['a dynamic relocation over the initial URL\'s lea', function (b) { b.writeUInt32LE(vaOf(b, MAP.lea), firstRela(b)); }, ['refused']],
];

/* ---- builds an older release patched ------------------------------------ */

/* The edits that release wrote where its locator found the sites: the variant
 * slot set to its IC_HIGH value, the self-start call, the default geometry
 * (138x61 mm, 1088x480, view 0,0, safe 504x368 at 286,72). */
function oldPatch(b, at) {
    b.writeUInt32LE(at.high, at.slot);
    Buffer.from(at.call, 'hex').copy(b, LOC.selfstart.offset);
    [[at.mm, 138, 61], [LOC.res.px, 1088, 480], [LOC.res.safe1, 0, 504], [LOC.res.safe2, 368, (72 << 16 | 286) >>> 0]].forEach(function (g) {
        b.writeUInt32LE(g[1], g[0]); b.writeUInt32LE(g[2], g[0] + 4);
    });
}
var STOCK = { slot: LOC.variant.slot, high: LOC.variant.high, call: LOC.selfstart.patched, mm: LOC.res.mm };
function variantOf(over) { var a = {}; Object.keys(STOCK).forEach(function (k) { a[k] = over[k] !== undefined ? over[k] : STOCK[k]; }); return a; }
function mutated(n) {
    var b = Buffer.from(REF);
    return CASES.filter(function (c) { return c[0] === n; })[0][1](b) || b;
}
function oldRelease() {
    var out = [];
    /* the decoy converter's slot 7 set to its slot 9 */
    var orig = mutated('a decoy converter with its own table, the real one compiled differently');
    var decoy = VICTIM.off + (((CONV.end - CONV.off) + 15) & ~15);
    out.push({ name: 'a decoy converter patched', original: orig, at: variantOf({ slot: decoy + 28, high: orig.readUInt32LE(decoy + 36) }) });
    /* 48 ba inside a displacement: the mm pair written 3 bytes into the code */
    orig = mutated('the mm pair loaded from memory, 48 ba inside the displacement');
    out.push({ name: 'a code displacement patched as the mm pair', original: orig, at: variantOf({ mm: BUILDER + 5 }) });
    /* requestStream inlined: onSuggestUi's call retargeted to its caller */
    orig = mutated('requestStream inlined into its caller');
    var caller = null;
    for (var p = 0; p + 5 <= 0x338000 && !caller; p++) if (orig[p] === 0xe8 && relTo(orig, p + 1, p + 5) === RS) caller = func(p);
    var call = Buffer.alloc(5); call[0] = 0xe8; call.writeInt32LE(vaOf(orig, caller.off) - vaOf(orig, LOC.selfstart.offset + 5), 1);
    out.push({ name: 'onSuggestUi retargeted to requestStream\'s caller', original: orig, at: variantOf({ call: call.toString('hex') }) });
    return out.map(function (f) {
        var b = Buffer.from(f.original); oldPatch(b, f.at);
        return { name: f.name, original: f.original, patched: b, at: f.at };
    });
}

return { LOC: LOC, EXPECT: EXPECT, sites: sites, CASES: CASES, MAP_CASES: MAP_CASES, oldRelease: oldRelease };
};
