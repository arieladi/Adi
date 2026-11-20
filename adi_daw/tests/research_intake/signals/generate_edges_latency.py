"""Deterministic bounded test inputs, no DAW execution. NumPy required."""
from pathlib import Path
import hashlib,json,shutil,struct,argparse
import numpy as np
parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,required=True);OUT=parser.parse_args().out;OUT.mkdir(parents=True,exist_ok=True)
disk=shutil.disk_usage(OUT);assert disk.free>100_000_000
def emit(name,x):
 x=np.asarray(x,dtype='<f4');assert x.ndim==2 and x.shape[1]==2 and np.all(np.isfinite(x))
 data=x.tobytes();fmt=struct.pack('<HHIIHH',3,2,48000,48000*8,8,32)
 blob=b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+struct.pack('<I',16)+fmt+b'data'+struct.pack('<I',len(data))+data
 p=OUT/name;p.write_bytes(blob);r=p.read_bytes()
 assert r[:4]==b'RIFF' and len(r)==int.from_bytes(r[4:8],'little')+8
 assert np.array_equal(np.frombuffer(r[44:],dtype='<f4').reshape(-1,2),x)
 return {'file':name,'sha256':hashlib.sha256(r).hexdigest(),'bytes':len(r),'frames':len(x),'rate':48000,'channels':2,'encoding':'IEEE float32','peak':float(np.max(np.abs(x)))}
rows=[]
for offset in [0,1,2,16,64]:
 x=np.zeros((96000,2),dtype=np.float32);x[offset]=.1;rows.append(emit(f'impulse_at_{offset}_48k_stereo_2s.wav',x))
x=np.full((96000,2),.1,dtype=np.float32);rows.append(emit('dc_0p1_48k_stereo_2s.wav',x))
# Exact integer LCG, channels seeded independently; no dependency on RNG library version.
channels=[]
for seed in [0xAD1DA701,0xAD1DA702]:
 s=seed;out=np.empty(48000*40,dtype=np.float32)
 for i in range(len(out)):
  s=(1664525*s+1013904223)&0xffffffff
  out[i]=((s>>8)/16777216.0*2-1)*.025
 channels.append(out)
rows.append(emit('dense_code_48k_stereo_40s.wav',np.column_stack(channels)))
report={'disk_free_before_bytes':disk.free,'total_bytes':sum(r['bytes'] for r in rows),'status':'SYNTHETIC INPUTS ONLY; DAW tests NOT RUN','self_checks':'Every RIFF length and float payload read back and compared exactly','generator':'32-bit LCG1664525/1013904223, high24bits, seeds0xAD1DA701/702, peak<=0.025 for dense file','files':rows}
(OUT/'MANIFEST.json').write_text(json.dumps(report,indent=2));print(json.dumps({'total_bytes':report['total_bytes'],'files':len(rows),'self_checks':report['self_checks']}))
