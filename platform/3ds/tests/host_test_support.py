"""Host SDL flags for tests, including custom prefixes containing spaces."""
import os
import shlex
import subprocess
import sys
from pathlib import Path


def devkitpro_root():
    configured = os.environ.get('DEVKITPRO')
    if configured:
        return Path(configured)
    for candidate in (Path('/opt/devkitpro'), Path.home() / 'devkitpro'):
        if (candidate / 'libctru/include/3ds.h').is_file():
            return candidate
    raise RuntimeError('Set DEVKITPRO to an installed devkitPro SDK')


def historical_engine_file(root, commit, relative_path):
    # Published commits predate the engine/ directory move; do not rewrite
    # history or substitute a newer implementation for a frozen reference.
    for prefix in ('engine/', 'app/jni/src/'):
        result = subprocess.run(['git', 'show', commit + ':' + prefix + relative_path],
                                cwd=root, capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout
    raise RuntimeError(f'Missing historical file {relative_path} at {commit}')


def sdl_flags(config=None):
    config = config or os.environ.get('SDL2_CONFIG', 'sdl2-config')
    prefix = subprocess.check_output([config, '--prefix'], text=True).strip()
    raw = subprocess.check_output(
        [config, '--cflags', '--libs' if sys.platform == 'darwin' else '--static-libs'],
        text=True)
    flags = shlex.split(raw.replace(prefix, shlex.quote(prefix)))
    if sys.platform == 'darwin':
        # CMake-built SDL uses @rpath, unlike some package-manager builds.
        flags.append('-Wl,-rpath,' + prefix + '/lib')
    return flags
