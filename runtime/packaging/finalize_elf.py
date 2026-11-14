"""Remove only debug data from a Linux production ELF before identity freezes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


def snapshot(binary):
    with os.fdopen(os.open(binary, os.O_RDONLY | os.O_NOFOLLOW), 'rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_mode & 0o022:
            raise ValueError('select an ordinary privately built executable')
        return dict(sha256=hashlib.file_digest(source, 'sha256').hexdigest(), bytes=info.st_size)


def finalize(binary, *, strip='strip'):
    binary = Path(binary)
    before = snapshot(binary)
    with binary.open('rb') as source:
        header = source.read(20)
    if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') not in (62, 183):
        raise ValueError('select a supported Linux ELF64 executable')
    tool = subprocess.check_output([strip, '--version'], text=True).splitlines()[0]
    if not tool.startswith('GNU strip '):
        raise ValueError('select GNU binutils strip for Linux release finalization')
    subprocess.run([strip, '--strip-debug', str(binary)], check=True)
    after = snapshot(binary)
    return dict(schemaVersion=1, operation='strip-debug', tool=tool,
                inputSha256=before['sha256'], inputBytes=before['bytes'],
                sha256=after['sha256'], bytes=after['bytes'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--record', required=True, type=Path)
    args = parser.parse_args()
    if args.record.exists() or args.record.is_symlink():
        parser.error('select a new finalization record in a disposable build directory')
    record = finalize(args.binary)
    with args.record.open('x') as stream:
        json.dump(record, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
