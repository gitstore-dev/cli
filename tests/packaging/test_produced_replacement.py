"""A replacement experiment must not build or mutate fixtures for foreign candidates."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

import test_layout as layout

ROOT = Path(__file__).resolve().parents[2]


class ProducedReplacement(unittest.TestCase):
    setUp = layout.LayoutTests.setUp
    metadata = layout.LayoutTests.metadata

    def test_foreign_source_inventory_fails_before_creating_output(self):
        candidate = self.root/'candidate'
        (candidate/'assets').mkdir(parents=True)
        (candidate/'assets/foreign.install.json').write_text(json.dumps(self.metadata()))
        output = self.root/'must-not-exist'
        reply = subprocess.run([sys.executable,str(ROOT/'tests/packaging/check-produced-replacement.py'),
            '--candidate',str(candidate),'--output',str(output)],capture_output=True,text=True,timeout=15)
        self.assertNotEqual(reply.returncode,0)
        self.assertIn('exact current source and version',reply.stderr)
        self.assertFalse(output.exists())

    def test_missing_production_inventory_never_creates_output(self):
        output = self.root/'must-not-exist'
        reply = subprocess.run([sys.executable,str(ROOT/'tests/packaging/check-produced-replacement.py'),
            '--candidate',str(self.root/'absent'),'--output',str(output)],capture_output=True,text=True,timeout=15)
        self.assertNotEqual(reply.returncode,0)
        self.assertIn('one actual production install inventory',reply.stderr)
        self.assertFalse(output.exists())


if __name__=='__main__':
    unittest.main()
