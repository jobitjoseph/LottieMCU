#!/usr/bin/env python3
"""
lottie2mcu.py - convert a Lottie (Bodymovin) JSON animation into the compact
binary format played by the LottieMCU Arduino library.

  python lottie2mcu.py anim.json                 -> anim.h   (C array for PROGMEM / flash)
  python lottie2mcu.py anim.json -o my_anim.h -n my_anim
  python lottie2mcu.py anim.json --bin anim.lmc  (raw binary for SD / LittleFS)

  python lottie2mcu.py anim.json --size 320x240  (store image layers at display size)

Only the Python standard library is needed (Pillow is used when installed:
better image resampling, and required for JPEG/WebP images).

What is converted
  layers    : shape, solid, null, image, precomp (nested, with time offsets)
  shapes    : group, path, rectangle, ellipse, polystar, fill, stroke,
              linear/radial gradient fill & stroke, trim paths, repeaters
  masking   : masks (add/subtract/inverted), track mattes (alpha, inverted)
  animation : bezier-eased and hold keyframes, per-dimension easing and
              curved motion paths (resampled per frame), parenting, in/out
              points, start-time offsets, skew, 3D X/Y rotation (approximated)
Not supported (a warning is printed, the rest still plays)
  effects, text layers, time remap/stretch, merge paths, round corners,
  dashes, expressions, blend modes

Part of LottieMCU 1.1.0
Copyright (c) 2026 Jobit Joseph <jobitjoseph1@gmail.com>
https://github.com/jobitjoseph/LottieMCU
SPDX-License-Identifier: MIT
"""
__version__ = '1.1.0'

import argparse, json, math, os, struct

WARN = {}
def warn(msg):
    WARN[msg] = WARN.get(msg, 0) + 1

# --------------------------------------------------------------------------
# property helpers
# --------------------------------------------------------------------------
def _first(v, d=0.0):
    if isinstance(v, (list, tuple)):
        return float(v[0]) if v else d
    return float(v) if v is not None else d

def _vec(v, dims):
    if not isinstance(v, (list, tuple)):
        v = [v]
    v = [float(x) for x in v]
    while len(v) < dims:
        v.append(v[-1] if v else 0.0)
    return v[:dims]

def is_animated(prop):
    if prop is None:
        return False
    k = prop.get('k')
    if prop.get('a') == 1:
        return isinstance(k, list) and k and isinstance(k[0], dict)
    return isinstance(k, list) and len(k) > 0 and isinstance(k[0], dict) and 't' in k[0]

def norm_keys(prop, conv):
    """Return list of (t, value(list of floats), hold, (ox,oy,ix,iy)).
    conv(raw) -> list of floats.  Handles old ('e') and new keyframe styles."""
    raw = prop['k']
    out = []
    prev_end = None
    for i, kf in enumerate(raw):
        t = float(kf.get('t', 0))
        if 's' in kf:
            val = conv(kf['s'])
        elif prev_end is not None:
            val = prev_end
        elif out:
            val = out[-1][1]
        else:
            continue
        prev_end = conv(kf['e']) if 'e' in kf else None
        hold = 1 if kf.get('h', 0) == 1 else 0
        o = kf.get('o', {}); ii = kf.get('i', {})
        ease = (_first(o.get('x', 0.0)), _first(o.get('y', 0.0)),
                _first(ii.get('x', 1.0), 1.0), _first(ii.get('y', 1.0), 1.0))
        out.append([t, val, hold, ease, kf.get('to'), kf.get('ti'), o, ii])
    # keys with identical time: keep the last one
    dedup = []
    for k in out:
        if dedup and abs(dedup[-1][0] - k[0]) < 1e-6:
            dedup[-1] = k
        else:
            dedup.append(k)
    return dedup

def static_value(prop, conv, default):
    if prop is None:
        return default
    if is_animated(prop):
        keys = norm_keys(prop, conv)
        return keys[0][1] if keys else default
    k = prop.get('k', None)
    if k is None:
        return default
    return conv(k)

class Prop:
    """A property evaluated on the device: default, static or keyframed.

    Encoding:  u8 kind (0 static, 1 keyframed, 2 default value -> nothing follows)
               u16 dims, f32 scale (value = int16 * scale)
      static : dims x i16
      keyed  : u16 count, then per key:
               i16 time*4, u8 flags (bit0 hold, bit1 eased),
               [eased: u8 out.x*255, u8 in.x*255, i16 out.y*4096, i16 in.y*4096],
               dims x i16 values
    """
    def __init__(self, dims, static=None, keys=None, default=None):
        self.dims = dims
        self.static = static
        self.keys = keys
        self.default = default
        if keys is not None and all(k[1] == keys[0][1] for k in keys):
            self.static, self.keys = keys[0][1], None

    @staticmethod
    def from_lottie(prop, dims, default, conv=None):
        if conv is None:
            conv = lambda v: _vec(v, dims)
        d = list(default) if default is not None else None
        if prop is None:
            return Prop(dims, static=list(default), default=d)
        if is_animated(prop):
            keys = norm_keys(prop, conv)
            if len(keys) == 0:
                return Prop(dims, static=list(default), default=d)
            if len(keys) == 1:
                return Prop(dims, static=keys[0][1], default=d)
            for k in keys:
                if len(k[1]) != dims:
                    k[1] = (k[1] + [0.0] * dims)[:dims]
            baked = bake_keys(keys, dims)
            if baked is not None:
                return Prop(dims, keys=baked, default=d)
            return Prop(dims, keys=[k[:4] for k in keys], default=d)
        k = prop.get('k', None)
        if k is None:
            return Prop(dims, static=list(default), default=d)
        return Prop(dims, static=conv(k), default=d)

    def is_default(self):
        return (self.keys is None and self.default is not None and
                len(self.static) == len(self.default) and
                all(abs(a - b) < 1e-6 for a, b in zip(self.static, self.default)))

    def encode(self):
        if self.is_default():
            return b'\x02'
        vals = self.static if self.keys is None else [x for k in self.keys for x in k[1]]
        mx = max([abs(v) for v in vals] + [0.0])
        scale = mx / 32000.0 if mx > 0 else 1.0
        q = lambda v: max(-32767, min(32767, int(round(v / scale))))
        b = bytearray()
        if self.keys is None:
            b += struct.pack('<BHf', 0, self.dims, scale)
            for v in self.static:
                b += struct.pack('<h', q(v))
        else:
            b += struct.pack('<BHfH', 1, self.dims, scale, len(self.keys))
            last = len(self.keys) - 1
            for n, (t, val, hold, (ox, oy, ix, iy)) in enumerate(self.keys):
                ti = max(-32767, min(32767, int(round(t * 4))))
                eased = n < last and not hold and not (abs(ox - oy) < 1e-4 and abs(ix - iy) < 1e-4)
                b += struct.pack('<hB', ti, (1 if hold and n < last else 0) | (2 if eased else 0))
                if eased:
                    u8 = lambda x: max(0, min(255, int(round(x * 255))))
                    i16 = lambda x: max(-32767, min(32767, int(round(x * 4096))))
                    b += struct.pack('<BBhh', u8(ox), u8(ix), i16(oy), i16(iy))
                for v in val:
                    b += struct.pack('<h', q(v))
        return bytes(b)

# --------------------------------------------------------------------------
# records
# --------------------------------------------------------------------------
class Node:
    def __init__(self, parent, op_parent, toff, vis_in, vis_out, tr):
        self.parent, self.op_parent = parent, op_parent
        self.toff, self.vis_in, self.vis_out = toff, vis_in, vis_out
        tr = tr or {}
        self.anchor = Prop.from_lottie(tr.get('a'), 2, [0, 0])
        p = tr.get('p')
        self.split = bool(p and p.get('s'))
        if self.split:
            self.px = Prop.from_lottie(p.get('x'), 1, [0])
            self.py = Prop.from_lottie(p.get('y'), 1, [0])
        else:
            self.pos = Prop.from_lottie(p, 2, [0, 0])
        self.scale = Prop.from_lottie(tr.get('s'), 2, [100, 100])
        self.rot = Prop.from_lottie(tr.get('r', tr.get('rz')), 1, [0])
        self.op = Prop.from_lottie(tr.get('o'), 1, [100])
        orient = _vec(static_value(tr.get('or'), lambda v: _vec(v, 3), [0, 0, 0]), 3)
        self.rx = Prop.from_lottie(tr.get('rx'), 1, [0], conv=lambda v: [_first(v) + orient[0]])
        self.ry = Prop.from_lottie(tr.get('ry'), 1, [0], conv=lambda v: [_first(v) + orient[1]])
        if abs(orient[2]) > 1e-6:
            self.rot = Prop.from_lottie(tr.get('r', tr.get('rz')), 1, [0],
                                        conv=lambda v: [_first(v) + orient[2]])
        self.rot3d = not (self.rx.is_default() and self.ry.is_default())
        if self.rot3d:
            warn('3D X/Y rotation approximated (orthographic, no perspective)')
        self.sk = Prop.from_lottie(tr.get('sk'), 1, [0])
        self.sa = Prop.from_lottie(tr.get('sa'), 1, [0])
        self.skew = not self.sk.is_default()

    def is_identity(self):
        if self.split or self.skew or self.rot3d or not (self.scale.is_default() and self.rot.is_default() and
                                           self.op.is_default()):
            return False
        a, p = self.anchor, self.pos
        if a.keys is not None or p.keys is not None:
            return False
        return all(abs(x - y) < 1e-6 for x, y in zip(a.static, p.static))

    def encode(self):
        b = struct.pack('<hhfffB', self.parent, self.op_parent, self.toff,
                        self.vis_in, self.vis_out, (1 if self.split else 0) | (2 if self.skew else 0) |
                        (4 if self.rot3d else 0))
        b += self.anchor.encode()
        b += (self.px.encode() + self.py.encode()) if self.split else self.pos.encode()
        b += self.scale.encode() + self.rot.encode() + self.op.encode()
        if self.skew:
            b += self.sk.encode() + self.sa.encode()
        if self.rot3d:
            b += self.rx.encode() + self.ry.encode()
        return b

def cubic_ease(x, ox, oy, ix, iy):
    if ox == oy and ix == iy:
        return x
    bz = lambda u, a, b: 3 * (1 - u) ** 2 * u * a + 3 * (1 - u) * u * u * b + u ** 3
    lo, hi = 0.0, 1.0
    for _ in range(40):
        mid = (lo + hi) / 2
        if bz(mid, ox, ix) < x:
            lo = mid
        else:
            hi = mid
    return bz((lo + hi) / 2, oy, iy)

def _ease_list(e, key, n, default):
    v = e.get(key, default) if isinstance(e, dict) else default
    v = v if isinstance(v, list) else [v]
    v = [float(x) for x in v] or [default]
    return (v + [v[-1]] * n)[:n]

def bake_keys(keys, dims):
    """Properties the device cannot evaluate exactly with one easing curve per key
    (curved motion paths, per-dimension easing) are resampled into linear keys,
    one per frame. Returns None when no baking is needed."""
    if len(keys) < 2 or dims > 3:
        return None
    curved = any(any(abs(x) > 1e-3 for x in (k[4] or []) + (k[5] or [])) for k in keys[:-1])
    perdim = False
    for k in keys[:-1]:
        for e, key in ((k[6], 'x'), (k[6], 'y'), (k[7], 'x'), (k[7], 'y')):
            v = e.get(key) if isinstance(e, dict) else None
            if isinstance(v, list) and len(v) > 1 and min(v[:dims]) != max(v[:dims]):
                perdim = True
    if not curved and not perdim:
        return None
    out = []
    for i in range(len(keys) - 1):
        t0, v0, hold, ease = keys[i][:4]
        t1, v1 = keys[i + 1][0], keys[i + 1][1]
        if hold or t1 - t0 <= 1e-6:
            out.append([t0, v0, hold, ease])
            continue
        to = _vec(keys[i][4] or [0] * dims, dims) if keys[i][4] else [0.0] * dims
        ti = _vec(keys[i][5] or [0] * dims, dims) if keys[i][5] else [0.0] * dims
        seg_curved = dims >= 2 and any(abs(x) > 1e-3 for x in to + ti)
        ox, oy = _ease_list(keys[i][6], 'x', dims, 0.0), _ease_list(keys[i][6], 'y', dims, 0.0)
        ix, iy = _ease_list(keys[i][7], 'x', dims, 1.0), _ease_list(keys[i][7], 'y', dims, 1.0)
        if seg_curved:
            P = [v0, [v0[j] + to[j] for j in range(dims)], [v1[j] + ti[j] for j in range(dims)], v1]
            def bez(u):
                a = (1 - u) ** 3; b = 3 * (1 - u) ** 2 * u; c = 3 * (1 - u) * u * u; d = u ** 3
                return [a * P[0][j] + b * P[1][j] + c * P[2][j] + d * P[3][j] for j in range(dims)]
            N = 200
            pts = [bez(j / N) for j in range(N + 1)]
            cum = [0.0]
            for j in range(N):
                cum.append(cum[-1] + math.sqrt(sum((pts[j + 1][q] - pts[j][q]) ** 2 for q in range(dims))))
        f = t0
        while f < t1 - 1e-6:
            u = (f - t0) / (t1 - t0)
            if seg_curved:  # arc-length parametrised, eased with the first dimension's curve
                target = cubic_ease(u, ox[0], oy[0], ix[0], iy[0]) * cum[-1]
                j = 0
                while j < N - 1 and cum[j + 1] < target:
                    j += 1
                seg = cum[j + 1] - cum[j]
                w = (target - cum[j]) / seg if seg > 1e-9 else 0.0
                val = [pts[j][q] + (pts[j + 1][q] - pts[j][q]) * w for q in range(dims)]
            else:
                val = [v0[q] + (v1[q] - v0[q]) * cubic_ease(u, ox[q], oy[q], ix[q], iy[q])
                       for q in range(dims)]
            out.append([f, val, 0, (0.0, 0.0, 1.0, 1.0)])
            f = math.floor(f) + 1.0
    out.append([keys[-1][0], keys[-1][1], 0, (0.0, 0.0, 1.0, 1.0)])
    return out

def invert_tr(tr):
    """Inverse of a static transform with uniform scale, as a Lottie transform."""
    if any(is_animated(tr.get(k)) for k in ('a', 'p', 's', 'r')):
        return None
    a = static_value(tr.get('a'), lambda v: _vec(v, 2), [0, 0])
    p = static_value(tr.get('p'), lambda v: _vec(v, 2), [0, 0])
    sc = static_value(tr.get('s'), lambda v: _vec(v, 2), [100, 100])
    r = _first(static_value(tr.get('r'), lambda v: _vec(v, 1), [0]))
    if abs(sc[0] - sc[1]) > 1e-6 or abs(sc[0]) < 1e-6:
        return None
    # T(a) S^-1 R(-r) T(-p)  ==  T(P'=a) R(-r) S'(=1/s) T(-A'=-p)  when S is uniform
    return {'ty': 'tr', 'a': {'k': p}, 'p': {'k': a}, 's': {'k': [1e4 / sc[0]] * 2},
            'r': {'k': -r}, 'o': {'k': 100}}

def eval_keys(keys, t):
    """Evaluate normalised 1-D keys (first-dimension easing) at time t."""
    if t <= keys[0][0]:
        return keys[0][1][0]
    for i in range(len(keys) - 1):
        t0, v0, hold, ease = keys[i][:4]
        t1, v1 = keys[i + 1][0], keys[i + 1][1]
        if t < t1:
            if hold or t1 <= t0:
                return v0[0]
            return v0[0] + (v1[0] - v0[0]) * cubic_ease((t - t0) / (t1 - t0), *ease)
    return keys[-1][1][0]

def expand_repeaters(items):
    """Rewrite repeaters as plain nested groups: copy k = the items above the
    repeater wrapped k times in the repeater transform."""
    out = list(items)
    while True:
        idx = next((i for i, it in enumerate(out) if it.get('ty') == 'rp' and not it.get('hd')), None)
        if idx is None:
            return [it for it in out if it.get('ty') != 'rp']
        rp = out[idx]
        if is_animated(rp.get('o')):
            warn('animated repeater offset uses the first value')
        count_keys = None
        if is_animated(rp.get('c')):
            count_keys = norm_keys(rp['c'], lambda v: _vec(v, 1))
            c = int(math.ceil(max(k[1][0] for k in count_keys) - 1e-6))
        else:
            c = int(round(_first(static_value(rp.get('c'), lambda v: _vec(v, 1), [1]))))
        off = _first(static_value(rp.get('o'), lambda v: _vec(v, 1), [0]))
        if abs(off - round(off)) > 1e-3:
            warn('fractional repeater offset rounded')
        off = int(round(off))
        c = max(0, min(c, 200))
        tr = rp.get('tr', {})
        so = _first(static_value(tr.get('so'), lambda v: _vec(v, 1), [100]))
        eo = _first(static_value(tr.get('eo'), lambda v: _vec(v, 1), [100]))
        rep_tr = {'ty': 'tr', 'a': tr.get('a'), 'p': tr.get('p'), 's': tr.get('s'),
                  'r': tr.get('r'), 'o': {'k': 100}}
        rep_tr = {k: v for k, v in rep_tr.items() if v is not None}
        inv_tr = None
        if off < 0:
            inv_tr = invert_tr(rep_tr)
            if inv_tr is None:
                warn('negative repeater offset needs a static, uniformly scaled transform: clamped to 0')
                off = 0
        before = [x for x in out[:idx] if x.get('ty') != 'tr']
        copies = []
        for k in range(c):
            op = so + (eo - so) * (k / (c - 1) if c > 1 else 0)
            opk = {'k': op}
            if count_keys:  # copy k shows while count > k (fractional count fades it)
                t0, t1 = count_keys[0][0], count_keys[-1][0]
                ks, f = [], math.floor(t0)
                while True:
                    vis = max(0.0, min(1.0, eval_keys(count_keys, f) - k))
                    ks.append({'t': f, 's': [op * vis], 'o': {'x': 0, 'y': 0}, 'i': {'x': 1, 'y': 1}})
                    if f >= t1:
                        break
                    f += 1
                opk = {'a': 1, 'k': ks} if len(ks) > 1 else {'k': ks[0]['s'][0]}
            # '_rep' tells group() to parent this copy on the k-th node of a shared
            # chain of repeater transforms (O(n) nodes instead of O(n^2) nesting)
            copies.append({'ty': 'gr', '_rep': (rep_tr, inv_tr, k + off),
                           'it': before + [{'ty': 'tr', 'o': opk}]})
        if rp.get('m', 1) == 1:  # "above": later copies on top (= earlier in the list)
            copies.reverse()
        out = copies + out[idx + 1:]

def conv_bezier(nv):
    def conv(k):
        if isinstance(k, list):
            k = k[0]
        v, ii, oo = k.get('v', []), k.get('i', []), k.get('o', [])
        out = []
        for j in range(nv):
            if j < len(v):
                vx, vy = v[j][0], v[j][1]
                ix, iy = (ii[j][0], ii[j][1]) if j < len(ii) else (0, 0)
                ox, oy = (oo[j][0], oo[j][1]) if j < len(oo) else (0, 0)
            else:  # pad with last vertex
                vx, vy = (v[-1][0], v[-1][1]) if v else (0, 0)
                ix = iy = ox = oy = 0
            out += [float(vx), float(vy), float(ix), float(iy), float(ox), float(oy)]
        return out
    return conv

class Path:
    def __init__(self, node, shape):
        self.node, self.trim = node, -1
        ty = shape['ty']
        self.closed = 1
        if ty == 'sh':
            ks = shape['ks']
            first = static_value(ks, lambda k: k[0] if isinstance(k, list) else k, {})
            nv = len(first.get('v', [])) if isinstance(first, dict) else 0
            if is_animated(ks):
                counts = set()
                for kf in ks['k']:
                    for key in ('s', 'e'):
                        if key in kf:
                            s = kf[key][0] if isinstance(kf[key], list) else kf[key]
                            counts.add(len(s.get('v', [])))
                if len(counts) > 1:
                    warn('animated path with changing vertex count (padded)')
                    nv = max(counts)
            self.closed = 1 if (isinstance(first, dict) and first.get('c')) else 0
            self.kind = 0
            self.props = [Prop.from_lottie(ks, nv * 6, [0] * (nv * 6), conv=conv_bezier(nv))]
            self.props[0].default = None
            self.nv = nv
        elif ty == 'rc':
            self.kind = 1
            self.props = [Prop.from_lottie(shape.get('p'), 2, [0, 0]),
                          Prop.from_lottie(shape.get('s'), 2, [0, 0]),
                          Prop.from_lottie(shape.get('r'), 1, [0])]
        elif ty == 'el':
            self.kind = 2
            self.props = [Prop.from_lottie(shape.get('p'), 2, [0, 0]),
                          Prop.from_lottie(shape.get('s'), 2, [0, 0])]
        elif ty == 'sr':
            self.kind = 0
            pts = star_to_bezier(shape)
            nv = len(pts) // 6
            self.props = [Prop(nv * 6, static=pts)]
        else:
            raise ValueError(ty)

    def encode(self):
        b = struct.pack('<hhBB', self.node, self.trim, self.kind, self.closed)
        for p in self.props:
            b += p.encode()
        return b

def star_to_bezier(sh):
    anim = any(is_animated(sh.get(k)) for k in ('p', 'pt', 'r', 'ir', 'or'))
    if anim:
        warn('animated polystar uses its first frame')
    g = lambda k, d: _first(static_value(sh.get(k), lambda v: _vec(v, 1), [d]))
    cx, cy = static_value(sh.get('p'), lambda v: _vec(v, 2), [0, 0])
    n = max(3, int(round(g('pt', 5))))
    rot = math.radians(g('r', 0))
    orad = g('or', 10)
    irad = g('ir', 5)
    star = sh.get('sy', 1) == 1
    pts = []
    count = n * 2 if star else n
    for i in range(count):
        a = rot - math.pi / 2 + i * (2 * math.pi / count)
        r = (orad if i % 2 == 0 else irad) if star else orad
        pts += [cx + r * math.cos(a), cy + r * math.sin(a), 0, 0, 0, 0]
    if any(sh.get(k) and _first(sh[k].get('k', 0)) for k in ('is', 'os')):
        warn('polystar roundness ignored')
    return pts

class Trim:
    def __init__(self, toff, tm):
        self.toff = toff
        self.s = Prop.from_lottie(tm.get('s'), 1, [0])
        self.e = Prop.from_lottie(tm.get('e'), 1, [100])
        self.o = Prop.from_lottie(tm.get('o'), 1, [0])

    def encode(self):
        return struct.pack('<f', self.toff) + self.s.encode() + self.e.encode() + self.o.encode()

def color_conv(k):
    v = _vec(k, 3)
    if max(v) > 1.0:  # very old files use 0..255
        v = [x / 255.0 for x in v]
    return v

class Item:
    """A fill or stroke (solid or gradient) applied to a list of paths.

    u8 type (bit0 stroke, bit1 gradient), u8 flags, i16 node, u16 miter*100, i16 clip
    solid   : color(3), opacity(1), [width(1)]
    gradient: opacity(1), [width(1)], u8 kind (1 linear, 2 radial), u8 colour stops,
              u8 alpha stops, start(2), end(2), stops(4*colour + 2*alpha)
    u16 n, n x u16 path index
    """
    def __init__(self, node, style, paths):
        ty = style['ty']
        self.node, self.paths = node, paths
        self.clip = -1
        self.stroke = ty in ('st', 'gs')
        self.grad = ty in ('gf', 'gs')
        if self.grad:
            g = style.get('g', {})
            self.nc = int(g.get('p', 0))
            gk = g.get('k', {})
            first = static_value(gk, lambda v: list(v), [])
            na = max(0, (len(first) - 4 * self.nc) // 2)
            if self.nc < 1 or self.nc > 32 or na > 32:
                warn('malformed gradient replaced by grey')
                self.grad = False
                self.color = Prop(3, static=[0.5, 0.5, 0.5])
            else:
                self.na = na
                dims = 4 * self.nc + 2 * na
                self.stops = Prop.from_lottie(gk, dims, first, conv=lambda v: _vec(v, dims))
                self.stops.default = None  # no implicit default on the device
                self.kind = 2 if style.get('t', 1) == 2 else 1
                self.gs = Prop.from_lottie(style.get('s'), 2, [0, 0])
                self.ge = Prop.from_lottie(style.get('e'), 2, [100, 0])
                if self.kind == 2 and style.get('h') and _first(style['h'].get('k', 0)) != 0:
                    warn('radial gradient highlight ignored')
        else:
            self.color = Prop.from_lottie(style.get('c'), 3, [0, 0, 0], conv=color_conv)
        self.op = Prop.from_lottie(style.get('o'), 1, [100])
        self.flags = 0
        self.miter = 4.0
        if self.stroke:
            self.width = Prop.from_lottie(style.get('w'), 1, [1])
            lc = int(style.get('lc', 2)); lj = int(style.get('lj', 2))
            cap = {1: 0, 2: 1, 3: 2}.get(lc, 1)   # 0 butt 1 round 2 square
            join = {1: 0, 2: 1, 3: 2}.get(lj, 1)  # 0 miter 1 round 2 bevel
            self.flags = cap | (join << 2)
            self.miter = float(style.get('ml', 4))
            if style.get('d'):
                warn('stroke dashes ignored')
        else:
            if style.get('r', 1) == 2:
                self.flags = 1  # even-odd

    def encode(self):
        ty = (1 if self.stroke else 0) | (2 if self.grad else 0)
        b = struct.pack('<BBhHh', ty, self.flags, self.node,
                        min(65535, int(self.miter * 100)), self.clip)
        if not self.grad:
            b += self.color.encode()
        b += self.op.encode()
        if self.stroke:
            b += self.width.encode()
        if self.grad:
            b += struct.pack('<BBB', self.kind, self.nc, self.na)
            b += self.gs.encode() + self.ge.encode() + self.stops.encode()
        b += struct.pack('<H', len(self.paths))
        for p in self.paths:
            b += struct.pack('<H', p)
        return bytes(b)

# --------------------------------------------------------------------------
# bitmap images
# --------------------------------------------------------------------------
import base64, zlib

try:
    from PIL import Image as _PIL  # optional: faster, better resampling, JPEG/WebP support
except Exception:  # pragma: no cover
    _PIL = None

def png_decode(data):
    """Minimal PNG decoder (non-interlaced, any colour type / bit depth) -> (w, h, RGBA bytes)."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(data):
        ln, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
    if inter:
        raise ValueError('interlaced PNG: install Pillow (pip install pillow) to convert it')
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    bpp = max(1, ch * depth // 8)
    stride = (w * ch * depth + 7) // 8
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    rows = []
    for y in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    maxv = (1 << depth) - 1
    for line in rows:
        if depth == 16:
            samples = line[0::2]
        elif depth == 8:
            samples = line
        else:  # 1, 2, 4 bit
            samples = bytearray()
            per = 8 // depth
            for byte in line:
                for k in range(per):
                    samples.append((byte >> (8 - depth * (k + 1))) & maxv)
            samples = samples[:w * ch]
        for x in range(w):
            if ctype == 3:
                i = samples[x]
                r, g, b = plte[3 * i:3 * i + 3]
                a = trns[i] if trns and i < len(trns) else 255
            else:
                v = samples[x * ch:x * ch + ch]
                if depth < 8:
                    v = [c * 255 // maxv for c in v]
                if ctype == 0:
                    r = g = b = v[0]; a = 255
                elif ctype == 4:
                    r = g = b = v[0]; a = v[1]
                elif ctype == 2:
                    r, g, b = v; a = 255
                else:
                    r, g, b, a = v
            out += bytes((r, g, b, a))
    return w, h, out

def load_image_bytes(asset, base_dir):
    p = asset.get('p', '')
    if p.startswith('data:'):
        return base64.b64decode(p.split(',', 1)[1])
    path = os.path.join(base_dir, asset.get('u', '') or '', p)
    with open(path, 'rb') as f:
        return f.read()

def decode_image(raw):
    if _PIL is not None:
        import io
        im = _PIL.open(io.BytesIO(raw)).convert('RGBA')
        return im.size[0], im.size[1], bytearray(im.tobytes())
    if raw[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('only PNG images can be decoded without Pillow (pip install pillow)')
    return png_decode(raw)

def resample(w, h, px, nw, nh):
    """Area-average downscale of RGBA bytes with premultiplied alpha."""
    if (nw, nh) == (w, h):
        return bytearray(px)
    if _PIL is not None:
        im = _PIL.frombytes('RGBA', (w, h), bytes(px))
        return bytearray(im.resize((nw, nh), _PIL.LANCZOS if (nw < w) else _PIL.BILINEAR).tobytes())
    def weights(n_src, n_dst):
        k = n_src / n_dst
        table = []
        for j in range(n_dst):
            a, b = j * k, (j + 1) * k
            ws = []
            for i in range(int(a), min(n_src, int(math.ceil(b)))):
                wgt = min(b, i + 1) - max(a, i)
                if wgt > 0:
                    ws.append((i, wgt / k))
            table.append(ws)
        return table
    wx, wy = weights(w, nw), weights(h, nh)
    pm = [0.0] * (w * h * 4)  # premultiplied floats
    for i in range(w * h):
        a = px[4 * i + 3] / 255.0
        pm[4 * i] = px[4 * i] * a; pm[4 * i + 1] = px[4 * i + 1] * a
        pm[4 * i + 2] = px[4 * i + 2] * a; pm[4 * i + 3] = px[4 * i + 3]
    tmp = [0.0] * (nw * h * 4)
    for y in range(h):
        row = y * w * 4
        for x, ws in enumerate(wx):
            acc = [0.0, 0.0, 0.0, 0.0]
            for i, wt in ws:
                q = row + 4 * i
                acc[0] += pm[q] * wt; acc[1] += pm[q + 1] * wt
                acc[2] += pm[q + 2] * wt; acc[3] += pm[q + 3] * wt
            o = (y * nw + x) * 4
            tmp[o:o + 4] = acc
    out = bytearray(nw * nh * 4)
    for y, ws in enumerate(wy):
        for x in range(nw):
            acc = [0.0, 0.0, 0.0, 0.0]
            for i, wt in ws:
                q = (i * nw + x) * 4
                acc[0] += tmp[q] * wt; acc[1] += tmp[q + 1] * wt
                acc[2] += tmp[q + 2] * wt; acc[3] += tmp[q + 3] * wt
            a = acc[3]
            o = (y * nw + x) * 4
            if a > 0.5:
                f = 255.0 / a
                out[o] = min(255, int(acc[0] * f + 0.5)); out[o + 1] = min(255, int(acc[1] * f + 0.5))
                out[o + 2] = min(255, int(acc[2] * f + 0.5))
            out[o + 3] = min(255, int(a + 0.5))
    return out

def bleed_colours(w, h, px, passes=3):
    """Give fully transparent pixels the colour of opaque neighbours, so bilinear
    filtering on the device doesn't pull dark fringes in at the edges."""
    known = [px[4 * i + 3] > 0 for i in range(w * h)]
    for _ in range(passes):
        upd = []
        for y in range(h):
            for x in range(w):
                i = y * w + x
                if known[i]:
                    continue
                acc = [0, 0, 0]; n = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < w and 0 <= yy < h and known[yy * w + xx]:
                            j = 4 * (yy * w + xx)
                            acc[0] += px[j]; acc[1] += px[j + 1]; acc[2] += px[j + 2]; n += 1
                if n:
                    upd.append((i, [c // n for c in acc]))
        if not upd:
            break
        for i, c in upd:
            px[4 * i:4 * i + 3] = bytes(c)
            known[i] = True
    return px

class ImageAsset:
    """One bitmap, stored at the resolution it is actually shown at.

    u16 w, u16 h, f32 kx, f32 ky, i16 offx, i16 offy, u8 flags (bit0 alpha), u8 0,
    w*h x u16 RGB565, [w*h x u8 alpha]
    Stored pixel (i, j) covers layer coordinates ((offx+i)/kx, (offy+j)/ky).
    """
    def __init__(self, asset, base_dir):
        self.asset, self.base_dir = asset, base_dir
        self.layer_w = float(asset.get('w', 0)) or None
        self.layer_h = float(asset.get('h', 0)) or None
        self.max_scale = 0.0   # largest on-screen scale (output px per layer unit)
        self.index = -1
        self.info = ''

    def encode(self, budget_scale):
        raw = load_image_bytes(self.asset, self.base_dir)
        w, h, px = decode_image(raw)
        lw, lh = self.layer_w or w, self.layer_h or h
        sc = self.max_scale * budget_scale
        nw = max(1, min(w, int(math.ceil(lw * sc))))
        nh = max(1, min(h, int(math.ceil(lh * sc))))
        px = resample(w, h, px, nw, nh)
        # crop to the visible pixels (+1 px for filtering)
        xs = [i % nw for i in range(nw * nh) if px[4 * i + 3]]
        ys = [i // nw for i in range(nw * nh) if px[4 * i + 3]]
        if not xs:
            x0 = y0 = 0; x1 = y1 = 0
        else:
            x0, x1 = max(0, min(xs) - 1), min(nw - 1, max(xs) + 1)
            y0, y1 = max(0, min(ys) - 1), min(nh - 1, max(ys) + 1)
        cw, ch = x1 - x0 + 1, y1 - y0 + 1
        crop = bytearray()
        for y in range(y0, y1 + 1):
            crop += px[4 * (y * nw + x0):4 * (y * nw + x1 + 1)]
        crop = bleed_colours(cw, ch, crop)
        alpha = any(crop[4 * i + 3] < 255 for i in range(cw * ch))
        b = bytearray(struct.pack('<HHffhhBB', cw, ch, nw / lw, nh / lh, x0, y0, 1 if alpha else 0, 0))
        for i in range(cw * ch):
            r, g, bl = crop[4 * i], crop[4 * i + 1], crop[4 * i + 2]
            r, g, bl = (r * 31 + 127) // 255, (g * 63 + 127) // 255, (bl * 31 + 127) // 255
            b += struct.pack('<H', (r << 11) | (g << 5) | bl)
        if alpha:
            b += bytes(crop[3::4])
        self.info = '%s: %dx%d px -> %dx%d stored (%d bytes%s)' % (
            self.asset.get('id'), w, h, cw, ch, len(b), ', alpha' if alpha else '')
        return bytes(b)

class ImageItem:
    """Draws an image asset in its layer's coordinate space."""
    stroke = False
    paths = []

    def __init__(self, node, image):
        self.node, self.image, self.clip = node, image, -1

    def encode(self):
        return struct.pack('<BBhHhH', 4, 0, self.node, 0, self.clip, self.image.index)

# --------------------------------------------------------------------------
# scene flattening
# --------------------------------------------------------------------------
class Converter:
    def __init__(self, anim, base_dir='.', out_size=None, image_scale=1.0):
        self.anim = anim
        self.base_dir, self.image_scale = base_dir, image_scale
        aw, ah = float(anim.get('w', 100)), float(anim.get('h', 100))
        # output pixels per comp unit (None: keep comp resolution)
        self.fit = min(out_size[0] / aw, out_size[1] / ah) if out_size else 1.0
        self.images = {}   # asset id -> ImageAsset
        self.assets = {a['id']: a for a in anim.get('assets', []) if 'id' in a}
        self.nodes, self.paths, self.trims, self.items = [], [], [], []
        self.order = []  # item indices, bottom-most first
        self.clips = []  # (parent clip, [(path, +1 add / -1 subtract)])
        self.cur_clip = -1
        self.chains = {}

    def rep_chain(self, rep_tr, node, k, toff):
        key = (id(rep_tr), node)
        chain = self.chains.setdefault(key, [node])
        while len(chain) <= k:
            prev = chain[-1]
            n = self.nodes[prev]
            chain.append(self.add_node(prev, prev, toff, n.vis_in, n.vis_out, rep_tr))
        return chain[k]

    def add_node(self, *a):
        n = Node(*a)
        own = n.scale.static if n.scale.keys is None else [x for k in n.scale.keys for x in k[1]]
        own = max([abs(v) for v in own] + [0.0]) / 100.0
        parent = a[0]
        n.max_scale = own * (self.nodes[parent].max_scale if 0 <= parent < len(self.nodes) else 1.0)
        self.nodes.append(n)
        return len(self.nodes) - 1

    # shape contents ----------------------------------------------------------
    def group(self, items, node, toff):
        items = expand_repeaters(items)
        elems = []
        for it in items:
            if it.get('hd'):
                continue
            ty = it.get('ty')
            if ty in ('sh', 'rc', 'el', 'sr'):
                self.paths.append(Path(node, it))
                elems.append(('path', [len(self.paths) - 1]))
            elif ty == 'gr':
                tr = next((x for x in it.get('it', []) if x.get('ty') == 'tr'), None)
                parent = node
                if '_rep' in it:
                    rt, inv, k = it['_rep']
                    parent = (self.rep_chain(rt, node, k, toff) if k >= 0 else
                              self.rep_chain(inv, node, -k, toff))
                n = self.nodes[parent]
                g = self.add_node(parent, parent, toff, n.vis_in, n.vis_out, tr)
                if self.nodes[g].is_identity():   # fold no-op group transforms
                    self.nodes.pop()
                    g = parent
                sub_paths, sub_ops = self.group(it.get('it', []), g, toff)
                elems.append(('group', (sub_paths, sub_ops)))
            elif ty in ('fl', 'st', 'gf', 'gs'):
                elems.append(('style', it))
            elif ty == 'tm':
                self.trims.append(Trim(toff, it))
                tid = len(self.trims) - 1
                covered = 0
                for kind, data in elems:
                    covered += len(data) if kind == 'path' else (len(data[0]) if kind == 'group' else 0)
                if it.get('m', 1) == 1 and covered > 1:
                    warn('trim paths "simultaneously" over several paths treated as "individually"')
                for kind, data in elems:
                    pids = data if kind == 'path' else (data[0] if kind == 'group' else [])
                    for pid in pids:
                        if self.paths[pid].trim < 0:
                            self.paths[pid].trim = tid
                elems.append(('trim', tid))
            elif ty == 'tr':
                pass
            else:
                names = {'mm': 'merge paths', 'rd': 'round corners',
                         'op': 'offset path', 'pb': 'pucker/bloat', 'tw': 'twist', 'zz': 'zig-zag'}
                warn('unsupported shape item ignored: ' + names.get(ty, str(ty)))
        ops = []
        for idx in range(len(elems) - 1, -1, -1):
            kind, data = elems[idx]
            if kind == 'style':
                pids = []
                for k2, d2 in elems[:idx]:
                    if k2 == 'path':
                        pids += d2
                    elif k2 == 'group':
                        pids += d2[0]
                if pids:
                    self.items.append(Item(node, data, pids))
                    self.items[-1].clip = self.cur_clip
                    ops.append(len(self.items) - 1)
            elif kind == 'group':
                ops += data[1]
        all_paths = []
        for kind, data in elems:
            if kind == 'path':
                all_paths += data
            elif kind == 'group':
                all_paths += data[0]
        return all_paths, ops

    # clips (masks and track mattes) -------------------------------------------
    def add_clip(self, parent, entries):
        if not entries:
            return parent
        if all(sg < 0 for _, sg in entries):  # only subtractive: start from "everything"
            entries = [(self.big_rect(entries[0][0]), 1)] + entries
        self.clips.append((parent, entries))
        return len(self.clips) - 1

    def big_rect(self, like_path):
        self.paths.append(Path(self.paths[like_path].node,
                               {'ty': 'rc', 'p': {'k': [0, 0]}, 's': {'k': [1e5, 1e5]}, 'r': {'k': 0}}))
        return len(self.paths) - 1

    def mask_clip(self, layer, node, parent):
        masks = [m for m in layer.get('masksProperties', []) if m.get('mode', 'a') != 'n']
        entries = []
        for m in masks:
            mode = m.get('mode', 'a')
            if mode not in ('a', 's'):
                warn('mask modes intersect/difference treated as add')
            if m.get('o') and (is_animated(m['o']) or _first(m['o'].get('k', 100)) < 99.9):
                warn('mask opacity ignored')
            if m.get('x') and (is_animated(m['x']) or abs(_first(m['x'].get('k', 0))) > 0.01):
                warn('mask expansion ignored')
            self.paths.append(Path(node, {'ty': 'sh', 'ks': m.get('pt', {'k': {}})}))
            sign = -1 if mode == 's' else 1
            if m.get('inv'):
                if len(masks) > 1:
                    warn('inverted mask combined with other masks: inversion ignored')
                else:
                    sign = -sign
            entries.append((len(self.paths) - 1, sign))
        return self.add_clip(parent, entries)

    def matte_clip(self, matte, node, parent, inverted):
        """Coverage of everything a track-matte layer would fill, as a clip."""
        n_items, n_order, saved = len(self.items), len(self.order), self.cur_clip
        ty = matte.get('ty')
        self.cur_clip = -1
        if ty == 4:
            self.order += self.group(matte.get('shapes', []), node, self.nodes[node].toff)[1]
        elif ty == 1:
            w, h = float(matte.get('sw', 0)), float(matte.get('sh', 0))
            self.paths.append(Path(node, {'ty': 'rc', 'p': {'k': [w / 2, h / 2]},
                                          's': {'k': [w, h]}, 'r': {'k': 0}}))
            self.items.append(Item(node, {'ty': 'fl', 'c': {'k': [0, 0, 0]}}, [len(self.paths) - 1]))
        elif ty == 0:
            asset = self.assets.get(matte.get('refId'))
            if asset and 'layers' in asset:
                nd = self.nodes[node]
                self.comp(asset['layers'], nd.toff + float(matte.get('st', 0)), node,
                          nd.vis_in, nd.vis_out)
        elif ty == 2:
            asset = self.assets.get(matte.get('refId')) or {}
            w, h = float(asset.get('w', 0)), float(asset.get('h', 0))
            warn('image used as track matte: its rectangle is used, transparency ignored')
            self.paths.append(Path(node, {'ty': 'rc', 'p': {'k': [w / 2, h / 2]},
                                          's': {'k': [w, h]}, 'r': {'k': 0}}))
            self.items.append(Item(node, {'ty': 'fl', 'c': {'k': [0, 0, 0]}}, [len(self.paths) - 1]))
        else:
            warn('track matte from text layer: matted layer hidden')
        if matte.get('masksProperties'):
            warn('masks on matte layers ignored')
        paths = []
        for it in self.items[n_items:]:
            if not it.stroke:
                paths += [p for p in it.paths if p not in paths]
        if any(it.stroke for it in self.items[n_items:]):
            warn('strokes inside track mattes ignored')
        del self.items[n_items:]
        del self.order[n_order:]
        self.cur_clip = saved
        if not paths:
            if inverted:
                return parent
            self.clips.append((parent, []))  # empty clip -> layer hidden
            return len(self.clips) - 1
        return self.add_clip(parent, [(p, -1 if inverted else 1) for p in paths])

    # layers --------------------------------------------------------------------
    def comp(self, layers, comp_off, container, vis_lo, vis_hi, clip=-1):
        """container: node index of enclosing precomp layer (or -1)."""
        by_ind = {l.get('ind'): l for l in layers if 'ind' in l}
        node_of = {}

        def make(layer, stack=()):
            key = id(layer)
            if key in node_of:
                return node_of[key]
            par = container
            if 'parent' in layer and layer['parent'] in by_ind and layer['parent'] not in stack:
                par = make(by_ind[layer['parent']], stack + (layer.get('ind'),))
            vin = max(vis_lo, float(layer.get('ip', 0)) + comp_off)
            vout = min(vis_hi, float(layer.get('op', 1e9)) + comp_off)
            toff = comp_off  # layer keyframes are in comp time; 'st' only shifts precomp contents
            if layer.get('sr', 1) not in (1, 1.0):
                warn('layer time stretch ignored')
            n = self.add_node(par, container, toff, vin, vout, layer.get('ks'))
            node_of[key] = n
            return n

        for l in layers:
            make(l)
        # draw bottom (last) layer first
        for li in range(len(layers) - 1, -1, -1):
            layer = layers[li]
            n = node_of[id(layer)]
            node = self.nodes[n]
            if layer.get('hd') or layer.get('td'):
                continue
            layer_clip = clip
            tt = layer.get('tt')
            if tt:
                matte = None
                if 'tp' in layer:
                    matte = by_ind.get(layer['tp'])
                elif li > 0 and layers[li - 1].get('td'):
                    matte = layers[li - 1]
                if matte is None:
                    warn('track matte source missing')
                else:
                    if tt in (3, 4):
                        warn('luma mattes treated as alpha mattes')
                    layer_clip = self.matte_clip(matte, node_of[id(matte)], layer_clip, tt in (2, 4))
            if layer.get('masksProperties'):
                layer_clip = self.mask_clip(layer, n, layer_clip)
            self.cur_clip = layer_clip
            if layer.get('ef'):
                warn('layer effects ignored')
            ty = layer.get('ty')
            toff = node.toff
            if ty == 4:
                self.order += self.group(layer.get('shapes', []), n, toff)[1]
            elif ty == 1:
                w, h = float(layer.get('sw', 0)), float(layer.get('sh', 0))
                sc = (layer.get('sc') or '').lstrip('#')
                if len(sc) == 3:
                    sc = ''.join(ch * 2 for ch in sc)
                if len(sc) < 6:  # empty colour = transparent solid
                    continue
                rgb = [int(sc[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
                self.paths.append(Path(n, {'ty': 'rc', 'p': {'k': [w / 2, h / 2]},
                                           's': {'k': [w, h]}, 'r': {'k': 0}}))
                self.items.append(Item(n, {'ty': 'fl', 'c': {'k': rgb}, 'o': {'k': 100}},
                                       [len(self.paths) - 1]))
                self.items[-1].clip = layer_clip
                self.order.append(len(self.items) - 1)
            elif ty == 0:
                asset = self.assets.get(layer.get('refId'))
                if layer.get('tm'):
                    warn('precomp time remap ignored')
                if asset and 'layers' in asset:
                    self.comp(asset['layers'], toff + float(layer.get('st', 0)), n,
                              node.vis_in, node.vis_out, layer_clip)
                else:
                    warn('precomp asset missing')
            elif ty == 3:
                pass
            elif ty == 2:
                asset = self.assets.get(layer.get('refId'))
                if not asset or 'p' not in asset:
                    warn('image asset missing')
                    continue
                img = self.images.get(asset['id'])
                if img is None:
                    img = self.images[asset['id']] = ImageAsset(asset, self.base_dir)
                img.max_scale = max(img.max_scale, node.max_scale * self.fit)
                self.items.append(ImageItem(n, img))
                self.items[-1].clip = layer_clip
                self.order.append(len(self.items) - 1)
            elif ty == 5:
                warn('text layers ignored')
            else:
                warn('layer type %s ignored' % ty)

    def run(self):
        a = self.anim
        ip, op = float(a.get('ip', 0)), float(a.get('op', 60))
        self.comp(a.get('layers', []), 0.0, -1, -1e9, 1e9)
        hdr_len = 36
        body = b''.join(x.encode() for x in self.nodes)
        body += b''.join(x.encode() for x in self.paths)
        body += b''.join(x.encode() for x in self.trims)
        for parent, entries in self.clips:
            body += struct.pack('<hH', parent, len(entries))
            for pid, sg in entries:
                body += struct.pack('<Hb', pid, sg)
        used = set(id(self.items[i].image) for i in self.order if isinstance(self.items[i], ImageItem))
        self.image_list = [im for im in self.images.values() if id(im) in used]
        for k, im in enumerate(self.image_list):
            im.index = k
            body += im.encode(self.image_scale)
        assert sorted(self.order) == list(range(len(self.items)))
        body += b''.join(self.items[i].encode() for i in self.order)
        hdr = struct.pack('<4sHHfffHHHHHHI', b'LMC1', int(a.get('w', 100)), int(a.get('h', 100)),
                          float(a.get('fr', 30)), ip, op,
                          len(self.nodes), len(self.paths), len(self.trims), len(self.items),
                          len(self.clips), len(self.image_list), hdr_len + len(body))
        assert len(hdr) == hdr_len
        return hdr + body


def to_header(data, name, src):
    lines = ['// Generated by lottie2mcu.py %s (LottieMCU) from %s' % (__version__, os.path.basename(src)),
             '// %d bytes' % len(data),
             '#pragma once',
             '#include <stdint.h>',
             '#if defined(__AVR__)',
             '#include <avr/pgmspace.h>',
             '#elif !defined(PROGMEM)',
             '#define PROGMEM',
             '#endif',
             '',
             'const uint8_t %s[] PROGMEM = {' % name]
    for i in range(0, len(data), 16):
        lines.append('  ' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    lines.append('};')
    lines.append('const uint32_t %s_len = %d;' % (name, len(data)))
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description='Convert Lottie JSON to LottieMCU binary '
                                 '(LottieMCU %s, github.com/jobitjoseph/LottieMCU)' % __version__)
    ap.add_argument('--version', action='version', version='lottie2mcu ' + __version__)
    ap.add_argument('input')
    ap.add_argument('-o', '--output', help='output .h file (default: <input>.h)')
    ap.add_argument('-n', '--name', help='C array name (default: from file name)')
    ap.add_argument('--bin', help='also/instead write raw binary to this path')
    ap.add_argument('--size', help='display size the animation is shown at, e.g. 320x240 '
                    '(images are stored at that resolution; strongly recommended with images)')
    ap.add_argument('--image-scale', type=float, default=1.0,
                    help='extra image resolution factor (e.g. 0.5 halves image flash use)')
    args = ap.parse_args()
    out_size = None
    if args.size:
        try:
            out_size = tuple(int(v) for v in args.size.lower().split('x'))
            assert len(out_size) == 2
        except Exception:
            ap.error('--size must look like 320x240')

    with open(args.input, 'r', encoding='utf-8') as f:
        anim = json.load(f)
    conv = Converter(anim, os.path.dirname(os.path.abspath(args.input)), out_size, args.image_scale)
    data = conv.run()

    base = os.path.splitext(os.path.basename(args.input))[0]
    name = args.name or ''.join(c if c.isalnum() else '_' for c in base)
    if name[0].isdigit():
        name = 'anim_' + name
    if args.bin:
        with open(args.bin, 'wb') as f:
            f.write(data)
    if args.output or not args.bin:
        out = args.output or os.path.splitext(args.input)[0] + '.h'
        with open(out, 'w') as f:
            f.write(to_header(data, name, args.input))
        print('wrote %s' % out)
    if args.bin:
        print('wrote %s' % args.bin)
    src_size = os.path.getsize(args.input)
    print('%dx%d @ %.1f fps, frames %g..%g' % (anim.get('w', 0), anim.get('h', 0),
          anim.get('fr', 0), anim.get('ip', 0), anim.get('op', 0)))
    print('nodes %d, paths %d, trims %d, draw items %d, clips %d' % (len(conv.nodes),
          len(conv.paths), len(conv.trims), len(conv.items), len(conv.clips)))
    print('binary %d bytes (json %d bytes, %.0f%%)' % (len(data), src_size, 100.0 * len(data) / max(1, src_size)))
    if conv.image_list:
        print('images: %d (Config.maxImages must be >= the number visible at once)' % len(conv.image_list))
        for im in conv.image_list:
            print('  ' + im.info)
        if not out_size:
            print('note: no --size given, images are stored at animation resolution; '
                  'pass --size WxH of your display to shrink them')
    maxv = max([p.nv for p in conv.paths if p.kind == 0 and hasattr(p, 'nv')] + [0])
    print('largest path: %d vertices (Config.maxPathVerts must be >= this)' % maxv)
    for m, c in sorted(WARN.items()):
        print('warning: %s%s' % (m, ' (x%d)' % c if c > 1 else ''))


if __name__ == '__main__':
    main()
