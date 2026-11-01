#!/usr/bin/env python3
"""Render production menus on both screen formats; keep private art outside Git."""
from pathlib import Path
import os, argparse, shlex, subprocess, sys, tempfile
from host_test_support import sdl_flags

root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser()
p.add_argument('--out',type=Path,required=True)
p.add_argument('--art',type=Path,help='Private USA artwork exported for local review')
p.add_argument("--production",action="store_true")
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
flags=sdl_flags()+(["-DZELDA3_ENABLE_DEVELOPER_TESTS=0"]if a.production else [])
with tempfile.TemporaryDirectory() as td:
 exe=str(Path(td)/'ui')
 subprocess.run(['cc','-DSDL_MAIN_HANDLED','-O1','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
  '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
  '-I'+str(root/'engine'),'-I'+str(root/'platform/3ds/source'),
  str(root/'platform/3ds/tests/native_ui_layout_test.c'),str(root/'engine/src/ui_strings.c'),str(root/'engine/src/quality_of_life.c'),str(root/'engine/src/test_scenarios.c'),*flags,'-o',exe],check=True)
 # INI changes from navigation tests belong in the temporary fixture directory.
 subprocess.run([exe,str(a.out.resolve()),*([str(a.art.resolve())] if a.art else [])],cwd=td,check=True)
