"""Fetch explicit public interface dependencies into a new, caller-selected directory."""
import argparse, json, subprocess
from pathlib import Path

PINS={
 'pluginterfaces':('https://github.com/steinbergmedia/vst3_pluginterfaces.git','4f547e8e102b47de4a8b8aaf343c73b700786372','MIT','LICENSE.txt'),
 'clap':('https://github.com/free-audio/clap.git','a47f6badb49d948fd009998f28309cdab78979c9','MIT','LICENSE'),
}
def run(*args):return subprocess.check_output(args,text=True).strip()
def main():
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
 for name,(url,commit,license_id,license_file) in PINS.items():
  dest=a.out/name
  if dest.exists():raise ValueError(f'Refuse existing dependency directory: {dest}')
  run('git','init',str(dest));run('git','-C',str(dest),'remote','add','origin',url)
  run('git','-C',str(dest),'fetch','--depth','1','origin',commit)
  run('git','-C',str(dest),'checkout','--detach','FETCH_HEAD')
  if run('git','-C',str(dest),'rev-parse','HEAD')!=commit:raise ValueError('Dependency identity mismatch')
  if not (dest/license_file).is_file():raise ValueError('Missing dependency license')
 print(json.dumps({'status':'pinned dependencies fetched','pins':PINS},indent=2))
if __name__=='__main__':main()
