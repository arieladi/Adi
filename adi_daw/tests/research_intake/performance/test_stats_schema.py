"""Synthetic integration/schema rejection tests; no application is run."""
import copy,json,tempfile
from pathlib import Path
import stats_original as c

def run():
    tests=[]
    with tempfile.TemporaryDirectory() as td:
        p=Path(td)/'run.json'
        identity={'schema_version':1,'block_id':'b','run_id':'r','case_id':'CPU-TEST','arm':'A','pair_id':0,'target_pid':123}
        ms=[]
        for seq,(state,t) in enumerate(zip(['run_start','kept_start','kept_end','run_end'],[1,31,91,92])):
            ms.append(dict(identity,sequence=seq,state=state,marker_qpc_ns=t*10**9,guard={'passed':True,'transport_expected':'playing','transport_observed':'playing'}))
        def snap(boundary,t,cycles,cpu):
            return {'boundary':boundary,'qpc_ns_start':int(t*1e9),'qpc_ns_end':int(t*1e9)+1000,'processes':[{'pid':123,'create_time_100ns':4,'role':'target','name':'target.exe','cycles':cycles,'cpu_kernel_100ns':0,'cpu_user_100ns':cpu,'threads':[{'tid':456,'create_time_100ns':5,'cycles':cycles,'kernel_100ns':0,'user_100ns':cpu,'name':'synthetic','attribution':'unattributed'}]}],'pdh':{r'\Processor Information(*)\% Processor Performance':{'0,0':100.0}}}
        good={'schema_version':1,'status':'complete','valid':True,'case_id':'CPU-TEST','run_id':'r','header':{k:identity[k] for k in ['block_id','arm','pair_id','target_pid']},'control_markers':ms,'snapshots':[snap('kept_start',31,100,0),snap('kept_end',91,60100,300000000)]}
        def check(name,obj,expected=True):
            p.write_text(json.dumps(obj))
            try:r=c.read_run(p);passed=expected
            except ValueError:passed=not expected;r=None
            if not passed:raise AssertionError(name)
            tests.append({'name':name,'passed':True});return r
        r=check('known cumulative boundary deltas',good)
        assert abs(r['processes'][0]['cpu_percent_one_logical_cpu']-50)<1e-10
        assert abs(r['processes'][0]['raw_cycles_per_second']-1000)<1e-10
        assert len(r['processes'][0]['threads'])==1
        for name,change in [
            ('reject incomplete',lambda x:x.update(status='recording')),
            ('reject invalid',lambda x:x.update(valid=False)),
            ('reject reversed markers',lambda x:x['control_markers'].reverse()),
            ('reject missing kept marker',lambda x:x['control_markers'].pop(1)),
            ('reject failed guard',lambda x:x['control_markers'][2]['guard'].update(passed=False)),
            ('reject mismatched run',lambda x:x['control_markers'][1].update(run_id='another')),
            ('reject short warmup',lambda x:x['control_markers'][1].update(marker_qpc_ns=10*10**9)),
            ('reject short kept samples',lambda x:x['snapshots'][1].update(qpc_ns_start=80*10**9,qpc_ns_end=80*10**9+1000)),
            ('reject reset counter',lambda x:x['snapshots'][1]['processes'][0].update(cycles=0)),
        ]:
            obj=copy.deepcopy(good);change(obj);check(name,obj,False)
        obj=copy.deepcopy(good);obj['snapshots'][1]['processes'][0]['threads']=[];r=check('thread exit retained as churn',obj);assert r['processes'][0]['thread_churn']
        obj=copy.deepcopy(good);obj['snapshots'][1]['processes']=[];r=check('process exit retained as incomplete',obj)
        try:c.metric(r,'raw_cycles_per_second',['target'])
        except ValueError:tests.append({'name':'process tree sum rejects exited process','passed':True})
        else:raise AssertionError('process churn')
        w=Path(td)/'wakes.json';wr={'lost_events':0,'lost_buffers':0,'attribution_evidence':'synthetic proof','callback_boundary_evidence':'synthetic callback markers','wakes':[{'thread_id':1,'wake_ns':1000,'completion_ns':7000,'runtime_ns':5000,'period_ns':5000,'completion_observed':True,'dpc_ns':0,'isr_ns':0},{'thread_id':1,'wake_ns':9000,'completion_ns':10000,'runtime_ns':1000,'period_ns':5000,'completion_observed':True,'dpc_ns':0,'isr_ns':0}]}
        w.write_text(json.dumps(wr));assert c.wake_stats(w)['deadline_overruns']==1;tests.append({'name':'explicit callback overrun arithmetic','passed':True})
        for key,val in [('lost_events',1),('callback_boundary_evidence','')]:
            bad=copy.deepcopy(wr);bad[key]=val;w.write_text(json.dumps(bad))
            try:c.wake_stats(w)
            except ValueError:tests.append({'name':'reject '+key,'passed':True})
            else:raise AssertionError(key)
    output={'status':'MEASURED','scope':'Synthetic integration fixtures only, not DAW CPU measurements','passed':True,'tests':tests}

    print(json.dumps(output,indent=2))
if __name__=='__main__':run()
