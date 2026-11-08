#!/usr/bin/env python3
"""Prepare real Linux relink objects for review; never publish or assert compliance.

Corresponding patched sources and full notices remain separately required. This
tool consumes the actual production CMake link command, not a guessed library list.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'runtime/packaging'))
from formats import tar_bytes, write_new

SYSTEM = {'m', 'dl', 'pthread', 'rt', 'resolv', 'c', 'gcc', 'gcc_s'}
FLAGS = {'-Os', '-DNDEBUG', '-Wl,--gc-sections', '-Wl,-Bstatic,--start-group',
         '-Wl,--end-group,-Bdynamic', '-pthread'}


def ordinary(path):
    # Cargo may hardlink its static archive inside a selected build tree. Read
    # that trusted build input through a pinned descriptor and copy checked bytes
    # into a new ordinary kit file; package output never contains hardlinks.
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC), 'rb') as stream:
        info = os.fstat(stream.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid not in (0, os.geteuid())
                or info.st_mode & 0o022 or not 0 < info.st_size <= 1024 * 1024 * 1024):
            raise ValueError('unsafe or oversized linker input: '+str(path))
        data = stream.read()
        after = os.fstat(stream.fileno())
        if (info.st_size,info.st_mtime_ns,info.st_ctime_ns) != (after.st_size,after.st_mtime_ns,after.st_ctime_ns):
            raise ValueError('linker input changed during snapshot')
        return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    build, output = args.build_dir, args.output
    if (sys.platform != 'linux' or os.geteuid() == 0 or not build.is_absolute()
            or build.is_symlink() or not output.is_absolute() or output.exists()):
        parser.error('select non-root Linux, an ordinary production build and a new absolute output')
    cache = ordinary(build/'CMakeCache.txt').decode()
    for setting in ('GITSTORED_TESTING:BOOL=OFF', 'GITSTORED_STATIC_DISTRIBUTION:BOOL=ON'):
        if setting not in cache.splitlines():
            raise ValueError('relink inputs must come from the production private-static build')
    tokens = shlex.split(ordinary(build/'CMakeFiles/gitstored.dir/link.txt').decode())
    if not tokens or tokens.count('-o') != 1 or tokens[tokens.index('-o')+1] != 'gitstored':
        raise ValueError('unexpected production linker command')
    compiler = Path(tokens[0])
    if not compiler.is_absolute() or compiler.name not in ('cc','gcc','clang','clang-18'):
        raise ValueError('review the production compiler invocation before preparing objects')
    search = [Path(t[2:]) for t in tokens if t.startswith('-L')]
    payload, recorded, command = {}, {}, []
    def material(path):
        data = ordinary(path)
        digest = hashlib.sha256(data).hexdigest()
        name = 'objects/'+digest+'-'+path.name
        if not (data.startswith(b'!<arch>\n') or data.startswith(b'\x7fELF')):
            raise ValueError('expected an actual object or static archive')
        payload[name] = (data, 0o644)
        recorded[name] = dict(sha256=digest, bytes=len(data), input=str(path))
        return name
    skip = False
    for token in tokens[1:]:
        if skip:
            skip = False
            continue
        if token == '-o':
            skip = True
            continue
        if token.startswith('-L'):
            continue
        if token.startswith('-l') and token[2:] not in SYSTEM:
            selected = [directory/('lib'+token[2:]+'.a') for directory in search
                        if (directory/('lib'+token[2:]+'.a')).is_file()]
            if not selected:
                raise ValueError('no explicit static archive for '+token)
            command.append(material(selected[0]))
        elif token.endswith(('.a','.o')):
            path = Path(token)
            command.append(material(path if path.is_absolute() else build/path))
        elif token in FLAGS or token.startswith('-l') and token[2:] in SYSTEM:
            command.append(token)
        else:
            raise ValueError('unreviewed linker argument: '+token)
    if not any(name.endswith('main.c.o') for name in recorded) or not any(name.endswith('libgitstored_core.a') for name in recorded):
        raise ValueError('application object and core archive are required')
    script = '''#!/usr/bin/env python3
import argparse, hashlib, json, pathlib, subprocess
p = argparse.ArgumentParser(description="Relink the selected application objects; supply matching Linux C toolchain and libc development files")
p.add_argument("--cc", default="cc")
p.add_argument("--output", required=True, type=pathlib.Path)
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parent
manifest = json.loads((root/"relink.json").read_text())
if not a.output.is_absolute() or a.output.exists():
    p.error("select a new absolute executable output")
for name, record in manifest["materials"].items():
    path = root/name
    if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
        p.error("relink material digest mismatch; review deliberate replacements and update the local manifest before relinking")
subprocess.run([a.cc, *manifest["arguments"], "-o", str(a.output)], cwd=root, check=True)
'''
    payload['relink.py'] = (script.encode(), 0o755)
    payload['README.txt'] = (('UNQUALIFIED REVIEW MATERIAL; NOT A RELEASE.\n'
        'Contains actual production application objects and private native archives.\n'
        'Full corresponding patched sources and license notices must accompany a release.\n'
        'Requires the matching Linux architecture, C toolchain and libc development files.\n'
        'No glibc or OS loader is bundled. Ordinary host math/resolver/runtime libraries remain host inputs.\n'
        'Run: python3 relink.py --cc /usr/bin/cc --output /absolute/new/gitstored\n'
        'To replace a library, rebuild its archive from corresponding source with the recorded build recipe,\n'
        'replace its objects/ entry and update that entry in relink.json after reviewing the change.\n'
        'Do not install or publish unreviewed modified outputs. No compatibility or data preservation is guaranteed.\n').encode(), 0o644)
    manifest = dict(schemaVersion=1, qualification='unqualified relink review material',
        machine=platform.machine(), compiler=subprocess.check_output([str(compiler),'--version'],text=True).splitlines()[0],
        arguments=command, materials=recorded,
        originalSha256=hashlib.sha256(ordinary(build/'gitstored')).hexdigest())
    payload['relink.json'] = ((json.dumps(manifest,indent=2)+'\n').encode(), 0o644)
    output.mkdir(mode=0o700)
    for name,(data,mode) in payload.items():
        path = output/name
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
    rebuilt = output/'gitstored-relinked'
    subprocess.run([sys.executable,str(output/'relink.py'),'--cc',str(compiler),'--output',str(rebuilt)],check=True)
    subprocess.run([str(rebuilt),'--version'],env={'PATH':'/nonexistent'},check=True)
    subprocess.run([sys.executable,str(ROOT/'runtime/packaging/closure.py'),'--binary',str(rebuilt)],check=True)
    # The executable is a test output; publishable kit inputs contain only objects/instructions.
    archive = output/'UNQUALIFIED-relink-kit.tar.gz'
    write_new(archive,tar_bytes(payload))
    print(json.dumps(dict(kit=str(archive), sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        relinkedSha256=hashlib.sha256(rebuilt.read_bytes()).hexdigest(), materials=len(recorded),
        qualification='actual relink passed; corresponding sources/notices and review still required')))


if __name__ == '__main__':
    main()
