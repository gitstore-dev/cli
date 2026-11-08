#!/usr/bin/env python3
"""Complete a per-host Rust review packet with actual native source/full notices.

Produces proposed committed-input files, not a legal approval or release claim.
Ubuntu sources are authenticated through the fixture's signed apt source index.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from inventory import relative
from notices import read_file
from source_materials import archive_members, expand, normalize_source, notice_members, selected_texts, select_text

SYSTEM = {
    'libsystemd.a':('libsystemd-dev','LGPL-2.1-or-later'),
    'libseccomp.a':('libseccomp-dev','LGPL-2.1-or-later'),
    'libcap.a':('libcap-dev','BSD-3-Clause OR GPL-2.0-only'),
    'libjson-c.a':('libjson-c-dev','MIT'),
    'libglib-2.0.a':('libglib2.0-dev','LGPL-2.1-or-later'),
    'libpcre2-8.a':('libpcre2-dev','BSD-3-Clause'),
    'libcrypt.a':('libcrypt-dev','LGPL-2.1-or-later'),
    'libssl.a':('libssl-dev','Apache-2.0'),
    'libcrypto.a':('libssl-dev','Apache-2.0'),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def download(url):
    if not url.startswith('https://'):
        raise ValueError('source material must use public HTTPS')
    with urlopen(url,timeout=60) as response:
        if not response.url.startswith('https://'):
            raise ValueError('source redirect left HTTPS')
        data = response.read(512*1024*1024+1)
    if len(data)>512*1024*1024:
        raise ValueError('source material exceeds bounds')
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rust-review',required=True,type=Path)
    p.add_argument('--build-dir',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--relink-json',type=Path)
    p.add_argument('--distro-sources',type=Path)
    p.add_argument('--apt-sources',type=Path,help='Explicit deb-src file used for signed source-index queries')
    args = p.parse_args()
    if os.geteuid()==0 or not args.output.is_absolute() or args.output.exists() or not args.build_dir.is_absolute():
        p.error('select a non-root producer, explicit production native build and new absolute output')
    review = json.loads((args.rust_review/'review.json').read_text())
    if review['unresolved']:
        raise ValueError('resolve Rust material gaps before native review')
    cache = (args.build_dir/'CMakeCache.txt').read_text()
    if 'GITSTORED_TESTING:BOOL=OFF' not in cache.splitlines():
        raise ValueError('native material review requires an actual production build')
    linux = sys.platform=='linux'
    if linux and (args.relink_json is None or args.distro_sources is None):
        p.error('select actual Linux relink inventory and a private distro source cache')
    args.output.mkdir(mode=0o700)
    inputs = args.output/'inputs'
    inputs.mkdir(mode=0o700)
    components = json.loads((args.rust_review/'proposed-notices.json').read_text())['components']
    materials = json.loads((args.rust_review/'materials.json').read_text())
    for entry in materials:
        expand(inputs,entry,read_file(args.rust_review/'inputs',entry))
    toolchain = json.loads((args.rust_review/'toolchain.json').read_text())
    components.append(toolchain['component'])
    closure = [{**toolchain['closure'],'toolchainInventory':True}]
    unresolved = []
    source_cache = args.output/'uri-probe'
    source_cache.mkdir(mode=0o700)
    def add_source(path,url,expected=None,normalization=None,data=None):
        relative(path)
        payload = normalize_source(download(url),normalization) if data is None else data
        if expected and digest(payload)!=expected:
            raise ValueError('pinned native source digest mismatch: '+path)
        entry = dict(path=path,url=url,sha256=digest(payload),files=[])
        if normalization:
            entry['normalization']=normalization
        materials.append(entry)
        return entry,payload
    def bind_texts(identity,entry,payload,texts):
        licenses = []
        for index,(member,selector,data) in enumerate(texts):
            name = 'licenses/'+identity+'/LICENSE-'+str(index)+'.txt'
            file = dict(member=member,path=name,sha256=digest(data))
            if selector:
                file['selector']=selector
            entry['files'].append(file)
            licenses.append(dict(path=name,sha256=file['sha256']))
        expand(inputs,entry,payload)
        return licenses
    lock = json.loads((ROOT/'runtime/dependencies.lock.json').read_text())['dependencies']
    for name,archive in [('jansson','_deps/jansson-build/lib/libjansson.a'),('sqlite','libgitstored_sqlite.a')]:
        pin = lock[name]
        identity = name+'@'+pin['version']
        entry,data = add_source('sources/native/'+pin['archive'],pin['url'],pin['sha256'])
        if name=='sqlite':
            member='sqlite-amalgamation-'+pin['archive'].split('-')[-1].removesuffix('.zip')+'/sqlite3.h'
            header=select_text(selected_texts(data,[member])[member],'first-comment')
            if b'The author disclaims copyright' not in header or b'blessing' not in header:
                raise ValueError('pinned SQLite public-domain header changed')
            texts=[(member,'first-comment',header)]
        else:
            texts=[(member,None,text) for member,text in sorted(notice_members(data,'').items())]
        licenses=bind_texts(identity,entry,data,texts)
        components.append(dict(id=identity,license=pin['license'],source=pin['url'],licenses=licenses))
        closure.append(dict(name=identity,license=pin['license'],source=pin['url'],delivery='static',
            sha256='@BUILD_SHA256@',dependencyPin=name,buildArchive=archive,sourceMaterial=entry['path'],sourceSha256=entry['sha256']))
    if linux:
        relink=json.loads(args.relink_json.read_text())
        archives={Path(value['input']).name:value for value in relink['materials'].values()}
        system_sources={}
        def distro_source(name,version=None):
            key=name+'='+version if version else name
            env=dict(PATH='/usr/bin:/bin',LANG='C',HOME=str(args.output))
            command=['apt-get']
            if args.apt_sources:
                if not args.apt_sources.is_absolute():
                    raise ValueError('select an absolute source-list file')
                command+=['-o','Dir::Etc::sourcelist='+str(args.apt_sources),'-o','Dir::Etc::sourceparts=-']
            result=subprocess.run([*command,'--print-uris','source','--download-only',key],cwd=source_cache,
                env=env,stdin=subprocess.DEVNULL,capture_output=True,check=True,timeout=60)
            records=[]
            for url,filename,size,algorithm,checksum in re.findall(rb"^'([^']+)' (\S+) (\d+) (SHA256|SHA512):([a-f0-9]+)$",result.stdout,re.M):
                filename=filename.decode()
                relative(filename)
                if '/' in filename:
                    raise ValueError('source index filename must be ordinary')
                url=url.decode().replace('http://','https://',1)
                cached=args.distro_sources/name/filename
                data=cached.read_bytes() if cached.is_file() else download(url)
                if len(data)!=int(size) or hashlib.new(algorithm.decode().lower(),data).hexdigest()!=checksum.decode():
                    raise ValueError('distro source differs from authenticated source index')
                entry,payload=add_source('sources/distro/'+name+'/'+filename,url,data=data)
                records.append((entry,payload))
            if not records or not any(e['path'].endswith('.dsc') for e,_ in records):
                raise ValueError('complete authenticated distro source set is missing')
            return records
        common_records=distro_source('base-files')
        common=[]
        for entry,data in common_records:
            if '.tar.' not in entry['path']:
                expand(inputs,entry,data)
                continue
            archive,members=archive_members(data)
            found={}
            try:
                for name,member in members.items():
                    if member.isfile() and 0<member.size<=4*1024*1024:
                        text=archive.extractfile(member).read()
                        found[digest(text)]=(name,text)
            finally:
                archive.close()
            for path in sorted(Path('/usr/share/common-licenses').iterdir()):
                text=path.read_bytes()
                if digest(text) not in found:
                    raise ValueError('installed common license is absent from the authenticated base-files source: '+path.name)
                member,_=found[digest(text)]
                name='licenses/common/'+path.name
                entry['files'].append(dict(member=member,path=name,sha256=digest(text)))
                common.append(dict(path=name,sha256=digest(text)))
            expand(inputs,entry,data)
        if not common:
            raise ValueError('complete referenced common license texts are required')
        for archive,(package,license_id) in SYSTEM.items():
            if archive not in archives:
                raise ValueError('native archive is missing from the actual link: '+archive)
            fmt=chr(36)+'{source:Package}\t'+chr(36)+'{source:Version}\n'
            source,version=subprocess.check_output(['dpkg-query','-W','-f',fmt,package],text=True).strip().split('\t')
            if source not in system_sources:
                records=distro_source(source,version)
                identity='distro-'+source+'@'+version.replace(':','_')
                copyright_data=(Path('/usr/share/doc')/package/'copyright').read_bytes()
                licenses=[]
                xcrypt_texts=None
                if source=='libxcrypt':
                    original=next(data for entry,data in records if '.orig.tar.' in entry['path'])
                    debian=next(data for entry,data in records if '.debian.tar.' in entry['path'])
                    template=selected_texts(debian,['debian/copyright.in'])['debian/copyright.in']
                    named=notice_members(original,'')
                    licensing=next(text for member,text in named.items() if member.endswith('/LICENSING'))
                    if template+licensing!=copyright_data:
                        raise ValueError('generated libxcrypt copyright differs from its complete source inputs')
                    xcrypt_texts=[(member,None,text) for member,text in sorted(named.items())]
                    seen={digest(text) for _,_,text in xcrypt_texts}
                    upstream,members=archive_members(original)
                    try:
                        for member_name,member in members.items():
                            if '/lib/' in member_name and member_name.endswith(('.c','.h')) and member.isfile():
                                data=upstream.extractfile(member).read()
                                if data.startswith(b'/*'):
                                    text=select_text(data,'first-comment')
                                    if digest(text) not in seen:
                                        seen.add(digest(text))
                                        xcrypt_texts.append((member_name,'first-comment',text))
                    finally:
                        upstream.close()
                for entry,data in records:
                    if xcrypt_texts is not None:
                        if '.orig.tar.' in entry['path']:
                            licenses+=bind_texts(identity,entry,data,xcrypt_texts)
                        elif '.debian.tar.' in entry['path']:
                            licenses+=bind_texts(identity+'-debian',entry,data,[('debian/copyright.in',None,template)])
                        else:
                            expand(inputs,entry,data)
                        continue
                    if '.debian.tar.' in entry['path'] or entry['path'].endswith('.diff.gz'):
                        if entry['path'].endswith('.diff.gz'):
                            entry['format']='debian-diff'
                        texts=selected_texts(data,['debian/copyright'],entry.get('format'))
                        if texts['debian/copyright']!=copyright_data:
                            raise ValueError('installed copyright differs from selected corresponding source')
                        licenses=bind_texts(identity,entry,data,[('debian/copyright',None,copyright_data)])
                    else:
                        expand(inputs,entry,data)
                if not licenses:
                    raise ValueError('distro source lacks its complete copyright file')
                system_sources[source]=(records,licenses)
            records,licenses=system_sources[source]
            dsc=next(entry for entry,_ in records if entry['path'].endswith('.dsc'))
            identity=archive.removesuffix('.a')+'@'+version.replace(':','_')
            components.append(dict(id=identity,license=license_id,source=dsc['url'],licenses=licenses+common))
            closure.append(dict(name=identity,sha256=archives[archive]['sha256'],license=license_id,source=dsc['url'],
                delivery='static',systemArchive=archive,sourceMaterial=dsc['path'],sourceSha256=dsc['sha256']))
        for name,path,archive,license_id in [('libcrun','runtime/experiments/libcrun/source-lock.json',
                '_deps/libcrun-build/.libs/libcrun.a','LGPL-2.1-or-later'),
                ('libslirp','runtime/experiments/network-library/source-lock.json','_deps/libslirp-build/libslirp.a','BSD-3-Clause')]:
            pin=json.loads((ROOT/path).read_text())
            normalized=name=='libslirp'
            entry,data=add_source('sources/native/'+name+'-'+pin['version']+('.tar' if normalized else '.tar.gz'),
                pin['url'],pin['normalizedTarSha256'] if normalized else pin['sha256'],
                'libslirp-gitiles' if normalized else None)
            identity=name+'@'+pin['version']
            texts=[(member,None,text) for member,text in sorted(notice_members(data,'').items())]
            licenses=bind_texts(identity,entry,data,texts)
            if not licenses:
                raise ValueError('pinned native source has no full license texts')
            components.append(dict(id=identity,license=license_id,source=pin['url'],licenses=licenses))
            closure.append(dict(name=identity,sha256='@BUILD_SHA256@',license=license_id,source=pin['url'],delivery='static',
                dependencyPin=name,buildArchive=archive,sourceMaterial=entry['path'],sourceSha256=entry['sha256']))
        curl=json.loads((ROOT/'packaging/curl-source.json').read_text())
        licenses=[]
        for filename,sha in curl['files'].items():
            url='https://archive.ubuntu.com/ubuntu/pool/main/c/curl/'+filename
            entry,data=add_source('sources/native/'+filename,url,sha)
            if filename.endswith('.orig.tar.gz'):
                texts=[(member,None,text) for member,text in sorted(notice_members(data,'').items())]
                licenses=bind_texts('curl@'+curl['version'],entry,data,texts)
            else:
                expand(inputs,entry,data)
        if not licenses:
            raise ValueError('pinned curl source lacks full notices')
        dsc=next(e for e in materials if e['path'].endswith('.dsc') and '/curl_' in e['path'])
        components.append(dict(id='curl@'+curl['version'],license='curl',source=dsc['url'],licenses=licenses+common))
        closure.append(dict(name='curl@'+curl['version'],sha256='@BUILD_SHA256@',license='curl',source=dsc['url'],delivery='static',
            dependencyPin='curl',buildArchive='lib/libgitstored_curl.a',buildRoot='static-curl',sourceMaterial=dsc['path'],sourceSha256=dsc['sha256']))
    for component in components:
        component['correspondingSource']=dict(generated='corresponding-source',releaseAsset='gitstore-source-@VERSION@-@OS@-@ARCH@.tar.gz')
        if 'LGPL' in component['license']:
            component['relinkKit']=dict(generated='daemon-relink',releaseAsset='gitstore-relink-@VERSION@-@OS@-@ARCH@.tar.gz')
    for filename,value in [('notices.json',dict(components=components)),('closure.json',closure),('materials.json',materials)]:
        (args.output/filename).write_text(json.dumps(value,indent=2)+'\n')
    report=dict(qualification='proposed review inputs, not a qualified release',components=len(components),
        materials=len(materials),fullTexts=sum(len(c['licenses']) for c in components),unresolved=unresolved)
    (args.output/'review.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':
    main()
