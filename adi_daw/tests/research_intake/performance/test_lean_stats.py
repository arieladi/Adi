"""Synthetic end-to-end strict-adapter checks; no application is launched."""
import copy,json,tempfile,unittest
from pathlib import Path
import lean_stats

def fixture():
 identity={'pid':123,'create_time_100ns':4,'image':r'C:\fixture\target.exe','app':'ADI','build':'synthetic'}
 base={'schema_version':1,'block_id':'b','run_id':'r','case_id':'synthetic','arm':'A','pair_id':0,'target_pid':123,'condition_id':'a'}
 markers=[dict(base,sequence=i,state=s,marker_qpc_ns=t*10**9,guard={'passed':True,'transport_expected':'playing','transport_observed':'playing','target_foreground':True,'foreground_pid':123}) for i,(s,t) in enumerate(zip(['run_start','kept_start','kept_end','run_end'],[1,31,91,92]))]
 def snapshot(boundary,t,cpu,cycles):
  return {'boundary':boundary,'qpc_ns_start':t*10**9,'qpc_ns_end':t*10**9+1000,'pdh':{r'\Processor Information(*)\% Processor Performance':{'0,0':100.0}},'processes':[dict(identity,role='target',source='GetProcessTimes',identity_verified=True,cpu_kernel_100ns=0,cpu_user_100ns=cpu,cycles=cycles,threads=[])]}
 header=dict(base,scope_manifest={'schema_version':1,'target':identity,'helpers':[],'observers':[]},fixture_sha256='1'*64,operator_start_evidence='synthetic assertion only',observer_fingerprint='synthetic')
 return {'schema_version':1,'case_id':'synthetic','run_id':'r','status':'complete','valid':True,'header':header,'control_markers':markers,'snapshots':[snapshot('kept_start',31,0,100),snapshot('kept_end',91,300000000,60100)],'scope_audit':{'before_after_review_passed':True,'scope_complete_operator_claim':True}}

class Adapter(unittest.TestCase):
 def read(self,obj):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'run.json';p.write_text(json.dumps(obj));return lean_stats.read_run(p)
 def test_known_cpu_denominator(self):self.assertAlmostEqual(self.read(fixture())['target_cpu_percent_one_logical_cpu'],50.0)
 def test_fail_closed(self):
  changes=[lambda x:x['header'].pop('fixture_sha256'),lambda x:x['header'].pop('operator_start_evidence'),lambda x:x['scope_audit'].update(before_after_review_passed=False),lambda x:x['snapshots'][1]['processes'][0].update(create_time_100ns=5),lambda x:x['snapshots'][1]['processes'].clear(),lambda x:x['snapshots'][1]['processes'][0].update(cpu_user_100ns=-1),lambda x:x['snapshots'][1]['processes'][0].update(identity_verified=False),lambda x:x['control_markers'][1]['guard'].update(foreground_pid=9)]
  for change in changes:
   obj=copy.deepcopy(fixture());change(obj)
   with self.assertRaises((ValueError,KeyError)):self.read(obj)
if __name__=='__main__':unittest.main()
