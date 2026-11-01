#!/usr/bin/env python3
"""Exercise actual profile preparation with non-overwriting SD rename and faults."""
from pathlib import Path
import subprocess, tempfile, os
r=Path(__file__).resolve().parents[3]
s=(r/'platform/3ds/source/platform_3ds.c').read_text()
def fn(sig):
 a=s.index(sig);b=s.index('{',a)+1;n=1
 while n:n+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
static int renames, extract_calls, selected_writes, upgrade_calls;
static int sd_rename(const char *from,const char *to) {
 renames++;
 const char *fault=getenv("RENAME_FAULT");
 if(fault && (atoi(fault)==renames || (!strcmp(fault,"2+3") && renames==3))) {
  errno=EIO;return -1;
 }
 struct stat st;if(stat(to,&st)==0){errno=EEXIST;return -1;}
 return rename(from,to);
}
#define rename sd_rename
#include "file_store.h"
#define LogSetup(...) ((void)0)
static const char *kBundledConfig="bundled.ini",*kProfilesDirectory="profiles";
static const char *kAssetsFilename="zelda3_assets.dat";
// The port runs with the profile folder as its working directory and reaches
// the ROM through this absolute SD path. Here the case directory is the
// storage root, the way sdmc:/3ds/Zelda 3DS is on the console.
static const char kStorageDirectory[]=".";
static const char *g_profile_prepare_status="ROM preparation failed";
static char g_active_rom_path[1024];
static bool ApplyProfileLanguage(void) { return true; }
static void Platform3DS_ShowOperationProgress(bool dumping,int percent) {(void)dumping;(void)percent;}
typedef struct {char filename[256],profile[256];uint32_t hash; bool french;} RomEntry;
static const char *RomModLanguage(const char *path) { (void)path; return "us"; }
#define LOCALIZED_CART_FILE_FR_C "zelda3_fr-c.sfc"
'''
for sig in ['static bool ProfileSetupFailure(', 'static bool RomFileShouldBeIgnored(const char *name) {', 'static bool HasExtension(', 'static bool IsRegularFile(const char *path) {', 'static uint32_t HashRomFile(', 'static void SanitizeProfileName(', 'static void MakeProfilePath(', 'static int ScanRoms(', 'static bool EnsureDirectory(const char *path) {', 'static char *Trim(', 'static bool CopyFileIfMissing(', 'static bool CopyFileReplacing(const char *source, const char *destination) {', 'static bool MigrateWideDefaults(', 'static bool IniDeclaresLanguage(']:code+=fn(sig)
# Fake the expensive extractor/asset validator only. All storage operations,
# migration, selection gating and copy ordering are the production functions.
code+=r'''
static bool AssetsFileLooksValid(const char *path) {
 FILE *f=fopen(path,"rb");if(!f)return false;
 char magic[6]={0};fread(magic,1,5,f);fclose(f);return !strcmp(magic,"VALID");
}
static bool ExtractAssetsFromRom(const char *path) {
 extract_calls++;g_profile_prepare_status="Incompatible ROM";return false;
}
static bool PrepareSaveDirectory(const RomEntry *rom,bool legacy) {
 if(getenv("SAVE_FAULT")){errno=ENOSPC;return false;}return true;
}
static void WriteSelectedRom(const RomEntry *rom){selected_writes++;}
// Stands for the rebuild from the ROM: it leaves the cartridge copy in the
// profile folder, the working directory it runs in.
static void UpgradeLocalizedProfile(const char *rom_path) {
 upgrade_calls++;assert(!strcmp(rom_path,"./game.sfc"));
 FILE *f=fopen(LOCALIZED_CART_FILE_FR_C,"wb");assert(f);fputs("CART",f);fclose(f);
}
'''+fn('static bool EnsureProfileReady(')+r'''
int main(int argc,char **argv) {
 assert(argc==3);
 if(!strcmp(argv[2],"scan")) {
  RomEntry roms[8];int n=ScanRoms(roms,8);assert(n==atoi(argv[1]));
  for(int i=0;i<n;i++)assert(strcasecmp(roms[i].filename,LOCALIZED_CART_FILE_FR_C) && strncmp(roms[i].filename,"._",2));
  return 0;
 }
 RomEntry rom={0};snprintf(rom.filename,sizeof(rom.filename),"game.sfc");
 snprintf(rom.profile,sizeof(rom.profile),"profiles/%s",argv[1]);
 bool ok=EnsureProfileReady(&rom,false);
 if(!strcmp(argv[2],"ok")) {
  assert(ok);assert(selected_writes==1);assert(extract_calls==0);assert(upgrade_calls==0);
 } else if(!strcmp(argv[2],"upgrade")) {
  assert(ok);assert(selected_writes==1);assert(extract_calls==0);assert(upgrade_calls==1);
 } else if(!strcmp(argv[2],"unsupported")) {
  assert(!ok);assert(selected_writes==0);assert(extract_calls==1);
  assert(!strcmp(g_profile_prepare_status,"Incompatible ROM"));
 } else {
  assert(!ok);assert(selected_writes==0);assert(extract_calls==0);
  assert(strcmp(g_profile_prepare_status,"Incompatible ROM"));
  printf("Expected storage failure: %s\n",g_profile_prepare_status);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-profile-') as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(['cc','-I'+str(r/'engine/src'),'-O1','-fsanitize=address,undefined',p/'test.c','-o',p/'test'],check=True)
 original='[General]\nDisplayMode = Original\nWideEdgeMode = Standard\nWideZoom = 1.5\n[Sound]\nEnableAudio = 0\n'
 bundled='[General]\nDisplayMode = Wide\nWideEdgeMode = FixedCamera\n'
 cases=['normal','missing','recovery','completed','no-general','duplicate','fault1','fault2','fault2+3','fault3','save','unsupported']
 for case in cases:
  cwd=p/case;profile=cwd/'profiles/rom';profile.mkdir(parents=True)
  (cwd/'bundled.ini').write_text(bundled)
  ini=profile/'zelda3.ini';backup=profile/'zelda3.ini.wide-defaults.bak';marker=profile/'zelda3.ini.wide-defaults-v1'
  if case=='recovery':backup.write_text(original)
  elif case!='missing':ini.write_text(original if case!='no-general' else '[Sound]\nEnableAudio = 0\n')
  if case=='completed':marker.write_text('1\n')
  if case=='duplicate':ini.write_text(original+'[General]\nDisplayMode = Stretch\nWideEdgeMode = Standard\n')
  (profile/'zelda3_assets.dat').write_bytes(b'INVALID' if case=='unsupported' else b'VALID_private_fixture')
  env=dict(os.environ)
  if case.startswith('fault'):env['RENAME_FAULT']=case[5:]
  if case=='save':env['SAVE_FAULT']='1'
  expected='storage' if case.startswith('fault') or case=='save' else 'unsupported' if case=='unsupported' else 'ok'
  subprocess.run([p/'test','rom',expected],cwd=cwd,env=env,check=True)
  if case.startswith('fault'):
   assert not marker.exists()
   assert (ini.exists() and ini.read_text()==original) or (backup.exists() and backup.read_text()==original)
   subprocess.run([p/'test','rom','ok'],cwd=cwd,check=True)
  if case=='unsupported':continue
  assert marker.read_text()=='1\n'
  if case=='completed':assert ini.read_text()==original
  else:assert ini.read_text().count('DisplayMode = Wide')==1 and 'WideEdgeMode = FixedCamera' in ini.read_text()
  if case!='missing':assert 'EnableAudio = 0' in ini.read_text()
  # Another ROM must perform its own migration, regardless of this marker.
  other=cwd/'profiles/other';other.mkdir();(other/'zelda3.ini').write_text(original)
  (other/'zelda3_assets.dat').write_bytes(b'VALID_other_fixture')
  subprocess.run([p/'test','other','ok'],cwd=cwd,check=True)
  assert (other/'zelda3.ini.wide-defaults-v1').exists()

 # French Canadian profiles: one made before the cartridge copy existed is
 # brought up to date once and the copy stays in the profile folder, where the
 # game reads it. The copy earlier builds put beside the ROMs goes at the next
 # boot, whatever the profile, and is never offered as a ROM.
 for case in ['frc-old','frc-new','us-after-frc']:
  cwd=p/case;profile=cwd/'profiles/rom';profile.mkdir(parents=True)
  (cwd/'bundled.ini').write_text(bundled);(profile/'zelda3.ini.wide-defaults-v1').write_text('1\n')
  ini=profile/'zelda3.ini';cart=profile/'zelda3_fr-c.sfc';root=cwd/'zelda3_fr-c.sfc'
  ini.write_text(original+('\n[General]\nLanguage = fr-c\n' if case!='us-after-frc' else ''))
  (profile/'zelda3_assets.dat').write_bytes(b'VALID_private_fixture')
  if case=='frc-new':cart.write_bytes(b'CART')
  if case!='frc-old':root.write_bytes(b'STALE')
  subprocess.run([p/'test','rom','upgrade' if case=='frc-old' else 'ok'],cwd=cwd,check=True)
  assert not root.exists()
  if case=='us-after-frc':assert not cart.exists()
  else:assert cart.read_bytes()==b'CART'
  # The next boot finds the copy and does nothing more.
  if case=='frc-old':subprocess.run([p/'test','rom','ok'],cwd=cwd,check=True);assert not root.exists()
 # The ROM list before any profile is prepared: the stale copy and a resource
 # fork beside the one ROM leave a single ROM, which boots without the list.
 cwd=p/'frc-new';(cwd/'game.sfc').write_bytes(b'ROM');(cwd/'zelda3_fr-c.sfc').write_bytes(b'STALE');(cwd/'._game.sfc').write_bytes(b'FORK')
 subprocess.run([p/'test','1','scan'],cwd=cwd,check=True)
 print('PASS 16 profile scenarios: SD rename, cached assets, first install, completed migration, independent ROMs, missing INI, interrupted replacement, rollback/retry, save errors, incompatible-ROM classification, the French Canadian cartridge copy kept in its profile and the ROM list without it')
