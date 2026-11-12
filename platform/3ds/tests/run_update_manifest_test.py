#!/usr/bin/env python3
"""Host checks for update_manifest.c: the API manifest, the release-URL tag and
the sha256sums fallback. Needs jansson (devkitPro portlibs or the host package)."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def find_jansson():
    inc = os.environ.get('JANSSON_INCLUDE')
    lib = os.environ.get('JANSSON_LIB')
    if inc and lib:
        return ['-I' + inc], [lib]
    devkitpro = os.environ.get('DEVKITPRO')
    candidates = []
    if devkitpro:
        candidates.append(Path(devkitpro) / 'portlibs/3ds/include')
    candidates.append(Path('/opt/devkitpro/portlibs/3ds/include'))
    for path in candidates:
        if (path / 'jansson.h').exists():
            return ['-I' + str(path)], ['-ljansson']
    return ['-I/usr/include'], ['-ljansson']


def jansson_linkable(cc, inc, link):
    """The container ships jansson headers for the 3DS target only, so the host
    link can fail there; that is a skip, not a failure."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'probe.c'
        src.write_text('#include <jansson.h>\nint main(void){return json_object()?0:0;}\n')
        try:
            subprocess.run([cc] + inc + [str(src), '-o', str(Path(tmp) / 'probe')] + link,
                           check=True, capture_output=True)
        except (subprocess.CalledProcessError, OSError):
            return False
    return True


def main():
    cc = os.environ.get('CC', 'cc')
    inc, link = find_jansson()
    if not jansson_linkable(cc, inc, link):
        print('SKIP update manifest: no host jansson to link against')
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'update_manifest_test'
        subprocess.run([cc, '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror'] + inc +
                       ['-iquote', str(ROOT / 'platform/3ds/source'),
                        str(ROOT / 'platform/3ds/tests/update_manifest_test.c'),
                        str(ROOT / 'platform/3ds/source/update_manifest.c'),
                        '-o', str(exe)] + link, check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
