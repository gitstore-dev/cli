"""Real npm links must remain safe under a permissive hosted-account umask."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
from formats import tar_bytes

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('npm') and shutil.which('node'), 'requires fixture npm/Node')
class NpmLifecycle(unittest.TestCase):
    def test_install_replace_remove_with_inherited_group_writable_umask(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            root.chmod(0o700)
            os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
            arch = {'arm64': 'arm64', 'aarch64': 'arm64', 'x86_64': 'x64'}[platform.machine()]
            versions = ['0.1.0-alpha.1', '0.1.0-alpha.2']
            artifacts = []
            for version in versions:
                metadata = dict(name=f'@gitstore/cli-{os_name}-{arch}', version=version,
                    os=[os_name], cpu=[arch], bin={'gs': 'bin/gs', 'gitstored': 'bin/gitstored'})
                script = ('#!/bin/sh\ncase "$1" in\n'
                    f'--version) echo "fixture {version}";;\n'
                    '--help) echo "synthetic package fixture";;\n'
                    'deployment) exit 6;;\n*) exit 1;;\nesac\n').encode()
                payload = {'package/package.json': (json.dumps(metadata).encode(), 0o644),
                    **{f'package/bin/{name}': (script, 0o755) for name in ['gs', 'gitstored']}}
                artifact = root / (version + '.tgz')
                artifact.write_bytes(tar_bytes(payload))
                artifact.chmod(0o600)
                artifacts.append(artifact)
            parent = root / 'experiments'
            parent.mkdir(mode=0o700)
            command = [sys.executable, str(ROOT / 'tests/packaging/run.py'), '--channel', 'npm',
                '--fixture-parent', str(parent)]
            for prefix, artifact, version in zip(['artifact', 'replacement'], artifacts, versions):
                command += ['--' + prefix, str(artifact),
                    '--' + ('sha256' if prefix == 'artifact' else 'replacement-sha256'),
                    hashlib.sha256(artifact.read_bytes()).hexdigest(),
                    '--' + ('version' if prefix == 'artifact' else 'replacement-version'), version]
            result = subprocess.run(command, cwd=root, umask=0o002,
                env=dict(PATH=os.environ['PATH'], HOME=str(root), LANG='C'),
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            receipts = list(parent.glob('npm-lifecycle-*/receipt.json'))
            self.assertEqual(len(receipts), 1)
            receipt = json.loads(receipts[0].read_text())
            self.assertTrue(receipt['removed'])
            self.assertEqual([item['applicationVersion'] for item in receipt['artifacts']], versions)


if __name__ == '__main__':
    unittest.main()
