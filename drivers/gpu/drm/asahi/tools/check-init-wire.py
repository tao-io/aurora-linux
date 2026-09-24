#!/usr/bin/env python3
"""Compare actual M4 initialization and control-channel methods on host buffers.

Allocation, UAT, MMIO and publication are not emulated. The real initialization
method is extracted verbatim from each source version and receives distinct
owned addresses, varied requested performance states and identical table bytes.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path


def method(source,name):
    m=re.search(r'    (?:pub\(crate\) )?fn '+name+r'\(',source)
    if not m:raise ValueError(name)
    opening=source.index('{',m.start());depth=1;end=opening+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[m.start():end]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--baseline',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False);rel='drivers/gpu/drm/asahi';driver=a.kernel/rel
    tree=subprocess.check_output(['git','-C',str(a.kernel),'rev-parse','--verify','--end-of-options',a.baseline+'^{tree}'],text=True).strip()
    fixture=(driver/'tools/m4_init_wire_fixture.rs').read_text();sources={}
    for version in ['baseline','candidate']:
        d=a.output/version;d.mkdir();hashes={}
        for name in ['g16_config.rs','g16_config_layout.rs','g16_initdata.rs','g16_power.rs']:
            raw=subprocess.check_output(['git','-C',str(a.kernel),'show',f'{tree}:{rel}/{name}']) if version=='baseline' else (driver/name).read_bytes()
            (d/name).write_bytes(raw);hashes[name]=hashlib.sha256(raw).hexdigest()
        src=(d/'g16_config.rs').read_text();power=(d/'g16_power.rs').read_text()
        tables='\n'.join(re.findall(r'pub\(crate\) const \w+: \[u32; 10\] =\s*\[.*?\];',power,re.S));assert len(re.findall(r'pub\(crate\) const',tables))==4
        methods='\n'.join(method(src,n) for n in ['va','object','ptr','initialize','enqueue_control','control_indices','consumed_generation'])
        imports='use layout::{runtime,channel,globals,main_data,power_data,firmware_data};' if 'use layout::{runtime' in src else ''
        program=fixture.replace('/* IMPORTS */',imports).replace('/* METHODS */',methods).replace('/* POWER TABLES */',tables)
        (d/'fixture.rs').write_text(program);hashes['extracted_fixture']=hashlib.sha256(program.encode()).hexdigest()
        subprocess.run(['rustc','--edition=2021','-A','dead_code','-A','unused_imports',str(d/'fixture.rs'),'-o',str(d/'fixture')],check=True);sources[version]=hashes
    comparisons=[]
    for variant in [0,1,2,15,31,63]:
        data=[]
        for version in sources:
            raw=subprocess.check_output([str((a.output/version/'fixture').resolve()),str(variant)]);data.append(raw);(a.output/f'{version}-{variant}.bin').write_bytes(raw)
        if data[0]!=data[1]:raise RuntimeError(f'init wire mismatch in {variant}; both outputs retained')
        comparisons.append(dict(variant=variant,bytes=len(data[0]),sha256=hashlib.sha256(data[0]).hexdigest()))
    (a.output/'result.json').write_text(json.dumps(dict(baseline_tree=tree,sources=sources,comparisons=comparisons,result='pass'),indent=2)+'\n')
    print('PASS: six actual-source initialization and control-channel variants match baseline')


if __name__=='__main__':main()
