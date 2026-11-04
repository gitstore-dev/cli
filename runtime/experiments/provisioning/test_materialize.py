#!/usr/bin/env python3
"""Developer-only real API-cache materialization check; never executes the image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


def snapshot(root):
    rows = []
    total = 0
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in sorted(dirs + files):
            path = Path(directory) / name
            info = path.lstat()
            row = [str(path.relative_to(root)), info.st_uid, info.st_gid,
                   stat.S_IFMT(info.st_mode), stat.S_IMODE(info.st_mode)]
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
                if total > 8 * 1024**3:
                    raise RuntimeError("source snapshot byte bound")
                with path.open("rb") as stream:
                    row += [info.st_size, hashlib.file_digest(stream, "sha256").hexdigest()]
            elif stat.S_ISLNK(info.st_mode):
                row.append(os.readlink(path))
            elif not stat.S_ISDIR(info.st_mode):
                raise RuntimeError("unexpected source file type")
            rows.append(row)
            if len(rows) > 131072:
                raise RuntimeError("source snapshot entry bound")
    return hashlib.sha256(json.dumps(sorted(rows), separators=(",", ":")).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() == 0:
        raise RuntimeError("select the unprivileged disposable fixture user")
    source = args.source.resolve(strict=True)
    probe = args.probe.resolve(strict=True)
    info = source.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise RuntimeError("select a private owned API image cache fixture")
    before = snapshot(source)
    target = Path(tempfile.mkdtemp(prefix="gitstore-materialize-"))
    os.chmod(target, 0o700)
    # Failure retains this exact owned root; successful supervisor completion
    # means all acquisition workers were observed/reaped before cleanup below.
    env = {"HOME": str(target), "PATH": "/nonexistent"}
    result = subprocess.run([str(probe), "api", str(target), "60000", str(source), "materialize"],
                            env=env, capture_output=True, timeout=65, check=True)
    report = json.loads(result.stdout)
    if report["status"] != 0 or result.stderr or snapshot(source) != before:
        raise RuntimeError(f"materialization/source isolation failed; retained {target}")
    original = (source / "rootfs/app/api").stat()
    copied = (target / "rootfs/app/api").stat()
    if (original.st_dev, original.st_ino) == (copied.st_dev, copied.st_ino):
        raise RuntimeError(f"shared workload/cache inode; retained {target}")
    print(json.dumps({"scope": "real API cache only; image never executed", "sourceUnchanged": True,
                      "independentApiFileInode": True, "sourceSnapshotSha256": before,
                      "worker": report}))
    shutil.rmtree(target)


if __name__ == "__main__":
    main()
