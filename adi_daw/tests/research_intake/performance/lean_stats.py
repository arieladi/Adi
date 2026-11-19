"""Strict adapter around unchanged original paired statistics. No cycle-to-time conversion."""
import argparse,json,math
from pathlib import Path
import lean_cpu as l
import stats_original as original
def read_run(path):
    path=Path(path);raw=json.loads(path.read_text(encoding='utf-8-sig'));h=raw['header'];m=l.manifest_check(h['scope_manifest'])
    if not h.get('fixture_sha256') or not h.get('operator_start_evidence'):raise ValueError('Missing fixture/start evidence')
    expected={x['pid']:l.key(x) for x in [m['target']]+m.get('helpers',[])}
    for marker in raw['control_markers']:
        l.guard_check(marker,m['target']['pid'])
        if marker['condition_id']!=h['condition_id']:raise ValueError('Condition mismatch')
    for s in raw['snapshots']:
        found={}
        for p in s['processes']:
            if p['source']!='GetProcessTimes' or p.get('identity_verified') is not True:raise ValueError('Unverified process counter')
            if p['pid'] in expected:
                if l.key(p)!=expected[p['pid']]:raise ValueError('Scoped identity changed')
                found[p['pid']]=True
            if any(p.get(k) is None or p[k]<0 for k in ['cpu_kernel_100ns','cpu_user_100ns','cycles']):raise ValueError('Unavailable/negative counter')
        if found.keys()!=expected.keys():raise ValueError('Scoped process missing')
    if not raw.get('scope_audit',{}).get('before_after_review_passed'):raise ValueError('Unverified final scope')
    r=original.read_run(path);r['scope_complete_operator_claim']=raw['scope_audit']['scope_complete_operator_claim'];r['fixture_sha256']=h['fixture_sha256'];r['condition_id']=h['condition_id'];r['observer_fingerprint']=h['observer_fingerprint']
    r['target_cpu_percent_one_logical_cpu']=original.metric(r,'cpu_percent_one_logical_cpu',['target','daw-helper'])
    r['observer_cpu_percent_one_logical_cpu']=sum(x['cpu_percent_one_logical_cpu'] for x in r['processes'] if x['role'].startswith('observer/') and x['cpu_percent_one_logical_cpu'] is not None)
    r['status']='MEASURED';r['limitation']+=' Explicit-process scope only; foreground/transport tested only at boundaries; no continuous playback/sample-level proof.'
    return r
def compare(path):
    path=Path(path);m=json.loads(path.read_text(encoding='utf-8-sig'));seen=[]
    if m.get('metric')!='cpu_percent_one_logical_cpu':raise ValueError('Primary metric must use GetProcessTimes CPU seconds')
    if m.get('roles')!=['target','daw-helper']:raise ValueError('Explicit target/helper roles required')
    if m.get('instances',1)!=1:raise ValueError('Keep total-eight-device delta; per-device linear scaling not assumed')
    for group in ['aa_pairs','pairs']:
        for pair in m[group]:
            for arm in ['A','B']:
                entry=pair[arm];r=read_run(path.parent/entry['file']);seen.append(r)
                if entry['condition_id']!=r['condition_id'] or entry['observer_fingerprint']!=r['observer_fingerprint']:raise ValueError('Comparison metadata disagrees with run')
                if m['condition_fixtures'][entry['condition_id']]!=r['fixture_sha256']:raise ValueError('Fixture condition disagrees with run')
    result=original.compare_manifest(path)
    result['scope_complete_operator_claim']=all(r['scope_complete_operator_claim'] for r in seen)
    result['frequency_compatible']=not result['frequency_flags']
    # Historical output key: within-target gates only, not ranking authorization.
    result['ranking_allowed']=result['frequency_compatible'] and result['scope_complete_operator_claim'] and result['separable']
    result['ranking_limit']='Per-app condition statistics only; no cross-DAW efficiency ranking is authorized.'
    result['validated_run_summaries']=seen
    return result
def main():
    p=argparse.ArgumentParser();g=p.add_mutually_exclusive_group(required=True);g.add_argument('--run',type=Path);g.add_argument('--compare',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    r=compare(a.compare) if a.compare else read_run(a.run);a.out.write_text(json.dumps(r,indent=2),encoding='utf-8');print(json.dumps({k:r[k] for k in ['status','target_cpu_percent_one_logical_cpu','observer_cpu_percent_one_logical_cpu','ranking_allowed','separable'] if k in r}))
if __name__=='__main__':main()
