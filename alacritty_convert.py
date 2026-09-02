#!/usr/bin/env python3
"""Konvertera en alacritty-konfiguration (tema + font) till titty.h."""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from titty_config import parse_color, fc_pattern, patch_header, report

try:
    import tomllib
except ModuleNotFoundError:
    sys.exit('alacritty_convert: kräver python 3.11+ (tomllib)')

ANSI = ['black', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white']


def deep_merge(base, extra):
    for k, v in extra.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            deep_merge(base[k], v)
        else:
            base[k] = v
    return base


def load(path, seen=None):
    seen = seen if seen is not None else set()
    real = os.path.realpath(path)
    if real in seen:
        return {}
    seen.add(real)

    with open(path, 'rb') as f:
        cfg = tomllib.load(f)

    imports = cfg.get('general', {}).get('import') or cfg.get('import') or []
    if isinstance(imports, str):
        imports = [imports]

    merged = {}
    for imp in imports:
        p = os.path.expanduser(os.path.expandvars(imp))
        if not os.path.isabs(p):
            p = os.path.join(os.path.dirname(path), p)
        if os.path.exists(p):
            deep_merge(merged, load(p, seen))
        else:
            sys.stderr.write('warning: import saknas: %s\n' % p)
    return deep_merge(merged, cfg)


def convert(cfg, want_font=True, want_colors=True, want_window=True):
    v = {}
    colors = cfg.get('colors', {})

    if want_colors and colors:
        prim = colors.get('primary', {})
        bg = parse_color(prim.get('background'))
        fg = parse_color(prim.get('foreground'))
        if bg:
            v['COLOR_BG'] = bg
            v['COLOR_BORDER'] = bg
        if fg:
            v['COLOR_FG'] = fg

        cur = colors.get('cursor', {})
        c = parse_color(cur.get('cursor'))
        ct = parse_color(cur.get('text'))
        if c:
            v['COLOR_CURSOR'] = c
        if ct:
            v['COLOR_CURSOR_TEXT'] = ct

        sel = colors.get('selection', {})
        sb = parse_color(sel.get('background'))
        st = parse_color(sel.get('text'))
        if sb:
            v['COLOR_SELECT_BG'] = sb
        if st:
            v['COLOR_SELECT_FG'] = st

        for group, base in (('normal', 0), ('bright', 8)):
            g = colors.get(group, {})
            for i, name in enumerate(ANSI):
                col = parse_color(g.get(name))
                if col:
                    v['COLOR_%d' % (base + i)] = col

    if want_font:
        font = cfg.get('font', {})
        if 'size' in font:
            v['FONT_SIZE'] = '%.1f' % float(font['size'])

        slots = (('normal', 'FONT_REGULAR', 'Regular'),
                 ('bold', 'FONT_BOLD', 'Bold'),
                 ('italic', 'FONT_ITALIC', 'Italic'),
                 ('bold_italic', 'FONT_BOLD_ITALIC', 'Bold Italic'))
        default_family = font.get('normal', {}).get('family')
        for key, define, fallback_style in slots:
            spec = font.get(key, {})
            family = spec.get('family') or default_family
            style = spec.get('style') or fallback_style
            pat = fc_pattern(family, style)
            if pat:
                v[define] = pat

        offset = font.get('offset', {})
        if 'x' in offset:
            v['GLYPH_X_OFFSET'] = int(offset['x'])
        if 'y' in offset:
            v['GLYPH_Y_OFFSET'] = int(offset['y'])

    if not want_window:
        return v

    win = cfg.get('window', {})
    pad = win.get('padding', {})
    if pad:
        px = int(pad.get('x', pad.get('y', 0)))
        py = int(pad.get('y', pad.get('x', 0)))
        v['INNER_BORDER'] = max(px, py)
    if 'opacity' in win:
        v['BG_OPACITY'] = '%.2f' % float(win['opacity'])
    dims = win.get('dimensions', {})
    if 'columns' in dims:
        v['WINDOW_COLS'] = int(dims['columns'])
    if 'lines' in dims:
        v['WINDOW_ROWS'] = int(dims['lines'])
    if win.get('blur'):
        v['FX_BLUR'] = 1

    return v


def main():
    ap = argparse.ArgumentParser(description='Konvertera alacritty-config/tema till titty.h')
    ap.add_argument('config', nargs='+',
                    help='alacritty.toml och/eller temafiler (senare fil vinner)')
    ap.add_argument('-o', '--output', default=None, help='titty.h att uppdatera')
    ap.add_argument('--colors-only', action='store_true')
    ap.add_argument('--font-only', action='store_true')
    ap.add_argument('--no-window', action='store_true',
                    help='hoppa över padding/opacitet/fönsterstorlek')
    ap.add_argument('-n', '--dry-run', action='store_true')
    args = ap.parse_args()

    out = args.output or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'titty.h')
    if not os.path.exists(out):
        sys.exit('hittar inte %s' % out)

    cfg = {}
    for path in args.config:
        p = os.path.expanduser(path)
        if not os.path.exists(p):
            sys.exit('hittar inte %s' % p)
        if p.endswith(('.yml', '.yaml')):
            sys.exit('%s: YAML stöds inte, konvertera till TOML först' % p)
        deep_merge(cfg, load(p))

    values = convert(cfg,
                     want_font=not args.colors_only,
                     want_colors=not args.font_only,
                     want_window=not args.no_window)
    if not values:
        sys.exit('inget att konvertera i angivna filer')

    report(patch_header(out, values, args.dry_run), out, args.dry_run)
    if not args.dry_run:
        print('kör `make` för att bygga om')


if __name__ == '__main__':
    main()
