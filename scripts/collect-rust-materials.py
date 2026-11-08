#!/usr/bin/env python3
"""Collect checksum-bound Rust source/full notices for release review, not approval.

Consumes explicit target-filtered Cargo metadata and matching lockfiles. Retains
embedded native notices. Unresolved components stay visible and block completion.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tomllib
from urllib.parse import urlsplit
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from source_materials import archive_members, expand, notice_members, production_packages, rlib_inventory, license_expression


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git_source(package):
    source = urlsplit(package['source'].removeprefix('git+'))
    if (source.scheme != 'https' or source.hostname != 'github.com' or source.username or
            not re.fullmatch(r'/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+',source.path) or
            not re.fullmatch('[0-9a-f]{40}',source.fragment)):
        raise ValueError('Git source requires an explicitly reviewed GitHub commit')
    repository = source.path.removesuffix('.git')
    url = 'https://codeload.github.com'+repository+'/tar.gz/'+source.fragment
    with urlopen(url,timeout=60) as response:
        data = response.read(128*1024*1024+1)
    if len(data)>128*1024*1024:
        raise ValueError('Git source archive exceeds bounds')
    prefix = repository.rsplit('/',1)[1]+'-'+source.fragment
    env = dict(PATH=os.environ.get('PATH','/usr/bin:/bin'),LANG='C',GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null')
    listing = subprocess.check_output(['git','-C',str(Path(package['manifest_path']).parent),
        'ls-tree','--full-tree','-r','-z',source.fragment],env=env)
    expected = {}
    for entry in listing.split(b'\0'):
        if not entry:
            continue
        header,name = entry.split(b'\t',1)
        mode,kind,sha = header.decode().split()
        if kind != 'blob':
            raise ValueError('Git submodule sources require separate reviewed material')
        expected[name.decode()] = (mode,sha)
    archive,members = archive_members(data)
    seen = set()
    try:
        for name,member in members.items():
            if member.isdir():
                continue
            if not name.startswith(prefix+'/') or name[len(prefix)+1:] not in expected:
                raise ValueError('Git archive contains an unexpected file')
            name = name[len(prefix)+1:]
            mode,sha = expected[name]
            if mode == '120000' and member.issym():
                payload = member.linkname.encode()
            elif mode in ('100644','100755') and member.isfile():
                payload = archive.extractfile(member).read()
            else:
                raise ValueError('Git archive object type mismatch')
            if hashlib.sha1(b'blob '+str(len(payload)).encode()+b'\0'+payload).hexdigest() != sha:
                raise ValueError('Git archive differs from the pinned Cargo Git object')
            seen.add(name)
    finally:
        archive.close()
    if seen != set(expected):
        raise ValueError('Git archive is missing committed source files')
    return data,url,prefix


def toolchain_materials(inputs):
    info = subprocess.check_output(['rustc','--version','--verbose'],cwd=ROOT,text=True)
    fields = dict(line.split(': ',1) for line in info.splitlines() if ': ' in line)
    expected = tomllib.loads((ROOT/'rust-toolchain.toml').read_text())['toolchain']['channel']
    if fields['release'] != expected or not re.fullmatch('[0-9a-f]{40}',fields['commit-hash']):
        raise ValueError('notice collection requires the exact pinned Rust toolchain')
    sysroot = Path(subprocess.check_output(['rustc','--print','sysroot'],cwd=ROOT,text=True).strip())
    identity = 'rust-standard-library@'+expected
    files = [sysroot/'share/doc/rust/COPYRIGHT-library.html',*sorted((sysroot/'share/doc/rust/licenses').glob('*'))]
    materials,licenses = [],[]
    for source in files:
        if source.is_symlink() or not source.is_file() or not 0 < source.stat().st_size <= 8*1024*1024:
            raise ValueError('full toolchain notice file is missing or unsafe')
        data = source.read_bytes()
        name = 'licenses/'+identity+'/'+source.name
        target = inputs/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o644)
        materials.append(dict(path=name,toolchainFile=str(source.relative_to(sysroot)),sha256=digest(data)))
        licenses.append(dict(path=name,sha256=digest(data)))
    if len(licenses)<2:
        raise ValueError('complete standard-library copyright and bundled full texts are required')
    libraries,inventory = rlib_inventory(sysroot,fields['host'])
    (inputs/'sources/toolchain').mkdir(parents=True,exist_ok=True)
    (inputs/'sources/toolchain/rlib-inventory.json').write_bytes(inventory)
    materials.append(dict(path='sources/toolchain/rlib-inventory.json',toolchainInventory=True,sha256=digest(inventory)))
    source = 'https://github.com/rust-lang/rust/tree/'+fields['commit-hash']+'/library'
    return dict(version=expected,revision=fields['commit-hash'],target=fields['host'],
        identityKind='SHA-256 of the canonical complete installed rlib-set inventory; conservative attribution coverage',
        inventory=dict(path='sources/toolchain/rlib-inventory.json',sha256=digest(inventory)),
        component=dict(id=identity,license='MIT OR Apache-2.0',source=source,licenses=licenses,
            bundledTerms='COPYRIGHT-library.html and all supplied full third-party texts'),
        closure=dict(name=identity,sha256=digest(inventory),license='MIT OR Apache-2.0',source=source,delivery='static'),
        materials=materials,libraries=libraries)


def collect(metadata_files,lock_files,output):
    if not output.is_absolute() or output.exists() or os.geteuid()==0:
        raise ValueError('select a non-root producer and new absolute review directory')
    packages,locked = {},{}
    for metadata_file,lock_file in zip(metadata_files,lock_files,strict=True):
        metadata = json.loads(metadata_file.read_text())
        for package in production_packages(metadata):
            if package['source'] is None:
                continue
            identity = package['name']+'@'+package['version']
            if identity in packages and packages[identity]['source'] != package['source']:
                raise ValueError('same notice identity has conflicting Cargo sources')
            packages[identity] = package
        for package in tomllib.loads(lock_file.read_text())['package']:
            if 'source' in package:
                locked[(package['name'],package['version'],package['source'])] = package.get('checksum')
    supplements = json.loads((ROOT/'packaging/notices/supplemental.json').read_text())
    output.mkdir(mode=0o700)
    inputs = output/'inputs'
    inputs.mkdir(mode=0o700)
    components,materials,unresolved = [],[],[]
    for identity,package in sorted(packages.items()):
        try:
            key = (package['name'],package['version'],package['source'])
            if key not in locked:
                raise ValueError('resolved package is absent from the selected lockfiles')
            if package['source'].startswith('registry+'):
                if package['source'] != 'registry+https://github.com/rust-lang/crates.io-index':
                    raise ValueError('non-default registry requires explicit review')
                crate = package['name']+'-'+package['version']+'.crate'
                directory = Path(package['manifest_path']).parent
                source = directory.parents[2]/'cache'/directory.parent.name/crate
                if source.is_symlink() or not 0 < source.stat().st_size <= 128*1024*1024:
                    raise ValueError('unsafe cached registry archive')
                data = source.read_bytes()
                if digest(data) != locked[key]:
                    raise ValueError('registry archive differs from Cargo.lock checksum')
                url = 'https://static.crates.io/crates/'+package['name']+'/'+crate
                prefix = package['name']+'-'+package['version']
            elif package['source'].startswith('git+'):
                data,url,prefix = git_source(package)
                crate = package['name']+'-'+package['version']+'-git.tar.gz'
            else:
                raise ValueError('unsupported Cargo source requires explicit review')
            explicit = package.get('license_file')
            if explicit and Path(explicit).is_absolute():
                explicit = str(Path(explicit).relative_to(Path(package['manifest_path']).parent))
            texts = notice_members(data,prefix,explicit)
            files = [dict(member=name,path='licenses/'+identity+'/'+name[len(prefix)+1:],sha256=digest(text))
                for name,text in sorted(texts.items())]
            material = dict(path='sources/rust/'+crate,url=url,sha256=digest(data),files=files)
            expand(inputs,material,data)
            materials.append(material)
            licenses = [dict(path=f['path'],sha256=f['sha256']) for f in files]
            for supplement in supplements.get(identity,[]):
                payload = (ROOT/supplement['path']).read_bytes()
                if digest(payload) != supplement['sha256']:
                    raise ValueError('committed supplemental license digest mismatch')
                name = 'licenses/'+identity+'/SUPPLEMENTAL-'+Path(supplement['path']).name
                target = inputs/name
                target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(payload)
                target.chmod(0o644)
                licenses.append(dict(path=name,sha256=digest(payload)))
                materials.append(dict(path=name,url=supplement['url'],sha256=digest(payload)))
            if not package['license'] or not licenses:
                raise ValueError('declared license identity and full source notices are required')
            components.append(dict(id=identity,license=license_expression(package['license']),declaredLicense=package['license'],source=url,licenses=licenses,
                cargoSource=package['source'],sourceArchive=dict(path=material['path'],sha256=material['sha256'])))
        except (ValueError,OSError,KeyError,tarfile.TarError) as error:
            unresolved.append(dict(id=identity,reason=str(error)))
    try:
        toolchain = toolchain_materials(inputs)
        (output/'toolchain.json').write_text(json.dumps(toolchain,indent=2)+'\n')
        materials.extend(toolchain['materials'])
    except (ValueError,OSError,KeyError) as error:
        unresolved.append(dict(id='rust-standard-library',reason=str(error)))
    (output/'proposed-notices.json').write_text(json.dumps(dict(components=components),indent=2)+'\n')
    (output/'materials.json').write_text(json.dumps(materials,indent=2)+'\n')
    (output/'cargo-packages.json').write_text(json.dumps(dict(packages=[dict(name=p['name'],version=p['version'],source=p['source'],
        sourceChecksum=locked[(p['name'],p['version'],p['source'])])
        for p in packages.values()]),indent=2)+'\n')
    report = dict(qualification='review material only; native closure/source obligations remain required',
        packages=len(packages),components=len(components),fullTexts=sum(len(c['licenses']) for c in components),unresolved=unresolved)
    (output/'review.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata',required=True,action='append',type=Path)
    parser.add_argument('--lockfile',required=True,action='append',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args = parser.parse_args()
    if len(args.metadata)!=len(args.lockfile):
        parser.error('select one matching lockfile for each target-filtered metadata record')
    report = collect(args.metadata,args.lockfile,args.output)
    print(json.dumps(report))
    raise SystemExit(1 if report['unresolved'] else 0)


if __name__=='__main__':
    main()
