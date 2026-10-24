#!/usr/bin/env python3
"""
Generates eco_green_bulb.json - an all-vector Lottie recreation of the
"Ecosystem Green Light" animation (grey bulb fills with green while a vine
grows up its side and a leaf unfolds), using only features LottieMCU supports.

1000x1000, 30 fps, 181 frames.

Part of LottieMCU - https://github.com/jobitjoseph/LottieMCU
Copyright (c) 2026 Jobit Joseph. MIT License (the generated animation too).
"""
import json, copy

W = H = 1000
FR, OP = 30, 181

# ---------------------------------------------------------------- helpers
def S(v):                      # static property
    return {'a': 0, 'k': v}

EASE_OUT = ({'x': [0.33], 'y': [0]}, {'x': [0.25], 'y': [1]})   # smooth in/out
BOUNCE = ({'x': [0.3], 'y': [0]}, {'x': [0.4], 'y': [1.45]})     # slight overshoot

def A(*keys, ease=EASE_OUT):
    """Animated property: keys = (frame, value), ..."""
    out = []
    for n, (t, v) in enumerate(keys):
        k = {'t': t, 's': v if isinstance(v, list) else [v]}
        if n < len(keys) - 1:
            k['o'], k['i'] = copy.deepcopy(ease[0]), copy.deepcopy(ease[1])
        out.append(k)
    return {'a': 1, 'k': out}

def path(verts, closed=True):
    """verts: list of (x, y, in_dx, in_dy, out_dx, out_dy)"""
    return {'v': [[v[0], v[1]] for v in verts], 'i': [[v[2], v[3]] for v in verts],
            'o': [[v[4], v[5]] for v in verts], 'c': closed}

def sh(verts, closed=True):
    return {'ty': 'sh', 'ks': S(path(verts, closed))}

def ell(cx, cy, w, h):
    return {'ty': 'el', 'p': S([cx, cy]), 's': S([w, h])}

def rect(cx, cy, w, h, r=0):
    return {'ty': 'rc', 'p': S([cx, cy]), 's': S([w, h]), 'r': S(r)}

def fill(rgb, o=100):
    return {'ty': 'fl', 'c': S(list(rgb) + [1]), 'o': S(o), 'r': 1}

def stroke(rgb, w, o=100, cap=2, join=2):
    return {'ty': 'st', 'c': S(list(rgb) + [1]), 'o': S(o), 'w': S(w), 'lc': cap, 'lj': join, 'ml': 4}

def grad(kind, start, end, stops, o=100, is_stroke=False, w=None):
    """stops: list of (pos, (r,g,b)) ; kind 1 linear, 2 radial"""
    flat = [x for p, c in stops for x in (p,) + tuple(c)]
    g = {'ty': 'gs' if is_stroke else 'gf', 'o': S(o), 'r': 1, 't': kind,
         's': S(list(start)), 'e': S(list(end)), 'g': {'p': len(stops), 'k': S(flat)}}
    if is_stroke:
        g.update({'w': S(w), 'lc': 2, 'lj': 2, 'ml': 4})
    return g

def tr(p=(0, 0), a=(0, 0), s=(100, 100), r=0, o=100):
    def P(v): return v if isinstance(v, dict) else S(list(v) if isinstance(v, tuple) else v)
    return {'ty': 'tr', 'p': P(p), 'a': P(a), 's': P(s), 'r': P(r), 'o': P(o)}

def group(name, *items, t=None):
    return {'ty': 'gr', 'nm': name, 'it': list(items) + [t or tr()]}

def layer(name, ind, shapes, ks=None, masks=None):
    L = {'ddd': 0, 'ind': ind, 'ty': 4, 'nm': name, 'ip': 0, 'op': OP, 'st': 0, 'sr': 1,
         'ks': ks or {'o': S(100), 'r': S(0), 'p': S([0, 0, 0]), 'a': S([0, 0, 0]),
                      's': S([100, 100, 100])},
         'ao': 0, 'shapes': shapes, 'bm': 0}
    if masks:
        L['hasMask'] = True
        L['masksProperties'] = masks
    return L

# ---------------------------------------------------------------- palette
GLASS_HI = (0.98, 0.98, 0.98)
GLASS_MID = (0.86, 0.87, 0.88)
GLASS_EDGE = (0.66, 0.68, 0.70)
OUTLINE = (0.55, 0.57, 0.60)
LIME = (0.86, 0.95, 0.40)
GREEN = (0.55, 0.80, 0.14)
GREEN_DK = (0.30, 0.58, 0.06)
LEAF_LT = (0.70, 0.88, 0.35)
LEAF_DK = (0.22, 0.52, 0.10)
STEM = (0.36, 0.64, 0.16)
METAL_DK = (0.35, 0.36, 0.38)
METAL_MD = (0.62, 0.63, 0.65)
METAL_HI = (0.95, 0.95, 0.96)
WIRE = (0.50, 0.52, 0.55)

# ---------------------------------------------------------------- bulb outline
BULB = [
    (500, 150, -118, 0, 118, 0),
    (715, 365, 0, -118, 0, 95),
    (604, 598, 36, -62, -6, 14),
    (598, 660, 0, -22, 0, 0),
    (402, 660, 0, 0, 0, -22),
    (396, 598, 6, 14, -36, -62),
    (285, 365, 0, 95, 0, -118),
]

# grey glass bulb (always visible)
grey_bulb = layer('Grey bulb', 6, [
    group('glass', sh(BULB),
          stroke(OUTLINE, 5, 90),
          grad(2, (430, 300), (430, 600), [(0, GLASS_HI), (0.55, GLASS_MID), (1, GLASS_EDGE)])),
])

# green "liquid light" - same outline, revealed bottom-to-top by a rising mask
def mask_rect(top):
    return [path([(240, top, 0, 0, 0, 0), (760, top, 0, 0, 0, 0),
                  (760, 700, 0, 0, 0, 0), (240, 700, 0, 0, 0, 0)])]

green_bulb = layer('Green bulb', 4, [
    group('green glass', sh(BULB),
          stroke(GREEN_DK, 5, 90),
          grad(2, (485, 360), (485, 650), [(0, (0.97, 1.0, 0.82)), (0.35, LIME), (0.72, GREEN),
                                           (1, GREEN_DK)])),
], masks=[{'mode': 'a', 'inv': False, 'o': S(100), 'x': S(0), 'nm': 'fill level',
           'pt': A((60, mask_rect(668)), (120, mask_rect(130)))}])

# filament, supports and glass stem (seen through the glass)
coil = [(440, 430, 0, 0, 0, 0)]
for k in range(1, 9):
    coil.append((440 + k * 15, 430 + (12 if k % 2 else -12), 0, 0, 0, 0))
filament = layer('Filament', 5, [
    group('stem glass', rect(500, 615, 96, 90, 18), fill((1, 1, 1), 45), stroke(WIRE, 3, 50)),
    group('wires',
          sh([(470, 640, 0, 0, 0, 0), (470, 470, 0, 0, 0, 0), (440, 430, 0, 0, 0, 0)], False),
          sh([(530, 640, 0, 0, 0, 0), (530, 470, 0, 0, 0, 0), (560, 430, 0, 0, 0, 0)], False),
          sh(coil, False),
          sh([(470, 520, 0, 0, 0, 0), (440, 400, 0, 0, 0, 0)], False),
          sh([(530, 520, 0, 0, 0, 0), (560, 400, 0, 0, 0, 0)], False),
          sh([(500, 560, 0, 0, 0, 0), (500, 395, 0, 0, 0, 0)], False),
          stroke(WIRE, 5, 85)),
    group('loops', ell(440, 392, 18, 18), ell(500, 386, 18, 18), ell(560, 392, 18, 18),
          stroke(WIRE, 4, 85)),
])

# glossy highlights on top of the glass
gloss = layer('Gloss', 3, [
    group('arc', sh([(345, 420, 0, 0, -18, -80), (420, 220, -60, 30, 0, 0)], False),
          stroke((1, 1, 1), 16, 75)),
    group('spot', ell(620, 250, 46, 84), fill((1, 1, 1), 55)),
    group('rim', sh([(650, 470, 0, 0, 25, -40), (690, 330, 0, 60, 0, 0)], False),
          stroke((1, 1, 1), 8, 45)),
])

# metal screw base
ribs = []
for k in range(5):
    y = 676 + k * 26
    ribs.append(group('rib%d' % k, rect(500, y, 214 - k * 4, 24, 12),
                      grad(1, (390, y), (610, y),
                           [(0, METAL_DK), (0.3, METAL_HI), (0.55, METAL_MD), (1, METAL_DK)]),
                      stroke(METAL_DK, 2, 80)))
base = layer('Base', 2, [
    group('collar', rect(500, 662, 206, 18, 6),
          grad(1, (397, 662), (603, 662), [(0, METAL_DK), (0.3, METAL_HI), (1, METAL_DK)])),
    *reversed(ribs),
    group('neck', sh([(412, 800, 0, 0, 0, 0), (588, 800, 0, 0, 0, 0), (556, 836, 0, 0, 0, 0),
                      (444, 836, 0, 0, 0, 0)]), fill(METAL_DK)),
    group('tip', ell(500, 842, 64, 26), fill((0.22, 0.22, 0.24))),
])

# ---------------------------------------------------------------- leaves
def leaf_shape(length, bend=0.0, rib=True, light=LEAF_LT, dark=LEAF_DK):
    """Leaf pointing along +x from (0,0); bend > 0 curls the tip upward."""
    L = length
    tip_y = -L * bend
    items = [sh([(0, 0, L * 0.35, L * 0.36, L * 0.25, -L * 0.42),
                 (L, tip_y, -L * 0.30, -L * 0.18, -L * 0.42, L * 0.28)]),
             grad(1, (0, 0), (L, tip_y), [(0, dark), (0.6, (0.48, 0.74, 0.20)), (1, light)])]
    out = [group('blade', *items)]
    if rib:
        out.insert(0, group('rib', sh([(L * 0.06, 0, 0, 0, L * 0.35, -L * 0.02),
                                       (L * 0.88, tip_y * 0.9, -L * 0.30, L * 0.03, 0, 0)], False),
                            stroke((0.82, 0.95, 0.55), max(2, L * 0.03), 80)))
    return out

# vine: stem drawn on with trim paths, leaves pop out as it passes
STEM_PATH = [
    (540, 845, 0, 0, 30, -30),
    (630, 720, -25, 40, 25, -40),
    (690, 575, -8, 45, 8, -55),
    (750, 430, -35, 40, 30, -35),
    (815, 305, -18, 45, 8, -25),
    (850, 262, -6, 18, 0, 0),
]
VINE_LEAVES = [  # x, y, rotation, length, appears at frame
    (575, 800, -20, 105, 6),
    (612, 752, -165, 92, 12),
    (650, 690, -30, 118, 18),
    (684, 622, -170, 96, 24),
    (700, 565, -20, 122, 29),
    (728, 478, -150, 98, 34),
    (758, 428, -40, 112, 39),
    (785, 362, -135, 84, 44),
    (812, 318, -5, 88, 49),
    (836, 280, -100, 62, 54),
]
vine_items = []
for n, (x, y, rot, length, t0) in enumerate(VINE_LEAVES):
    vine_items.append(group('leaf%d' % n, *leaf_shape(length, bend=0.12),
                            t=tr(p=(x, y), r=rot, s=A((t0, [0, 0]), (t0 + 14, [100, 100]), ease=BOUNCE))))
tendril = group('tendril', sh([(850, 262, 0, 0, 14, -10), (868, 236, 0, 14, 0, -14),
                               (852, 222, 10, 0, -8, 2)], False),
                {'ty': 'tm', 's': S(0), 'e': A((50, 0), (62, 100)), 'o': S(0), 'm': 1},
                stroke(STEM, 5, 100))
vine_items.reverse()   # later leaves drawn on top
vine = layer('Vine', 1, [
    *vine_items,
    tendril,
    group('stem', sh(STEM_PATH, False),
          {'ty': 'tm', 's': S(0), 'e': A((0, 0), (55, 100)), 'o': S(0), 'm': 1},
          stroke(STEM, 9, 100)),
], ks={'o': S(100), 'p': S([555, 747, 0]), 'a': S([555, 747, 0]), 's': S([100, 100, 100]),
       'r': A((60, 0), (120, -10), (180, 5))})

# big leaf behind the bulb on the left, unfolding then swaying
big_leaf = layer('Left leaf', 7, [
    group('stalk', sh([(0, 0, 0, 0, -10, -6), (-30, -12, 0, 0, 0, 0)], False), stroke(STEM, 8)),
    group('drop', ell(-150, -98, 18, 24), fill((1, 1, 1), 55), stroke((1, 1, 1), 2, 70)),
    group('leaf', *leaf_shape(250, bend=-0.10, light=(0.80, 0.93, 0.45), dark=(0.26, 0.60, 0.12)),
          t=tr(p=(-20, -10), r=-142)),
], ks={'o': S(100), 'p': S([418, 735, 0]), 'a': S([0, 0, 0]),
       's': A((30, [0, 0, 100]), (60, [100, 100, 100]), ease=BOUNCE),
       'r': A((60, 0), (120, -6), (180, 6))})

# top first; the filament sits above the green glass so it stays visible through it
layers = [vine, base, gloss, filament, green_bulb, grey_bulb, big_leaf]
for n, L in enumerate(layers):
    L['ind'] = n + 1

anim = {'v': '5.7.0', 'fr': FR, 'ip': 0, 'op': OP, 'w': W, 'h': H, 'nm': 'Eco Green Bulb',
        'ddd': 0, 'assets': [], 'layers': layers}
json.dump(anim, open('eco_green_bulb.json', 'w'), separators=(',', ':'))
print('wrote eco_green_bulb.json')
