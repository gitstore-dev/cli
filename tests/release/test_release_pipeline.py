"""Run the real workflow graph's success/failure/recovery fixtures with the existing Node tooling."""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]


class ReleasePipeline(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GITSTORE_RELEASE_PLEASE_MODULE'),
                         'select the pinned Release Please module already used by CI')
    def test_pipeline_graph_blocks_failed_qualification_and_preserves_manual_recovery(self):
        reply=subprocess.run(['node',str(ROOT/'tests/release/pipeline-fixture.cjs'),str(ROOT)],
                             capture_output=True,text=True,check=True,timeout=30)
        self.assertTrue(all(json.loads(reply.stdout).values()))


if __name__=='__main__':unittest.main()
