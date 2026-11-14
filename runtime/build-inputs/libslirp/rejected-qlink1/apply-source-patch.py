#!/usr/bin/env python3
"""Apply one hash-bound private experiment patch; never patch an unknown source."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent
lock = json.loads((root / "patch-lock.json").read_text())
target = args.source.resolve(strict=True) / lock["file"]
source = target.read_bytes()
if hashlib.sha256(source).hexdigest() != lock["originalSha256"]:
    raise SystemExit("Refusing unknown/already-patched input")
old = b"struct qlink {\n    void *next, *prev;\n};\n"
new = (b"/* Queue overlays can be unaligned after an IPv4 header. Keep pointer\n"
       b" * accesses defined without changing the internal queue layout. */\n"
       b"SLIRP_PACKED_BEGIN\nstruct qlink {\n    void *next, *prev;\n} SLIRP_PACKED_END;\n")
if source.count(old) != 1:
    raise SystemExit("Refusing ambiguous patch location")
patched = source.replace(old, new)
if hashlib.sha256(patched).hexdigest() != lock["patchedSha256"]:
    raise SystemExit("Refusing unexpected patch output")
if hashlib.sha256((root / "qlink-alignment.patch").read_bytes()).hexdigest() != lock["patchSha256"]:
    raise SystemExit("Patch record changed")
target.write_bytes(patched)
print("Applied exact internal queue alignment patch")
