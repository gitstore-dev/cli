#!/usr/bin/env python3
"""Measure/package an explicitly selected own executable pair; never publish."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--prefix", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
if not args.prefix.is_absolute() or not args.output.is_absolute():
    parser.error("Select absolute candidate prefix and new output paths")
metadata = {"schemaVersion": 1, "publication": "unpublished development candidate",
            "coreBudgetBytes": 5 * 1024 * 1024, "executables": {}, "hostLibraries": []}
payloads = {}
libraries = set()
for name in ("gs", "gitstored"):
    path = args.prefix / "bin" / name
    before = path.lstat()
    if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or
            before.st_mode & 0o022 or not before.st_mode & 0o111 or
            not 0 < before.st_size <= 134217728):
        parser.error("Select ordinary owned candidate executables with trusted modes")
    payload = path.read_bytes()
    after = path.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) or payload[:4] != b"\x7fELF":
        parser.error("Candidate executable changed or is not an ELF input")
    payloads[f"bin/{name}"] = payload
    metadata["executables"][name] = {"bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "budgetStatus": "within" if len(payload) <= metadata["coreBudgetBytes"] else "miss"}
    result = subprocess.run(["/usr/bin/ldd", str(path)], capture_output=True, check=True,
                            env={"PATH": "/usr/bin:/bin", "LANG": "C"}, timeout=10)
    if b"not found" in result.stdout:
        parser.error("Candidate has unmet host library dependencies")
    for line in result.stdout.decode().splitlines():
        match = re.search(r"(?:=>\s+)?(/\S+)\s+\(", line)
        if match:
            libraries.add(Path(match.group(1)).resolve(strict=True))
for path in sorted(libraries):
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        parser.error("Host library identity is not trusted")
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    metadata["hostLibraries"].append({"path": str(path), "bytes": info.st_size, "sha256": digest})
metadata["hostLibraryBytes"] = sum(item["bytes"] for item in metadata["hostLibraries"])
metadata["hostLibraryScope"] = "unique ldd dependency closure on selected fixture; reused, not packaged"
payloads["share/gitstore/candidate.json"] = (json.dumps(metadata, indent=2) + "\n").encode()
fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "wb") as raw:
    with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for name, payload in sorted(payloads.items()):
                entry = tarfile.TarInfo(name)
                entry.mode = 0o755 if name.startswith("bin/") else 0o644
                entry.size = len(payload)
                archive.addfile(entry, io.BytesIO(payload))
    raw.flush()
    os.fsync(raw.fileno())
with args.output.open("rb") as stream:
    package_hash = hashlib.file_digest(stream, "sha256").hexdigest()
metadata["package"] = {"path": str(args.output), "bytes": args.output.stat().st_size,
                       "sha256": package_hash, "entries": sorted(payloads)}
record = args.output.with_name(args.output.name + ".json")
fd = os.open(record, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "w") as stream:
    json.dump(metadata, stream, indent=2)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
print(json.dumps({"record": str(record), "packageBytes": metadata["package"]["bytes"],
                  "hostLibraryBytes": metadata["hostLibraryBytes"],
                  "core": metadata["executables"]}))
