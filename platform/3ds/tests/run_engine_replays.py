#!/usr/bin/env python3
"""Run the native engine beside the cartridge using privately supplied inputs.

No game data belongs in the source tree. The work directory must be outside it.
Reference replays are the snesrev/zelda3 saves/ref chapter recordings.
"""
from pathlib import Path
import argparse, json, os, shlex, shutil, subprocess

root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser()
p.add_argument('--rom',type=Path,required=True)
p.add_argument('--assets',type=Path,required=True)
p.add_argument('--refs',type=Path,required=True)
p.add_argument('--work-dir',type=Path,required=True)
p.add_argument('--frames',type=int,default=2000)
p.add_argument('--chapters',type=int,nargs='*',default=list(range(13)))
a=p.parse_args(); work=a.work_dir.resolve()
if work==root or root in work.parents: p.error('Keep private inputs outside tracked source')
work.mkdir(parents=True,exist_ok=True)
config=os.environ.get('SDL2_CONFIG','sdl2-config')
prefix=subprocess.check_output([config,'--prefix'],text=True).strip()
raw=subprocess.check_output([config,'--cflags','--libs'],text=True)
flags=shlex.split(raw.replace(prefix,shlex.quote(prefix)))
sources=[str(x) for x in (root/'engine/src').glob('*.c') if x.name not in ['main.c','opengl.c','glsl_shader.c']]
sources += [str(x) for x in (root/'engine/src/rando').glob('*.c')]
sources += [str(root/'engine/third_party/sha256/sha256.c')]
sources += [str(x) for x in (root/'engine/snes').glob('*.c')]
sources += [str(root/'engine/src/platform/linux/second_screen_sdl.c'),str(root/'engine/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c')]
exe=work/'engine-replay'
with (work/'build.log').open('w') as log:
    subprocess.run(['cc','-DSDL_MAIN_HANDLED','-DSYSTEM_VOLUME_MIXER_AVAILABLE=0','-O1','-g',
        '-fsanitize=address,undefined','-fno-sanitize=shift-base,alignment',
        '-ffunction-sections','-fdata-sections','-Wl,-dead_strip',
        '-I'+str(root/'engine'),'-iquote',str(root/'engine/src'),
        '-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),
        str(root/'platform/3ds/tests/engine_replay_test.c'),*sources,*flags,'-o',str(exe)],
        stdout=log,stderr=subprocess.STDOUT,check=True)
data=work/'data'; (data/'saves/ref').mkdir(parents=True,exist_ok=True)
shutil.copyfile(a.assets,data/'zelda3_assets.dat')
for source in a.refs.glob('*.sav'): shutil.copyfile(source,data/'saves/ref'/source.name)
(data/'zelda3.ini').write_text('[General]\nAutosave = 0\n[Sound]\nEnableAudio = 0\n')
results=[]
for chapter in a.chapters:
    result=subprocess.run([str(exe),str(data),str(a.rom.resolve()),str(chapter),str(a.frames)],
        capture_output=True,text=True,timeout=60)
    log=result.stdout+result.stderr
    (work/f'chapter-{chapter}.log').write_text(log)
    passed=result.returncode==0 and 'compare failed' not in log.lower() and 'runtime error:' not in log
    results.append({'chapter':chapter,'frames':a.frames,'passed':passed,'exit':result.returncode})
    print(('PASS' if passed else 'FAIL'), 'native/cartridge chapter',chapter,flush=True)
(work/'results.json').write_text(json.dumps(results,indent=2)+'\n')
raise SystemExit(0 if all(x['passed'] for x in results) else 1)
