"""Native diagnostics must never print fixture intents, results or raw messages."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


class NativeDiagnostic(unittest.TestCase):
    @unittest.skipIf(os.geteuid()==0,'select a non-root contributor fixture')
    def test_only_selected_operation_codes_and_resource_counts_are_emitted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();root.chmod(0o700)
            state=root/'gs-public-runtime-sample00/state';state.mkdir(parents=True)
            secret='synthetic-private-message-never-emit'
            with sqlite3.connect(state/'state.db') as db:
                db.execute('CREATE TABLE operations(state,error,intent,result)')
                db.execute('INSERT INTO operations VALUES(?,?,?,?)',('failed',json.dumps(dict(
                    code='OPERATION_FAILED',stage='unavailable',message=secret)),secret,secret))
                db.execute('CREATE TABLE assets(asset_key,descriptor,state)')
                db.execute('INSERT INTO assets VALUES(?,?,?)',('a'*64,json.dumps(dict(images=[dict(component='git')])),'staging'))
                db.execute('CREATE TABLE resource_ownership(resource_type,lifecycle_stage,ownership_state,backend_identity)')
                db.execute('INSERT INTO resource_ownership VALUES(?,?,?,?)',('workload','reserved','held',
                    json.dumps(dict(execution=dict(private=secret),unrelated=secret))))
            fixture=root/'fixture.json';fixture.write_text(json.dumps(dict(
                dedicatedTestAccount=True,disposableUid=os.geteuid(),fixtureParent=str(root))))
            fixture.chmod(0o600)
            output=root/'diagnostic.json'
            reply=subprocess.run([sys.executable,str(Path(__file__).with_name('diagnose-native.py')),
                '--fixture',str(fixture),'--output',str(output)],capture_output=True,text=True,check=True)
            self.assertNotIn(secret,reply.stdout+reply.stderr+output.read_text())
            value=json.loads(output.read_text())
            self.assertEqual(value['fixtures'][0]['operations'],[dict(state='failed',code='OPERATION_FAILED',stage='unavailable')])
            self.assertEqual(value['fixtures'][0]['assets'],[dict(state='staging',count=1)])
            self.assertEqual(value['fixtures'][0]['workloadMilestoneCounts'],dict(execution=1,population=0,native=0))
            self.assertEqual(output.stat().st_mode&0o777,0o600)
            with sqlite3.connect(state/'state.db') as db:
                db.execute("UPDATE assets SET state='active'")
            (state/'assets').mkdir()
            (state/'assets'/('a'*64)).mkdir(mode=0o700)
            probe=root/'synthetic-probe'
            probe.write_text('#!'+sys.executable+'\nimport json,sys\nprint(json.dumps(dict(status=4,unrelated='+repr(secret)+')))\nprint('+repr(secret)+',file=sys.stderr)\n')
            probe.chmod(0o500)
            second=root/'materialization.json'
            reply=subprocess.run([sys.executable,str(Path(__file__).with_name('diagnose-native.py')),
                '--fixture',str(fixture),'--output',str(second),'--provision-probe',str(probe)],
                capture_output=True,text=True,check=True)
            self.assertNotIn(secret,reply.stdout+reply.stderr+second.read_text())
            row=json.loads(second.read_text())['fixtures'][0]['materializationProbes'][0]
            self.assertEqual((row['component'],row['exit'],row['status']),('git',0,4))


if __name__=='__main__':
    unittest.main()
