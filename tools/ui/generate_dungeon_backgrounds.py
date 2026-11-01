#!/usr/bin/env python3
"""Original native-resolution dungeon backdrops. Standard library only.
No ROM data, fonts or extracted game artwork. Integer pixels, no smoothing.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, zipfile
from generate_native_ui import png
from capture_sheet import caption

# Native dungeon IDs, material colors, and original relief motifs.
THEMES = [
 ('sewers','ALCANTARILLAS',(57,70,65),'drain'),
 ('hyrule-castle','CASTILLO DE HYRULE',(69,77,94),'crest'),
 ('eastern-palace','PALACIO DEL ESTE',(57,82,65),'eye'),
 ('desert-palace','PALACIO DEL DESIERTO',(119,97,61),'sand'),
 ('castle-tower','TORRE DEL CASTILLO',(72,62,94),'arcane'),
 ('swamp-palace','PALACIO DEL PANTANO',(48,83,84),'water'),
 ('dark-palace','PALACIO OSCURO',(69,77,91),'mask'),
 ('misery-mire','CIENAGA DE LA MISERIA',(67,74,48),'vines'),
 ('skull-woods','BOSQUE DE LAS CALAVERAS',(72,63,49),'skull'),
 ('ice-palace','PALACIO DE HIELO',(69,99,117),'ice'),
 ('tower-of-hera','TORRE DE HERA',(105,73,74),'hera'),
 ('thieves-town','PUEBLO DE LOS LADRONES',(77,62,69),'lock'),
 ('turtle-rock','ROCA DE LA TORTUGA',(64,58,60),'lava'),
 ('ganons-tower','TORRE DE GANON',(60,59,88),'ganon'),
]

def rgb565(c):
 r,g,b=[max(0,min(255,int(v))) for v in c]
 return ((r>>3)*255//31,(g>>2)*255//63,(b>>3)*255//31,255)

class Canvas:
 def __init__(self,w,h,color):
  self.w,self.h=w,h; self.data=bytearray(bytes(color)*w*h)
 def pixel(self,x,y,c):
  if 0<=x<self.w and 0<=y<self.h:
   o=(y*self.w+x)*4;self.data[o:o+4]=bytes(c)
 def rect(self,x,y,w,h,c):
  for yy in range(max(0,y),min(self.h,y+h)):
   for xx in range(max(0,x),min(self.w,x+w)):self.pixel(xx,yy,c)
 def line(self,x,y,xx,yy,c):
  dx,dy=abs(xx-x),-abs(yy-y);sx=1 if x<xx else -1;sy=1 if y<yy else -1;e=dx+dy
  while True:
   self.pixel(x,y,c)
   if (x,y)==(xx,yy):break
   e2=e*2
   if e2>=dy:e+=dy;x+=sx
   if e2<=dx:e+=dx;y+=sy
 def path(self,points,c):
  for a,b in zip(points,points[1:]):self.line(*a,*b,c)

def background(index):
 slug,title,base,motif=THEMES[index]
 colors=[rgb565(tuple(v+d for v in base)) for d in (-28,-17,-8,0,7,17,25)]
 mortar,shade,dark,face,light,edge,glint=colors
 c=Canvas(320,240,face)
 bh=32 if motif not in ('sand','ice','lava','skull') else 40
 bw=64 if motif not in ('sand','hera','ganon') else 80
 for row,y in enumerate(range(0,240,bh)):
  shift=bw//2 if row%2 else 0
  for x in range(-shift,320,bw):
   c.rect(x,y,bw,bh,mortar);c.rect(x+2,y+2,bw-3,bh-3,face)
   c.line(x+2,y+2,x+bw-3,y+2,edge);c.line(x+2,y+3,x+2,y+bh-3,light)
   c.line(x+bw-2,y+2,x+bw-2,y+bh-2,shade);c.line(x+2,y+bh-2,x+bw-2,y+bh-2,shade)
   # Sparse, deterministic chips; no dithering or resampling.
   for k in range(7):
    xx=x+6+((row*29+k*17+index*7)%max(1,bw-14));yy=y+6+((k*11+row*5)%max(1,bh-12))
    c.rect(xx,yy,2 if k%3==0 else 1,1,dark if k%2 else light)
   if (row+x//bw+index)%3==0:
    c.path([(x+bw-14,y+3),(x+bw-14,y+7),(x+bw-18,y+11),(x+bw-18,y+15)],dark)
 # A pair of inset ornament courses. Keep central field calm for map/text.
 accent=rgb565({'lava':(120,65,47),'vines':(67,91,48),'water':(53,107,107),'ice':(110,145,160),'ganon':(112,98,70),'crest':(111,100,72),'hera':(137,112,78)}.get(motif,tuple(v+19 for v in base)))
 for cy in (24,216):
  for cx in (32,112,192,272):
   def line(a,b,col=dark):c.line(cx+a[0],cy+a[1],cx+b[0],cy+b[1],col)
   def path(points,col=dark):c.path([(cx+x,cy+y) for x,y in points],col)
   def rect(x,y,w,h,col=dark):c.rect(cx+x,cy+y,w,h,col)
   if motif=='drain':
    rect(-11,-8,23,16,shade);rect(-9,-6,19,12,dark)
    for x in range(-8,10,4):rect(x,-6,1,12,edge)
    line((-11,8),(11,8),light)
   elif motif in ('crest','hera','ganon'):
    path([(-12,-8),(12,-8),(10,4),(0,12),(-10,4),(-12,-8)],shade)
    if motif=='crest':
     path([(-8,-4),(-5,0),(0,-5),(5,0),(8,-4),(6,5),(-6,5),(-8,-4)],accent)
    elif motif=='hera':
     path([(-7,6),(0,-8),(7,6),(-7,6)],accent);line((-9,9),(9,9),edge)
    else:
     path([(-8,0),(0,-9),(8,0),(0,9),(-8,0)],accent);path([(-4,0),(0,-4),(4,0),(0,4),(-4,0)],light)
   elif motif in ('eye','mask'):
    path([(-12,0),(-6,-5),(6,-5),(12,0),(6,5),(-6,5),(-12,0)],shade)
    rect(-2,-3,5,7,accent);rect(0,-2,1,5,shade)
    if motif=='mask':path([(-12,-9),(-12,7),(-6,11),(6,11),(12,7),(12,-9)],edge)
   elif motif=='sand':
    path([(-13,7),(-13,-7),(-4,-7),(-4,3),(4,3),(4,-7),(13,-7),(13,7)],shade)
    line((-10,10),(10,10),edge);rect(-1,-6,3,3,accent)
   elif motif=='arcane':
    path([(0,-11),(10,0),(0,11),(-10,0),(0,-11)],shade)
    path([(0,-7),(6,0),(0,7),(-6,0),(0,-7)],edge)
    line((-13,0),(13,0),dark);line((0,-13),(0,13),dark)
   elif motif=='water':
    for y in (-6,0,6):path([(-13,y),(-8,y-2),(-3,y-2),(2,y+1),(7,y+1),(12,y-1)],accent if y==0 else dark)
   elif motif=='vines':
    path([(-14,10),(-8,3),(-3,4),(1,-2),(8,-4),(12,-12)],shade)
    for x,y in [(-8,3),(1,-2),(8,-4)]:
     path([(x,y),(x-5,y-5),(x-1,y-5),(x,y)],accent)
   elif motif=='skull':
    path([(-8,3),(-10,-1),(-10,-6),(-6,-10),(6,-10),(10,-6),(10,-1),(8,3),(5,3),(5,9),(-5,9),(-5,3),(-8,3)],edge)
    rect(-6,-3,4,4,shade);rect(3,-3,4,4,shade);rect(0,2,1,3,shade)
    for x in (-3,0,3):rect(x,7,1,3,shade)
   elif motif=='ice':
    for a,b in [((0,-12),(0,12)),((-10,-6),(10,6)),((-10,6),(10,-6))]:line(a,b,accent)
    for x in (-1,1):
     path([(x*4,-9),(0,-5),(-x*4,-9)],edge);path([(x*4,9),(0,5),(-x*4,9)],edge)
   elif motif=='lock':
    rect(-11,-10,23,22,shade);rect(-9,-8,19,18,dark)
    path([(-4,-1),(-4,-5),(-2,-7),(2,-7),(4,-5),(4,-1)],edge)
    rect(-6,-1,13,8,accent);rect(0,1,1,4,shade)
   elif motif=='lava':
    path([(-13,-11),(-5,-4),(-8,1),(1,4),(5,12)],shade)
    path([(-12,-11),(-4,-4),(-7,1),(2,4),(6,12)],accent)
    path([(-4,-4),(4,-7),(11,-5)],dark)
 if motif=='skull':
  for x in (0,156,312):
   c.rect(x,0,7,240,shade);c.line(x+2,0,x+2,239,light)
   for y in range(12,240,32):c.path([(x+4,y),(x+3,y+7),(x+5,y+14)],dark)
 if motif in ('water','drain','vines'):
  moss=rgb565((48,74,53))
  for x in range(12,320,47):
   for j in range(7):c.rect(x+j*2,33+(x//47%4)*bh,2,1+(j*7+x)%4,moss)
 return c

def make_sheet(out,images,start,stop,name):
 cols=2;rows=(stop-start+1)//2;w=688;h=24+rows*274
 c=Canvas(w,h,(17,19,22,255))
 for j,i in enumerate(range(start,stop)):
  x=16+(j%2)*336;y=16+(j//2)*274
  caption(c.data,w,x,y,THEMES[i][1])
  for yy in range(240):
   off=((y+16+yy)*w+x)*4;c.data[off:off+1280]=images[i].data[yy*1280:(yy+1)*1280]
 (out/name).write_bytes(png(w,h,c.data))

def native_header():
 palettes=[];streams=[];offsets=[0]
 for i in range(14):
  data=background(i).data;colors=[];ids=[]
  for k in range(0,len(data),4):
   c=tuple(data[k:k+4])
   if c not in colors:colors.append(c)
   ids.append(colors.index(c))
  palettes.append([0xff000000|(r<<16)|(g<<8)|b for r,g,b,a in colors]+[0]*(16-len(colors)))
  run=1
  for k in range(1,len(ids)+1):
   if k<len(ids) and ids[k]==ids[k-1] and run<255:run+=1
   else:streams.extend((run,ids[k-1]));run=1
  offsets.append(len(streams))
 text='// Generated by tools/ui/generate_dungeon_backgrounds.py --native-header.\n#pragma once\n#include <stdint.h>\n'
 text+='static const uint32_t kDungeonBgPalette[14][16]={\n'+''.join('{'+','.join(hex(c) for c in pal)+'},\n' for pal in palettes)+'};\n'
 text+='static const uint32_t kDungeonBgOffsets[15]={'+','.join(map(str,offsets))+'};\n'
 text+='static const uint8_t kDungeonBgRuns[]={\n'+''.join(','.join(map(str,streams[i:i+32]))+',\n' for i in range(0,len(streams),32))+'};\n'
 return text

def main():
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path);p.add_argument('--native-header',type=Path);a=p.parse_args();
 if a.native_header:a.native_header.write_text(native_header())
 if not a.out:return
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 images=[];manifest=[]
 for i,(slug,title,base,motif) in enumerate(THEMES):
  c=background(i);assert c.data==background(i).data
  assert len(c.data)==320*240*4 and all(c.data[k]==255 for k in range(3,len(c.data),4))
  colors={bytes(c.data[k:k+4]) for k in range(0,len(c.data),4)};assert len(colors)<=10
  data=png(320,240,c.data);name=f'{i:02d}-{slug}.png';(out/name).write_bytes(data);images.append(c)
  manifest.append(dict(dungeon_id=i,file=name,title=title,size=[320,240],colors=len(colors),sha256=hashlib.sha256(data).hexdigest()))
 assert len({m['sha256'] for m in manifest})==14
 make_sheet(out,images,0,8,'01-light-world-and-mire.png');make_sheet(out,images,8,14,'02-dark-world.png')
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 (out/'README.txt').write_text('DUNGEON BACKGROUNDS - ART REVIEW v1\n\n14 original backgrounds, 320 x 240 opaque pixels each.\nNative dungeon IDs follow the port ordering. RGB565-compatible palette,\ninteger-pixel drawing, no antialiasing. No text, buttons or game-map data\nare embedded in the backgrounds. The two contact sheets add external labels.\nThese are custom material studies, not historical game screenshots.\nThis delivery does not change the game renderer.\n\nRegenerate: python3 generate_dungeon_backgrounds.py --out output\nKeep the three Python source files together. Standard library only.\n')
 for name in ('generate_dungeon_backgrounds.py','generate_native_ui.py','capture_sheet.py'):shutil.copyfile(Path(__file__).with_name(name),out/name)
 with zipfile.ZipFile(out.with_suffix('.zip'),'w',zipfile.ZIP_DEFLATED) as z:
  for f in sorted(out.iterdir()):
   if f.is_file():z.write(f,f.name)
 print(f'PASS: 14 distinct deterministic opaque 320x240 backgrounds, <=10 colors; {out}')
if __name__=='__main__':main()
