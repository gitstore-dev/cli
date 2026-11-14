"""Remove only debug data from a Linux production ELF before identity freezes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile


def snapshot(binary):
    with os.fdopen(os.open(binary, os.O_RDONLY | os.O_NOFOLLOW), 'rb') as source:
        info = os.fstat(source.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_mode & 0o022 or
                info.st_uid != os.geteuid()):
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


def finalize_record(binary, record):
    if record.exists() or record.is_symlink():
        snapshot(record)
        previous = json.loads(record.read_text())
        if previous.get('schemaVersion') != 1 or previous.get('operation') != 'strip-debug':
            raise ValueError('unrecognized previous build finalization record')
        current = snapshot(binary)
        if current == {key: previous[key] for key in ('sha256', 'bytes')}:
            # An unchanged incremental CMake target remains finalized. Retain
            # the original unstripped identity instead of recording a no-op.
            return previous
    value = finalize(binary)
    with tempfile.NamedTemporaryFile(mode='w', dir=record.parent, prefix='.finalization-', delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
            os.replace(temporary, record)
        finally:
            temporary.unlink(missing_ok=True)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--record', required=True, type=Path)
    args = parser.parse_args()
    finalize_record(args.binary, args.record)


if __name__ == '__main__':
    main()
