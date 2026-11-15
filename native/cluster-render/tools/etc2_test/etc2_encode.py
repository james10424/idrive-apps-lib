#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ETC2 encoder and decoder for etc2rgba and etc2rgb assets (PROTOCOL.md)
and the ETC2 car test (TEST-ETC2.md). Small, numpy alone.

Colour uses ETC1's individual and differential modes only (no T, H or
planar), which every ETC2 decoder reads as ETC2 blocks; differential deltas
never overflow, so no block falls into T/H/planar by accident. Colour error
is weighted by alpha. Alpha is EAC (all 16 tables, multipliers 1-15). Each
16-byte block is the EAC alpha block, then the colour block, both
big-endian; blocks in rows top to bottom (GL_COMPRESSED_RGBA8_ETC2_EAC).
etc2rgb is the colour blocks alone, 8 bytes each, every pixel opaque
(GL_COMPRESSED_RGB8_ETC2).

  etc2_encode.py frame ATLAS.rgba ATLAS.json OUTDIR   cut the close-up's
      middle pose, pad to 4x4 blocks, write frame.rgba, frame.etc2 and PNGs
  etc2_encode.py atlas ATLAS.rgba ATLAS.json OUTDIR   the close-up atlas
      twice side by side, 2048x1024, edges marked: atlas.etc2 and PNGs
  etc2_encode.py encode IN.rgba W H OUT.etc2 [etc2rgb]
  etc2_encode.py decode IN.etc2 W H OUT.rgba [etc2rgb]
"""
import json
import os
import sys

import numpy as np

ETC1_TABLES = np.array([[2, 8], [5, 17], [9, 29], [13, 42], [18, 60], [24, 80], [33, 106], [47, 183]])
# Modifier index (msb << 1 | lsb): 0 +a, 1 +b, 2 -a, 3 -b.
ETC1_MODS = np.stack([ETC1_TABLES[:, 0], ETC1_TABLES[:, 1], -ETC1_TABLES[:, 0], -ETC1_TABLES[:, 1]], 1)  # (8, 4)
EAC_TABLES = np.array([
    [-3, -6, -9, -15, 2, 5, 8, 14], [-3, -7, -10, -13, 2, 6, 9, 12], [-2, -5, -8, -13, 1, 4, 7, 12],
    [-2, -4, -6, -13, 1, 3, 5, 12], [-3, -6, -8, -12, 2, 5, 7, 11], [-3, -7, -9, -11, 2, 6, 8, 10],
    [-4, -7, -8, -11, 3, 6, 7, 10], [-3, -5, -8, -11, 2, 4, 7, 10], [-2, -6, -8, -10, 1, 5, 7, 9],
    [-2, -5, -8, -10, 1, 4, 7, 9], [-2, -4, -8, -10, 1, 3, 7, 9], [-2, -5, -7, -10, 1, 4, 6, 9],
    [-3, -4, -7, -10, 2, 3, 6, 9], [-1, -2, -3, -10, 0, 1, 2, 9], [-4, -6, -8, -9, 3, 5, 7, 8],
    [-3, -5, -7, -9, 2, 4, 6, 8]])

# Sub-blocks, as pixel indices: flip 0 left/right 2x4, flip 1 top/bottom 4x2.
SUBS = {0: ([i for i in range(16) if i // 4 < 2], [i for i in range(16) if i // 4 >= 2]),
        1: ([i for i in range(16) if i % 4 < 2], [i for i in range(16) if i % 4 >= 2])}
OFFS = np.array([(a, b, c) for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1)])  # (27, 3)


def blocks_of(img):
    """(H, W, 4) -> (B, 16, 4) in index order, blocks in rows."""
    h, w, _ = img.shape
    b = img.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 3, 1, 4)  # by, bx, x, y, c
    return b.reshape(-1, 16, 4)


def unblock(b, w, h):
    return b.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 3, 1, 2, 4).reshape(h, w, 4)


def sub_errors(P, W, bases):
    """P (B, 8, 3), W (B, 8), bases (B, K, 3) expanded colours: per base and
    table, the error and the pixels' modifier indices."""
    val = np.clip(bases[:, :, None, None, None, :] + ETC1_MODS[None, None, :, :, None, None], 0, 255)
    d = P[:, None, None, None, :, :] - val  # B K T 4 8 3
    e = (d * d).sum(-1) * W[:, None, None, None, :]  # B K T 4 8
    idx = e.argmin(3)  # B K T 8
    err = e.min(3).sum(-1)  # B K T
    return err, idx


def encode_color(rgb, a):
    B = rgb.shape[0]
    rgb = rgb.astype(np.float64)
    W = a.astype(np.float64) / 255.0 + 0.002
    best_err = np.full(B, np.inf)
    best = [None] * B
    for flip in (0, 1):
        res = []
        for s in SUBS[flip]:
            P, w = rgb[:, s, :], W[:, s]
            avg = (P * w[..., None]).sum(1) / w.sum(1)[:, None]
            q4 = np.clip(np.round(avg / 17)[:, None, :] + OFFS[None], 0, 15)
            q5 = np.clip(np.round(avg * 31 / 255)[:, None, :] + OFFS[None], 0, 31)
            e4, i4 = sub_errors(P, w, q4 * 17)
            e5, i5 = sub_errors(P, w, (q5 * 8 + np.floor(q5 / 4)))
            res.append((q4, e4, i4, q5, e5, i5))
        # Individual: each sub-block on its own.
        e1 = res[0][1].reshape(B, -1)
        e2 = res[1][1].reshape(B, -1)
        m1, m2 = e1.argmin(1), e2.argmin(1)
        ind_err = e1[np.arange(B), m1] + e2[np.arange(B), m2]
        # Differential: pairs whose delta is -4..3 in every channel.
        q51, q52 = res[0][3], res[1][3]
        dl = q52[:, None, :, :] - q51[:, :, None, :]  # B 27 27 3
        okp = ((dl >= -4) & (dl <= 3)).all(-1)
        ea = res[0][4].min(2)  # B 27 (best table)
        eb = res[1][4].min(2)
        pair = ea[:, :, None] + eb[:, None, :]
        pair = np.where(okp, pair, np.inf)
        flat = pair.reshape(B, -1)
        pm = flat.argmin(1)
        dif_err = flat[np.arange(B), pm]
        for bi in range(B):
            if ind_err[bi] < best_err[bi]:
                k1, t1 = divmod(m1[bi], 8)
                k2, t2 = divmod(m2[bi], 8)
                best_err[bi] = ind_err[bi]
                best[bi] = (flip, 0, res[0][0][bi, k1], res[1][0][bi, k2], t1, t2,
                            res[0][2][bi, k1, t1], res[1][2][bi, k2, t2])
            if dif_err[bi] < best_err[bi]:
                k1, k2 = divmod(pm[bi], 27)
                t1 = res[0][4][bi, k1].argmin()
                t2 = res[1][4][bi, k2].argmin()
                best_err[bi] = dif_err[bi]
                best[bi] = (flip, 1, res[0][3][bi, k1], res[1][3][bi, k2], t1, t2,
                            res[0][5][bi, k1, t1], res[1][5][bi, k2, t2])
    out = np.zeros((B, 8), np.uint8)
    for bi, (flip, diff, c1, c2, t1, t2, i1, i2) in enumerate(best):
        c1 = [int(x) for x in c1]
        c2 = [int(x) for x in c2]
        v = 0
        if diff:
            d = [c2[ch] - c1[ch] for ch in range(3)]
            for ch in range(3):
                v |= (c1[ch] << 3 | (d[ch] & 7)) << (56 - 8 * ch)
        else:
            for ch in range(3):
                v |= (c1[ch] << 4 | c2[ch]) << (56 - 8 * ch)
        v |= int(t1) << 37 | int(t2) << 34 | int(diff) << 33 | int(flip) << 32
        for s, ids in zip(SUBS[flip], (i1, i2)):
            for p, m in zip(s, ids):
                v |= (int(m) >> 1) << (16 + p) | (int(m) & 1) << p
        out[bi] = np.frombuffer(v.to_bytes(8, 'big'), np.uint8)
    return out


def encode_alpha(a):
    B = a.shape[0]
    a = a.astype(np.int64)
    out = np.zeros((B, 8), np.uint8)
    mults = np.arange(1, 16)
    lo, hi = EAC_TABLES.min(1), EAC_TABLES.max(1)  # (16,)
    for bi in range(B):
        px = a[bi]
        mn, mx = px.min(), px.max()
        if mn == mx:  # one value: table 13 has 0 at index 4
            base, mul, tab, idx = int(mn), 1, 13, np.full(16, 4)
        else:
            centre = np.round((mn + mx) / 2 - (lo + hi)[:, None] * mults[None, :] / 2)  # T M
            bases = np.clip(centre[..., None] + np.arange(-3, 4), 0, 255)  # T M 7
            mod = EAC_TABLES[:, None, None, :] * mults[None, :, None, None]  # T M 1 8
            val = np.clip(bases[..., None] + mod, 0, 255)  # T M 7 8
            e = (px[None, None, None, None, :] - val[..., None]) ** 2  # T M 7 8 16
            err = e.min(3).sum(-1)
            t, m, k = np.unravel_index(err.argmin(), err.shape)
            base, mul, tab = int(bases[t, m, k]), int(mults[m]), int(t)
            idx = e[t, m, k].argmin(0)
        v = base << 56 | mul << 52 | tab << 48
        for p in range(16):
            v |= int(idx[p]) << (45 - 3 * p)
        out[bi] = np.frombuffer(v.to_bytes(8, 'big'), np.uint8)
    return out


def encode(img, chunk=1024, fmt='etc2rgba'):
    """(H, W, 4) uint8, H and W multiples of 4 -> bytes. etc2rgb: the colour
    blocks alone, alpha ignored."""
    b = blocks_of(img)
    out = []
    for i in range(0, b.shape[0], chunk):
        c = b[i:i + chunk]
        if fmt == 'etc2rgb':
            out.append(encode_color(c[:, :, :3], np.full(c.shape[:2], 255)))
        else:
            out.append(np.concatenate([encode_alpha(c[:, :, 3]), encode_color(c[:, :, :3], c[:, :, 3])], 1))
    return np.concatenate(out).tobytes()


def decode(data, w, h, fmt='etc2rgba'):
    rgb = fmt == 'etc2rgb'
    raw = np.frombuffer(data, np.uint8).reshape(-1, 8 if rgb else 16)
    out = np.zeros((raw.shape[0], 16, 4), np.uint8)
    for bi, blk in enumerate(raw):
        if rgb:
            out[bi, :, 3] = 255
        else:
            av = int.from_bytes(blk[:8].tobytes(), 'big')
            base, mul, tab = av >> 56, (av >> 52) & 15, (av >> 48) & 15
            for p in range(16):
                out[bi, p, 3] = min(255, max(0, base + EAC_TABLES[tab][(av >> (45 - 3 * p)) & 7] * mul))
        v = int.from_bytes(blk[-8:].tobytes(), 'big')
        diff, flip = (v >> 33) & 1, (v >> 32) & 1
        if diff:
            c1 = [(v >> (59 - 8 * ch)) & 31 for ch in range(3)]
            d = [((v >> (56 - 8 * ch)) & 7) for ch in range(3)]
            d = [x - 8 if x >= 4 else x for x in d]
            c2 = [c1[ch] + d[ch] for ch in range(3)]
            assert all(0 <= x <= 31 for x in c2), 'T/H/planar block: not written by this encoder'
            ex = lambda q: (q << 3) | (q >> 2)
        else:
            c1 = [(v >> (60 - 8 * ch)) & 15 for ch in range(3)]
            c2 = [(v >> (56 - 8 * ch)) & 15 for ch in range(3)]
            ex = lambda q: q * 17
        tabs = [(v >> 37) & 7, (v >> 34) & 7]
        for si, s in enumerate(SUBS[flip]):
            base = [ex(x) for x in (c1 if si == 0 else c2)]
            for p in s:
                m = ((v >> (16 + p)) & 1) << 1 | ((v >> p) & 1)
                for ch in range(3):
                    out[bi, p, ch] = min(255, max(0, base[ch] + ETC1_MODS[tabs[si]][m]))
    return unblock(out, w, h)


def psnr(a, b, mask=None):
    d = (a.astype(np.float64) - b.astype(np.float64)) ** 2
    if mask is not None:
        d = d[mask]
    mse = d.mean()
    return float('inf') if mse == 0 else 10 * np.log10(255 * 255 / mse)


def save_png(path, img):
    from PIL import Image
    Image.fromarray(img, 'RGBA').save(path)


def cmd_frame(atlas_rgba, atlas_json, outdir):
    c = json.load(open(atlas_json))['close']
    aw, ah = c['atlas']
    fw, fh = c['frame']
    cw, ch, cols, pad = c['grid']
    n = c['zoomSteps'] + 1
    x, y = (n % cols) * cw + pad, (n // cols) * ch + pad
    atlas = np.fromfile(atlas_rgba, np.uint8)
    assert atlas.size == aw * ah * 4, 'atlas size does not match atlas.json'
    atlas = atlas.reshape(ah, aw, 4)
    pw, ph = (fw + 3) // 4 * 4, (fh + 3) // 4 * 4
    frame = np.zeros((ph, pw, 4), np.uint8)
    frame[:fh, :fw] = atlas[y:y + fh, x:x + fw]
    os.makedirs(outdir, exist_ok=True)
    frame.tofile(os.path.join(outdir, 'frame.rgba'))
    data = encode(frame)
    open(os.path.join(outdir, 'frame.etc2'), 'wb').write(data)
    back = decode(data, pw, ph)
    back.tofile(os.path.join(outdir, 'frame-etc2-decoded.rgba'))
    save_png(os.path.join(outdir, 'frame.png'), frame)
    save_png(os.path.join(outdir, 'frame-etc2-decoded.png'), back)
    vis = frame[..., 3] > 0
    info = {'source': os.path.abspath(atlas_rgba), 'frame_index': n, 'at': [x, y], 'frame': [fw, fh],
            'padded': [pw, ph], 'rgba_bytes': int(frame.size), 'etc2_bytes': len(data),
            'psnr_alpha_db': round(psnr(frame[..., 3], back[..., 3]), 2),
            'psnr_rgb_visible_db': round(psnr(frame[..., :3], back[..., :3], vis), 2),
            'encoder': 'tools/etc2_test/etc2_encode.py: ETC1 individual/differential colour modes '
                       '(alpha-weighted, 3x3x3 base search, 2 flips, 8 tables), EAC alpha '
                       '(16 tables, multipliers 1-15, 7 bases each)'}
    json.dump(info, open(os.path.join(outdir, 'frame.json'), 'w'), indent=1)
    print(json.dumps(info, indent=1))


def cmd_atlas(atlas_rgba, atlas_json, outdir):
    """The close-up atlas twice side by side as one 2048x1024 texture: each
    copy padded to 1024x1024, its padding marked (a bar along the bottom:
    red in the left copy, green in the right) and a white 4 px frame round
    the whole, so its far edges and both halves show on the display."""
    c = json.load(open(atlas_json))['close']
    aw, ah = c['atlas']
    assert aw == 1024 and ah <= 1024 - 16, 'expects a 1024 px wide atlas, at most 1008 high'
    atlas = np.fromfile(atlas_rgba, np.uint8)
    assert atlas.size == aw * ah * 4, 'atlas size does not match atlas.json'
    atlas = atlas.reshape(ah, aw, 4)
    tile = np.zeros((1024, 1024, 4), np.uint8)
    tile[:ah] = atlas
    img = np.concatenate([tile, tile], 1)
    img[1024 - 12:1024 - 4, 4:1024] = (255, 0, 0, 255)
    img[1024 - 12:1024 - 4, 1024:2044] = (0, 255, 0, 255)
    img[:4], img[-4:], img[:, :4], img[:, -4:] = 255, 255, 255, 255
    os.makedirs(outdir, exist_ok=True)
    # Each half encoded on its own: their bars and frame edges differ.
    left = np.frombuffer(encode(img[:, :1024]), np.uint8).reshape(256, 256, 16)
    right = np.frombuffer(encode(img[:, 1024:]), np.uint8).reshape(256, 256, 16)
    data = np.concatenate([left, right], 1).tobytes()
    open(os.path.join(outdir, 'atlas.etc2'), 'wb').write(data)
    back = decode(data, 2048, 1024)
    save_png(os.path.join(outdir, 'atlas.png'), img)
    save_png(os.path.join(outdir, 'atlas-etc2-decoded.png'), back)
    vis = img[..., 3] > 0
    info = {'source': os.path.abspath(atlas_rgba), 'atlas': [aw, ah], 'size': [2048, 1024],
            'etc2_bytes': len(data), 'rgba_bytes': 2048 * 1024 * 4,
            'psnr_alpha_db': round(psnr(img[..., 3], back[..., 3]), 2),
            'psnr_rgb_visible_db': round(psnr(img[..., :3], back[..., :3], vis), 2)}
    json.dump(info, open(os.path.join(outdir, 'atlas.json'), 'w'), indent=1)
    print(json.dumps(info, indent=1))


def main(argv):
    if len(argv) == 5 and argv[1] == 'frame':
        cmd_frame(argv[2], argv[3], argv[4])
    elif len(argv) == 5 and argv[1] == 'atlas':
        cmd_atlas(argv[2], argv[3], argv[4])
    elif len(argv) in (6, 7) and argv[1] == 'encode' and argv[6:] in ([], ['etc2rgb']):
        w, h = int(argv[3]), int(argv[4])
        img = np.fromfile(argv[2], np.uint8).reshape(h, w, 4)
        open(argv[5], 'wb').write(encode(img, fmt=(argv[6:] or ['etc2rgba'])[0]))
    elif len(argv) in (6, 7) and argv[1] == 'decode' and argv[6:] in ([], ['etc2rgb']):
        w, h = int(argv[3]), int(argv[4])
        decode(open(argv[2], 'rb').read(), w, h, (argv[6:] or ['etc2rgba'])[0]).tofile(argv[5])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
