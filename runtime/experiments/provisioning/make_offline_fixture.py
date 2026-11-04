#!/usr/bin/env python3
"""Developer-only: copy verified cache blobs into a disposable OCI layout.

Does not authorize profiles, import rootfs, or provide product provisioning.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: make_offline_fixture.py OWNED_CACHE NEW_LAYOUT MANIFEST_SHA256")
    cache, layout = map(Path, sys.argv[1:3])
    expected = sys.argv[3]
    raw = (cache / "manifest.json").read_bytes()
    digest = "sha256:" + hashlib.sha256(raw).hexdigest()
    if digest != expected:
        raise SystemExit("manifest differs from explicit fixture pin")
    manifest = json.loads(raw)
    layout.mkdir(mode=0o700)
    blobs = layout / "blobs" / "sha256"
    blobs.mkdir(parents=True, mode=0o700)
    for descriptor, source in [
        (manifest["config"], cache / "config.json"),
        *((d, cache / "blobs" / f"layer-{i}") for i, d in enumerate(manifest["layers"])),
    ]:
        pinned = descriptor["digest"]
        if not pinned.startswith("sha256:") or len(pinned) != 71:
            raise SystemExit("unsupported fixture descriptor")
        if source.stat().st_size != descriptor["size"]:
            raise SystemExit("fixture blob size mismatch")
        with source.open("rb") as stream:
            observed = "sha256:" + hashlib.file_digest(stream, "sha256").hexdigest()
        if observed != pinned:
            raise SystemExit("fixture blob digest mismatch")
        destination = blobs / pinned[7:]
        if not destination.exists():
            shutil.copyfile(source, destination)
            destination.chmod(0o600)
    (blobs / digest[7:]).write_bytes(raw)
    (layout / "oci-layout").write_text(json.dumps({"imageLayoutVersion": "1.0.0"}))
    (layout / "index.json").write_text(json.dumps({
        "schemaVersion": 2,
        "manifests": [{"mediaType": manifest["mediaType"], "digest": digest, "size": len(raw)}],
    }))
    print(json.dumps({"manifestDigest": digest, "network": False, "layout": os.fspath(layout)}))


if __name__ == "__main__":
    main()
