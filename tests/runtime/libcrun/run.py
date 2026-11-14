#!/usr/bin/env python3
"""Runs only owned tiny-payload libcrun library probes on a selected Linux fixture."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('payload', type=Path)
parser.add_argument('--delegated', action='store_true')
parser.add_argument('--namespace-uid', type=int, choices=(0, 10000), default=0, help='developer-only single caller mapping comparison')
parser.add_argument('--fixture-parent', type=Path)
parser.add_argument('--keep', action='store_true')
parser.add_argument('--seccomp', type=Path)
parser.add_argument('--network', action='store_true', help='registered launch with daemon-owned namespace descriptors')
parser.add_argument('--namespace-owner', type=int, help='developer-only owned user/network namespace probe')
parser.add_argument('--registered', action='store_true', help='synthetic payload through the registered Git launch factory')
args = parser.parse_args()
if os.geteuid() == 0 or (args.seccomp and not args.delegated) or (args.registered and not args.seccomp):
    raise SystemExit('select a non-root fixture; seccomp policy requires delegated mode')
if args.namespace_uid and (args.registered or args.namespace_owner):
    raise SystemExit('Nonzero mapping comparison uses only the raw owned payload probe')
if args.fixture_parent:
    args.fixture_parent = args.fixture_parent.resolve(strict=True)
    info = args.fixture_parent.stat()
    if not args.fixture_parent.is_dir() or info.st_uid != os.geteuid() or info.st_mode & 0o022:
        raise SystemExit('Select an owned private fixture parent')
if args.network and not args.registered:
    raise SystemExit('Owned daemon namespace mode requires the registered payload')
if args.namespace_owner:
    owner = Path(f'/proc/{args.namespace_owner}')
    if args.registered or not args.delegated or owner.stat().st_uid != os.getuid():
        raise SystemExit('Shared namespace probe requires an owned non-registered fixture')
    if (owner / 'uid_map').read_text().split() != ['0', str(os.getuid()), '1'] or \
       (owner / 'gid_map').read_text().split() != ['0', str(os.getgid()), '1']:
        raise SystemExit('Unexpected namespace mapping')
probe, payload = (p.resolve(strict=True) for p in (args.probe, args.payload))
delegated = args.delegated
policy = None
if args.seccomp:
    with args.seccomp.open('rb') as source:
        raw = source.read(65537)
    if len(raw) > 65536:
        raise SystemExit('seccomp fixture bound')
    policy = json.loads(raw)
security_proof = ('nnp=1 seccomp=2 capabilities=0 explicit-filter=clone3-enosys\n' if policy
                  else 'nnp=1 seccomp=2 capabilities=0 explicit-filter=denied\n')
for scenario in ('normal', 'start-crash'):
    root = Path(tempfile.mkdtemp(prefix='gitstore-libcrun-', dir=args.fixture_parent))
    os.chmod(root, 0o700)
    for directory in ('bundle', 'bundle/rootfs', 'bundle/rootfs/proc', 'bundle/rootfs/dev', 'bundle/rootfs/out', 'state', 'output'):
        (root / directory).mkdir(mode=0o700)
    shutil.copy2(payload, root / 'bundle/rootfs/payload')
    if args.registered:
        (root / 'volumes').mkdir(mode=0o700)
        (root / 'bundle/rootfs/app').mkdir(mode=0o700)
        shutil.copy2(payload, root / 'bundle/rootfs/app/git-service')
        config_file = root / 'registered-config.toml'
        config_file.write_text('# synthetic config; not actual GitStore service input\n')
        config_file.chmod(0o600)
        if args.network:
            resolver = root / 'resolver.conf'
            resolver.write_text('nameserver 10.0.2.3\noptions timeout:2 attempts:2\n')
            resolver.chmod(0o600)
    config = {
        'ociVersion': '1.0.2',
        'hostname': 'gitstore-owned-probe',
        'process': {'terminal': False, 'user': {'uid': args.namespace_uid, 'gid': args.namespace_uid}, 'args': ['/payload'], 'env': ['PATH=/'], 'cwd': '/', 'noNewPrivileges': True},
        'root': {'path': str(root / 'bundle/rootfs'), 'readonly': True},
        'mounts': [
            {'destination': '/proc', 'type': 'proc', 'source': 'proc', 'options': ['nosuid', 'noexec', 'nodev']},
            {'destination': '/out', 'type': 'bind', 'source': str(root / 'output'), 'options': ['rbind', 'rw', 'nosuid', 'nodev', 'noexec']},
        ],
        'linux': {
            'namespaces': [{'type': kind} for kind in ('user', 'pid', 'mount', 'uts', 'ipc', 'network')],
            'uidMappings': [{'containerID': args.namespace_uid, 'hostID': os.getuid(), 'size': 1}],
            'gidMappings': [{'containerID': args.namespace_uid, 'hostID': os.getgid(), 'size': 1}],
        },
    }
    if args.namespace_owner:
        for item in config['linux']['namespaces']:
            if item['type'] in ('user', 'network'):
                name = 'user' if item['type'] == 'user' else 'net'
                item['path'] = f'/proc/{args.namespace_owner}/ns/{name}'
        config['linux'].pop('uidMappings')
        config['linux'].pop('gidMappings')
    if delegated:
        config['process']['env'].append('GSTD_FIXTURE_REQUIRE_SECURITY=1')
        if policy:
            config['process']['env'].append('GSTD_FIXTURE_SECURITY_FILTER=clone3')
        config['process']['capabilities'] = {name: [] for name in ('bounding', 'effective', 'inheritable', 'permitted', 'ambient')}
        config['linux']['resources'] = {'memory': {'limit': 134217728}, 'pids': {'limit': 64}, 'cpu': {'quota': 50000, 'period': 100000}}
        config['linux']['seccomp'] = policy or {'defaultAction': 'SCMP_ACT_ALLOW', 'syscalls': [{'names': ['getpriority'], 'action': 'SCMP_ACT_ERRNO', 'errnoRet': 1}]}
    (root / 'bundle/config.json').write_text(json.dumps(config))
    env = {'PATH': '/nonexistent', 'HOME': str(root), 'XDG_RUNTIME_DIR': str(root)}
    if delegated:
        env['XDG_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        env['DBUS_SESSION_BUS_ADDRESS'] = f'unix:path=/run/user/{os.getuid()}/bus'
    # The shared comparison needs exactly the selected build-library location.
    for name in ('LD_LIBRARY_PATH', 'ASAN_OPTIONS', 'UBSAN_OPTIONS'):
        if name in os.environ:
            env[name] = os.environ[name]
    command = [str(probe), str(root)] + (['registered'] if args.registered else []) + (['network'] if args.network else []) + (['start-crash'] if scenario == 'start-crash' else [])
    result = subprocess.run(command, env=env, timeout=30, capture_output=True, text=True)
    diagnostic = root / 'adapter.stderr'
    descriptor = os.open(diagnostic, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as stream:
        stream.write(result.stderr)
    excerpt = result.stderr if len(result.stderr) <= 8192 else result.stderr[:4096] + result.stderr[-4096:]
    print(json.dumps({'scenario': scenario, 'mode': 'registered-synthetic-git' if args.registered else (('delegated-core-policy' if policy else 'delegated') if delegated else 'namespace-only'), 'root': str(root), 'exit': result.returncode, 'stdout': result.stdout[-16384:], 'stderr': excerpt, 'diagnosticFile': str(diagnostic), 'diagnosticCharacters': len(result.stderr)}))
    if result.returncode:
        # A failed probe may have a surviving native workload. Retain its owned
        # root/state for explicit backend observation; never assume termination.
        raise SystemExit(result.returncode)
    output = root / ('volumes/volume-repositories/data' if args.registered else 'output')
    if (output / 'lifecycle').read_text() != 'ready\nterminated\n':
        raise SystemExit('Unexpected real payload lifecycle; fixture retained')
    if delegated and (output / 'security').read_text() != security_proof:
        raise SystemExit('Kernel security enforcement not observed; fixture retained')
    if not args.keep:
        shutil.rmtree(root)
