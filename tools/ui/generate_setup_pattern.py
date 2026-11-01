#!/usr/bin/env python3
"""Sample the approved, complete purple background onto its native pixel grid.
No fractional tiling, JPEG seams, AI generation or per-screen pattern reset.
The source image is the project's assets/images/BACKGROUND_BLUE.png.
"""
from pathlib import Path
import argparse
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('background',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[2]
im=Image.open(a.background).convert('RGB').resize((400,300),Image.Resampling.NEAREST).crop((0,0,400,240))
values=[(red>>3)<<11|(green>>2)<<5|(blue>>3) for red,green,blue in im.getdata()]
text='// Complete approved purple pattern, sampled at 400x240; no short tile repeat.\n#pragma once\n#include <stdint.h>\nstatic const uint16_t kSetupPattern[400*240] = {\n'
text+=''.join('  '+','.join(f'0x{v:04x}' for v in values[i:i+16])+',\n' for i in range(0,len(values),16))+'};\n'
(r/'engine/src/platform/linux/setup_pattern.h').write_text(text)
