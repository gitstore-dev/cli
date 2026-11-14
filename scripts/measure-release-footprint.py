#!/usr/bin/env python3
"""Attribute an immutable Linux release using its matching production relink kit.

Run in a disposable Linux environment with matching architecture/compiler/libc.
Never modifies inputs, publishes artifacts or establishes release qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    kit, binary, output = (p.resolve() for p in (args.kit, args.binary, args.output))
    manifest = json.loads((kit / 'relink.json').read_text())
    if digest(binary) != manifest['originalSha256']:
        parser.error('release binary does not match original relink identity')
    for name, record in manifest['materials'].items():
        path = (kit / name).resolve()
        if not path.is_relative_to(kit) or digest(path) != record['sha256']:
            parser.error('invalid production relink input')
    output.mkdir(exist_ok=False)
    linked = output / 'gitstored-baseline'
    mapfile = output / 'baseline.map'
    subprocess.run(['cc', *manifest['arguments'], '-Wl,-Map=' + str(mapfile),
                    '-o', str(linked)], cwd=kit, check=True)
    identical = digest(linked) == digest(binary)
    # The release and kit identity check above is exact. Relinked bytes can
    # differ with the host's startup objects/linker; record that limitation.
    stripped = output / 'gitstored-strip-unneeded'
    subprocess.run(['strip', '--strip-unneeded', '-o', str(stripped), str(linked)], check=True)
    public_stripped = output / 'published-strip-unneeded'
    debug_stripped = output / 'published-strip-debug'
    subprocess.run(['strip', '--strip-unneeded', '-o', str(public_stripped), str(binary)], check=True)
    subprocess.run(['strip', '--strip-debug', '-o', str(debug_stripped), str(binary)], check=True)
    # Input-section rows follow an optional section name, VMA and byte count.
    # Discarded sections occur before the linker memory map and are excluded.
    # BSS counts memory, not file bytes. Merge/dedup/alignment remain linker costs.
    allocated = mapfile.read_text().split('Linker script and memory map', 1)[1]
    contributors = {}
    for line in allocated.splitlines():
        match = re.match(r'^\s*(?:\.[^\s]+\s+)?0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(objects/\S+)', line)
        if match and int(match[1], 16):
            owner = match[3].split('(')[0]
            contributors[owner] = contributors.get(owner, 0) + int(match[2], 16)
    report = dict(schemaVersion=1, sourceRevision=manifest['sourceRevision'],
                  machine=manifest['machine'], compiler=run('cc', '--version').splitlines()[0],
                  linker=run('ld', '--version').splitlines()[0],
                  originalCompiler=manifest['compiler'], exactReconstruction=identical,
                  baseline=dict(bytes=binary.stat().st_size, sha256=digest(binary)),
                  stripUnneeded=dict(bytes=public_stripped.stat().st_size, sha256=digest(public_stripped)),
                  stripDebug=dict(bytes=debug_stripped.stat().st_size, sha256=digest(debug_stripped)),
                  savedBytes=binary.stat().st_size-public_stripped.stat().st_size,
                  allocatedInputSections=sorted(
                      [dict(archive=name, bytes=size) for name, size in contributors.items()],
                      key=lambda row: row['bytes'], reverse=True),
                  interpretation='Reconstructed link map, not the original producer map. Allocated input section bytes include BSS, before merge/dedup/alignment; not archive file sizes or an exact additive file-byte decomposition. Consult exactReconstruction before comparing byte identities.',
                  baselineSections=run('size', '-A', str(binary)),
                  reconstructedSections=run('size', '-A', str(linked)),
                  strippedSections=run('size', '-A', str(public_stripped)),
                  dynamicDependencies=run('readelf', '-d', str(binary)),
                  qualification='Unsigned disposable experiment; no lifecycle, recovery, image, network or package qualification implied')
    (output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: report[k] for k in ('machine', 'exactReconstruction', 'baseline', 'stripUnneeded', 'savedBytes')}))


if __name__ == '__main__':
    main()
