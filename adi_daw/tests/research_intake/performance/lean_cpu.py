"""Explicit-identity Windows CPU observer. No UI input, elevation, or target writes.
Stdlib+ctypes. No broad process/thread/module/GPU enumeration during retained windows.
GetProcessTimes CPU seconds are primary; QueryProcessCycleTime remains raw cycles.
"""
import argparse,ctypes as C,ctypes.wintypes as W,hashlib,json,math,ntpath,os,statistics,sys,time
from pathlib import Path
import winapi_source as api

VERSION='adi-intake-lean-cpu-v1'
api.sig(api.K,'QueryFullProcessImageNameW',W.BOOL,[api.PTR,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)])
api.sig(api.K,'GetSystemTimes',W.BOOL,[C.POINTER(api.U64),C.POINTER(api.U64),C.POINTER(api.U64)])

def image_key(s):
    if not isinstance(s,str) or not ntpath.isabs(s) or not ntpath.splitdrive(s)[0]:raise ValueError('Absolute expected image required')
    return ntpath.normcase(ntpath.normpath(s))
def key(x):
    if type(x.get('pid')) is not int or x['pid']<=0 or type(x.get('create_time_100ns')) is not int or x['create_time_100ns']<=0:raise ValueError('PID and creation identity required')
    return x['pid'],x['create_time_100ns'],image_key(x['image'])
def image_of(h):
    n=W.DWORD(32768);b=C.create_unicode_buffer(n.value)
    if not api.K.QueryFullProcessImageNameW(h,0,b,C.byref(n)):raise OSError(api.err())
    return b.value
def identify(pid):
    h=api.K.OpenProcess(0x1000,False,pid)
    if not h:raise OSError(api.err())
    try:
        t=api.times(h)
        if 'times_error' in t or t['exit_time_100ns']:raise OSError('Times unavailable/process exited')
        return {'pid':pid,'create_time_100ns':t['create_time_100ns'],'image':image_of(h)}
    finally:api.K.CloseHandle(h)
def manifest_check(m):
    if m.get('schema_version')!=1:raise ValueError('Unknown manifest schema')
    key(m['target'])
    if not m['target'].get('app') or not m['target'].get('build'):raise ValueError('Target app/build required')
    seen=set()
    for role,x in [('target',m['target'])]+[('daw-helper',x) for x in m.get('helpers',[])]+[('observer/background-analysis',x) for x in m.get('observers',[])]:
        k=key(x)
        if k[0] in seen:raise ValueError('Duplicate PID in manifest')
        seen.add(k[0])
        if role=='daw-helper' and not x.get('ownership_evidence'):raise ValueError('Helper ownership evidence required')
    return m
def guard_check(m,target_pid):
    g=m.get('guard',{});expected=g.get('transport_expected')
    if m.get('target_pid')!=target_pid:raise ValueError('Marker target PID differs from manifest')
    if expected not in ('playing','stopped') or g.get('transport_observed')!=expected or g.get('passed') is not True:raise ValueError('Invalid transport guard')
    if g.get('target_foreground') is not True or g.get('foreground_pid')!=target_pid:raise ValueError('Exact foreground PID guard required')
    if m.get('condition_id') is None:raise ValueError('Condition identity required')

class Pdh:
    def __init__(self,enabled=True):
        self.q=api.PTR();self.handles={};self.errors={}
        if not enabled:return
        st=api.D.PdhOpenQueryW(None,0,C.byref(self.q))
        if st:raise OSError('PDH open '+str(st))
        for path in [r'\Processor Information(*)\% Processor Performance',r'\Processor(*)\% Processor Time',r'\Processor(*)\% DPC Time',r'\Processor(*)\% Interrupt Time']:
            h=api.PTR();st=api.D.PdhAddEnglishCounterW(self.q,path,0,C.byref(h))
            if st:self.errors[path]=st
            else:self.handles[path]=h
        api.D.PdhCollectQueryData(self.q)
    def sample(self):
        if not self.q:return {}
        st=api.D.PdhCollectQueryData(self.q)
        if st:return {'collect_error':st}
        result={}
        for path,h in self.handles.items():
            size=W.DWORD();count=W.DWORD();api.D.PdhGetFormattedCounterArrayW(h,0x200|0x8000,C.byref(size),C.byref(count),None)
            if not size.value:result[path]={'error':'empty'};continue
            b=C.create_string_buffer(size.value);st=api.D.PdhGetFormattedCounterArrayW(h,0x200|0x8000,C.byref(size),C.byref(count),b)
            if st:result[path]={'error':st};continue
            vals={};items=C.cast(b,C.POINTER(api.PDHITEM))
            for i in range(count.value):
                x=items[i];vals[x.name]=x.value.value if x.value.status in (0,1) and math.isfinite(x.value.value) else {'status':x.value.status}
            result[path]=vals
        return result
    def close(self):
        if self.q:api.D.PdhCloseQuery(self.q);self.q=None

class Handles:
    def __init__(self,m,pdh=True):
        self.m=manifest_check(m);self.entries=[];self.counters=None
        entries=[('target',m['target'])]+[('daw-helper',x) for x in m.get('helpers',[])]+[('observer/background-analysis',x) for x in m.get('observers',[])]
        if os.getpid() not in [x['pid'] for _,x in entries]:entries.append(('observer/harness',identify(os.getpid())))
        try:
            for role,x in entries:
                h=api.K.OpenProcess(0x1000,False,x['pid'])
                if not h:raise OSError(f"OpenProcess PID{x['pid']}: {api.err()}")
                self.entries.append((role,x,h))
                actual={'pid':x['pid'],'create_time_100ns':api.times(h).get('create_time_100ns'),'image':image_of(h)}
                if key(actual)!=key(x):raise ValueError('Identity mismatch before collection')
            self.counters=Pdh(pdh)
        except BaseException:self.close();raise
    def snapshot(self,boundary):
        start=time.perf_counter_ns();rows=[]
        for role,expected,h in self.entries:
            a=time.perf_counter_ns();t=api.times(h);cy=api.cycles(h);b=time.perf_counter_ns()
            if 'times_error' in t or cy.get('cycles') is None:raise OSError('Required counter unavailable')
            if t['create_time_100ns']!=expected['create_time_100ns'] or t['exit_time_100ns']!=0:raise ValueError('Process exit/reuse: scoped run invalid')
            rows.append({'pid':expected['pid'],'create_time_100ns':t['create_time_100ns'],'name':ntpath.basename(expected['image']),
                         'image':expected['image'],'role':role,'cpu_kernel_100ns':t['kernel_100ns'],'cpu_user_100ns':t['user_100ns'],
                         'cycles':cy['cycles'],'cycles_source':'QueryProcessCycleTime','source':'GetProcessTimes',
                         'counter_qpc_ns_start':a,'counter_qpc_ns_end':b,'identity_verified':True,'get_process_times':t,'threads':[]})
        idle,kernel,user=api.U64(),api.U64(),api.U64()
        system={'idle_100ns':None,'kernel_including_idle_100ns':None,'user_100ns':None}
        if api.K.GetSystemTimes(C.byref(idle),C.byref(kernel),C.byref(user)):system={'idle_100ns':idle.value,'kernel_including_idle_100ns':kernel.value,'user_100ns':user.value}
        perf=self.counters.sample()
        return {'boundary':boundary,'qpc_ns_start':start,'qpc_ns_end':time.perf_counter_ns(),'utc':api.utc(),'processes':rows,'pdh':perf,'system_cpu':system,
                'scope_limit':'Only explicit identities; no short-lived/undeclared helper claim; no thread/callback or GPU measurement'}
    def close(self):
        if self.counters:self.counters.close();self.counters=None
        for _,_,h in self.entries:api.K.CloseHandle(h)
        self.entries=[]

def environment(m):
    return {**api.environment(),'collector':VERSION,'scope_manifest':m,'scope_kind':'explicit fixed processes only',
            'observer_fingerprint':VERSION+';1s;process-times+rawcycles;4-CPU-PDH;no-GPU;no-in-window-inventory',
            'primary_metric':'GetProcessTimes CPU seconds per actual wall second; one logical CPU denominator',
            'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'api_source_sha256':hashlib.sha256(Path(api.__file__).read_bytes()).hexdigest()}

def observe(control,session,manifest,timeout=300):
    manifest_check(manifest);control=Path(control);session=Path(session);ack=control.with_suffix('.harness.ack.json')
    collector=Handles(manifest);active=None;last=-1;block=None;stage=None;lastchange=time.monotonic();nextsample=0
    def save():
        if active:api.atomic(session/'samples'/f"{active['case_id']}_{active['run_id']}.json",active)
    try:
        while True:
            if time.monotonic()-lastchange>timeout:raise TimeoutError('Control timeout')
            try:m=json.loads(control.read_text(encoding='utf-8-sig'))
            except (OSError,ValueError):m=None
            if m and m.get('sequence',-1)>last:
                err=None;state=m.get('state');seq=m['sequence'];lastchange=time.monotonic()
                try:
                    if type(seq) is not int or (last>=0 and seq!=last+1):raise ValueError('Missed sequence')
                    if m.get('schema_version')!=1 or state not in api.STATES:raise ValueError('Bad marker schema/state')
                    if not isinstance(m.get('marker_qpc_ns'),int) or not 0<=time.perf_counter_ns()-m['marker_qpc_ns']<5_000_000_000:raise ValueError('Stale/future marker')
                    if not m.get('block_id') or (block is not None and block!=m['block_id']):raise ValueError('Block identity changed')
                    block=m['block_id']
                    if state in ['run_start','kept_start','kept_end','run_end']:
                        guard_check(m,manifest['target']['pid'])
                        if state=='run_start':
                            if active:raise ValueError('Run already active')
                            if not all(isinstance(m.get(k),str) and m[k] and all(c.isalnum() or c in '-_.' for c in m[k]) for k in ['case_id','run_id']):raise ValueError('Bad IDs')
                            if (session/'samples'/f"{m['case_id']}_{m['run_id']}.json").exists():raise ValueError('Refuse existing run output')
                            active={'schema_version':1,'case_id':m['case_id'],'run_id':m['run_id'],'header':{**environment(manifest),'block_id':block,'arm':m.get('arm'),'pair_id':m.get('pair_id'),'target_pid':manifest['target']['pid'],'measurement_protocol':m.get('measurement_protocol',{'name':'standard','warmup_seconds':30,'kept_seconds':60,'tolerance_seconds':1})},'status':'recording','valid':True,'snapshots':[],'control_markers':[],'rejection_reasons':[]}
                        else:
                            expected={'kept_start':'run_start','kept_end':'kept_start','run_end':'kept_end'}[state]
                            if not active or stage!=expected:raise ValueError('Marker ordering')
                            first=active['control_markers'][0]
                            if any(m.get(k)!=first.get(k) for k in ['run_id','case_id','condition_id','pair_id','arm','target_pid']):raise ValueError('Run identity/condition changed')
                        if state=='run_start':
                            for name in ['condition_id','fixture_sha256','operator_start_evidence']:
                                if not m.get(name):raise ValueError('Missing run metadata: '+name)
                                active['header'][name]=m[name]
                        if state=='run_end':
                            active['scope_audit']=m.get('scope_audit',{})
                        active['control_markers'].append(m);active['snapshots'].append(collector.snapshot(state));stage=state;nextsample=time.monotonic()+1
                        if state=='run_end':active['status']='complete'
                        save()
                    elif state in ['abort','block_end'] and active:
                        active['valid']=False;active['status']='rejected';active['rejection_reasons'].append(state);active['control_markers'].append(m);save()
                except BaseException as e:err=repr(e)
                last=seq;api.atomic(ack,{'schema_version':1,'block_id':block,'sequence':seq,'state':state,'run_id':m.get('run_id'),'ok':err is None,'status':'ready' if err is None else 'error','error':err,'acknowledgment_qpc_ns':time.perf_counter_ns()})
                if err:raise ValueError(err)
                if state in ['run_end','abort']:active=None;stage=None
                if state=='block_end':break
            if active and stage in ['run_start','kept_start'] and time.monotonic()>=nextsample:
                active['snapshots'].append(collector.snapshot('periodic'));nextsample=time.monotonic()+1
            time.sleep(.025)
    except BaseException as e:
        if active:active['status']='rejected';active['valid']=False;active['rejection_reasons'].append(repr(e));save()
        raise
    finally:collector.close()

def inspect(output,pid):
    api.atomic(output,{'target':identify(pid),'warning':'Explicit identity only. Add helpers only with verified ownership; no automatic descendant or name-wide adoption.'})

def calibrate(output,seconds=30,pdh=True):
    target={**identify(os.getpid()),'app':'owned-calibration','build':sys.version.split()[0]};m={'schema_version':1,'target':target,'helpers':[],'observers':[],'scope_review':{'complete':False,'reason':'owned collector self-test'}}
    collector=Handles(m,pdh);snaps=[];beg=time.perf_counter();deadline=beg+seconds
    try:
        while time.perf_counter()<deadline:
            snaps.append(collector.snapshot('calibration'));time.sleep(min(1,max(0,deadline-time.perf_counter())))
        snaps.append(collector.snapshot('calibration_end'))
    finally:collector.close()
    a,b=snaps[0],snaps[-1];pa=a['processes'][0];pb=b['processes'][0];wall=((b['qpc_ns_start']+b['qpc_ns_end'])-(a['qpc_ns_start']+a['qpc_ns_end']))/2e9
    costs=[(s['qpc_ns_end']-s['qpc_ns_start'])/1e9 for s in snaps]
    cpu=((pb['cpu_kernel_100ns']+pb['cpu_user_100ns'])-(pa['cpu_kernel_100ns']+pa['cpu_user_100ns']))/1e7
    result={'status':'MEASURED owned-observer calibration only, not DAW CPU','header':environment(m),'pdh_enabled':pdh,'wall_seconds':wall,'collector_cpu_seconds':cpu,'collector_percent_one_logical_cpu':100*cpu/wall,
            'snapshot_cost_seconds':{'mean':statistics.mean(costs),'max':max(costs),'n':len(costs)},'snapshots':snaps,'limitation':'Not causal perturbation estimate on target; does not include controller/ETW; background load captured, no performance ranking'}
    api.atomic(output,result);print(json.dumps({k:result[k] for k in ['wall_seconds','collector_percent_one_logical_cpu','snapshot_cost_seconds']}))

def main():
    p=argparse.ArgumentParser();s=p.add_subparsers(dest='cmd',required=True)
    a=s.add_parser('inspect');a.add_argument('--pid',type=int,required=True);a.add_argument('--out',required=True)
    a=s.add_parser('observe');a.add_argument('--control',required=True);a.add_argument('--session',required=True);a.add_argument('--manifest',required=True);a.add_argument('--timeout',type=float,default=300)
    a=s.add_parser('calibrate');a.add_argument('--out',required=True);a.add_argument('--seconds',type=float,default=30);a.add_argument('--no-pdh',action='store_true')
    a=p.parse_args()
    if a.cmd=='inspect':inspect(a.out,a.pid)
    elif a.cmd=='calibrate':calibrate(a.out,a.seconds,not a.no_pdh)
    else:observe(a.control,a.session,json.loads(Path(a.manifest).read_text(encoding='utf-8-sig')),a.timeout)
if __name__=='__main__':main()
