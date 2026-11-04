#!/usr/bin/env python3
"""Explicit, isolated developer fixture. Never discovers installed tools/auth."""
import argparse
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry-fixture", type=Path, required=True)
    parser.add_argument("--rust-probe", type=Path, required=True)
    parser.add_argument("--c-probe", type=Path, required=True)
    args = parser.parse_args()
    binaries = [path.resolve(strict=True) for path in
                (args.registry_fixture, args.rust_probe, args.c_probe)]
    for binary in binaries:
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("Select explicit executable experiment binaries")
    with tempfile.TemporaryDirectory(prefix="gitstore-image-bridge-") as tmp:
        env = {"PATH": "/nonexistent", "HOME": tmp, "TMPDIR": tmp}
        fixture = subprocess.Popen([str(binaries[0]), "serve-fixture"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, env=env)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(fixture.stdout, selectors.EVENT_READ)
                if not ready.select(timeout=5):
                    raise RuntimeError("Synthetic registry startup deadline")
                line = fixture.stdout.readline(4097)
            if len(line) > 4096:
                raise RuntimeError("Synthetic registry metadata bound")
            reference = json.loads(line)["reference"]
            for index, binary in enumerate(binaries[1:]):
                output = Path(tmp) / f"output-{index}"
                command = [str(binary)]
                if index == 0:
                    command.append("fetch-fixture")
                subprocess.run(command + [reference, str(output)], check=True,
                               timeout=10, env=env)
                assert (output / "retained").read_text() == "two"
                assert not (output / "gone").exists()
                print(f"{binary.name}: private digest transfer/unpack passes")
        finally:
            fixture.stdin.close()
            try:
                fixture.wait(timeout=3)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait(timeout=3)


if __name__ == "__main__":
    main()
