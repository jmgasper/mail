#!/usr/bin/env python3
"""Mail's own small HVIF icons for items Haiku's artwork has no match for.

Run from any directory. Writes resources/icons/tag.hvif and attachment.hvif.
"""
import math
from pathlib import Path
import hvif


def tag():
    styles = [
        {'color': (0, 0, 0, 40)},
        {'color': (96, 64, 8, 255)},
        hvif.linear_gradient((10, 12), (50, 52), [(0, (255, 227, 128, 255)), (1, (240, 160, 32, 255))]),
        {'color': (255, 255, 255, 255)},
    ]
    outline = [(6, 10), (30, 10), (58, 38), (36, 60), (6, 30)]
    shadow = [(x + 3, y + 3) for x, y in outline]
    hole = [(16, 16), (22, 16), (22, 22), (16, 22)]
    paths = [{'closed': True, 'points': shadow}, {'closed': True, 'points': outline}, {'closed': True, 'points': hole}]
    shapes = [
        {'style': 0, 'paths': [0]},
        {'style': 1, 'paths': [1], 'transformers': [{'type': 'contour', 'width': 2.0}]},
        {'style': 2, 'paths': [1]},
        {'style': 1, 'paths': [2], 'transformers': [{'type': 'contour', 'width': 2.0}]},
        {'style': 3, 'paths': [2]},
    ]
    return {'styles': styles, 'paths': paths, 'shapes': shapes}


def arc(cx, cy, r, start, end, steps=8):
    return [(round(cx + r * math.cos(math.radians(start + (end - start) * i / steps))),
             round(cy + r * math.sin(math.radians(start + (end - start) * i / steps)))) for i in range(steps + 1)]


def attachment():
    styles = [{'color': (40, 48, 60, 255)}, {'color': (170, 182, 196, 255)}]
    # A paper clip: three straight runs joined by half circles, drawn as a stroke.
    points = [(26, 44)] + [(26, 20)] + arc(33, 20, 7, 180, 360)[1:] + [(40, 48)] \
        + arc(30, 48, 10, 0, 180)[1:] + [(20, 16)] + arc(32, 16, 12, 180, 360)[1:] + [(44, 40)]
    path = {'closed': False, 'points': points}
    shapes = [
        {'style': 0, 'paths': [0], 'transformers': [{'type': 'stroke', 'width': 7.0, 'join': 2, 'cap': 2}]},
        {'style': 1, 'paths': [0], 'transformers': [{'type': 'stroke', 'width': 3.0, 'join': 2, 'cap': 2}]},
    ]
    return {'styles': styles, 'paths': [path], 'shapes': shapes}


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1] / 'resources/icons'
    for name, icon in (('tag', tag()), ('attachment', attachment())):
        data = hvif.encode(icon)
        assert hvif.encode(hvif.decode(data)) == data
        (root / f'{name}.hvif').write_bytes(data)
        print(f'{name}.hvif: {len(data)} bytes')
