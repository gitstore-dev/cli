"""Reference receipts require measured PRD latency boundaries and owned cleanup."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('qualifier_costs',Path(__file__).resolve().parents[2]/'scripts/qualify-release.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PerformanceReceipts(unittest.TestCase):
    def setUp(self):
        self.commands={name:dict(samples=100,p95Milliseconds=99.999) for name in
            ('gs --help','gs --version','gitstored --help','gitstored --version')}
        self.commands['gitstored --version']['p95Milliseconds']=249.999
        self.native=dict(status='passed',managerChildStopped=True,
            commandLatency=dict(commands=self.commands),daemonInspect=dict(samples=100,p95Milliseconds=249.999))

    def test_documented_distinct_command_and_inspection_targets(self):
        module.validate_performance(self.native,True)
        module.validate_performance(dict(commands=self.commands),False)
        for name,target in [('gs --help',100),('gs --version',100),('gitstored --version',250)]:
            value=copy.deepcopy(self.native)
            value['commandLatency']['commands'][name]['p95Milliseconds']=target
            with self.subTest(name=name),self.assertRaises(ValueError):
                module.validate_performance(value,True)
        value=copy.deepcopy(self.native);value['daemonInspect']['p95Milliseconds']=250
        with self.assertRaises(ValueError):
            module.validate_performance(value,True)

    def test_missing_samples_nonfinite_results_and_unstopped_owner_cannot_pass(self):
        for change in ('short','missing','nonfinite','failed','live'):
            value=copy.deepcopy(self.native)
            if change=='short':value['daemonInspect']['samples']=10
            elif change=='missing':del value['commandLatency']['commands']['gs --help']
            elif change=='nonfinite':value['daemonInspect']['p95Milliseconds']=float('nan')
            elif change=='failed':value['status']='failed'
            else:value['managerChildStopped']=False
            with self.subTest(change=change),self.assertRaises(ValueError):
                module.validate_performance(value,True)


if __name__=='__main__':
    unittest.main()
