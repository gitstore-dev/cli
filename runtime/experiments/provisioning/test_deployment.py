#!/usr/bin/env python3
"""Opt-in private deployment actor integration on an owner-selected Linux fixture.
Rebuild offline OCI inputs from verified blob caches; the SDK rechecks all bytes.
Retain the entire synthetic fixture, ledger, credentials and volume on every exit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import stat
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", type=Path)
parser.add_argument("broker", type=Path)
parser.add_argument("assets", type=Path)
parser.add_argument("--fixture-parent", type=Path, required=True)
args = parser.parse_args()
uid = os.geteuid()
if not uid or not Path("/proc/self/ns/user").exists():
    raise SystemExit("Select a disposable non-root Linux account")
parent = args.fixture_parent.resolve(strict=True)
if parent.stat().st_uid != uid or parent.stat().st_mode & 0o022:
    raise SystemExit("Fixture parent must be owned and not group/other writable")
os.umask(0o077)
root = Path(tempfile.mkdtemp(prefix="deployment-actor-", dir=parent))
run = root / "run"
run.mkdir(mode=0o700)
credentials = root / "credentials"
credentials.mkdir(mode=0o700)
source_broker = args.broker.resolve(strict=True)
broker = root / "gs"
shutil.copyfile(source_broker, broker)
broker.chmod(0o500)
if hashlib.file_digest(source_broker.open("rb"), "sha256").digest() != hashlib.file_digest(broker.open("rb"), "sha256").digest():
    raise SystemExit(f"Broker digest mismatch; fixture retained: {root}")
layouts = {}
for component in ("api", "git", "controller"):
    candidates = [p for p in (args.assets / component / "cache").iterdir()
                  if len(p.name) == 64 and all(c in "0123456789abcdef" for c in p.name) and p.is_dir() and not p.is_symlink()]
    if len(candidates) != 1:
        raise SystemExit(f"Select exactly one verified {component} image cache")
    source = candidates[0]
    layout = root / f"offline-{component}"
    blobs = layout / "blobs" / "sha256"
    blobs.mkdir(parents=True, mode=0o700)
    raw = (source / "manifest.json").read_bytes()
    manifest = json.loads(raw)
    descriptors = [manifest["config"], *manifest["layers"]]
    for index, descriptor in enumerate(descriptors):
        algorithm, digest = descriptor["digest"].split(":", 1)
        if algorithm != "sha256" or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise SystemExit("Unexpected blob identity")
        candidates = [source / "blobs" / digest, source / "blobs" / "sha256" / digest]
        if descriptor is manifest["config"]:
            candidates.append(source / "config.json")
        else:
            candidates.append(source / "blobs" / f"layer-{index - 1}")
        original = next((p for p in candidates if p.exists()), None)
        if original is None or not stat.S_ISREG(original.lstat().st_mode):
            raise SystemExit("Missing ordinary cached blob")
        if original.stat().st_size != descriptor["size"] or hashlib.file_digest(original.open("rb"), "sha256").hexdigest() != digest:
            raise SystemExit("Cached blob mismatch")
        shutil.copyfile(original, blobs / digest)
    digest = hashlib.sha256(raw).hexdigest()
    (blobs / digest).write_bytes(raw)
    (layout / "oci-layout").write_text(json.dumps({"imageLayoutVersion": "1.0.0"}))
    (layout / "index.json").write_text(json.dumps({"schemaVersion": 2, "manifests": [{
        "mediaType": manifest.get("mediaType", "application/vnd.oci.image.manifest.v1+json"),
        "digest": f"sha256:{digest}", "size": len(raw)}]}))
    layouts[component] = str(layout)
(run / "settings.json").write_text(json.dumps({"schemaVersion": 1,
    "bootstrapCredentialSource": {"kind": "protected-directory", "path": str(credentials)},
    "offlineLayouts": layouts}))
environment = {"HOME": pwd.getpwuid(uid).pw_dir, "PATH": "/nonexistent", "LANG": "C",
               "XDG_RUNTIME_DIR": f"/run/user/{uid}",
               "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{uid}/bus"}
print(json.dumps({"fixture": str(root), "scope": "private deployment actor"}), flush=True)
try:
    result = subprocess.run([str(args.probe.resolve(strict=True)), str(run), str(broker),
                             *[layouts[c] for c in ("api", "git", "controller")]],
                            env=environment, timeout=620, check=False)
except subprocess.TimeoutExpired:
    raise SystemExit(f"Deadline expired; inspect retained ownership before cleanup: {root}") from None
print(json.dumps({"exit": result.returncode, "fixture": str(root)}), flush=True)
raise SystemExit(result.returncode)
