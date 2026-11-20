"""Verify generated original inputs against retained identities; never update baselines."""
import argparse,hashlib,json,struct
from pathlib import Path

ROOT=Path(__file__).resolve().parent
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def verify_hashes(manifest,folder,key='path'):
 rows=json.loads(manifest.read_text());seen=set()
 for r in rows:
  name=r[key]
  if Path(name).name!=name or name in seen:raise ValueError('Nonlocal/duplicate fixture name')
  seen.add(name);p=folder/name
  if digest(p)!=r['sha256']:raise ValueError(f'Hash mismatch: {name}')
 return len(rows)
def events(path):
 data=path.read_bytes()
 if data[:4]!=b'MThd' or struct.unpack('>IHHH',data[4:14])!=(6,0,1,480) or data[14:18]!=b'MTrk':raise ValueError('Unexpected SMF layout')
 length=struct.unpack('>I',data[18:22])[0]
 if len(data)!=22+length:raise ValueError('Track size mismatch')
 off=22;tick=0;found=[];meta=[]
 def vlq():
  nonlocal off
  n=0
  for _ in range(4):
   b=data[off];off+=1;n=(n<<7)|(b&127)
   if not b&128:return n
  raise ValueError('Invalid VLQ')
 while off<len(data):
  tick+=vlq();status=data[off];off+=1
  if status==255:
   kind=data[off];off+=1;n=vlq();value=data[off:off+n];off+=n;meta.append((kind,value))
   if kind==47:
    if off!=len(data):raise ValueError('Bytes after end-of-track')
    break
  else:
   if not 128<=status<=239:raise ValueError('Unexpected status/running status')
   n=1 if status>>4 in [12,13] else 2
   value=data[off:off+n];off+=n
   if len(value)!=n or any(x>=128 for x in value):raise ValueError('Invalid event data')
   found.append({'tick':tick,'hex':(bytes([status])+value).hex()})
 if (81,bytes.fromhex('07a120')) not in meta or (88,bytes.fromhex('04021808')) not in meta:raise ValueError('Tempo/meter mismatch')
 return found,tick
def main():
 p=argparse.ArgumentParser();p.add_argument('--audio66',type=Path,required=True);p.add_argument('--edges',type=Path,required=True);p.add_argument('--midi',type=Path,required=True);p.add_argument('--out',type=Path);a=p.parse_args()
 checks={'audio66':verify_hashes(ROOT/'signals/expected66.json',a.audio66),'edges_latency':verify_hashes(ROOT/'signals/expected_edges_latency.json',a.edges),'midi':verify_hashes(ROOT/'midi/manifest.json',a.midi,'file')}
 rows=json.loads((ROOT/'midi/manifest.json').read_text())
 for r in rows:
  actual,end=events(a.midi/r['file'])
  if actual!=r['events'] or end!=r['end_tick']:raise ValueError('MIDI event mismatch')
 checks['midi_event_lists']=len(rows)
 tuning=json.loads((ROOT/'tuning/manifest.json').read_text())['records']
 for r in tuning:
  if digest(ROOT/'tuning'/r['file'])!=r['sha256']:raise ValueError('Tuning bytes changed')
 checks['tuning']=len(tuning)
 grid=list((ROOT/'specs/grid').glob('*.json'));assert len(grid)==10
 for f in grid:assert json.loads(f.read_text())['adi_result']=='NOT RUN'
 checks['grid_specs']=len(grid)
 result={'status':'PASS original input reproduction and data checks only','checks':checks,'provenance':"The director's research."}
 if a.out:a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result,indent=2))
if __name__=='__main__':main()
