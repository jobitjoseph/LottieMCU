# Generates demo_spinner.json: an original Lottie animation used by the examples.
# Part of LottieMCU - https://github.com/jobitjoseph/LottieMCU
# Copyright (c) 2026 Jobit Joseph. MIT License (the generated animation too).
import json
FR, OP = 30, 60
def kf(t, s, ease=True):
    k = {'t': t, 's': s if isinstance(s, list) else [s]}
    if ease:
        k['o'] = {'x': [0.4], 'y': [0]}; k['i'] = {'x': [0.2], 'y': [1]}
    return k
def anim(*keys): return {'a': 1, 'k': list(keys)}
def stat(v): return {'a': 0, 'k': v}
def tr(p=(0, 0), r=None, s=(100, 100), o=100):
    return {'ty': 'tr', 'p': stat(list(p)), 'a': stat([0, 0]), 's': stat(list(s)) if not isinstance(s, dict) else s,
            'r': r or stat(0), 'o': stat(o) if not isinstance(o, dict) else o}
layers = []
# rotating arc drawn with trim paths
layers.append({'ty': 4, 'ind': 1, 'nm': 'arc', 'ip': 0, 'op': OP, 'st': 0,
  'ks': {'p': stat([120, 120]), 'a': stat([0, 0]), 's': stat([100, 100]),
         'r': anim(kf(0, 0, False), kf(OP, 360, False)), 'o': stat(100)},
  'shapes': [{'ty': 'gr', 'it': [
      {'ty': 'el', 'p': stat([0, 0]), 's': stat([150, 150])},
      {'ty': 'tm', 's': anim(kf(15, 0), kf(45, 95)), 'e': anim(kf(0, 5), kf(30, 100)), 'o': stat(0), 'm': 1},
      {'ty': 'gs', 'o': stat(100), 'w': stat(14), 'lc': 2, 'lj': 2, 't': 1, 's': stat([-75, 0]), 'e': stat([75, 0]),
       'g': {'p': 2, 'k': stat([0, 0.1, 0.7, 1.0, 1, 0.9, 0.2, 0.9])}},
      tr()]}]})
# pulsing core with radial gradient
layers.append({'ty': 4, 'ind': 2, 'nm': 'core', 'ip': 0, 'op': OP, 'st': 0,
  'ks': {'p': stat([120, 120]), 'a': stat([0, 0]), 's': anim(kf(0, [70, 70]), kf(30, [100, 100]), kf(60, [70, 70])),
         'r': stat(0), 'o': stat(100)},
  'shapes': [{'ty': 'gr', 'it': [
      {'ty': 'el', 'p': stat([0, 0]), 's': stat([80, 80])},
      {'ty': 'gf', 'o': stat(100), 'r': 1, 't': 2, 's': stat([0, 0]), 'e': stat([40, 0]),
       'g': {'p': 2, 'k': stat([0, 1.0, 0.85, 0.3, 1, 1.0, 0.4, 0.1])}},
      tr()]}]})
# three orbiting dots (repeater)
layers.append({'ty': 4, 'ind': 3, 'nm': 'dots', 'ip': 0, 'op': OP, 'st': 0,
  'ks': {'p': stat([120, 120]), 'a': stat([0, 0]), 's': stat([100, 100]),
         'r': anim(kf(0, 0), kf(OP, -360)), 'o': stat(100)},
  'shapes': [
      {'ty': 'gr', 'it': [{'ty': 'el', 'p': stat([0, -105]), 's': stat([14, 14])},
                          {'ty': 'fl', 'c': stat([1, 1, 1, 1]), 'o': stat(100), 'r': 1}, tr()]},
      {'ty': 'rp', 'c': stat(3), 'o': stat(0), 'm': 1,
       'tr': {'ty': 'tr', 'p': stat([0, 0]), 'a': stat([0, 0]), 's': stat([100, 100]), 'r': stat(120),
              'so': stat(100), 'eo': stat(30)}}]})
# background
layers.append({'ty': 1, 'ind': 4, 'nm': 'bg', 'ip': 0, 'op': OP, 'st': 0, 'sw': 240, 'sh': 240, 'sc': '#14141e',
  'ks': {'p': stat([120, 120]), 'a': stat([120, 120]), 's': stat([100, 100]), 'r': stat(0), 'o': stat(100)}})
json.dump({'v': '5.7.0', 'fr': FR, 'ip': 0, 'op': OP, 'w': 240, 'h': 240, 'nm': 'demo_spinner',
           'ddd': 0, 'assets': [], 'layers': layers}, open('demo_spinner.json', 'w'), indent=1)
