#!/usr/bin/env python3
"""Measure stripped copies of explicitly selected Linux cores; never publish."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
from command_measurements import measure

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--gs", required=True, type=Path)
parser.add_argument("--daemon", required=True, type=Path)
parser.add_argument("--output-directory", required=True, type=Path)
parser.add_argument("--runtime-costs", type=Path, help="Optional saved isolated runtime sample")
parser.add_argument("--samples", type=int, default=100, help="Warm local command samples, default 100")
args = parser.parse_args()
if not 10 <= args.samples <= 1000:
    parser.error("Select 10–1000 command samples")
if sys.platform != "linux":
    parser.error("Select the Linux fixture; this uses GNU strip on disposable copies")
if not args.output_directory.is_absolute() or args.output_directory.exists():
    parser.error("Select a new absolute output directory")
inputs = {}
for name, path in (("gs", args.gs), ("gitstored", args.daemon)):
    if not path.is_absolute():
        parser.error("Select absolute input binaries")
    before = path.lstat()
    if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or
            before.st_mode & 0o022 or not before.st_mode & 0o111 or
            not 0 < before.st_size <= 134217728):
        parser.error("Select owned ordinary executable inputs with trusted modes")
    with path.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            parser.error("Select Linux ELF artifacts")
        stream.seek(0)
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    inputs[name] = {"path": str(path), "bytes": before.st_size, "sha256": digest}
root = args.output_directory
root.mkdir(mode=0o700)
prefix = root / "prefix"
(prefix / "bin").mkdir(mode=0o700, parents=True)
for name, source in inputs.items():
    destination = prefix / "bin" / name
    shutil.copyfile(source["path"], destination, follow_symlinks=False)
    with destination.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != source["sha256"]:
            parser.error("Input changed during copy; retain the output for inspection")
    destination.chmod(0o700)
    subprocess.run(["/usr/bin/strip", "--strip-unneeded", str(destination)], check=True,
                   env={"PATH": "/usr/bin:/bin", "LANG": "C"}, timeout=60)
    destination.chmod(0o500)
package = root / "candidate.tar.gz"
subprocess.run([sys.executable, str(Path(__file__).with_name("candidate.py")),
                "--prefix", str(prefix), "--output", str(package)], check=True,
               env={"PATH": "/usr/bin:/bin", "LANG": "C"}, timeout=120)
record = json.loads(package.with_name(package.name + ".json").read_bytes())
record["sourceInputs"] = inputs
record["acceptedBaselineBytes"] = {"gs": 5590568, "gitstored": 5723600}
record["sizePolicy"] = "Report current sizes; published-binary optimization is deferred"
record["commandLatency"] = measure(prefix / "bin", root / "command-fixture", args.samples)
record["runtimeCosts"] = {"status": "unmeasured in this artifact-only run"}
if args.runtime_costs:
    info = args.runtime_costs.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or
            info.st_mode & 0o077 or info.st_nlink != 1 or info.st_size > 65536):
        parser.error("Select a bounded owned protected runtime cost record")
    sample = json.loads(args.runtime_costs.read_bytes())
    # No arbitrary diagnostics, paths or private configuration are copied.
    names = ("coldOfflineReadySeconds", "warmReadySeconds", "managerMemoryOnly",
             "stateLogicalBytes", "assetLogicalBytes")
    record["runtimeCosts"] = {name: sample[name] for name in names if name in sample}
    record["runtimeCosts"]["scope"] = "Separate historical runtime sample; manager memory excludes workload/worker peaks"
output = root / "measurement.json"
fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "w") as stream:
    json.dump(record, stream, indent=2)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
print(json.dumps({"measurement": str(output), "executables": record["executables"],
                  "packageBytes": record["package"]["bytes"],
                  "hostLibraryBytes": record["hostLibraryBytes"]}))
