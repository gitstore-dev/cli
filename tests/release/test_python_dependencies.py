"""Publication dependency setup must not run with the repository write token."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[2]


class PublicationDependencies(unittest.TestCase):
    def test_every_locked_requirement_has_an_exact_version_and_sha256(self):
        lines=(ROOT/'packaging/requirements.txt').read_text().splitlines()
        entries=[];current=[]
        for line in lines:
            if not line.strip() or line.startswith('#'):continue
            current.append(line)
            if not line.endswith('\\'):
                entries.append(' '.join(current));current=[]
        self.assertFalse(current)
        self.assertGreaterEqual(len(entries),6)
        for entry in entries:
            self.assertRegex(entry,r'^[a-zA-Z0-9_-]+==[^\s]+\s')
            self.assertTrue(re.search(r'--hash=sha256:[0-9a-f]{64}(?:\s|$)',entry))

    def test_publish_token_is_absent_during_dependency_installation(self):
        workflow=(ROOT/'.github/workflows/release.yml').read_text().split('\n  publish:\n',1)[1]
        header,steps=workflow.split('    steps:\n',1)
        self.assertNotIn('GH_TOKEN',header)
        installations=0;write_steps=0
        for step in re.split(r'(?m)^      - ',steps):
            if 'pip" install' in step:
                installations+=1
                self.assertNotIn('GH_TOKEN',step)
                self.assertNotIn('GITSTORE_TAP_TOKEN',step)
                self.assertIn('--require-hashes --only-binary=:all: -r packaging/requirements.txt',step)
            if 'GH_TOKEN: ${{ github.token }}' in step:
                write_steps+=1
                self.assertNotIn('pip" install',step)
        self.assertLess(workflow.index('--require-hashes'),workflow.index('id: tap-app'))
        self.assertIn('permission-pull-requests: write',workflow)
        self.assertEqual(installations,1)
        self.assertEqual(write_steps,1)


if __name__=='__main__':unittest.main()
