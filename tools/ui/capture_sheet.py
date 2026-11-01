#!/usr/bin/env python3
"""Convert native BMP captures to PNG and make lossless 1x review sheets.
Uses only Python's standard library. Screenshots stay exactly 320x240 pixels;
all captions and spacing live outside the captured image.
"""
from pathlib import Path
import argparse, importlib.util, struct
spec=importlib.util.spec_from_file_location('native_ui',Path(__file__).with_name('generate_native_ui.py'))
ui=importlib.util.module_from_spec(spec);spec.loader.exec_module(ui)

def load_bmp(path):
 data=path.read_bytes();offset=struct.unpack_from('<I',data,10)[0]
 width,height=struct.unpack_from('<ii',data,18);bits=struct.unpack_from('<H',data,28)[0]
 assert bits in (24,32) and width>0 and height!=0
 stride=((width*bits+31)//32)*4;pixels=bytearray(width*abs(height)*4)
 for y in range(abs(height)):
  source=offset+(abs(height)-1-y if height>0 else y)*stride
  for x in range(width):
   i=source+x*(bits//8);o=(y*width+x)*4
   pixels[o:o+4]=bytes((data[i+2],data[i+1],data[i],255))
 return width,abs(height),pixels

def caption(pixels,width,x,y,text):
 for char in text:
  if char in ui.ACCENTS:char=ui.ACCENTS[char][0]
  rows=ui.LETTERS[ord(char)-65] if 'A'<=char<='Z' else ui.DIGITS[int(char)] if char.isdigit() else ui.PUNCT.get(char,[0]*7)
  for yy,row in enumerate(rows):
   for xx in range(5):
    if row&(1<<(4-xx)):
     o=((y+yy)*width+x+xx)*4;pixels[o:o+4]=bytes((226,206,156,255))
  x+=6

def sheet(folder,name,entries):
 cols=3;width=1024;rows=(len(entries)+cols-1)//cols;height=16+rows*276
 pixels=bytearray(bytes((14,16,14,255))*width*height)
 for i,(file,title) in enumerate(entries):
  x=16+(i%cols)*336;y=16+(i//cols)*276
  caption(pixels,width,x,y,title)
  w,h,image=load_bmp(folder/file);assert(w,h)==(320,240)
  for yy in range(h):
   o=((y+16+yy)*width+x)*4;pixels[o:o+w*4]=image[yy*w*4:(yy+1)*w*4]
 (folder/(name+'.png')).write_bytes(ui.png(width,height,pixels))

def main():
 p=argparse.ArgumentParser();p.add_argument('folder',type=Path);a=p.parse_args();folder=a.folder
 for path in folder.glob('*.bmp'):
  w,h,data=load_bmp(path);path.with_suffix('.png').write_bytes(ui.png(w,h,data))
 sheet(folder,'menus-es',[(f'es-{name}-old.bmp',title) for name,title in [
  ('settings','AJUSTES'),('game','JUEGO'),('display','PANTALLA'),
  ('developer','DESARROLLO'),('information','INFORMACION'),('load-confirm','CONFIRMACION')]])
 sheet(folder,'game-es',[(f'es-{name}-old.bmp',title) for name,title in [
  ('gear','EQUIPO'),('items','OBJETOS'),('dungeon-13','MAZMORRA - VARIOS PISOS'),
  ('map-0-0','MAPA - CERCA'),('map-1-1','MAPA - VISTA COMPLETA'),('update-3-1','ACTUALIZADOR')]])
 sheet(folder,'title-and-updates',[(f'es-{name}-old.bmp',title) for name,title in [('cinema','TITULO - REPOSO'),('title-settings','AJUSTES DESDE EL TITULO'),('update-3-0','ACTUALIZACION DISPONIBLE'),('update-5-0','DESCARGA'),('update-3-2','CONFIRMACION DE INSTALACION'),('update-9-0','ERROR DE CONEXION')]])
 sheet(folder,'languages',[(f'{code}-game-old.bmp',label) for code,label in [
  ('en','ENGLISH'),('es','ESPANOL'),('pt','PORTUGUES'),('fr','FRANCAIS'),('it','ITALIANO'),('de','DEUTSCH')]])
 print('PASS lossless native PNG captures and four 1x review sheets')
if __name__=='__main__':main()
