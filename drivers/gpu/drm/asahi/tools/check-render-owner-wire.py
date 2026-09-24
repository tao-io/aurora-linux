#!/usr/bin/env python3
"""Compare actual retained-render patch/readback methods with an immutable baseline.

Only Buffer ownership is substituted with checked host byte buffers. Extracted
methods are copied verbatim; constructors, GEM, locking and DMA are not emulated.
"""
import argparse, hashlib, json, re, subprocess
from pathlib import Path

METHODS = ['request_completion_flush','set_attachments','index','wait_for_prior',
    'compute_dependency','wait_for_prior_fragment','init_address','fragment_dependency',
    'uma_completed','status','set_user_timestamps','set_fragment_timestamp_copies',
    'omit_internal_timestamps','timestamps']


def method(source, name):
    match = re.search(r'    pub\(crate\) fn '+name+r'\(',source)
    if not match:
        raise ValueError('missing method '+name)
    opening=source.index('{',match.start());depth=1;end=opening+1
    # These selected methods have no braces in string literals. Retain them
    # verbatim, including their checks and mutation order.
    while depth:
        if source[end]=='{': depth+=1
        elif source[end]=='}': depth-=1
        end+=1
    return source[match.start():end]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel',type=Path,required=True);p.add_argument('--baseline',required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False);rel='drivers/gpu/drm/asahi';driver=a.kernel/rel
    tree=subprocess.check_output(['git','-C',str(a.kernel),'rev-parse','--verify','--end-of-options',a.baseline+'^{tree}'],text=True).strip()
    fixture=(driver/'tools/m4_render_owner_fixture.rs').read_text();sources={}
    for version in ['baseline','candidate']:
        directory=a.output/version;directory.mkdir();hashes={}
        for name in ['g16_render.rs','g16_compute.rs','g16_render_command.rs','g16_attachments.rs','g16_render_job.rs']:
            raw=subprocess.check_output(['git','-C',str(a.kernel),'show',f'{tree}:{rel}/{name}']) if version=='baseline' else (driver/name).read_bytes()
            (directory/name).write_bytes(raw);hashes[name]=hashlib.sha256(raw).hexdigest()
        source=(directory/'g16_render_job.rs').read_text()
        imports='use wire::{metadata,dependency,init_buffer}; use g16_compute::uma_layout;' if 'use wire::{metadata' in source else ''
        program=fixture.replace('/* IMPORTS */',imports).replace('/* METHODS */','\n'.join(method(source,n) for n in METHODS))
        (directory/'fixture.rs').write_text(program);hashes['extracted_fixture']=hashlib.sha256(program.encode()).hexdigest()
        subprocess.run(['rustc','--edition=2021','-A','dead_code','-A','unused_imports',str(directory/'fixture.rs'),'-o',str(directory/'fixture')],check=True)
        sources[version]=hashes
    results=[]
    for variant in [0,1,2,15,31,63]:
        outputs=[]
        for version in sources:
            raw=subprocess.check_output([str((a.output/version/'fixture').resolve()),str(variant)])
            (a.output/f'{version}-{variant}.bin').write_bytes(raw);outputs.append(raw)
        if outputs[0]!=outputs[1]:raise RuntimeError(f'owner wire mismatch in variant {variant}; outputs retained')
        results.append(dict(variant=variant,bytes=len(outputs[0]),sha256=hashlib.sha256(outputs[0]).hexdigest()))
    (a.output/'result.json').write_text(json.dumps(dict(baseline_tree=tree,sources=sources,comparisons=results,result='pass'),indent=2)+'\n')
    print('PASS: six actual-source render owner patch/readback variants match baseline')


if __name__=='__main__':main()
