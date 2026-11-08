"""Warm local command latency on exact selected bytes; no runtime startup."""
import math
import platform
import subprocess
import time


def measure(pair, root, samples=100):
    if not 10 <= samples <= 1000:
        raise ValueError('select between 10 and 1000 command samples')
    root.mkdir(mode=0o700)
    env = {'PATH': '/nonexistent', 'LANG': 'C', 'GIT_CONFIG_NOSYSTEM': '1'}
    for name, variable in [('home','HOME'), ('config','XDG_CONFIG_HOME'),
            ('state','XDG_STATE_HOME'), ('cache','XDG_CACHE_HOME'), ('runtime','XDG_RUNTIME_DIR')]:
        (root/name).mkdir(mode=0o700)
        env[variable] = str(root/name)
    config = root/'config/config.toml'
    config.touch(mode=0o600)
    env['GITSTORE_CONFIG'] = str(config)
    before = {str(p.relative_to(root)) for p in root.rglob('*')}
    result = dict(host=platform.platform(), machine=platform.machine(), samples=samples,
        clock='perf_counter_ns; wall time includes process startup',
        scope='warm local commands; excludes network, image readiness and daemon IPC inspection', commands={})
    for binary in ('gs','gitstored'):
        for flag in ('--help','--version'):
            timings = []
            for index in range(samples + 1):
                started = time.perf_counter_ns()
                reply = subprocess.run([str(pair/binary), flag], cwd=root, env=env,
                    capture_output=True, timeout=10, check=True)
                elapsed = (time.perf_counter_ns() - started) / 1_000_000
                if reply.stderr or not reply.stdout:
                    raise ValueError('measured command did not return clean local output')
                if index:  # One unrecorded warm-up per command.
                    timings.append(elapsed)
            ordered = sorted(timings)
            result['commands'][binary+' '+flag] = dict(samples=samples,
                p95Milliseconds=round(ordered[math.ceil(samples * .95)-1], 3),
                minimumMilliseconds=round(ordered[0], 3), maximumMilliseconds=round(ordered[-1], 3))
    if before != {str(p.relative_to(root)) for p in root.rglob('*')}:
        raise ValueError('measured help/version initialized user state')
    result['daemonInspect'] = 'not measured; requires separately selected live owned deployment'
    return result
