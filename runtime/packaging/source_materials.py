"""Select production Cargo edges and extract only reviewed source-archive notices."""
import hashlib
import io
import json
from pathlib import PurePosixPath
import re
import tarfile
import stat
import zipfile
import importlib.util
import gzip
from pathlib import Path

from formats import write_new
from inventory import relative


def license_expression(value):
    # Cargo's historical slash syntax denotes the dual-license alternatives.
    # Normalize only the observed, reviewed MIT/Apache forms; retain the original
    # declaration in the component record and all full texts in the package.
    aliases={'MIT/Apache-2.0':'MIT OR Apache-2.0','Apache-2.0/MIT':'Apache-2.0 OR MIT',
        'Apache-2.0 / MIT':'Apache-2.0 OR MIT'}
    result=aliases.get(value,value)
    if not result or '/' in result:
        raise ValueError('license expression requires explicit review')
    return result


def normalize_source(data,kind=None):
    if kind is None:
        return data
    if kind!='libslirp-gitiles':
        raise ValueError('unknown source normalization')
    path = Path(__file__).resolve().parents[2]/'runtime/experiments/network-library/fetch-source.py'
    spec = importlib.util.spec_from_file_location('release_slirp_source',path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if len(data)>module.MAX_ARCHIVE:
        raise ValueError('libslirp source exceeds its pinned normalization bounds')
    return module.normalized_archive(data)


def rlib_inventory(sysroot,target):
    libraries = []
    for source in sorted((sysroot/'lib/rustlib'/target/'lib').glob('*.rlib')):
        if source.is_symlink() or not 0 < source.stat().st_size <= 128*1024*1024:
            raise ValueError('unsafe standard-library archive')
        libraries.append(dict(path=source.name,bytes=source.stat().st_size,
            sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
    if not libraries:
        raise ValueError('selected standard-library archive inventory is empty')
    return libraries,(json.dumps(libraries,sort_keys=True,separators=(',',':'))+'\n').encode()


def production_packages(metadata):
    if metadata.get('resolve') is None:
        raise ValueError('resolved target-filtered Cargo metadata is required')
    nodes = {node['id']: node for node in metadata['resolve']['nodes']}
    pending = list(metadata['workspace_members'])
    selected = set()
    while pending:
        identity = pending.pop()
        if identity in selected:
            continue
        selected.add(identity)
        for dependency in nodes[identity]['deps']:
            kinds = dependency['dep_kinds']
            if not kinds or any(kind['kind'] != 'dev' for kind in kinds):
                pending.append(dependency['pkg'])
    return [package for package in metadata['packages'] if package['id'] in selected]


def locked_packages(metadata,lock):
    locked={(p['name'],p['version'],p.get('source')):p.get('checksum') for p in lock['package']}
    result=[]
    for package in production_packages(metadata):
        key=(package['name'],package['version'],package['source'])
        if package['source'] and key not in locked:
            raise ValueError('resolved source is absent from its production lockfile')
        result.append({**package,'sourceChecksum':locked.get(key)})
    return result


def archive_members(payload):
    archive = tarfile.open(fileobj=io.BytesIO(payload),mode='r:*')
    try:
        members = archive.getmembers()
        if len(members) > 50000 or sum(m.size for m in members) > 1024 * 1024 * 1024:
            raise ValueError('source archive exceeds review bounds')
        names = {}
        for member in members:
            name = member.name.rstrip('/')
            while name.startswith('./'):
                name = name[2:]
            if name in ('','.') and member.isdir():
                continue
            relative(name)
            if name in names:
                raise ValueError('duplicate source archive member')
            names[name] = member
        return archive,names
    except BaseException:
        archive.close()
        raise


def debian_added_texts(payload,names):
    with gzip.GzipFile(fileobj=io.BytesIO(payload)) as compressed:
        data=compressed.read(4*1024*1024+1)
    if len(data)>4*1024*1024:
        raise ValueError('Debian source patch exceeds bounds')
    lines=data.splitlines(keepends=True)
    result={}
    for index,line in enumerate(lines):
        if not line.startswith(b'+++ '):
            continue
        name=line[4:].split(b'\t',1)[0].split(b' ',1)[0].decode().strip()
        if '/' not in name:
            continue
        name=name.split('/',1)[1]
        if name not in names:
            continue
        relative(name)
        if name in result or index+1>=len(lines):
            raise ValueError('duplicate or incomplete added Debian source file')
        hunk=re.fullmatch(rb'@@ -0,0 \+1,(\d+) @@[^\n]*\n',lines[index+1])
        if not hunk:
            raise ValueError('notice must be a complete newly added file, not a partial source patch')
        count=int(hunk[1])
        added=lines[index+2:index+2+count]
        if len(added)!=count or not all(line.startswith(b'+') for line in added):
            raise ValueError('incomplete added Debian notice')
        text=b''.join(line[1:] for line in added)
        if index+2+count<len(lines) and lines[index+2+count].startswith(b'\\ No newline at end of file'):
            text=text.removesuffix(b'\n')
        result[name]=text
    if set(result)!=set(names):
        raise ValueError('complete requested Debian source notices are missing')
    return result


def selected_texts(payload,names,format=None):
    if format=='debian-diff':
        return debian_added_texts(payload,names)
    if format is not None:
        raise ValueError('unknown source archive format')
    result = {}
    if zipfile.is_zipfile(io.BytesIO(payload)):
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            members = archive.infolist()
            if len(members)>50000 or sum(m.file_size for m in members)>1024*1024*1024:
                raise ValueError('source ZIP exceeds review bounds')
            indexed = {}
            for member in members:
                name = member.filename.rstrip('/')
                relative(name)
                if name in indexed:
                    raise ValueError('duplicate source ZIP member')
                indexed[name] = member
            for name in names:
                member = indexed.get(name)
                if (not member or member.is_dir() or stat.S_ISLNK(member.external_attr>>16)
                        or not 0<member.file_size<=4*1024*1024):
                    raise ValueError('reviewed ZIP notice must be an ordinary bounded source file')
                result[name] = archive.read(member)
    else:
        archive,members = archive_members(payload)
        try:
            for name in names:
                member = members.get(name)
                if not member or not member.isfile() or not 0<member.size<=4*1024*1024:
                    raise ValueError('reviewed notice must be an ordinary bounded source file')
                result[name] = archive.extractfile(member).read()
        finally:
            archive.close()
    return result


def select_text(data,selector=None):
    if selector is None:
        return data
    if selector=='first-comment' and data.startswith(b'/*') and b'*/' in data:
        return data[:data.index(b'*/')+2]+b'\n'
    raise ValueError('unsupported or missing reviewed source comment selector')


def notice_members(payload, prefix, explicit=None):
    """Candidates for review, including bundled native licenses; never infer grants."""
    if prefix:
        relative(prefix)
    archive, members = archive_members(payload)
    result = {}
    try:
        for name, member in members.items():
            if name.rstrip('/') == prefix and member.isdir():
                continue
            if prefix and not name.startswith(prefix+'/'):
                raise ValueError('source member is outside the expected crate root')
            path = PurePosixPath(name[len(prefix)+1:] if prefix else name)
            named = re.match(r'^(?:licen[sc]e|licensing|copying|copyright|notice|unlicense)(?:$|[._-])',path.name,re.I)
            nested = any(part.lower() in ('licenses','licences') for part in path.parts[:-1])
            if member.isdir() or not (named or nested or str(path) == explicit):
                continue
            if not member.isfile() or not 0 < member.size <= 4 * 1024 * 1024:
                raise ValueError('selected source notice must be a bounded ordinary file')
            result[name] = archive.extractfile(member).read()
        if explicit and prefix+'/'+explicit not in result:
            raise ValueError('explicit Cargo license_file is missing from the checksum-bound source')
    finally:
        archive.close()
    return result


def expand(root, entry, payload):
    """Check the whole source and selected full texts before writing any file."""
    relative(entry['path'])
    if hashlib.sha256(payload).hexdigest() != entry['sha256']:
        raise ValueError('reviewed source archive digest mismatch')
    files = entry.get('files',[])
    selected = []
    destinations = {entry['path']}
    if files:
        texts = selected_texts(payload,[file['member'] for file in files],entry.get('format'))
        for file in files:
            relative(file['member'])
            relative(file['path'])
            data = select_text(texts[file['member']],file.get('selector'))
            if hashlib.sha256(data).hexdigest() != file['sha256'] or file['path'] in destinations:
                raise ValueError('reviewed notice digest or destination mismatch')
            destinations.add(file['path'])
            selected.append((file['path'],data))
    for name in destinations:
        current = root
        for part in relative(name).parts:
            current /= part
            if current.is_symlink():
                raise ValueError('material destination must not follow symbolic links')
        if current.exists():
            raise FileExistsError(current)
    for name,data in [(entry['path'],payload),*selected]:
        path = root/name
        path.parent.mkdir(parents=True,exist_ok=True)
        write_new(path,data)
