#!/usr/bin/env python3
"""Validate bounded official-spoiler parsing using the full engine test binary.

Fixture JSON is supplied outside the checkout; no seed spoilers ship in source.
Run run_randomizer_game_test.py first to build the --parse-alttpr test seam.
"""
from pathlib import Path
import argparse, copy, json, os, subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--seed',type=Path,required=True)
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--hunt-seed',type=Path)
    a=p.parse_args(); out=a.work_dir.resolve(); out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[3]
    if out==root or root in out.parents: p.error('Keep private fixtures outside the source checkout')
    original=json.loads(a.seed.read_text()); first=next(k for k in original['spoiler'] if k not in ('meta','Equipped','Special','Shops','Bosses','playthrough'))
    location=next(iter(original['spoiler'][first]))
    cases={}
    def change(name,fn):
        value=copy.deepcopy(original);fn(value);cases[name]=json.dumps(value)
    change('hidden',lambda d:d.update(spoiler={'meta':d['spoiler']['meta']}))
    change('missing-location',lambda d:d['spoiler'][first].pop(location))
    change('foreign-world',lambda d:d['spoiler'][first].update({location:'Hookshot:2'}))
    change('unknown-item',lambda d:d['spoiler'][first].update({location:'UnknownItem:1'}))
    change('unknown-location',lambda d:d['spoiler'][first].update({'Unknown Place:1':'Hookshot:1'}))
    change('duplicate-normalized-location',lambda d:d['spoiler'][first].update({location.removesuffix(':1'):d['spoiler'][first][location]}))
    for key,value in [('logic','NoLogic'),('mode','inverted'),('weapons','unknown'),('dungeon_items','unknown'),('goal','unknown'),('item_pool','hard'),('enemizer.enemy_shuffle','shuffled'),('pseudoboots',True),('worlds',2),('entry_crystals_ganon','random'),('item_placement','unknown')]:
        change('unsupported-'+key,lambda d,k=key,v=value:d['spoiler']['meta'].update({k:v}))
    change('custom-inventory',lambda d:d['spoiler']['Equipped'].update({'Extra':'Hammer:1'}))
    change('custom-shop',lambda d:d['spoiler']['Shops'][0]['item_0'].update(price=1))
    change('missing-fairy-bottle',lambda d:d['spoiler']['Special'].pop('Waterfall Bottle:1'))
    change('duplicate-fairy-bottle',lambda d:d['spoiler']['Special'].update({'Waterfall Bottle':'BottleWithBee:1'}))
    change('pedestal-without-triforce',lambda d:d['spoiler']['meta'].update(goal='pedestal'))
    change('triforce-wrong-goal',lambda d:d['spoiler'][first].update({location:'Triforce:1'}))
    change('missing-equipped',lambda d:d['spoiler'].pop('Equipped'))
    change('assured-without-sword',lambda d:d['spoiler']['meta'].update(weapons='assured'))
    if a.hunt_seed:
        hunt=json.loads(a.hunt_seed.read_text())
        cases['hunt-without-target']=json.dumps({k:v for k,v in hunt.items() if k!='patch'})
        cases['hunt-without-pieces']=json.dumps(dict(hunt,spoiler=original['spoiler']|{'meta':hunt['spoiler']['meta']}))
        for name,values in [('zero',[0,0]),('excessive',[255,255]),('fractional',[1.5,0]),('string',['20',0]),('partial',[20])]:
            d=copy.deepcopy(hunt);d['patch']=[{'1573223':values}];cases['hunt-target-'+name]=json.dumps(d)
        d=copy.deepcopy(hunt);d['patch']=[{'1573223':[20,0]},{'1573224':[0]}];cases['hunt-duplicate-target']=json.dumps(d)
    compact=json.dumps(original)
    cases.update({'duplicate-key':'{"spoiler":{},'+compact[1:],
                  'trailing-bytes':compact+' false', 'truncated':compact[:-20],
                  'nul-key':compact.replace('"mode":','"mode\\u0000hidden":',1),
                  'deep-json':'['*34+'0'+']'*34,
                  'oversized':' '*1048577,
                  'wide-object':json.dumps({str(i):0 for i in range(513)})})
    env=dict(os.environ,SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
    def run(path):
        return subprocess.run([str(a.binary.resolve()),'--parse-alttpr',str(path)],env=env,text=True,capture_output=True)
    good=run(a.seed.resolve());assert good.returncode==0,good.stderr
    entries=[tuple(map(int,line.split())) for line in good.stdout.splitlines()]
    assert len(entries)==232 and len(dict(entries))==232
    # Check the import-only denomination has an exact runtime identity instead
    # of silently converting 50 rupees to another amount.
    assert any(item==234 for _,item in entries)
    if a.hunt_seed:
        result=run(a.hunt_seed.resolve());assert result.returncode==0,result.stderr
        d=copy.deepcopy(hunt);d['patch']=[{'1573223':[19,0]}]
        path=out/'valid-explicit-custom-target.json';path.write_text(json.dumps(d));result=run(path)
        assert result.returncode==0,result.stderr
    aliases={'Bottle':43,'BottleWithGoldBee':46,'TwentyRupees2':101}
    for name,want in aliases.items():
        value=copy.deepcopy(original);value['spoiler'][first][location]=name+':1'
        path=out/('valid-'+name+'.json');path.write_text(json.dumps(value));result=run(path)
        assert result.returncode==0 and any(int(line.split()[1])==want for line in result.stdout.splitlines()),(name,result.stderr)
    for name,text in cases.items():
        path=out/(name+'.json');path.write_text(text)
        result=run(path)
        assert result.returncode==1 and result.stderr.strip(),(name,result.returncode,result.stdout,result.stderr)
        assert 'AddressSanitizer' not in result.stderr,(name,result.stderr)
    (out/'result.json').write_text(json.dumps({'placements':len(entries),'rejected_cases':sorted(cases)},indent=2)+'\n')
    print(f'PASS: 232 exact placements; {len(cases)} malformed, hidden, multiworld or unsupported inputs rejected with reasons')
    return 0

if __name__=='__main__':raise SystemExit(main())
