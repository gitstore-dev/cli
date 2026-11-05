#!/usr/bin/env python3
"""Explicit disposable rootless Scylla image-helper checks; no product activation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
from fixture import base_config

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image-root', required=True, type=Path)
parser.add_argument('--native-probe', required=True, type=Path)
parser.add_argument('--fixture-parent', required=True, type=Path)
parser.add_argument('--trace-memory', action='store_true', help='developer-only existing host strace, memory syscalls only')
parser.add_argument('--action', choices=('version', 'help'), default='version', help='fixed read-only helper action')
parser.add_argument('--helper', choices=('scylla', 'cqlsh'), help='run one fixed image helper check')
args = parser.parse_args()
if os.geteuid() == 0 or platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64'):
    raise SystemExit('Select the dedicated non-root disposable Linux fixture')
parent = args.fixture_parent.resolve(strict=True)
image = args.image_root.resolve(strict=True)
probe = args.native_probe.resolve(strict=True)
for path in (parent, image):
    info = path.stat()
    if info.st_uid != os.geteuid() or info.st_mode & 0o022 or not path.is_dir():
        raise SystemExit('Only owned non-shared fixture/image directories are allowed')
rootfs = image / 'rootfs'
manifest = image / 'manifest.json'
if hashlib.sha256(manifest.read_bytes()).hexdigest() != 'e65d9d7113cb5b324baa4cf982ab27f9ace4a2f11666942bd9a8c09293297768':
    raise SystemExit('Select the exact acquired arm64 Scylla candidate')
if 'scylla:x:998:1000:' not in (rootfs / 'etc/passwd').read_text():
    raise SystemExit('The pinned image user no longer matches the fixed policy')
for executable in ((f'/usr/bin/{args.helper}',) if args.helper else ('/usr/bin/scylla', '/usr/bin/cqlsh')):
    root = Path(tempfile.mkdtemp(prefix='scylla-native-', dir=parent))
    root.chmod(0o700)
    for directory in ('bundle', 'state'):
        (root / directory).mkdir(mode=0o700)
    config = base_config(rootfs, [executable, '--' + args.action])
    config_path = root / 'bundle/config.json'
    config_path.write_text(json.dumps(config))
    config_path.chmod(0o600)
    env = {'PATH': '/nonexistent', 'HOME': str(root), 'LANG': 'C',
           'XDG_RUNTIME_DIR': f'/run/user/{os.geteuid()}',
           'DBUS_SESSION_BUS_ADDRESS': f'unix:path=/run/user/{os.geteuid()}/bus'}
    command = [str(probe), str(root)]
    if args.trace_memory:
        command = ['/usr/bin/strace', '-f', '-e', 'trace=mmap,munmap,mprotect,brk', '-o', str(root / 'memory.trace'), *command]
    result = subprocess.run(command, env=env, capture_output=True, timeout=45)
    record = {'scope': 'raw image helper prototype, not registered deployment execution',
              'executable': executable, 'action': args.action, 'probeSha256': hashlib.sha256(probe.read_bytes()).hexdigest(),
              'kernel': platform.release(), 'namespaceUid': 0, 'namespaceGid': 0, 'declaredImageUid': 998, 'declaredImageGid': 1000,
              'exitCode': result.returncode, 'fixture': str(root),
              'probeDiagnostic': result.stderr.decode('utf-8', errors='replace')[:1024]}
    if (root / 'output.log').exists():
        record['nativeDiagnostic'] = (root / 'output.log').read_bytes()[:4096].decode('utf-8', errors='replace')
    if result.returncode == 0:
        output = (root / 'output.log').read_bytes()
        if not output or len(output) > 262144:
            raise SystemExit('Expected bounded image helper version output')
        record['versionOutput'] = output.decode('utf-8', errors='replace')[:4096]
    (root / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
    (root / 'evidence.json').chmod(0o600)
    print(json.dumps(record), flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
