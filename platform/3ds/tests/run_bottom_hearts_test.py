#!/usr/bin/env python3
"""Compare retained hearts with the production sidebar under ASan/UBSan."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from host_test_support import sdl_flags

parser = argparse.ArgumentParser()
parser.add_argument('--sdl-config', default=os.environ.get('SDL2_CONFIG', 'sdl2-config'))
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
flags = sdl_flags(args.sdl_config)
with tempfile.TemporaryDirectory(prefix='alttp-hearts-') as tmp:
    exe = str(Path(tmp) / 'hearts')
    subprocess.run(['cc', '-DSDL_MAIN_HANDLED', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-ffunction-sections', '-fdata-sections',
        '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
        '-I' + str(root / 'engine'), '-I' + str(root / 'platform/3ds/source'),
        str(root / 'platform/3ds/tests/bottom_hearts_test.c'), str(root/'engine/src/quality_of_life.c'), str(root/'engine/src/ui_strings.c'), *flags, '-o', exe], check=True)
    subprocess.run([exe], check=True)
