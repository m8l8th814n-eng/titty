#!/usr/bin/env python3
"""Konvertera en kitty.conf (färger, tema, inner border, cursor trail, font) till titty.h."""

import argparse
import os
import re
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from titty_config import parse_color, fc_pattern, patch_header, report

FONT_SLOTS = (('font_family', 'FONT_REGULAR', 'Regular'),
              ('bold_font', 'FONT_BOLD', 'Bold'),
              ('italic_font', 'FONT_ITALIC', 'Italic'),
              ('bold_italic_font', 'FONT_BOLD_ITALIC', 'Bold Italic'))


def read_conf(path, seen=None):
    """kitty.conf är 'nyckel värde' per rad, med include/globinclude."""
    seen = seen if seen is not None else set()
    real = os.path.realpath(path)
    if real in seen:
        return []
    seen.add(real)

    entries = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(None, 1)
            key = parts[0]
            val = parts[1].strip() if len(parts) > 1 else ''

            if key in ('include', 'globinclude', 'envinclude'):
                p = os.path.expanduser(os.path.expandvars(val))
                if not os.path.isabs(p):
                    p = os.path.join(os.path.dirname(path), p)
                if key == 'globinclude':
                    import glob
                    for g in sorted(glob.glob(p)):
                        entries += read_conf(g, seen)
                elif os.path.exists(p):
                    entries += read_conf(p, seen)
                else:
                    sys.stderr.write('warning: include saknas: %s\n' % p)
                continue

            entries.append((key, val))
    return entries


def to_dict(entries):
    d = {}
    for k, v in entries:
        d[k] = v
    return d


def parse_font(val, fallback_style):
    """kitty accepterar både 'Family Name' och 'family="X" style=Y'."""
    if not val or val.lower() in ('auto', 'monospace'):
        return None, None
    if '=' in val and re.search(r'\b(family|style|postscript_name)\s*=', val):
        family = style = None
        for tok in shlex.split(val):
            if '=' not in tok:
                continue
            k, _, v = tok.partition('=')
            if k == 'family':
                family = v
            elif k == 'style':
                style = v
            elif k == 'postscript_name' and not family:
                family = v
        return family, style or fallback_style
    return val.strip('"\''), fallback_style


def parse_padding(val):
    """window_padding_width: en, två eller fyra tal."""
    nums = [float(x) for x in val.replace(',', ' ').split() if x]
    if not nums:
        return None
    return int(round(max(nums)))


def convert(conf, want_font=True, want_colors=True, want_window=True, want_trail=True):
    v = {}

    if want_colors:
        for name, define in (('background', 'COLOR_BG'),
                             ('foreground', 'COLOR_FG'),
                             ('cursor', 'COLOR_CURSOR'),
                             ('cursor_text_color', 'COLOR_CURSOR_TEXT'),
                             ('selection_background', 'COLOR_SELECT_BG'),
                             ('selection_foreground', 'COLOR_SELECT_FG')):
            c = parse_color(conf.get(name))
            if c:
                v[define] = c
        if 'COLOR_BG' in v:
            v['COLOR_BORDER'] = v['COLOR_BG']

        for i in range(16):
            c = parse_color(conf.get('color%d' % i))
            if c:
                v['COLOR_%d' % i] = c

        if conf.get('bold_is_bright'):
            v['BOLD_IS_BRIGHT'] = 1 if conf['bold_is_bright'] == 'yes' else 0

    if want_font:
        if 'font_size' in conf:
            try:
                v['FONT_SIZE'] = '%.1f' % float(conf['font_size'])
            except ValueError:
                pass

        base_family, _ = parse_font(conf.get('font_family', ''), 'Regular')
        for key, define, fallback_style in FONT_SLOTS:
            family, style = parse_font(conf.get(key, ''), fallback_style)
            if family is None:
                family, style = base_family, fallback_style
            pat = fc_pattern(family, style)
            if pat:
                v[define] = pat

        if 'modify_font' in conf:
            m = re.match(r'cell_height\s+([+-]?\d+)', conf['modify_font'])
            if m:
                pass

        if 'text_composition_strategy' in conf:
            parts = conf['text_composition_strategy'].split()
            if parts and parts[0] != 'platform':
                try:
                    v['TEXT_GAMMA'] = '%.2f' % float(parts[0])
                    if len(parts) > 1:
                        v['TEXT_CONTRAST'] = '%.2f' % (float(parts[1]) / 100.0)
                except ValueError:
                    pass

    if want_window:
        if 'window_padding_width' in conf:
            pad = parse_padding(conf['window_padding_width'])
            if pad is not None:
                v['INNER_BORDER'] = pad
        if 'background_opacity' in conf:
            try:
                v['BG_OPACITY'] = '%.2f' % float(conf['background_opacity'])
            except ValueError:
                pass
        if 'background_blur' in conf:
            try:
                v['FX_BLUR'] = 1 if float(conf['background_blur']) > 0 else 0
            except ValueError:
                pass
        if 'remember_window_size' in conf and conf['remember_window_size'] == 'no':
            for key, define in (('initial_window_width', 'WINDOW_COLS'),
                                ('initial_window_height', 'WINDOW_ROWS')):
                val = conf.get(key, '')
                m = re.match(r'^(\d+)c$', val)
                if m:
                    v[define] = int(m.group(1))
        if 'cursor_shape' in conf:
            shape = {'block': 0, 'beam': 1, 'underline': 2}.get(conf['cursor_shape'])
            if shape is not None:
                v['CURSOR_SHAPE'] = shape
        if 'cursor_blink_interval' in conf:
            try:
                iv = float(conf['cursor_blink_interval'].split()[0])
                v['CURSOR_BLINK'] = 1 if iv > 0 else 0
                if iv > 0:
                    v['CURSOR_BLINK_MS'] = int(iv * 1000)
            except ValueError:
                pass

    if want_trail:
        if 'cursor_trail_decay' in conf:
            nums = [float(x) for x in conf['cursor_trail_decay'].split() if x]
            if nums:
                v['CURSOR_TRAIL_DECAY'] = '%.3f' % (sum(nums) / len(nums) / 3.0)
        if 'cursor_trail_start_threshold' in conf:
            try:
                v['CURSOR_TRAIL_START'] = '%.2f' % float(conf['cursor_trail_start_threshold'])
            except ValueError:
                pass
        c = parse_color(conf.get('cursor'))
        if c:
            v['CURSOR_TRAIL_COLOR'] = c

    return v


def main():
    ap = argparse.ArgumentParser(description='Konvertera kitty.conf till titty.h')
    ap.add_argument('config', nargs='+', help='kitty.conf och/eller temafiler (senare fil vinner)')
    ap.add_argument('-o', '--output', default=None, help='titty.h att uppdatera')
    ap.add_argument('--colors-only', action='store_true')
    ap.add_argument('--font-only', action='store_true')
    ap.add_argument('--no-window', action='store_true',
                    help='hoppa över padding/opacitet/fönsterstorlek')
    ap.add_argument('--no-trail', action='store_true', help='rör inte cursor trail')
    ap.add_argument('-n', '--dry-run', action='store_true')
    args = ap.parse_args()

    out = args.output or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'titty.h')
    if not os.path.exists(out):
        sys.exit('hittar inte %s' % out)

    entries = []
    for path in args.config:
        p = os.path.expanduser(path)
        if not os.path.exists(p):
            sys.exit('hittar inte %s' % p)
        entries += read_conf(p)

    values = convert(to_dict(entries),
                     want_font=not args.colors_only,
                     want_colors=not args.font_only,
                     want_window=not args.no_window,
                     want_trail=not args.no_trail)
    if not values:
        sys.exit('inget att konvertera i angivna filer')

    report(patch_header(out, values, args.dry_run), out, args.dry_run)
    if not args.dry_run:
        print('kör `make` för att bygga om')


if __name__ == '__main__':
    main()
