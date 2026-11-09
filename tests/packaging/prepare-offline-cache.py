#!/usr/bin/env python3
"""Copy explicitly selected verified core cache bytes into private OCI fixtures.

Developer-only preparation: no network, new images, hardlinks or product changes.
The subsequent native importer independently verifies every pinned input again.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import stat

ROOT=Path(__file__).resolve().parents[2]


def write_private(path,data):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'wb') as stream:
        stream.write(data)


def read(path,expected=None,size=None):
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
    with os.fdopen(fd,'rb') as stream:
        before=os.fstat(fd)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink!=1 or before.st_uid!=os.geteuid()
                or before.st_mode&0o022 or not 0<before.st_size<=512*1024*1024):
            raise ValueError('select ordinary owned immutable cache files')
        data=stream.read(512*1024*1024+1)
        after=os.fstat(fd)
        if (before.st_size,before.st_mtime_ns,before.st_ctime_ns)!=(after.st_size,after.st_mtime_ns,after.st_ctime_ns):
            raise ValueError('cache changed while copying')
    digest='sha256:'+hashlib.sha256(data).hexdigest()
    if expected and digest!=expected or size is not None and len(data)!=size:
        raise ValueError('cache bytes do not match the pinned descriptor')
    return data,digest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    if os.geteuid()==0 or not args.cache.is_absolute() or args.cache.is_symlink() or not args.output.is_absolute() or args.output.exists():
        parser.error('select an owned non-root cache and new absolute output')
    for root in (args.cache,args.output.parent):
        info=root.stat()
        if info.st_uid!=os.geteuid() or info.st_mode&0o077:
            parser.error('select owned private cache and output parent')
    arch={'aarch64':'arm64','x86_64':'amd64'}.get(platform.machine())
    if arch is None:
        parser.error('select a native Linux reference architecture')
    profile=json.loads((ROOT/'runtime/profiles/core.json').read_text())
    candidates={}
    for root in args.cache.iterdir():
        if root.is_symlink():
            raise ValueError('cache must not contain symlink roots')
        if root.is_dir():
            raw,digest=read(root/'manifest.json')
            candidates[digest]=(root,raw)
    prepared={}
    for image in profile['images']:
        pin=next(pin for pin in image['platforms'] if pin['architecture']==arch and pin['os']=='linux')
        root,raw=candidates[pin['digest']]
        manifest=json.loads(raw)
        if manifest['config']['digest']!=pin['configDigest']:
            raise ValueError('manifest config differs from the committed core profile')
        files={pin['digest'][7:]:raw}
        for descriptor,path in [(manifest['config'],root/'config.json'),
                *[(layer,root/'blobs'/('layer-'+str(index))) for index,layer in enumerate(manifest['layers'])]]:
            data,digest=read(path,descriptor['digest'],descriptor['size'])
            files[digest[7:]]=data
        prepared[image['component']]=(pin,manifest,files,len(raw))
    args.output.mkdir(mode=0o700)
    layouts={}
    for name,(pin,manifest,files,size) in prepared.items():
        root=args.output/name
        root.mkdir(mode=0o700)
        (root/'blobs').mkdir(mode=0o700)
        (root/'blobs/sha256').mkdir(mode=0o700)
        for digest,data in files.items():
            write_private(root/'blobs/sha256'/digest,data)
        write_private(root/'oci-layout',b'{"imageLayoutVersion":"1.0.0"}\n')
        write_private(root/'index.json',(json.dumps(dict(schemaVersion=2,manifests=[dict(
            mediaType=manifest['mediaType'],digest=pin['digest'],size=size)]))+'\n').encode())
        layouts[name]=str(root)
    write_private(args.output/'layouts.json',(json.dumps(layouts,indent=2)+'\n').encode())
    print(json.dumps(dict(layouts=layouts,scope='copied verified selected core cache; importer acceptance still required')))


if __name__=='__main__':
    main()
