#!/usr/bin/env python3
"""One owned raw Scylla startup/keyspace probe; not public runtime support."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import selectors
import shutil
import stat
import subprocess
import tempfile
import time
from fixture import base_config

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image-root', required=True, type=Path)
parser.add_argument('--api-image-root', type=Path)
parser.add_argument('--git-image-root', type=Path)
parser.add_argument('--backup-probe', type=Path)
parser.add_argument('--restart', action='store_true', help='Observe full stop before reopening the retained datastore')
parser.add_argument('--native-probe', required=True, type=Path)
parser.add_argument('--fixture-parent', required=True, type=Path)
args = parser.parse_args()
if os.geteuid() == 0 or platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64'):
    raise SystemExit('Select the dedicated non-root disposable Linux arm64 fixture')
image, parent, probe = (p.resolve(strict=True) for p in (args.image_root, args.fixture_parent, args.native_probe))
for path in (image, parent):
    info = path.stat()
    if not path.is_dir() or info.st_uid != os.geteuid() or info.st_mode & 0o022:
        raise SystemExit('Select owned non-shared image and fixture directories')
if hashlib.sha256((image / 'manifest.json').read_bytes()).hexdigest() != 'e65d9d7113cb5b324baa4cf982ab27f9ace4a2f11666942bd9a8c09293297768':
    raise SystemExit('Select the exact verified arm64 Scylla candidate')
api_image = args.api_image_root.resolve(strict=True) if args.api_image_root else None
git_image = args.git_image_root.resolve(strict=True) if args.git_image_root else None
if api_image:
    info = api_image.stat()
    if not api_image.is_dir() or info.st_uid != os.geteuid() or info.st_mode & 0o022:
        raise SystemExit('Select an owned non-shared API image directory')
    if hashlib.sha256((api_image / 'manifest.json').read_bytes()).hexdigest() != '3c61f77edcbe719069d9173f1bad270cc46a81e97e756844cea3db8dcfb31172':
        raise SystemExit('Select the exact verified arm64 API migration candidate')
if api_image:
    if not git_image or not git_image.is_dir() or git_image.stat().st_uid != os.geteuid() or git_image.stat().st_mode & 0o022:
        raise SystemExit('API check mode requires an owned verified Git image')
    if hashlib.sha256((git_image / 'manifest.json').read_bytes()).hexdigest() != 'b5dcff4321fd0ed799b7244d9f0eeda496868afb4a2dccec94e242ea2f951f43':
        raise SystemExit('Select the exact verified arm64 Git service image')
rootfs = image / 'rootfs'
root = Path(tempfile.mkdtemp(prefix='scylla-server-', dir=parent))
root.chmod(0o700)
env = {'PATH': '/nonexistent', 'HOME': str(root), 'LANG': 'C',
       'XDG_RUNTIME_DIR': f'/run/user/{os.geteuid()}',
       'DBUS_SESSION_BUS_ADDRESS': f'unix:path=/run/user/{os.geteuid()}/bus'}
def prepare(directory, config):
    for name in ('bundle', 'state'):
        (directory / name).mkdir(mode=0o700)
    path = directory / 'bundle/config.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
arguments = ['/usr/bin/scylla', '--developer-mode=1', '--options-file=/etc/scylla/scylla.yaml', '--overprovisioned', '--smp=1',
             '--memory=1536M', '--listen-address=127.0.0.1', '--rpc-address=127.0.0.1',
             '--broadcast-address=127.0.0.1', '--broadcast-rpc-address=127.0.0.1',
             '--workdir=/var/lib/scylla', '--api-address=127.0.0.1',
             '--log-to-stdout=1', '--log-to-syslog=0']
config = base_config(rootfs, arguments)
data = root / 'data'
data.mkdir(mode=0o700)
config['mounts'].append({'destination': '/var/lib/scylla', 'type': 'bind', 'source': str(data),
                         'options': ['rbind', 'rw', 'nosuid', 'nodev', 'noexec']})
prepare(root, config)
record = {'scope': 'raw datastore/keyspace prototype, not registered deployment execution',
          'fixture': str(root), 'probeSha256': hashlib.sha256(probe.read_bytes()).hexdigest(),
          'physicalMemoryLimit': 2 << 30, 'virtualAddressLimit': 1 << 46,
          'keyspaceCreated': False, 'passed': False}
process = subprocess.Popen([str(probe), str(root), 'server'], env=env,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
api_dependencies = {}
def verify_native_writer(controller):
    controller.stdin.write(b'lease\n')
    controller.stdin.flush()
    with selectors.DefaultSelector() as selector:
        selector.register(controller.stdout, selectors.EVENT_READ)
        if not selector.select(5):
            raise RuntimeError('Native writer observation deadline')
    reply = json.loads(controller.stdout.readline(4097))
    if reply.get('writerLockHeldAfterParentClose') is not True:
        raise RuntimeError('Native service did not retain the inherited writer lease')
def repository_digest(path):
    digest = hashlib.sha256()
    entries, total = 0, 0
    for directory, directories, files in os.walk(path, followlinks=False):
        directories.sort()
        for name in sorted(directories + files):
            entry = Path(directory) / name
            info = entry.lstat()
            entries += 1
            if entries > 131072 or info.st_uid != os.geteuid() or not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
                raise RuntimeError('Unsupported retained repository fixture entry')
            digest.update(json.dumps([str(entry.relative_to(path)),
                                      'file' if stat.S_ISREG(info.st_mode) else 'directory',
                                      info.st_size if stat.S_ISREG(info.st_mode) else 0],
                                     separators=(',', ':')).encode() + b'\0')
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
                if info.st_nlink != 1 or total > 512 * 1024 * 1024:
                    raise RuntimeError('Retained repository fixture exceeds bounds')
                with entry.open('rb') as stream:
                    while block := stream.read(1024 * 1024):
                        digest.update(block)
    return digest.hexdigest()
def helper(mode, statement=None):
    directory = Path(tempfile.mkdtemp(prefix=f'scylla-{mode}-', dir=parent))
    directory.chmod(0o700)
    statements = {
        'ledger': 'SELECT JSON name, checksum, done, end_time FROM gitstore.gocqlx_migrate;',
        'lock-hold': "INSERT INTO gitstore.schema_migrations_lock (lock_key,holder,acquired_at) VALUES ('migration','gitstore-native-fixture',toTimestamp(now())) IF NOT EXISTS USING TTL 120;",
        'lock-release': "DELETE FROM gitstore.schema_migrations_lock WHERE lock_key='migration' IF holder='gitstore-native-fixture';",
    }
    statement = statements.get(mode, statement)
    command = ['/usr/bin/cqlsh', '127.0.0.1', '9042', '--connect-timeout', '5',
               '--request-timeout', '10', '-e', statement]
    helper_root = rootfs
    git_process = None
    if mode in ('migration', 'migration-cancel'):
        command = ['/app/gitctl', 'migrate', '--hosts=127.0.0.1:9042', '--keyspace=gitstore',
                   '--disable-shard-aware-port', '--timeout=60s']
        helper_root = api_image / 'rootfs'
    if mode == 'api-check':
        if 'bootstrap' not in api_dependencies:
            prepared = subprocess.run([str(probe), str(directory), 'prepare'], env=env,
                                      capture_output=True, timeout=20)
            if prepared.returncode:
                raise RuntimeError('Protected synthetic bootstrap preparation failed')
            api_dependencies['bootstrap'] = directory / 'bootstrap'
        bootstrap = api_dependencies['bootstrap']
        git_directory = Path(tempfile.mkdtemp(prefix='scylla-git-', dir=parent))
        git_directory.chmod(0o700)
        git_root = git_directory / 'rootfs'
        shutil.copytree(git_image / 'rootfs', git_root, symlinks=True)
        git_config = git_root / 'etc/gitstore'
        git_config.mkdir(mode=0o755, parents=True, exist_ok=True)
        (git_config / 'gitstore.toml').touch(mode=0o600, exist_ok=False)
        if 'repositories' not in api_dependencies:
            api_dependencies['repositories'] = git_directory / 'repositories'
            api_dependencies['repositories'].mkdir(mode=0o700)
        repositories = api_dependencies['repositories']
        if 'repositoryDigest' in api_dependencies and repository_digest(repositories) != api_dependencies['repositoryDigest']:
            raise RuntimeError('Retained repository changed before restart')
        (git_root / 'var/lib/gitstore/repositories').mkdir(mode=0o700, parents=True, exist_ok=True)
        git_spec = base_config(git_root, ['/app/git-service'])
        git_spec['mounts'].append({'destination': '/var/lib/gitstore/repositories', 'source': str(repositories)})
        prepare(git_directory, git_spec)
        for entry in (git_directory, directory):
            shared_bootstrap = entry / 'shared-bootstrap.json'
            shared_bootstrap.write_text(json.dumps({'path': str(bootstrap)}))
            shared_bootstrap.chmod(0o600)
        shared_namespace = git_directory / 'shared-namespace.json'
        shared_namespace.write_bytes((root / 'namespace.json').read_bytes())
        shared_namespace.chmod(0o600)
        record['apiGitFixture'] = str(git_directory)
        # Mount targets belong to this disposable execution copy. Never let the
        # runtime create them inside the verified reusable image cache.
        helper_root = directory / 'rootfs'
        shutil.copytree(api_image / 'rootfs', helper_root, symlinks=True)
        targets = helper_root / 'etc/gitstore'
        targets.mkdir(mode=0o755, parents=True, exist_ok=True)
        for name in ('gitstore.toml', 'owned.toml', 'policy.yaml', 'users.yaml'):
            target = targets / name
            if target.is_symlink():
                raise RuntimeError('Unexpected API mount target symlink')
            if not target.exists():
                target.touch(mode=0o600)
        command = ['/app/api']
    prepare(directory, base_config(helper_root, command))
    shared = directory / 'shared-namespace.json'
    shared.write_bytes((root / 'namespace.json').read_bytes())
    shared.chmod(0o600)
    try:
        if mode == 'api-check':
            git_process = subprocess.Popen([str(probe), str(git_directory), 'git-server'], env=env,
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if git_process:
            with selectors.DefaultSelector() as selector:
                selector.register(git_process.stdout, selectors.EVENT_READ)
                if not selector.select(30):
                    raise RuntimeError('Git native startup deadline')
            started_git = json.loads(git_process.stdout.readline(4097))
            if (started_git.get('nativeStarted') is not True or started_git.get('registeredPlan') is not True or
                    started_git.get('registeredWorker') is not True):
                raise RuntimeError('Git native startup did not report its registered listener')
            record['apiGitPid'] = started_git['pid']
        result = subprocess.run([str(probe), str(directory), mode], env=env, capture_output=True,
                                timeout=170 if mode == 'migration-cancel' else
                                110 if mode in ('api-check', 'migration') else 50)
        diagnostic = directory / 'controller.log'
        diagnostic.write_bytes(result.stderr[:16384])
        diagnostic.chmod(0o600)
        if mode in ('readiness', 'keyspace', 'migration'):
            lifecycle = json.loads((directory / 'helper-lifecycle.json').read_bytes())
            if (lifecycle.get('productionHelperActor') is not True or
                    lifecycle.get('deleted') is not True or lifecycle.get('mayExist') is not False or
                    lifecycle.get('exitObserved') is not True):
                raise RuntimeError('Helper cleanup is uncertain; retry and dependent startup refused')
            if result.returncode == 0 and lifecycle.get('outcome') != 0:
                raise RuntimeError('Helper success lacks verified native exit and deletion')
            record['productionHelperActorVerified'] = True
        if mode == 'migration-cancel':
            lifecycle = json.loads((directory / 'helper-lifecycle.json').read_bytes())
            cleanup = json.loads((directory / 'helper-cleanup.json').read_bytes())
            if (lifecycle.get('cancellationPreservedNative') is not True or
                    lifecycle.get('outcome') != -2 or lifecycle.get('mayExist') is not True or
                    lifecycle.get('exitObserved') is not False or
                    lifecycle.get('deleted') is not False or cleanup.get('observedExit') is not True or
                    cleanup.get('emptyCgroup') is not True or cleanup.get('deleted') is not True or
                    cleanup.get('cancelOutcome') != -2):
                raise RuntimeError('Unsafe helper cancellation or subsequent cleanup lost its observation boundary')
    finally:
        if git_process:
            if git_process.poll() is None:
                try:
                    git_process.stdin.write(b'stop\n')
                    git_process.stdin.flush()
                except (BrokenPipeError, OSError):
                    pass
            _, diagnostic_bytes = git_process.communicate(timeout=40)
            diagnostic = git_directory / 'controller.log'
            diagnostic.write_bytes(diagnostic_bytes[:16384])
            diagnostic.chmod(0o600)
            record['apiGitControllerExit'] = git_process.returncode
            if git_process.returncode:
                raise RuntimeError('Git dependency did not finish observed cleanup')
            observed = repository_digest(repositories)
            if 'repositoryDigest' in api_dependencies:
                if observed != api_dependencies['repositoryDigest']:
                    raise RuntimeError('Retained repository changed across API/Git restart')
                record['retainedRepositoryRestartVerified'] = True
            else:
                api_dependencies['repositoryDigest'] = observed
            record['retainedRepositoryDigest'] = observed
    return result, directory
try:
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(15):
            raise RuntimeError('Native datastore creation deadline')
    line = process.stdout.readline(4097)
    started = json.loads(line)
    if (started.get('nativeStarted') is not True or started.get('registeredPlan') is not True or
            started.get('registeredWorker') is not True):
        raise RuntimeError('Expected actual native process identity')
    record['pid'] = started['pid']
    record['registeredServicePlanVerified'] = True
    record['registeredWorkerDispatchVerified'] = True
    deadline = time.monotonic() + 120
    while True:
        if process.poll() is not None:
            raise RuntimeError('Datastore exited before CQL readiness')
        result, directory = helper('readiness', 'SELECT release_version FROM system.local;')
        record['lastReadinessFixture'] = str(directory)
        if result.returncode == 0:
            record['cqlReady'] = True
            verify_native_writer(process)
            record['nativeWriterLeaseVerified'] = True
            break
        if time.monotonic() >= deadline:
            raise RuntimeError('CQL readiness deadline')
        time.sleep(1)
    result, directory = helper('keyspace', "CREATE KEYSPACE IF NOT EXISTS gitstore WITH replication = {'class': 'NetworkTopologyStrategy', 'replication_factor': '1'} AND durable_writes = true;")
    record['keyspaceFixture'] = str(directory)
    if result.returncode:
        raise RuntimeError('Image-native keyspace preparation failed')
    record['keyspaceCreated'] = True
    if api_image:
        for attempt in ('migration', 'idempotentMigration'):
            result, directory = helper('migration')
            record[attempt + 'Fixture'] = str(directory)
            if result.returncode:
                raise RuntimeError('Pinned image-native migration helper failed: ' + attempt)
            record[attempt + 'Completed'] = True
    if api_image:
        def ledger():
            result, directory = helper('ledger')
            if result.returncode:
                raise RuntimeError('Read-only ledger inspection failed')
            rows = [json.loads(line.strip()) for line in (directory / 'output.log').read_text().splitlines() if line.strip().startswith('{')]
            targets = json.loads((Path(__file__).resolve().parents[2] / 'profiles/migration-targets.json').read_text())
            expected = {f['name']: (f['ledgerMd5'], f['statementCount']) for f in targets['targets'][0]['schema']['files']}
            if len(rows) != len(expected) or {r['name']: (r['checksum'], r['done']) for r in rows} != expected or any(not r['end_time'] or r['done'] <= 0 for r in rows):
                raise RuntimeError('Actual ledger does not match the pinned complete migration bundle')
            return sorted(rows, key=lambda row: row['name'])
        before = ledger()
        record['verifiedLedgerEntries'] = len(before)
        checked, directory = helper('api-check')
        record['apiCheckModeFixture'] = str(directory)
        if checked.returncode:
            raise RuntimeError('Pinned API did not reach registered readiness in schema check mode')
        if before != ledger():
            raise RuntimeError('API schema check mode changed the migration ledger')
        record['apiCheckModeVerified'] = True
        held, directory = helper('lock-hold')
        if held.returncode or 'True' not in (directory / 'output.log').read_text():
            raise RuntimeError('Could not acquire actual datastore migration lock')
        cancelled, directory = helper('migration-cancel')
        record['unsafeCancellationFixture'] = str(directory)
        if cancelled.returncode or before != ledger():
            raise RuntimeError('Dispatched migration cancellation did not retain native ownership and the ledger')
        record['unsafeHelperCancellationVerified'] = True
        blocked, directory = helper('migration')
        record['conflictingMigrationFixture'] = str(directory)
        if blocked.returncode == 0 or 'lock held by another instance' not in (directory / 'output.log').read_text():
            raise RuntimeError('Expected actual datastore lock rejection')
        if before != ledger():
            raise RuntimeError('Conflicting migration changed the ledger')
        released, directory = helper('lock-release')
        if released.returncode or 'True' not in (directory / 'output.log').read_text():
            raise RuntimeError('Actual datastore lock release failed')
        record['datastoreLockConflictVerified'] = True
    process.stdin.write(b'stop\n')
    process.stdin.flush()
    output, diagnostic = process.communicate(timeout=40)
    record['exitCode'] = process.returncode
    if process.returncode:
        raise RuntimeError('Datastore did not complete observed graceful cleanup')
    backup_manifest = None
    if args.backup_probe:
        if not args.restart:
            raise RuntimeError('Backup verification requires observed restart')
        # Native controller success above proves complete prior process/scope
        # termination. Copy only this private stopped fixture; never live data.
        paths = list(data.rglob('*'))
        if len(paths) > 131072 or any(p.is_symlink() or not (p.is_dir() or stat.S_ISREG(p.stat().st_mode)) for p in paths):
            raise RuntimeError('Stopped fixture contains unsupported backup entries')
        total = sum(p.stat().st_size for p in paths if p.is_file())
        if total > 512 * 1024 * 1024:
            raise RuntimeError('Stopped fixture exceeds the explicit backup experiment bound')
        backup = root / 'backup-payload'
        restored = root / 'restored-data'
        shutil.copytree(data, backup, symlinks=True)
        backup.chmod(0o700)
        backup_probe = args.backup_probe.resolve(strict=True)
        described = subprocess.run([str(backup_probe), str(backup)], env=env, capture_output=True, timeout=60)
        if described.returncode:
            raise RuntimeError('C backup payload description failed')
        backup_manifest = json.loads(described.stdout)
        manifest_path = root / 'backup-manifest.json'
        with manifest_path.open('x') as stream:
            os.fchmod(stream.fileno(), 0o600)
            json.dump(backup_manifest, stream)
        shutil.copytree(backup, restored, symlinks=True)
        restored.chmod(0o700)
        validated = subprocess.run([str(backup_probe), str(restored), str(manifest_path)], env=env, capture_output=True, timeout=60)
        if validated.returncode or json.loads(validated.stdout) != {'validated': True}:
            raise RuntimeError('C restore payload integrity/compatibility check failed')
        if len({data.stat().st_ino, backup.stat().st_ino, restored.stat().st_ino}) != 3:
            raise RuntimeError('Backup/restore directories must have independent identities')
        config['mounts'][-1]['source'] = str(restored)
        record['backupPayloadDigest'] = backup_manifest['treeDigest']
        record['backupLogicalBytes'] = backup_manifest['logicalBytes']
        record['backupProbeSha256'] = hashlib.sha256(backup_probe.read_bytes()).hexdigest()
        record['restartData'] = 'independent-restored-copy'
    if args.restart:
        if not api_image:
            raise RuntimeError('Restart verification requires the pinned API schema candidate')
        # Successful native controller exit proves prior PID exit, empty cgroup
        # and non-forced deletion. Reuse data only after that observed boundary.
        initial_root = root
        root = Path(tempfile.mkdtemp(prefix='scylla-restart-', dir=parent))
        root.chmod(0o700)
        record['restartFixture'] = str(root)
        prepare(root, config)
        process = subprocess.Popen([str(probe), str(root), 'server'], env=env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(15):
                raise RuntimeError('Restart native creation deadline')
        started = json.loads(process.stdout.readline(4097))
        if started.get('nativeStarted') is not True or started.get('registeredPlan') is not True:
            raise RuntimeError('Expected new observed restart identity')
        record['restartPid'] = started['pid']
        deadline = time.monotonic() + 120
        while True:
            if process.poll() is not None:
                raise RuntimeError('Retained datastore exited before CQL readiness')
            ready, directory = helper('readiness', 'SELECT release_version FROM system.local;')
            if ready.returncode == 0:
                verify_native_writer(process)
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Retained datastore readiness deadline')
            time.sleep(1)
        if before != ledger():
            raise RuntimeError('Retained schema ledger changed across complete stop/restart')
        if api_image:
            checked, directory = helper('api-check')
            record['restartApiCheckModeFixture'] = str(directory)
            if checked.returncode or before != ledger():
                raise RuntimeError('Restored datastore/API check-mode startup failed or changed the ledger')
        record['retainedLedgerRestartVerified'] = True
        if backup_manifest:
            record['backupRestoreVerified'] = True
        process.stdin.write(b'stop\n')
        process.stdin.flush()
        process.communicate(timeout=40)
        record['restartControllerExit'] = process.returncode
        if process.returncode:
            raise RuntimeError('Restarted datastore did not finish observed cleanup')
        # Record the restart fixture separately; keep the canonical initial
        # evidence alongside its retained data and first native identity.
        root = initial_root
    record['passed'] = True
except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
    record['failure'] = str(error)
finally:
    if process.poll() is None:
        try:
            process.stdin.write(b'stop\n')
            process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        # The controller uses the held workload pidfd and observes its scope;
        # never signal a guessed database PID or remove retained data here.
        try:
            process.communicate(timeout=40)
        except subprocess.TimeoutExpired:
            record['failure'] = 'Owned controller cleanup requires inspection; fixture retained'
    record['controllerExit'] = process.poll()
    if process.poll() is not None:
        try:
            _, diagnostic = process.communicate(timeout=1)
            if diagnostic:
                path = root / 'controller-diagnostic.log'
                with path.open('x') as stream:
                    os.fchmod(stream.fileno(), 0o600)
                    stream.write(diagnostic[:4096].decode(errors='replace'))
        except (OSError, ValueError, subprocess.TimeoutExpired):
            pass
    path = root / 'evidence.json'
    path.write_text(json.dumps(record, indent=2) + '\n')
    path.chmod(0o600)
    print(json.dumps(record), flush=True)
raise SystemExit(0 if record['passed'] else 1)
