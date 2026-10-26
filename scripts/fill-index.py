#!/usr/bin/env python3
"""Insert the built package lists into the static GitHub Pages index."""
import glob
import os
import sys


def items(pattern):
    return ''.join(
        '        <li><code>%s</code></li>\n' % os.path.basename(p)
        for p in sorted(glob.glob(pattern))
    )


def main():
    arch, kver = sys.argv[1], sys.argv[2]
    root = 'public'
    path = os.path.join(root, 'index.html')
    with open(path) as fh:
        html = fh.read()
    html = html.replace('<!--PACKAGES-->',
                        items(os.path.join(root, arch, '*.apk')))
    html = html.replace('<!--KMODPACKAGES-->',
                        items(os.path.join(root, 'kmods', kver, '*.apk')))
    with open(path, 'w') as fh:
        fh.write(html)


if __name__ == '__main__':
    main()
