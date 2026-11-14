"""Fixed raw OCI fixture configuration, never imported by production code."""
import json
import os
from pathlib import Path

def base_config(rootfs, arguments):
    source = Path(__file__).resolve().parents[3]
    policy = json.loads((source / 'runtime/profiles/seccomp/arm64.json').read_text())
    return {
        'ociVersion': '1.0.2', 'hostname': 'scylla-image-helper',
        'root': {'path': str(rootfs), 'readonly': True},
        'process': {
            'terminal': False, 'user': {'uid': 0, 'gid': 0},
            'args': arguments, 'cwd': '/',
            'env': ['PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
                    'HOME=/tmp', 'LANG=C', 'PYTHONDONTWRITEBYTECODE=1'],
            'noNewPrivileges': True,
            'capabilities': {name: [] for name in ('bounding', 'effective', 'inheritable', 'permitted', 'ambient')},
            'rlimits': [{'type': 'RLIMIT_AS', 'soft': 1 << 46, 'hard': 1 << 46},
                        {'type': 'RLIMIT_CORE', 'soft': 0, 'hard': 0},
                        {'type': 'RLIMIT_NOFILE', 'soft': 1024, 'hard': 1024}],
        },
        'mounts': [
            {'destination': '/proc', 'type': 'proc', 'source': 'proc', 'options': ['nosuid', 'nodev', 'noexec']},
            *({'destination': path, 'type': 'tmpfs', 'source': 'tmpfs', 'options': ['nosuid', 'nodev', 'noexec', option]}
              for path, option in (('/tmp', 'size=67108864,mode=1777'), ('/run', 'size=4194304,mode=0700'), ('/dev', 'size=4194304,mode=0755'))),
        ],
        'linux': {
            'namespaces': [{'type': name} for name in ('user', 'pid', 'mount', 'uts', 'ipc', 'network')],
            'uidMappings': [{'containerID': 0, 'hostID': os.geteuid(), 'size': 1}],
            'gidMappings': [{'containerID': 0, 'hostID': os.getegid(), 'size': 1}],
            'resources': {'memory': {'limit': 2 << 30}, 'pids': {'limit': 256}, 'cpu': {'quota': 100000, 'period': 100000}},
            'seccomp': policy,
        },
    }
