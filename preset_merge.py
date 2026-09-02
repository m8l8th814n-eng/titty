#!/usr/bin/env python3
"""Slå ihop titty.h med ett preset till en fristående konfigheader."""

import os
import re
import sys

DEF = re.compile(r'^(#define\s+)([A-Z0-9_]+)(\s+)(.*?)(\s*)$')


def read_preset(path):
    over = {}
    order = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            m = DEF.match(line.rstrip('\n'))
            if m:
                name = m.group(2)
                if name not in over:
                    order.append(name)
                over[name] = m.group(4)
    return over, order


def merge(base_path, preset_path, out_path, preset_name):
    over, order = read_preset(preset_path)
    with open(base_path, encoding='utf-8') as f:
        lines = f.readlines()

    seen = set()
    out = []
    for line in lines:
        m = DEF.match(line.rstrip('\n'))
        if m and m.group(2) in over:
            name = m.group(2)
            seen.add(name)
            pad = m.group(3)
            width = len(name) + len(pad)
            pad = ' ' * max(1, width - len(name))
            line = '%s%s%s%s\n' % (m.group(1), name, pad, over[name])
        out.append(line)

    missing = [n for n in order if n not in seen]
    if missing:
        extra = ['\n/* från presets/%s.h, saknas i titty.h */\n' % preset_name]
        for n in missing:
            extra.append('#define %-17s %s\n' % (n, over[n]))
        for i, l in enumerate(out):
            if l.strip() == '#endif':
                out[i:i] = extra
                break

    banner = ('/* genererad av preset_merge.py: titty.h + presets/%s.h\n'
              '   kopiera till titty.h for att anvanda: cp %s titty.h */\n'
              % (preset_name, os.path.basename(out_path)))
    body = ''.join(out)
    body = body.replace('#ifndef TITTY_H\n', banner + '#ifndef TITTY_H\n', 1)

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(body)
    return len(seen), len(missing)


def main():
    if len(sys.argv) < 2:
        sys.exit('anvandning: preset_merge.py <preset> [preset ...]')
    here = os.path.dirname(os.path.abspath(__file__))
    for name in sys.argv[1:]:
        preset = os.path.join(here, 'presets', name + '.h')
        if not os.path.exists(preset):
            sys.exit('hittar inte %s' % preset)
        out = os.path.join(here, 'titty.h.' + name)
        n, extra = merge(os.path.join(here, 'titty.h'), preset, out, name)
        print('%-16s %d varden ersatta%s' %
              (os.path.basename(out), n, ', %d tillagda' % extra if extra else ''))


if __name__ == '__main__':
    main()
