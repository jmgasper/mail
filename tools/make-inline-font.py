#!/usr/bin/env python3
"""Regenerate Mail's blank inline-object font (development-only fontTools dependency).

The font supplies native text-layout metrics for an entire named image marker.
Every Unicode character has a blank, zero-width glyph except '['. Runtime copies
patch that glyph's advance and the private family name; original text and byte
offsets remain available for persistence, plain-text copying and accessibility.
"""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

root = Path(__file__).resolve().parents[1]
font = FontBuilder(1024, isTTF=True)
names = ['.notdef', 'blank', 'object']
font.setupGlyphOrder(names)
font.setupCharacterMap({32: 'blank', 91: 'object'})
font.setupGlyf({name: TTGlyphPen(None).glyph() for name in names})
font.setupHorizontalMetrics({'.notdef': (0, 0), 'blank': (0, 0), 'object': (1024, 0)})
font.setupHorizontalHeader(ascent=1024, descent=0, lineGap=0)
family = 'Mail Inline 0000000000000000'
font.setupNameTable({'familyName': family, 'styleName': 'Regular',
    'uniqueFontIdentifier': family, 'fullName': family,
    'psName': 'MailInline0000000000000000'})
font.setupOS2(sTypoAscender=1024, sTypoDescender=0, sTypoLineGap=0,
    usWinAscent=1024, usWinDescent=0)
font.setupPost()
font.setupMaxp()
cmap = CmapSubtable.newSubtable(13)
cmap.platformID, cmap.platEncID, cmap.language = 3, 10, 0
cmap.cmap = {code: 'blank' for code in range(0x110000)}
cmap.cmap[91] = 'object'
font.font['cmap'].tables = [cmap]
font.font['head'].created = font.font['head'].modified = 0xE7000000
font.font.recalcTimestamp = False
font.save(root / 'resources/InlineObject.ttf')
