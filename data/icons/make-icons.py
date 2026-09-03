#!/usr/bin/env python3
# make-icons.py - generate word42's icons
#
# Copyright (C) 2026 Andreas Røsdal
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The icons follow GNOME's Adwaita drawing conventions: flat vector shapes in
# the Adwaita palette, built on a 16-unit grid, with edges on whole units and
# one-unit strokes on half units so that nothing lands between pixels at the
# size the toolbar actually uses.
#
# Everything is a real path rather than a bitmap, so the same drawing serves
# the toolbar at 16 and the application icon at 256.  PNGs are rasterised
# beside the SVGs all the same, because a machine without librsvg cannot
# render an SVG icon and would otherwise show nothing at all.
#
#     python3 make-icons.py            regenerate everything
#     python3 make-icons.py --sheet    also write a magnified contact sheet
#
# Rasterising needs rsvg-convert.  The generated files are committed, so an
# ordinary build never runs this.

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The Adwaita palette, by its GNOME names.
BLUE_3, BLUE_4, BLUE_5 = '#3584e4', '#1c71d8', '#1a5fb4'
GREEN_4 = '#2ec27e'
YELLOW_3, YELLOW_5 = '#f6d32d', '#e5a50a'
ORANGE_3 = '#ff7800'
RED_3 = '#e01b24'
BROWN_3 = '#986a44'
LIGHT_1, LIGHT_3, LIGHT_4 = '#ffffff', '#deddda', '#c0bfbc'
DARK_1, DARK_2, DARK_4 = '#77767b', '#5e5c64', '#241f31'

INK = DARK_2          # the default weight for line work
INK_STRONG = DARK_4   # letterforms and anything that must read as text


# ---------------------------------------------------------------------------
# Shared shapes
# ---------------------------------------------------------------------------

def page(x=3, y=1, w=10, h=14, fold=4, fill=LIGHT_1, ink=INK):
    """A sheet of paper with its top-right corner turned back.

    Drawn on half units so the one-unit outline sits on the pixel grid."""
    l, t = x + 0.5, y + 0.5
    r, b = x + w - 0.5, y + h - 0.5
    return (
        '<path d="M{l} {t} H{fx} L{r} {fy} V{b} H{l} Z" fill="{fill}" '
        'stroke="{ink}" stroke-width="1" stroke-linejoin="round"/>'
        '<path d="M{fx} {t} V{fy} H{r}" fill="none" stroke="{ink}" '
        'stroke-width="1" stroke-linejoin="round"/>'
    ).format(l=l, t=t, r=r, b=b, fx=r - fold, fy=t + fold, fill=fill, ink=ink)


def rules(xs, ys, w, colour=DARK_1):
    """The short bars that stand in for text on a miniature page."""
    out = []
    for y in ys:
        out.append('<rect x="%s" y="%s" width="%s" height="1" rx="0.5" '
                   'fill="%s"/>' % (xs, y, w, colour))
    return "".join(out)


# ---------------------------------------------------------------------------
# Toolbar icons, on a 16-unit grid
# ---------------------------------------------------------------------------

ICONS = {}

ICONS['w42-new'] = (
    page() +
    rules(5, [7, 9], 6) +
    # The badge that says this page is a new one.
    '<circle cx="12" cy="12" r="3.6" fill="%s"/>' % GREEN_4 +
    '<path d="M12 10 v4 M10 12 h4" stroke="#ffffff" stroke-width="1.4" '
    'stroke-linecap="round"/>'
)

ICONS['w42-open'] = (
    # Back of the folder, and the tab it is cut from.
    '<path d="M1.5 4a1.5 1.5 0 0 1 1.5-1.5h3.2l1.4 1.8h5.9A1.5 1.5 0 0 1 '
    '14.5 5.8V12a1.5 1.5 0 0 1-1.5 1.5H3A1.5 1.5 0 0 1 1.5 12Z" '
    'fill="%s"/>' % YELLOW_5 +
    # Front flap, tilted open, lighter so the two planes read apart.
    '<path d="M2.6 13.5 4.4 7.2A1.5 1.5 0 0 1 5.8 6.1h9.1a1 1 0 0 1 '
    '1 1.3l-1.7 5.1a1.5 1.5 0 0 1-1.4 1Z" fill="%s"/>' % YELLOW_3
)

ICONS['w42-save'] = (
    '<rect x="1.5" y="1.5" width="13" height="13" rx="1.5" fill="%s"/>' % BLUE_4 +
    # Shutter.
    '<path d="M5 2h6v4.5H5Z" fill="%s"/>' % LIGHT_3 +
    '<rect x="8.6" y="2.6" width="1.6" height="3.2" rx="0.3" fill="%s"/>' % DARK_1 +
    # Label.
    '<rect x="3.8" y="8.6" width="8.4" height="5.9" rx="0.6" fill="%s"/>' % LIGHT_1 +
    rules(5.2, [10.2, 12], 5.6, LIGHT_4)
)

ICONS['w42-print'] = (
    # Sheet going in.
    '<path d="M4.5 1.5h7v3.5h-7Z" fill="%s" stroke="%s" stroke-width="1" '
    'stroke-linejoin="round"/>' % (LIGHT_1, INK) +
    # Body.
    '<rect x="1.5" y="5" width="13" height="6" rx="1.5" fill="%s"/>' % DARK_1 +
    '<circle cx="12.4" cy="7.2" r="0.8" fill="%s"/>' % GREEN_4 +
    # Sheet coming out.
    '<path d="M4.5 9.5h7v5h-7Z" fill="%s" stroke="%s" stroke-width="1" '
    'stroke-linejoin="round"/>' % (LIGHT_1, INK) +
    rules(6, [11.5], 4, LIGHT_4)
)

ICONS['w42-print-preview'] = (
    page(x=1, y=1, w=10, h=13, fold=3) +
    rules(3, [6, 8], 5) +
    # Glass, drawn over the corner of the page.
    '<circle cx="10.5" cy="10.5" r="3.4" fill="#ffffff" fill-opacity="0.9" '
    'stroke="%s" stroke-width="1.4"/>' % BLUE_4 +
    '<path d="M12.9 12.9 15 15" stroke="%s" stroke-width="1.8" '
    'stroke-linecap="round"/>' % BLUE_4
)

ICONS['w42-cut'] = (
    '<path d="M4.6 1.8 10.4 10.6M11.4 1.8 5.6 10.6" stroke="%s" '
    'stroke-width="1.5" stroke-linecap="round"/>' % INK +
    '<circle cx="4.4" cy="12.6" r="2.2" fill="none" stroke="%s" '
    'stroke-width="1.4"/>' % INK +
    '<circle cx="11.6" cy="12.6" r="2.2" fill="none" stroke="%s" '
    'stroke-width="1.4"/>' % INK
)

ICONS['w42-copy'] = (
    # The sheet behind, showing only its top-left corner.
    '<path d="M2.5 1.5h6.6l2.4 2.4V4H6.5A1.5 1.5 0 0 0 5 5.5v6.9H2.5Z" '
    'fill="%s" stroke="%s" stroke-width="1" stroke-linejoin="round"/>'
    % (LIGHT_3, INK) +
    page(x=5, y=4, w=10, h=11, fold=3) +
    rules(7, [9, 11], 5.5)
)

ICONS['w42-paste'] = (
    '<rect x="2.5" y="2.5" width="11" height="12" rx="1.5" fill="%s"/>' % BROWN_3 +
    '<rect x="4.5" y="4.5" width="7" height="8.5" rx="0.6" fill="%s"/>' % LIGHT_1 +
    rules(5.8, [6.5, 8.5, 10.5], 4.4) +
    # The clip at the top.
    '<rect x="5.5" y="1" width="5" height="3" rx="1" fill="%s"/>' % LIGHT_4 +
    '<rect x="6.6" y="0.2" width="2.8" height="2" rx="1" fill="%s"/>' % DARK_1
)

# Undo is drawn once; redo is the same path mirrored, so the two cannot
# drift out of step with each other.
_ARROW = (
    '<path d="M5 5.5h4.5a4 4 0 0 1 0 8H7" fill="none" stroke="{c}" '
    'stroke-width="2" stroke-linecap="round"/>'
    '<path d="M2 5.5 6.4 2.8v5.4Z" fill="{c}"/>'
)

ICONS['w42-undo'] = _ARROW.format(c=BLUE_4)
ICONS['w42-redo'] = ('<g transform="translate(16,0) scale(-1,1)">%s</g>'
                     % _ARROW.format(c=BLUE_4))

ICONS['w42-find'] = (
    '<circle cx="6.8" cy="6.8" r="4.3" fill="#ffffff" stroke="%s" '
    'stroke-width="1.6"/>' % INK +
    '<path d="M10.1 10.1 14.2 14.2" stroke="%s" stroke-width="2" '
    'stroke-linecap="round"/>' % INK
)

ICONS['w42-spelling'] = (
    page(x=1, y=1, w=10, h=12, fold=3) +
    rules(3, [6, 8], 5) +
    '<path d="M8.5 11.6 11 14.2 15 7.6" fill="none" stroke="%s" '
    'stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/>'
    % GREEN_4
)

# Letterforms.  Drawn as paths with their counters punched out by the
# even-odd rule, rather than set in a font, so that a machine without the
# font still shows a B that looks like every other B.
ICONS['w42-bold'] = (
    '<path fill-rule="evenodd" fill="%s" d="'
    'M4 2.6h4.7a3 3 0 0 1 1.9 5.3 3.1 3.1 0 0 1-1.6 5.5H4Z'
    'M6.3 4.7v2.4h2.2a1.2 1.2 0 0 0 0-2.4Z'
    'M6.3 9.1v2.6h2.5a1.3 1.3 0 0 0 0-2.6Z"/>' % INK_STRONG
)

ICONS['w42-italic'] = (
    '<path fill="%s" d="M6.2 2.6h6.3l-.4 1.8h-2L8.2 11.6h2l-.4 1.8H3.5'
    'l.4-1.8h2L7.8 4.4h-2Z"/>' % INK_STRONG
)

ICONS['w42-underline'] = (
    '<path fill="%s" d="M3.6 2.6h2.3v5.9a2.1 2.1 0 0 0 4.2 0V2.6h2.3v5.9'
    'a4.4 4.4 0 0 1-8.8 0Z"/>' % INK_STRONG +
    '<rect x="3" y="12.4" width="10" height="1.8" rx="0.9" fill="%s"/>'
    % INK_STRONG
)


def _align(widths):
    """Four bars; each entry is (x, width) on the 16-unit grid."""
    bars = []
    for i, (x, w) in enumerate(widths):
        bars.append('<rect x="%s" y="%s" width="%s" height="1.8" rx="0.9" '
                    'fill="%s"/>' % (x, 2.4 + i * 3.1, w, INK))
    return "".join(bars)


def _list_lines():
    bars = []
    for i in range(3):
        bars.append('<rect x="7" y="%s" width="7" height="1.8" rx="0.9" '
                    'fill="%s"/>' % (2.9 + i * 4.2, INK))
    return "".join(bars)


ICONS['w42-bullets'] = _list_lines() + "".join(
    '<circle cx="3.6" cy="%s" r="1.5" fill="%s"/>' % (3.8 + i * 4.2, INK_STRONG)
    for i in range(3)
)

# 1, 2, 3 as strokes rather than glyphs, for the same reason the W of the
# application icon is: at 16 units a typeset digit is a smudge.
ICONS['w42-numbering'] = _list_lines() + (
    '<path d="M2.6 3.2 3.8 2.4v3.2" fill="none" stroke="%s" '
    'stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/>'
    '<path d="M2.4 7.2h2.6v1.4l-2.6 1.4h2.6" fill="none" stroke="%s" '
    'stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/>'
    '<path d="M2.4 11.3h2.6l-1.4 1.5h1.4v1.6h-2.6" fill="none" stroke="%s" '
    'stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/>'
    % (INK_STRONG, INK_STRONG, INK_STRONG)
)

ICONS['w42-align-left'] = _align([(2, 12), (2, 8), (2, 12), (2, 8)])
ICONS['w42-align-center'] = _align([(2, 12), (4, 8), (2, 12), (4, 8)])
ICONS['w42-align-right'] = _align([(2, 12), (6, 8), (2, 12), (6, 8)])
ICONS['w42-align-justify'] = _align([(2, 12), (2, 12), (2, 12), (2, 12)])


# ---------------------------------------------------------------------------
# The application icon, on a 64-unit grid
# ---------------------------------------------------------------------------

# The face of the wordmark, on the banner and the sheet alike: Times
# New Roman where the system has it, and Liberation Serif -- drawn to
# the same metrics as its stand-in -- where the rasters are made.
# Times has no weight past bold, so the wordmark strokes its own
# outline to get the very bold it wants.
WORDMARK_FONT = ("'Times New Roman', 'Liberation Serif', Tinos, Times, serif")


def app_icon_body(with_number=True):
    """A sheet of paper carrying the wordmark's W, and the number when
    there is room, set in the wordmark's own face so the icon and the
    banner are one design."""
    body = [
        # Sheet, with the corner turned back.
        '<path d="M9.5 8.5a4 4 0 0 1 4-4h23l14 14v33a4 4 0 0 1-4 4h-33'
        'a4 4 0 0 1-4-4Z" fill="#ffffff" stroke="%s" stroke-width="2.5" '
        'stroke-linejoin="round"/>' % LIGHT_4,
        '<path d="M36.5 4.5v10a4 4 0 0 0 4 4h10Z" fill="%s" stroke="%s" '
        'stroke-width="2.5" stroke-linejoin="round"/>' % (LIGHT_3, LIGHT_4),
    ]

    if with_number:
        # "W42" as one word, set in the wordmark's face; textLength pins
        # the width so the word fills the sheet whichever face answers.
        body.append(
            '<text x="13" y="43" font-family="%s" font-size="17" '
            'font-weight="700" fill="%s" stroke="%s" stroke-width="0.65" '
            'textLength="34" lengthAdjust="spacingAndGlyphs">W42</text>'
            % (WORDMARK_FONT, BLUE_4, BLUE_4))
        # Two lines of text below the word, so the sheet reads as a page.
        body.append(
            '<rect x="14" y="49" width="32" height="2.5" rx="1.25" fill="%s"/>'
            '<rect x="14" y="54.5" width="22" height="2.5" rx="1.25" fill="%s"/>'
            % (LIGHT_4, LIGHT_4))
    else:
        # No number: the W grows into the space, and takes the face's
        # heaviest weight so it stays legible at sixteen pixels.
        body.append(
            '<text x="30" y="49" font-family="%s" font-size="34" '
            'font-weight="700" fill="%s" stroke="%s" stroke-width="1.4" '
            'text-anchor="middle">W</text>'
            % (WORDMARK_FONT, BLUE_4, BLUE_4))

    return "".join(body)


# ---------------------------------------------------------------------------
# Writing the files
# ---------------------------------------------------------------------------

def write_svg(path, body, size, note):
    out = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<!-- %s' % note,
        '     Generated by data/icons/make-icons.py - edit the drawing there,',
        '     not this file.  SPDX-License-Identifier: GPL-3.0-or-later -->',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d">' % (size, size, size, size),
        '  ' + body,
        '</svg>',
        '',
    ]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(out))


ABOUT_SVG = """<?xml version="1.0" encoding="UTF-8"?>
<!-- The About box banner.
     Generated by data/icons/make-icons.py.
     SPDX-License-Identifier: GPL-3.0-or-later -->
<svg xmlns="http://www.w3.org/2000/svg" width="420" height="128"
     viewBox="0 0 420 128">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#f6f5f4"/>
      <stop offset="1" stop-color="#deddda"/>
    </linearGradient>
  </defs>
  <rect width="420" height="128" fill="url(#sky)"/>
  <rect x="0" y="127" width="420" height="1" fill="#c0bfbc"/>

  <g transform="translate(20,16) scale(1.5)">
%(body)s
  </g>

  <!-- The wordmark's face, from WORDMARK_FONT.  textLength pins the
       width, so the rule drawn under it fits whichever face answers. -->
  <text x="136" y="74" font-family="%(font)s"
        font-size="44" font-weight="700" fill="#1c8cff"
        stroke="#1c8cff" stroke-width="1.7"
        textLength="182" lengthAdjust="spacingAndGlyphs">Word42</text>
  <rect x="136" y="81" width="182" height="3.5" rx="1.75" fill="#1c8cff"/>
</svg>
"""


def write_about(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(ABOUT_SVG % {'body': '    ' + app_icon_body(with_number=True),
                              'font': WORDMARK_FONT})


def rasterise(svg, png, width=None, height=None):
    os.makedirs(os.path.dirname(png), exist_ok=True)
    cmd = ['rsvg-convert', '-o', png]
    if width:
        cmd += ['-w', str(width), '-h', str(height)]
    cmd.append(svg)
    subprocess.run(cmd, check=True)


GRESOURCE_HEAD = """<?xml version="1.0" encoding="UTF-8"?>
<!-- Generated by data/icons/make-icons.py.  Do not edit by hand: add an icon
     to the script and re-run it.
     SPDX-License-Identifier: GPL-3.0-or-later -->
<gresources>
  <gresource prefix="/org/word42/word42/icons">
"""


def write_gresource(path):
    """GResource has no globbing, so the manifest is generated beside the
    files it lists and the two cannot drift apart."""
    entries = []
    for root, _dirs, files in os.walk(HERE):
        for name in sorted(files):
            if not name.endswith(('.png', '.svg')):
                continue
            rel = os.path.relpath(os.path.join(root, name), HERE)
            rel = rel.replace(os.sep, '/')
            if '/' not in rel or rel.startswith('contact-sheet'):
                continue
            entries.append(rel)

    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(GRESOURCE_HEAD)
        for rel in sorted(entries):
            fh.write('    <file>%s</file>\n' % rel)
        fh.write('  </gresource>\n</gresources>\n')

    return len(entries)


def contact_sheet(path, scale=4):
    names = sorted(ICONS)
    cols, cell = 5, 16 * scale + 20
    rows = (len(names) + cols - 1) // cols
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">'
           % (cols * cell, rows * cell),
           '<rect width="100%" height="100%" fill="#f6f5f4"/>']
    for i, name in enumerate(names):
        cx = (i % cols) * cell + 10
        cy = (i // cols) * cell + 10
        out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#fff" '
                   'stroke="#deddda"/>' % (cx, cy, 16 * scale, 16 * scale))
        out.append('<g transform="translate(%d,%d) scale(%d)">%s</g>'
                   % (cx, cy, scale, ICONS[name]))
    out.append('</svg>')
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(out))


def main():
    for name, body in sorted(ICONS.items()):
        svg = os.path.join(HERE, 'scalable', 'actions', name + '.svg')
        write_svg(svg, body, 16, '%s - word42 toolbar icon' % name)
        for size in (16, 24, 32, 48):
            rasterise(svg, os.path.join(HERE, '%dx%d' % (size, size),
                                        'actions', name + '.png'), size, size)

    big = os.path.join(HERE, 'scalable', 'apps', 'org.word42.word42.svg')
    write_svg(big, app_icon_body(True), 64, 'word42 application icon')
    for size in (32, 48, 64, 128, 256):
        rasterise(big, os.path.join(HERE, '%dx%d' % (size, size), 'apps',
                                    'org.word42.word42.png'), size, size)

    # Below 32 the number stops being readable and starts being grit, so the
    # small sizes get the drawing without it.
    small = os.path.join(HERE, '16x16', 'apps', 'org.word42.word42.svg')
    write_svg(small, app_icon_body(False), 64,
              'word42 application icon, small sizes')
    for size in (16, 24):
        rasterise(small, os.path.join(HERE, '%dx%d' % (size, size), 'apps',
                                      'org.word42.word42.png'), size, size)

    about = os.path.join(HERE, 'about.svg')
    write_about(about)
    rasterise(about, os.path.join(HERE, 'about.png'), 420, 128)

    n = write_gresource(os.path.join(HERE, 'icons.gresource.xml'))

    if '--sheet' in sys.argv:
        sheet = os.path.join(HERE, 'contact-sheet.svg')
        contact_sheet(sheet)
        rasterise(sheet, os.path.join(HERE, 'contact-sheet.png'))

    print('wrote %d toolbar icons, the application icon and the About banner; '
          'manifest lists %d files' % (len(ICONS), n))


if __name__ == '__main__':
    main()
