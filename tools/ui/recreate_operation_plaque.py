#!/usr/bin/env python3
"""Trace the owner-supplied ornamental plaque onto a native pixel grid.
Each source cell is sampled explicitly, palette-indexed and stored as a sprite.
The black lettering area becomes the progress opening. No filtering at runtime.
"""
from pathlib import Path
import argparse
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('reference',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[2]
image=Image.open(a.reference).convert('RGB')
assert image.size==(744,180),image.size
left,top,right,bottom=72,39,684,160
width,height=192,38
native=Image.new('RGB',(width,height));alpha=[]
for y in range(height):
 for x in range(width):
  # Center of each original logical pixel; the screenshot is about 3.18x.
  sx=left+(2*x+1)*(right-left)//(2*width)
  sy=top+(2*y+1)*(bottom-top)//(2*height)
  red,green,blue=image.getpixel((sx,sy))
  opening=20<=x<172 and 9<=y<30
  opaque=not opening and not (blue>red and blue>green)
  alpha.append(opaque)
  native.putpixel((x,y),(red,green,blue) if opaque else (0,0,0))
indexed=native.quantize(colors=16,method=Image.Quantize.MEDIANCUT)
palette=indexed.getpalette();colors=[]
for i in range(16):
 red,green,blue=palette[i*3:i*3+3];colors.append((red>>3)<<11|(green>>2)<<5|blue>>3)
indices=[i+1 if visible else 0 for i,visible in zip(indexed.getdata(),alpha)]
text='// Native 192x38 pixel trace of the approved ornamental progress plaque.\n#pragma once\n#include <stdint.h>\n'
text+='static const uint16_t kOperationPlaquePalette[17]={0,'+','.join(hex(c)for c in colors)+'};\n'
text+='static const uint8_t kOperationPlaque[192*38]={\n'+''.join(' '+','.join(map(str,indices[i:i+32]))+',\n'for i in range(0,len(indices),32))+'};\n'
(r/'engine/src/platform/linux/operation_plaque.h').write_text(text)
