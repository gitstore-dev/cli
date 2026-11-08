#!/usr/bin/env python3
"""Build a local-only next-alpha pair for package replacement tests.

Not the release producer. Copies Rust source, changes only fixture version fields,
and relinks a copied production C archive with a freshly compiled version object.
The selected native build and original source files are never modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
from versions import validate_alpha_proposal
from stage import reported_version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--cargo-cache', type=Path, help='Optional owned disposable fixture cache; never the original production target')
    args = parser.parse_args()
    if os.geteuid() == 0 or sys.platform not in ('linux','darwin'):
        parser.error('select a non-root Linux or macOS fixture producer')
    if not args.output.is_absolute() or args.output.exists() or args.output.is_symlink() or not args.native_build.is_absolute():
        parser.error('select absolute existing native build and new output directories')
    old = (ROOT / 'version.txt').read_text().strip()
    validate_alpha_proposal(old, args.version)
    if 'GITSTORED_TESTING:BOOL=OFF' not in (args.native_build / 'CMakeCache.txt').read_text():
        raise ValueError('select a production build without test facilities')
    references = [p for p in (ROOT / 'daemon/src').glob('*.c')
                  if not p.name.startswith('.') and b'GITSTORED_VERSION' in p.read_bytes()]
    if references != [ROOT / 'daemon/src/core.c']:
        raise ValueError('version definitions changed; use a full isolated native rebuild')
    args.output.mkdir(mode=0o700)
    source = args.output / 'source'
    source.mkdir()
    for name in ('Cargo.toml','Cargo.lock','rust-toolchain.toml','version.txt'):
        shutil.copyfile(ROOT / name, source / name)
    shutil.copytree(ROOT / 'crates/gs', source / 'crates/gs')
    cargo = source / 'Cargo.toml'
    text = cargo.read_text()
    previous = f'version = "{old}"'
    if text.count(previous) != 1:
        raise ValueError('workspace version field is ambiguous')
    cargo.write_text(text.replace(previous, f'version = "{args.version}"'))
    lock = source / 'Cargo.lock'
    text, count = re.subn(r'(\[\[package\]\]\nname = "gs"\n(?:#[^\n]*\n)*version = ")' + re.escape(old) + '"',
        lambda match: match[1] + args.version + '"', lock.read_text())
    if count != 1:
        raise ValueError('locked application version is ambiguous')
    lock.write_text(text)
    (source / 'version.txt').write_text(args.version + '\n')
    target = args.cargo_cache or args.output / 'target'
    if args.cargo_cache is not None:
        cache = args.cargo_cache.resolve(strict=True)
        if (not args.cargo_cache.is_absolute() or args.cargo_cache.is_symlink() or cache == (ROOT/'target').resolve() or
                cache.stat().st_uid != os.geteuid() or cache.parent.stat().st_mode & 0o077):
            raise ValueError('select a cache inside an owned private disposable version-fixture root')
        target = cache
    env = dict(os.environ, LIBZ_SYS_STATIC='1', CARGO_TARGET_DIR=str(target), CARGO_BUILD_JOBS='2')
    if sys.platform == 'darwin':
        env['MACOSX_DEPLOYMENT_TARGET'] = '15.0'
    subprocess.run(['cargo','build','--release','--locked','--offline','-p','gs'], cwd=source, env=env, check=True)
    pair = args.output / 'bin'
    pair.mkdir()
    shutil.copyfile(target / 'release/gs', pair / 'gs')
    (pair / 'gs').chmod(0o755)
    # The original production cache and immutable installed copies remain unchanged.
    # An explicitly selected disposable fixture cache may be reused across builds.
    flags = (args.native_build / 'CMakeFiles/gitstored_core.dir/flags.make').read_text()
    variables = {}
    for name in ('C_DEFINES','C_INCLUDES','C_FLAGS'):
        value = re.search(r'^' + name + r' = (.*)$', flags, re.M)
        if value is None:
            raise ValueError('missing production compiler flags')
        variables[name] = shlex.split(value[1])
    defines = [f'-DGITSTORED_VERSION="{args.version}"' if value.startswith('-DGITSTORED_VERSION=') else value
               for value in variables['C_DEFINES']]
    if any('GITSTORED_TESTING' in value for value in defines):
        raise ValueError('test facilities are forbidden in the replacement fixture')
    obj = args.output / 'core.c.o'
    compiler = shlex.split((args.native_build / 'CMakeFiles/gitstored.dir/link.txt').read_text())[0]
    subprocess.run([compiler, *defines, *variables['C_INCLUDES'], *variables['C_FLAGS'], '-c',
        str(ROOT / 'daemon/src/core.c'), '-o', str(obj)], cwd=args.native_build, check=True)
    archive = args.output / 'libgitstored_core.a'
    shutil.copyfile(args.native_build / 'libgitstored_core.a', archive)
    members = subprocess.check_output(['ar','t',str(archive)], text=True).splitlines()
    if members.count('core.c.o') != 1:
        raise ValueError('production archive version member is ambiguous')
    subprocess.run(['ar','r',str(archive),str(obj)], check=True)
    link = shlex.split((args.native_build / 'CMakeFiles/gitstored.dir/link.txt').read_text())
    if link.count('libgitstored_core.a') != 1:
        raise ValueError('production linker version archive is ambiguous')
    link[link.index('libgitstored_core.a')] = str(archive)
    link[link.index('-o') + 1] = str(pair / 'gitstored')
    subprocess.run(link, cwd=args.native_build, check=True)
    for name in ('gs','gitstored'):
        if reported_version(pair / name) != args.version:
            raise ValueError('replacement executable identity mismatch')
        subprocess.run([sys.executable,str(ROOT / 'runtime/packaging/closure.py'),'--binary',str(pair / name)],check=True)
    receipt = dict(qualification='UNQUALIFIED next-alpha package version fixture; never publish',
        version=args.version, sourceVersion=old,
        sha256={name:hashlib.sha256((pair / name).read_bytes()).hexdigest() for name in ('gs','gitstored')})
    (args.output / 'UNQUALIFIED.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
