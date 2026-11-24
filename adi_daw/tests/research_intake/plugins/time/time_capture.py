"""Bounded raw VST3 transport/marker capture. Original diagnostic code, MIT."""
import ctypes as C, mmap, os, time, json, pathlib, hashlib, argparse
CAP=16384
class Marker(C.Structure):_fields_=[('offset',C.c_int32),('reserved',C.c_int32),('value',C.c_double)]
class Parameter(C.Structure):_fields_=[('id',C.c_uint32),('offset',C.c_int32),('value',C.c_double)]
class Record(C.Structure):
 _fields_=[('committed',C.c_int64),('qpc',C.c_int64),('project_samples',C.c_int64),('continuous_samples',C.c_int64),('system_ns',C.c_int64),
 ('sample_rate',C.c_double),('project_music',C.c_double),('bar_music',C.c_double),('cycle_start_music',C.c_double),('cycle_end_music',C.c_double),('tempo',C.c_double),('gain_before',C.c_double),('gain_after',C.c_double),
 ('pid',C.c_uint32),('tid',C.c_uint32),('cpu',C.c_uint32),('id',C.c_uint32),('variant',C.c_uint32),('latency',C.c_uint32),('frames',C.c_uint32),('sample_size',C.c_uint32),('mode',C.c_uint32),('context_present',C.c_uint32),('context_state',C.c_uint32),('numerator',C.c_int32),('denominator',C.c_int32),
 ('input_count',C.c_uint32),('output_count',C.c_uint32),('param_count',C.c_uint32),('input_total',C.c_uint32),('output_total',C.c_uint32),('param_total',C.c_uint32),('context_bytes',C.c_uint32),
 ('raw_context',C.c_ubyte*256),('input',Marker*32),('output',Marker*32),('parameters',Parameter*64)]
class Header(C.Structure):
 _fields_=[('magic',C.c_char*8),('version',C.c_uint32),('record_size',C.c_uint32),('capacity',C.c_uint64),('next',C.c_int64),('lost',C.c_int64),('enabled',C.c_int32),('reserved',C.c_int32),('frequency',C.c_int64)]
assert C.sizeof(Record)==2488 and C.sizeof(Header)==56
SIZE=4096+CAP*C.sizeof(Record)
def row(r):
 d={n:getattr(r,n) for n,t in Record._fields_ if n not in ['raw_context','input','output','parameters']}
 d['raw_context_hex']=bytes(r.raw_context[:r.context_bytes]).hex()
 for n,c in [('input',r.input_count),('output',r.output_count),('parameters',r.param_count)]:
  d[n]=[{key:getattr(v,key) for key,t in v._fields_ if key!='reserved'}for v in getattr(r,n)[:c]]
 return d
class Capture:
 def __init__(self,name,create=False):
  self.name=name;self.m=mmap.mmap(-1,SIZE,tagname=name,access=mmap.ACCESS_WRITE);self.h=Header.from_buffer(self.m)
  if create:
   self.h.magic=b'ADITIME1';self.h.version=1;self.h.record_size=C.sizeof(Record);self.h.capacity=CAP;self.h.next=0;self.h.lost=0;self.h.enabled=0
   fq=C.c_int64();C.windll.kernel32.QueryPerformanceFrequency(C.byref(fq));self.h.frequency=fq.value
  elif self.h.magic!=b'ADITIME1':raise RuntimeError('Start keeper before host')
 def arm(self):self.h.enabled=0;time.sleep(.1);self.h.next=0;self.h.lost=0;self.h.enabled=1
 def stop(self):self.h.enabled=0;time.sleep(.15)
 def save(self,folder,name):
  self.stop();p=pathlib.Path(folder);p.mkdir(parents=True,exist_ok=True)
  n=min(self.h.next,CAP);data=self.m[4096:4096+n*C.sizeof(Record)];(p/(name+'.bin')).write_bytes(data)
  records=[Record.from_buffer_copy(data,i*C.sizeof(Record))for i in range(n)]
  meta=dict(name=name,map_name=self.name,record_size=C.sizeof(Record),records=n,attempted=self.h.next,lost=self.h.lost,
    bad_commits=sum(r.committed!=i+1 for i,r in enumerate(records)),truncated_markers=sum(r.input_count<r.input_total or r.output_count<r.output_total or r.param_count<r.param_total for r in records),
    pids=sorted(set(r.pid for r in records)),frequency=self.h.frequency,sha256=hashlib.sha256(data).hexdigest())
  (p/(name+'.json')).write_text(json.dumps(meta,indent=2))
  with(p/(name+'.jsonl')).open('w')as f:
   for r in records:f.write(json.dumps(row(r))+'\n')
  return meta
 def close(self):del self.h;self.m.close()
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('action',choices=['serve','capture']);a.add_argument('--name',default='Local\\ADI_Time_20261118');a.add_argument('--stop-file');a.add_argument('--output',default='.');a.add_argument('--label',default='capture');a.add_argument('--seconds',type=float,default=8);x=a.parse_args()
 c=Capture(x.name,x.action=='serve')
 if x.action=='serve':
  print('READY '+x.name+' bytes='+str(SIZE),flush=True)
  while not x.stop_file or not pathlib.Path(x.stop_file).exists():time.sleep(.25)
 else:c.arm();time.sleep(x.seconds);print(json.dumps(c.save(x.output,x.label)))
 c.close()

