import re
import sys

RGB = re.compile(r'^#?([0-9a-fA-F]{6})$')


def parse_color(v):
    if v is None:
        return None
    s = str(v).strip().strip("'\"")
    if s.startswith('0x') or s.startswith('0X'):
        s = s[2:]
    m = RGB.match(s)
    if not m:
        return None
    return '0x' + m.group(1).lower()


def fc_escape(name):
    out = ''
    for ch in name:
        if ch in '-:,\\':
            out += '\\'
        out += ch
    return out


def fc_pattern(family, style=None):
    if not family:
        return None
    p = fc_escape(family)
    if style:
        p += ':style=' + fc_escape(style)
    return '"%s"' % p


def patch_header(path, values, dry_run=False):
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    seen = set()
    out = []
    changed = []
    for line in lines:
        m = re.match(r'^(#define\s+)([A-Z0-9_]+)(\s+)(.*?)(\s*)$', line)
        if m and m.group(2) in values:
            name = m.group(2)
            new = str(values[name])
            old = m.group(4)
            seen.add(name)
            if old != new:
                changed.append((name, old, new))
                pad = m.group(3)
                width = len(m.group(2)) + len(pad)
                pad = ' ' * max(1, width - len(name))
                line = '%s%s%s%s\n' % (m.group(1), name, pad, new)
        out.append(line)

    missing = [k for k in values if k not in seen]
    if missing:
        sys.stderr.write('warning: not present in %s: %s\n' % (path, ', '.join(sorted(missing))))

    if not dry_run:
        with open(path, 'w', encoding='utf-8') as f:
            f.writelines(out)
    return changed


def report(changed, path, dry_run):
    if not changed:
        print('%s: inga ändringar' % path)
        return
    for name, old, new in changed:
        print('  %-18s %-22s -> %s' % (name, old, new))
    print('%s: %d värden %s' % (path, len(changed), 'skulle ändras' if dry_run else 'uppdaterade'))
