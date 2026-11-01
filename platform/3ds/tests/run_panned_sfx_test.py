#!/usr/bin/env python3
"""Verify actual SPC channel register writes for left/right and centered SFX."""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[3]
code = r'''
#include <stdio.h>
#include "src/spc_player.c"
static void sample(bool enabled, uint8 pan, int *left, int *right) {
  SpcPlayer *p=calloc(1,sizeof(*p));assert(p);
  DspRegWriteHistory history={0};p->reg_write_history=&history;
  Channel *c=&p->channel[6];c->index=6;c->instrument_pitch_base=1;
  p->sfx_channel_index=12;p->current_bit=0x40;p->sfx_sound_ptr_cur=0x2000;
  p->sfx_start_arg_pan=pan;
  p->ram[0x2000]=8;p->ram[0x2001]=100;p->ram[0x2002]=0x90;
  g_spc_panned_sfx=enabled;Sfx_ChannelTick(p,c,false);
  assert(history.count>=2);assert(history.addr[0]==0x60 && history.addr[1]==0x61);
  *left=history.val[0];*right=history.val[1];free(p);
}
int main(void) {
  int left,right;
  sample(false,0x40,&left,&right);assert(left==right && left>0);
  sample(false,0x80,&left,&right);assert(left==right && left>0);
  sample(true,0,&left,&right);assert(left==right && left>0);
  sample(true,0x40,&left,&right);assert(right>left && left>0);
  sample(true,0x80,&left,&right);assert(left>right && right>0);
  puts("PASS production SPC SFX: disabled centered both ways; enabled center and opposite stereo pans");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-panning-') as directory:
    work=Path(directory);(work/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-fsanitize=address,undefined','-fno-sanitize=shift-base,alignment',
                    '-ffunction-sections','-Wl,-dead_strip','-I'+str(root/'engine'),
                    str(work/'test.c'),str(root/'engine/snes/dsp.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
