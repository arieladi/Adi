"""Per-app CPU run-level statistics. Standard library only. No cross-DAW ranking.
Counters describe CPU consumption, never callback percentiles. Raw cycles are not seconds.
"""
import argparse,csv,hashlib,json,math,statistics,sys
from pathlib import Path
LIMIT='A sampled counter cannot prove callback timing or the absence of misses.'

def beta_cf(a,b,x):
    qab=a+b;qap=a+1;qam=a-1;c=1.;d=1-qab*x/qap;d=1/(d if abs(d)>1e-300 else 1e-300);h=d
    for m in range(1,501):
        m2=2*m;aa=m*(b-m)*x/((qam+m2)*(a+m2));d=1+aa*d;c=1+aa/c
        d=d if abs(d)>1e-300 else 1e-300;c=c if abs(c)>1e-300 else 1e-300;d=1/d;h*=d*c
        aa=-(a+m)*(qab+m)*x/((a+m2)*(qap+m2));d=1+aa*d;c=1+aa/c
        d=d if abs(d)>1e-300 else 1e-300;c=c if abs(c)>1e-300 else 1e-300;d=1/d;delta=d*c;h*=delta
        if abs(delta-1)<3e-14:return h
    raise ArithmeticError('beta failed to converge')

def ibeta(a,b,x):
    if x<=0:return 0.
    if x>=1:return 1.
    bt=math.exp(math.lgamma(a+b)-math.lgamma(a)-math.lgamma(b)+a*math.log(x)+b*math.log1p(-x))
    return bt*beta_cf(a,b,x)/a if x<(a+1)/(a+b+2) else 1-bt*beta_cf(b,a,1-x)/b

def t_cdf(t,df):
    tail=.5*ibeta(df/2,.5,df/(df+t*t))
    return 1-tail if t>=0 else tail

def tcrit(df,p=.975):
    if df<1:raise ValueError('Need df>=1')
    lo,hi=0.,1.
    while t_cdf(hi,df)<p:hi*=2
    for _ in range(100):
        mid=(lo+hi)/2
        if t_cdf(mid,df)<p:lo=mid
        else:hi=mid
    return (lo+hi)/2

def ci(values):
    if len(values)<2:raise ValueError('CI needs >=2 independent runs/pairs')
    if not all(math.isfinite(x) for x in values):raise ValueError('Nonfinite values')
    mean=statistics.mean(values);se=statistics.stdev(values)/math.sqrt(len(values));half=tcrit(len(values)-1)*se
    return {'mean':mean,'lower':mean-half,'upper':mean+half,'n':len(values),'method':'paired Student t, two-sided 95%, independent pair differences assumed'}

def percentile(values,p):
    v=sorted(values)
    if not v:return None
    i=(len(v)-1)*p;a=int(i);b=min(a+1,len(v)-1)
    return v[a]+(v[b]-v[a])*(i-a)

def process_map(snapshot):return {(p['pid'],p.get('create_time_100ns')):p for p in snapshot['processes']}

def read_run(path,protocol=None):
    raw=json.loads(path.read_text(encoding='utf-8-sig'))
    if raw.get('schema_version')!=1:raise ValueError('Unsupported harness schema')
    if raw.get('status')!='complete':raise ValueError('Run status is not complete')
    if raw.get('valid') is not True:raise ValueError('Harness run not explicitly valid')
    if raw.get('status') in {'invalid','rejected','error','aborted'}:raise ValueError('Harness invalidated run')
    markers=raw.get('control_markers',[])
    for m in markers:
        if m.get('state') in {'abort'}:raise ValueError('Aborted run')
        if m.get('state') in {'run_start','kept_start','kept_end','run_end'}:
            g=m.get('guard',{})
            if g.get('passed') is not True or g.get('transport_expected')!=g.get('transport_observed'):raise ValueError('Invalid guard')
    for state in ['run_start','kept_start','kept_end','run_end']:
        if sum(m.get('state')==state for m in markers)!=1:raise ValueError('Missing/duplicate marker '+state)
    expected=['run_start','kept_start','kept_end','run_end']
    if [m.get('state') for m in markers]!=expected:raise ValueError('Markers out of order')
    h=raw.get('header',{});seq=[m.get('sequence') for m in markers];qpc=[m.get('marker_qpc_ns') for m in markers]
    if any(not isinstance(v,int) for v in seq+qpc) or any(b!=a+1 for a,b in zip(seq,seq[1:])) or any(b<=a for a,b in zip(qpc,qpc[1:])):raise ValueError('Nonmonotonic marker sequence/QPC')
    for m in markers:
        if m.get('run_id')!=raw.get('run_id') or m.get('case_id')!=raw.get('case_id') or m.get('block_id')!=h.get('block_id') or m.get('arm')!=h.get('arm') or m.get('pair_id')!=h.get('pair_id') or m.get('target_pid')!=h.get('target_pid'):raise ValueError('Marker identity disagrees with header')
    protocol=protocol or h.get('measurement_protocol') or {'warmup_seconds':30,'kept_seconds':60,'tolerance_seconds':1,'name':'standard'}
    if protocol.get('name')!='standard' and not protocol.get('reason'):raise ValueError('Nonstandard protocol requires explicit reason')
    if any(not isinstance(protocol.get(k),(int,float)) or protocol[k]<0 for k in ['warmup_seconds','kept_seconds','tolerance_seconds']):raise ValueError('Invalid protocol duration')
    warmup=(qpc[1]-qpc[0])/1e9;kept=(qpc[2]-qpc[1])/1e9
    if abs(warmup-protocol['warmup_seconds'])>protocol['tolerance_seconds'] or abs(kept-protocol['kept_seconds'])>protocol['tolerance_seconds']:raise ValueError('Run duration does not match declared protocol')
    snaps=raw.get('snapshots',[])
    b=[s for s in snaps if s.get('boundary')=='kept_start'];e=[s for s in snaps if s.get('boundary')=='kept_end']
    if len(b)!=1 or len(e)!=1:raise ValueError('Missing/duplicate cumulative kept boundaries')
    b,e=b[0],e[0];bmid=(b['qpc_ns_start']+b['qpc_ns_end'])/2;emid=(e['qpc_ns_start']+e['qpc_ns_end'])/2;wall=(emid-bmid)/1e9
    if wall<=0:raise ValueError('Nonpositive kept duration')
    if abs(wall-protocol['kept_seconds'])>protocol['tolerance_seconds']:raise ValueError('Snapshot kept duration mismatch')
    bm,em=process_map(b),process_map(e);rows=[]
    for key in bm.keys() & em.keys():
        bp,ep=bm[key],em[key];row={'pid':key[0],'create_time_100ns':key[1],'name':bp.get('name'),'role':bp.get('role')}
        for field,out in [('cycles','raw_cycles_per_second'),('cpu_kernel_100ns','kernel_seconds_per_second'),('cpu_user_100ns','user_seconds_per_second')]:
            if bp.get(field) is None or ep.get(field) is None:row[out]=None;continue
            delta=ep[field]-bp[field]
            if delta<0:raise ValueError('Negative cumulative delta: counter reset/PID reuse')
            row[out]=delta/wall/(1e7 if '100ns' in field else 1)
        row['cpu_percent_one_logical_cpu']=None if row['kernel_seconds_per_second'] is None or row['user_seconds_per_second'] is None else 100*(row['kernel_seconds_per_second']+row['user_seconds_per_second'])
        bt={(t['tid'],t.get('create_time_100ns')):t for t in bp.get('threads',[])};et={(t['tid'],t.get('create_time_100ns')):t for t in ep.get('threads',[])}
        row['threads']=[]
        for tk in bt.keys()&et.keys():
            a,z=bt[tk],et[tk];tr={'tid':tk[0],'create_time_100ns':tk[1],'name':z.get('name'),'attribution':z.get('attribution')}
            for f,unit in [('cycles',1),('kernel_100ns',1e7),('user_100ns',1e7)]:
                if a.get(f) is None or z.get(f) is None:tr[f+'_per_second']=None
                else:
                    delta=z[f]-a[f]
                    if delta<0:raise ValueError('Thread counter reset')
                    tr[f+'_per_second']=delta/wall/unit
            row['threads'].append(tr)
        row['thread_churn']=[{'tid':tk[0],'create_time_100ns':tk[1],'side':'start_only' if tk in bt else 'end_only'} for tk in bt.keys()^et.keys()]
        row['thread_limit']='Exited/new threads are not silently assigned zero CPU; per-thread sum can be incomplete even when process total is complete.'
        rows.append(row)
    churn=[{'pid':k[0],'create_time_100ns':k[1],'side':'start_only' if k in bm else 'end_only','role':(bm.get(k) or em[k]).get('role')} for k in bm.keys() ^ em.keys()]
    # Never turn missing/exited target-process CPU into zero or silently sum incomplete process trees.
    freq={}
    for s in snaps:
        mid=(s['qpc_ns_start']+s['qpc_ns_end'])/2
        if bmid<=mid<=emid:
            for counter,instances in s.get('pdh',{}).items():
                if 'processor performance' in counter.lower() and isinstance(instances,dict):
                    for name,value in instances.items():
                        if name != 'error' and '_total' not in name.lower() and isinstance(value,(int,float)) and math.isfinite(value):freq.setdefault(name,[]).append(value)
    return {'file':str(path),'kept_start_qpc_ns':bmid,'kept_end_qpc_ns':emid,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'wall_seconds':wall,'boundary_uncertainty_seconds':((b['qpc_ns_end']-b['qpc_ns_start'])+(e['qpc_ns_end']-e['qpc_ns_start']))/2e9,'processes':rows,'process_churn':churn,'processor_performance_median':{k:statistics.median(v) for k,v in freq.items()},'limitation':LIMIT,'measurement_protocol':protocol,'cycles_note':'Query*CycleTime raw cycles/s; no conversion to seconds or assumed fixed TSC.'}

def metric(run,name,roles):
    if any(c['role'] in roles for c in run['process_churn']):raise ValueError('Target process churn prevents complete boundary total')
    rows=[p for p in run['processes'] if p['role'] in roles]
    if not rows or any(p.get(name) is None for p in rows):raise ValueError('Missing target role/counter; no zero substitution')
    return sum(p[name] for p in rows)

def compare_values(diffs,aa,minimum=5):
    if len(diffs)<minimum or len(aa)!=len(diffs):raise ValueError('Need minimum pairs and equal-size A/A control')
    delta=ci(diffs);control=ci(aa)
    # Transform the signed A/A CI through absolute value; upper bound conservative for |mean|.
    floor=max(abs(control['lower']),abs(control['upper']))
    separated=(delta['lower']>0 or delta['upper']<0) and abs(delta['mean'])>floor
    return {'difference':delta,'aa_signed_ci':control,'aa_absolute_mean_upper95':floor,'separable':separated,'decision':'separable' if separated else 'not separable','ranking_rule':'Per-app condition statistics only; no cross-DAW ranking','limitation':LIMIT}

def compare_manifest(path):
    m=json.loads(path.read_text(encoding='utf-8-sig'));base=path.parent
    minimum=8 if m.get('device_cost') else 5;metricname=m.get('metric','cpu_percent_one_logical_cpu');roles=m.get('roles',['target','daw-helper']);instances=m.get('instances',1)
    if metricname not in {'raw_cycles_per_second','cpu_percent_one_logical_cpu'}:raise ValueError('Unknown metric')
    if not isinstance(instances,int) or instances<1:raise ValueError('Invalid instance count')
    runs={};rejected=[];freqflags=[];fingerprints=set();groups={}
    for group in ['aa_pairs','pairs']:
        diffs=[]
        for i,pair in enumerate(m[group]):
            expected=['A','B'] if i%2==0 else ['B','A']
            if pair.get('order')!=expected:raise ValueError('Pair order must alternate AB/BA, including A/A slots')
            arms={}
            for arm in ['A','B']:
                entry=pair[arm];f=entry.get('observer_fingerprint')
                if not f:raise ValueError('Missing observer fingerprint')
                fingerprints.add(f)
                p=(base/entry['file']).resolve()
                if str(p) in runs:raise ValueError('A physical run reused across pairs')
                r=read_run(p,entry.get('measurement_protocol'));runs[str(p)]=r;arms[arm]=r
                if group=='aa_pairs' and pair['A'].get('condition_id') != pair['B'].get('condition_id'):raise ValueError('A/A conditions differ')
                if not entry.get('condition_id'):raise ValueError('Missing condition identity')
            if arms[expected[0]]['kept_end_qpc_ns'] >= arms[expected[1]]['kept_start_qpc_ns']:raise ValueError('Declared AB/BA order does not match measured boundaries')
            av=metric(arms['A'],metricname,roles);bv=metric(arms['B'],metricname,roles)
            diffs.append((bv-av)/instances)
            fa=arms['A']['processor_performance_median'];fb=arms['B']['processor_performance_median']
            common=fa.keys()&fb.keys();drift={k:fb[k]-fa[k] for k in common if abs(fb[k]-fa[k])>5}
            if drift or not common:freqflags.append({'group':group,'pair':i,'differ_by_more_than_5pp':drift,'covariate_missing':not bool(common)})
        groups[group]=diffs
    aaend=max(runs[str((base/p[a]['file']).resolve())]['kept_end_qpc_ns'] for p in m['aa_pairs'] for a in ['A','B'])
    firststart=min(runs[str((base/p[a]['file']).resolve())]['kept_start_qpc_ns'] for p in m['pairs'] for a in ['A','B'])
    if aaend >= firststart:raise ValueError('A/A did not precede comparison')
    if len(fingerprints)!=1:raise ValueError('Unmatched observer instrumentation')
    result=compare_values(groups['pairs'],groups['aa_pairs'],minimum)
    result.update({'status':'MEASURED','case_id':m['case_id'],'metric':metricname,'instances_divisor':instances,'roles':roles,'pair_differences':groups,'frequency_flags':freqflags,'runs':runs,'observer_fingerprint':next(iter(fingerprints))})
    # A/A must predate comparison block, not be an afterthought; metadata required from the run plan.
    if m.get('aa_precedes_comparison') is not True:raise ValueError('Missing A/A-before-comparison certification')
    # Historical output key: within-app frequency gate, not ranking authorization.
    result['ranking_allowed']=not freqflags
    if freqflags:result['interpretation_warning']='Frequency mismatch or unavailable frequency covariate; do not assign difference solely to workload.'
    return result

def wake_stats(path):
    m=json.loads(path.read_text(encoding='utf-8-sig'))
    if m.get('lost_events')!=0 or m.get('lost_buffers')!=0:raise ValueError('Lost-event counts missing/nonzero')
    if not m.get('attribution_evidence') or not m.get('callback_boundary_evidence'):raise ValueError('No independently validated thread attribution/callback boundaries')
    events=m['wakes']
    if not events:raise ValueError('No callback records')
    runtime=[];latency=[];intervals=[];dpc=[];isr=[];overruns=0;previous={}
    for e in events:
        if e.get('completion_observed') is not True:raise ValueError('Wake lacks observed callback completion; CSwitch block alone is insufficient')
        period=e['period_ns'];wake=e['wake_ns'];complete=e['completion_ns'];busy=e['runtime_ns']
        if period<=0 or complete<wake or busy<0 or busy>complete-wake:raise ValueError('Invalid callback timing')
        key=e['thread_id']
        if key in previous:
            if wake<=previous[key]:raise ValueError('Unordered callback wakes')
            intervals.append((wake-previous[key])/1e6)
        previous[key]=wake;runtime.append(busy/1e6);latency.append((complete-wake)/1e6);overruns+=int(complete-wake>period)
        if 'dpc_ns' not in e or 'isr_ns' not in e:raise ValueError('Missing DPC/ISR interval attribution')
        dpc.append(e['dpc_ns']/1e6);isr.append(e['isr_ns']/1e6)
    def stats(v):return {'p50':percentile(v,.5),'p95':percentile(v,.95),'p99':percentile(v,.99),'max':max(v) if v else None}
    return {'status':'MEASURED','source':str(path),'unit':'ms','wake_intervals':stats(intervals),'runtime_per_wake':stats(runtime),'wake_to_completion':stats(latency),'dpc':stats(dpc),'isr':stats(isr),'deadline_overruns':overruns,'count':len(events),'runtime_share_of_period':stats([e['runtime_ns']/e['period_ns'] for e in events]),'limitation':'Runtime can include spin-waiting. Callback attribution and completion must be independently demonstrated; ordinary CSwitch intervals are not callbacks.'}

def selftest(path):
    tests=[]
    def ok(name,value):
        if not value:raise AssertionError(name)
        tests.append({'name':name,'passed':True})
    def rejects(fn):
        try:fn()
        except ValueError:return True
        return False
    ok('Student t df1',abs(tcrit(1)-12.7062047364)<1e-8)
    ok('Student t df4',abs(tcrit(4)-2.7764451052)<1e-8)
    ok('Student t df7',abs(tcrit(7)-2.3646242516)<1e-8)
    ok('Student t df29',abs(tcrit(29)-2.0452296421)<1e-8)
    a=compare_values([.9,1,1.1,1.05,.95],[.01,-.01,.02,-.02,0]);ok('clear difference separable',a['separable'])
    b=compare_values([.001,-.001,0,.002,-.002],[.1,-.1,.2,-.2,0]);ok('noise difference not separable',not b['separable'])
    ok('reject insufficient device pairs',rejects(lambda:compare_values([1]*5,[0]*5,8)))
    ok('reject unequal AA pairs',rejects(lambda:compare_values([1]*5,[0]*6)))
    result={'status':'MEASURED','scope':'synthetic math/validation only; no target CPU runs','passed':True,'tests':tests}
    path.write_text(json.dumps(result,indent=2),encoding='utf-8');return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--self-test',action='store_true');p.add_argument('--run',type=Path);p.add_argument('--compare',type=Path);p.add_argument('--wakes',type=Path);p.add_argument('--out',type=Path);p.add_argument('--row',type=Path)
    a=p.parse_args()
    if a.self_test:r=selftest(Path(__file__).with_name('cpustats-selftest.json'))
    elif a.run:r=read_run(a.run)
    elif a.compare:r=compare_manifest(a.compare)
    elif a.wakes:r=wake_stats(a.wakes)
    else:p.error('Select --self-test, --run, --compare or --wakes')
    if a.out:a.out.write_text(json.dumps(r,indent=2),encoding='utf-8')
    if a.row:
        d=r.get('difference',{});row=f"| {r.get('case_id','run')} | {r.get('status','MEASURED')} | {d.get('mean','see JSON')} | [{d.get('lower','n/a')}, {d.get('upper','n/a')}] | {d.get('method','boundary deltas')} | {a.out or 'stdout'} | {r.get('decision','no comparison')} â€” {LIMIT} |\n";a.row.write_text(row,encoding='utf-8')
    print(json.dumps(r,indent=2))

if __name__=='__main__':main()
