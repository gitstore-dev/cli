"""Bind generated source/relink subjects to reviewed inputs and production objects."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import shlex

from formats import tar_bytes, write_new
from inventory import relative, snapshot_file
from notices import read_file
from release_candidate import safe_name, DISCLAIMER
from source_materials import rlib_inventory


def validate_source_pin(entry,project):
    name=entry.get('dependencyPin')
    if name in ('jansson','sqlite'):
        pin=json.loads((project/'runtime/dependencies.lock.json').read_text())['dependencies'][name]
        sha=pin['sha256']
        source=pin['url']
    elif name in ('libcrun','libslirp'):
        path='runtime/experiments/libcrun/source-lock.json' if name=='libcrun' else 'runtime/experiments/network-library/source-lock.json'
        pin=json.loads((project/path).read_text())
        sha=pin['normalizedTarSha256'] if name=='libslirp' else pin['sha256']
        source=pin['url']
    elif name=='curl':
        pin=json.loads((project/'packaging/curl-source.json').read_text())
        dsc=next(filename for filename in pin['files'] if filename.endswith('.dsc'))
        sha=pin['files'][dsc]
        source='https://archive.ubuntu.com/ubuntu/pool/main/c/curl/'+dsc
    else:
        raise ValueError('compiled native input lacks a recognized committed source pin')
    if entry['sourceSha256']!=sha or entry['source']!=source or entry['name']!=name+'@'+pin['version']:
        raise ValueError('reviewed native source differs from the actual committed build pin')


def resolve_closure(definitions, native, inputs, *, sysroot, target, relink=None, build_roots=None, project=None):
    roots = {'native':native,**(build_roots or {})}
    result = []
    bound=set()
    for entry in definitions:
        row = {key:entry[key] for key in ('name','sha256','license','source','delivery')}
        if entry['delivery'] == 'system':
            result.append(row)
            continue
        if entry.get('toolchainInventory') is True:
            _,data = rlib_inventory(sysroot,target)
            actual = hashlib.sha256(data).hexdigest()
        elif 'buildArchive' in entry:
            if project is not None:
                validate_source_pin(entry,project)
            relative(entry['buildArchive'])
            read_file(inputs,dict(path=entry['sourceMaterial'],sha256=entry['sourceSha256']))
            root = roots.get(entry.get('buildRoot','native'))
            if root is None:
                raise ValueError('unknown reviewed native build root')
            actual = snapshot_file(root,entry['buildArchive'])['sha256']
            bound.add(str(root/entry['buildArchive']))
        elif 'systemArchive' in entry:
            if relink is None:
                raise ValueError('system archive identity requires the actual production linker inventory')
            matches = [value for name,value in relink['materials'].items()
                if Path(value['input']).name == entry['systemArchive']]
            if len(matches)!=1:
                raise ValueError('reviewed system archive is missing or ambiguous in the actual link')
            actual = matches[0]['sha256']
            bound.add(matches[0]['input'])
            read_file(inputs,dict(path=entry['sourceMaterial'],sha256=entry['sourceSha256']))
        else:
            raise ValueError('each non-system component requires actual archive or toolchain identity')
        if row['sha256'] != '@BUILD_SHA256@' and row['sha256'] != actual:
            raise ValueError('actual library input differs from the reviewed closure: '+row['name'])
        if row['sha256'] == '@BUILD_SHA256@' and 'buildArchive' not in entry:
            raise ValueError('only libraries compiled from reviewed sources may have generated archive hashes')
        row['sha256'] = actual
        result.append(row)
    if project is not None:
        application={str(native/name) for name in ('CMakeFiles/gitstored.dir/src/main.c.o','libgitstored_core.a','image-sdk/release/libgitstored_image.a')}
        if relink is not None:
            actual={value['input'] for value in relink['materials'].values()}
        else:
            tokens=shlex.split((native/'CMakeFiles/gitstored.dir/link.txt').read_text())
            actual={str(Path(token) if Path(token).is_absolute() else native/token) for token in tokens if token.endswith(('.a','.o'))}
            if any(token.startswith('-l') and token not in ('-lm','-lc','-ldl','-lpthread','-lSystem') for token in tokens):
                raise ValueError('unknown native library input requires a reviewed closure')
        if actual-application != bound:
            raise ValueError('reviewed native closure does not cover every actual linked archive')
    return result


def generate(manifest, materials, inputs, *, project, output, version, os_name, arch, relink_bytes=None):
    """Resolve declared generated subjects; never fetch already-published own assets."""
    if not output.is_absolute() or not output.is_relative_to(inputs) or output.exists():
        raise ValueError('select a new generated-material directory inside the private input root')
    value = copy.deepcopy(manifest)
    requested = {}
    for component in value['components']:
        for field in ('correspondingSource','relinkKit'):
            entry = component.get(field)
            if entry and 'generated' in entry:
                selector = entry['generated']
                if (selector,field) not in {('corresponding-source','correspondingSource'),('daemon-relink','relinkKit')}:
                    raise ValueError('unknown generated license material selector')
                name = entry['releaseAsset'].replace('@VERSION@',version).replace('@OS@',os_name).replace('@ARCH@',arch)
                safe_name(name)
                if selector in requested and requested[selector] != name:
                    raise ValueError('conflicting names for the same generated subject')
                if selector not in requested and name in requested.values():
                    raise ValueError('source and relink subjects must have distinct names')
                requested[selector] = name
    assets = {}
    if 'corresponding-source' in requested:
        payload = {}
        for material in materials:
            for entry in [material,*material.get('files',[])]:
                path = str(relative(entry['path']))
                if path.startswith('project/') or path in ('README.txt','reviewed-materials.json'):
                    raise ValueError('source input collides with generated instructions/project source')
                data = read_file(inputs,entry)
                if path in payload and payload[path][0] != data:
                    raise ValueError('conflicting corresponding source inputs')
                payload[path] = (data,0o644)
        revision = subprocess.check_output(['git','rev-parse','HEAD'],cwd=project,text=True).strip()
        archive = subprocess.check_output(['git','archive','--format=tar',revision],cwd=project)
        payload['project/gitstore-cli-'+revision+'.tar'] = (archive,0o644)
        payload['reviewed-materials.json'] = ((json.dumps(materials,indent=2)+'\n').encode(),0o644)
        payload['README.txt'] = ((DISCLAIMER+'\n\n'
            'Corresponding source inputs and complete attribution texts accompany this build.\n'
            'Unpack project/gitstore-cli-*.tar in a new source directory. The project\n'
            'docs/contributing/building.md and scripts/build-release-candidate.sh record the\n'
            'build. Rust uses the pinned rust-toolchain.toml and both Cargo.lock files.\n'
            'sources/rust contains exact Cargo registry archives and pinned Git sources.\n'
            'For distro libraries use dpkg-source -x on each .dsc with its adjacent original\n'
            'and Debian patch archives; upstream build recipes/patches are included there.\n'
            'Pinned native sources use runtime/dependencies.lock.json and the libcrun/slirp\n'
            'source locks. CMake applies the project Jansson/slirp patches and configuration.\n'
            'The three pinned curl source files and scripts/build-static-curl.py reproduce\n'
            'the private HTTP(S)-only curl build with its distribution security patches.\n'
            'On Linux use the separate matching relink asset to replace native library\n'
            'archives and relink the application objects. The host supplies glibc and its\n'
            'loader; neither is bundled. Runtime assets/images are separate inputs.\n').encode(),0o644)
        if sum(len(data) for data,_ in payload.values())>1024*1024*1024:
            raise ValueError('corresponding source set exceeds release bounds')
        data = tar_bytes(payload)
        assets[requested['corresponding-source']] = ('source',data)
    if 'daemon-relink' in requested:
        if os_name != 'linux' or not relink_bytes:
            raise ValueError('actual production Linux relink objects are required')
        assets[requested['daemon-relink']] = ('relink',relink_bytes)
    output.mkdir(mode=0o700)
    for name,(_,data) in assets.items():
        write_new(output/name,data)
    for component in value['components']:
        for field in ('correspondingSource','relinkKit'):
            entry = component.get(field)
            if entry and 'generated' in entry:
                name = requested[entry['generated']]
                data = assets[name][1]
                component[field] = dict(path=str((output/name).relative_to(inputs)),
                    sha256=hashlib.sha256(data).hexdigest(),releaseAsset=name)
    return value,assets
