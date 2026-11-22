import copy,json,os,time,unittest
from pathlib import Path
import lean_cpu as l

class Validation(unittest.TestCase):
    def setUp(self):
        self.m={'schema_version':1,'target':{'pid':10,'create_time_100ns':23,'image':r'C:\app\target.exe','app':'adi','build':'fixture'},'helpers':[]}
        self.marker={'target_pid':10,'condition_id':'playing','guard':{'passed':True,'transport_expected':'playing','transport_observed':'playing','target_foreground':True,'foreground_pid':10}}
    def test_exact_manifest(self):self.assertEqual(l.key(l.manifest_check(self.m)['target']),(10,23,r'c:\app\target.exe'))
    def test_relative_image_rejected(self):
        self.m['target']['image']='target.exe'
        with self.assertRaises(ValueError):l.manifest_check(self.m)
    def test_missing_creation_rejected(self):
        del self.m['target']['create_time_100ns']
        with self.assertRaises(ValueError):l.manifest_check(self.m)
    def test_duplicate_helper_rejected(self):
        self.m['helpers']=[dict(self.m['target'],ownership_evidence='same PID')]
        with self.assertRaises(ValueError):l.manifest_check(self.m)
    def test_helper_needs_provenance(self):
        self.m['helpers']=[dict(self.m['target'],pid=11)]
        with self.assertRaises(ValueError):l.manifest_check(self.m)
    def test_missing_both_transport_not_valid(self):
        self.marker['guard']['transport_expected']=None;self.marker['guard']['transport_observed']=None
        with self.assertRaises(ValueError):l.guard_check(self.marker,10)
    def test_wrong_foreground_rejected(self):
        self.marker['guard']['foreground_pid']=11
        with self.assertRaises(ValueError):l.guard_check(self.marker,10)
    def test_missing_condition_rejected(self):
        del self.marker['condition_id']
        with self.assertRaises(ValueError):l.guard_check(self.marker,10)
    def test_wrong_target_rejected(self):
        with self.assertRaises(ValueError):l.guard_check(self.marker,11)
    def test_valid_guard(self):l.guard_check(self.marker,10)
    def test_handle_identity_mismatch_rejected(self):
        actual=l.identify(os.getpid());self.m['target'].update(actual);self.m['target']['create_time_100ns']+=1
        with self.assertRaises(ValueError):l.Handles(self.m,False)
    def test_actual_owned_counter_handles(self):
        self.m['target'].update(l.identify(os.getpid()));h=l.Handles(self.m,False)
        try:
            a=h.snapshot('owned_a');b=h.snapshot('owned_b');self.assertEqual(len(a['processes']),1)
            self.assertEqual(a['processes'][0]['source'],'GetProcessTimes')
            self.assertGreaterEqual(b['processes'][0]['cycles'],a['processes'][0]['cycles'])
            self.assertGreaterEqual(b['processes'][0]['cpu_user_100ns'],a['processes'][0]['cpu_user_100ns'])
        finally:h.close()

if __name__=='__main__':unittest.main()
