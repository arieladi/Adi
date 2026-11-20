"""Read-only Windows process counters; no target mutation or discovery by name."""
from __future__ import annotations
import ctypes as C, ctypes.wintypes as W, datetime as DT
import json,os,platform,sys,time
from pathlib import Path
if os.name!='nt':raise SystemExit('Windows only')
K=C.WinDLL('kernel32',use_last_error=True);N=C.WinDLL('ntdll');D=C.WinDLL('pdh')
U64=C.c_ulonglong;I64=C.c_longlong;SZ=C.c_size_t;PTR=C.c_void_p
def sig(lib,name,ret,args):
 f=getattr(lib,name);f.restype=ret;f.argtypes=args;return f

sig(K,'OpenProcess',PTR,[W.DWORD,W.BOOL,W.DWORD]);sig(K,'OpenThread',PTR,[W.DWORD,W.BOOL,W.DWORD])
sig(K,'CloseHandle',W.BOOL,[PTR]);sig(K,'GetCurrentProcess',PTR,[]);sig(K,'GetCurrentThread',PTR,[])
sig(K,'QueryProcessCycleTime',W.BOOL,[PTR,C.POINTER(U64)]);sig(K,'QueryThreadCycleTime',W.BOOL,[PTR,C.POINTER(U64)])
sig(K,'GetProcessTimes',W.BOOL,[PTR,C.POINTER(U64),C.POINTER(U64),C.POINTER(U64),C.POINTER(U64)])
sig(K,'GetThreadTimes',W.BOOL,[PTR,C.POINTER(U64),C.POINTER(U64),C.POINTER(U64),C.POINTER(U64)])
sig(K,'QueryPerformanceCounter',W.BOOL,[C.POINTER(I64)]);sig(K,'QueryPerformanceFrequency',W.BOOL,[C.POINTER(I64)])
sig(N,'NtQueryTimerResolution',C.c_long,[C.POINTER(W.ULONG),C.POINTER(W.ULONG),C.POINTER(W.ULONG)])
sig(D,'PdhOpenQueryW',C.c_long,[W.LPCWSTR,SZ,C.POINTER(PTR)])
sig(D,'PdhAddEnglishCounterW',C.c_long,[PTR,W.LPCWSTR,SZ,C.POINTER(PTR)])
sig(D,'PdhCollectQueryData',C.c_long,[PTR]);sig(D,'PdhCloseQuery',C.c_long,[PTR])
sig(D,'PdhGetFormattedCounterArrayW',C.c_long,[PTR,W.DWORD,C.POINTER(W.DWORD),C.POINTER(W.DWORD),PTR])
def err(): return {'winerror':C.get_last_error(),'message':C.FormatError(C.get_last_error()).strip()}

def utc(): return DT.datetime.now(DT.timezone.utc).isoformat()

def atomic(path,data):
 path=Path(path);path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(json.dumps(data,indent=2,allow_nan=False),encoding='utf-8')
 for attempt in range(100):
  try:os.replace(tmp,path);break
  except PermissionError:
   if attempt==99:raise
   time.sleep(.01)

def qpc():
 x=I64();K.QueryPerformanceCounter(C.byref(x));return x.value

def times(h,thread=False):
 vals=[U64() for _ in range(4)];f=K.GetThreadTimes if thread else K.GetProcessTimes
 if not f(h,*[C.byref(x) for x in vals]):return {'times_error':err()}
 return dict(zip(['create_time_100ns','exit_time_100ns','kernel_100ns','user_100ns'],[v.value for v in vals]))

def cycles(h,thread=False):
 x=U64();f=K.QueryThreadCycleTime if thread else K.QueryProcessCycleTime
 return {'cycles':x.value} if f(h,C.byref(x)) else {'cycles':None,'cycles_error':err()}

class PDHVALUE(C.Structure): _fields_=[('status',W.DWORD),('value',C.c_double)]

class PDHITEM(C.Structure): _fields_=[('name',W.LPWSTR),('value',PDHVALUE)]

FREQ=I64();K.QueryPerformanceFrequency(C.byref(FREQ))
def environment():
 minimum=W.ULONG();maximum=W.ULONG();current=W.ULONG();st=N.NtQueryTimerResolution(C.byref(minimum),C.byref(maximum),C.byref(current))
 return {'python':sys.version,'platform':platform.platform(),'pid':os.getpid(),'logical_cpus':os.cpu_count(),'qpc_frequency':FREQ.value,'timer_resolution':{'ntstatus':st,'minimum_100ns':minimum.value,'maximum_100ns':maximum.value,'current_100ns':current.value,'scope':'system-wide, not target request'},'counter_units':'cycles raw; GetTimes and SPI times 100 ns; QPC timestamps perf_counter_ns','cycles_to_seconds':'NOT SUPPORTED: Microsoft explicitly warns not to convert QueryThreadCycleTime cycles to elapsed time','cpu_time_limit':'GetThreadTimes/GetProcessTimes are tick charged (approximately 15.625 ms); do not claim sample-level timing','counter_limit':'A sampled counter cannot prove callback timing or the absence of misses.','throttling_limit':'GetProcessInformation exposes self-set policy, not proof Windows applied EcoQoS','observer_scope':'Explicit manifest identities plus sampler PID only; unlisted helpers are not covered'}

STATES=['run_start','kept_start','kept_end','run_end','abort','block_end']
