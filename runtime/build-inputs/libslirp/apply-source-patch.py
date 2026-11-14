#!/usr/bin/env python3
"""Apply only the exact hash-bound queue-access experiment to known source."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
args = parser.parse_args()
record = Path(__file__).resolve().parent
lock = json.loads((record / "patch-lock.json").read_text())
if hashlib.sha256((record / lock["patch"]).read_bytes()).hexdigest() != lock["patchSha256"]:
    raise SystemExit("Patch record changed")
changes = []
for item in lock["files"]:
    target = args.source.resolve(strict=True) / item["file"]
    source = target.read_bytes()
    if hashlib.sha256(source).hexdigest() != item["originalSha256"]:
        raise SystemExit("Refusing unknown/already-patched input")
    updated = source
    for old, new in item["replacements"]:
        if updated.count(old.encode()) != 1:
            raise SystemExit("Refusing ambiguous patch location")
        updated = updated.replace(old.encode(), new.encode())
    if hashlib.sha256(updated).hexdigest() != item["patchedSha256"]:
        raise SystemExit("Refusing unexpected patch output")
    changes.append((target, updated))
# Verify every input/output before changing any file in the owned source copy.
for target, updated in changes:
    target.write_bytes(updated)
print("Applied exact queue access-view patch; original container types unchanged")
