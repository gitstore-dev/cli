#!/usr/bin/env python3
"""Build-time specialization of the pinned Moby profile, never runtime input.

GitStore grants no capabilities and uses only pinned native 64-bit images. OCI
runtime JSON has no Docker includes/excludes predicates; resolve those here for
the registered architecture and minimum kernel. libseccomp still compiles/enforces
the resulting syscall policy. No new seccomp execution engine or runtime helper.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARCHES = {"amd64": "SCMP_ARCH_X86_64", "arm64": "SCMP_ARCH_AARCH64"}
MINIMUM_KERNEL = "6.8"


def kernel(value):
    if not isinstance(value, str) or not value or any(not x.isdigit() for x in value.split(".")):
        raise ValueError("unsupported policy kernel predicate")
    result = tuple(map(int, value.split(".")))
    if len(result) > 3:
        raise ValueError("unsupported policy kernel predicate")
    return (result + (0, 0, 0))[:3]


def condition(value, arch, includes):
    if not isinstance(value, dict) or set(value) - {"arches", "caps", "minKernel"}:
        raise ValueError("unrecognized policy predicate")
    for key in ("arches", "caps"):
        if key in value and (not isinstance(value[key], list) or
                             any(not isinstance(x, str) for x in value[key])):
            raise ValueError("invalid policy predicate")
    if includes:
        return (not value.get("arches") or arch in value["arches"]) and not value.get("caps") and (
            "minKernel" not in value or kernel(value["minKernel"]) <= kernel(MINIMUM_KERNEL))
    return bool(value.get("arches") and arch in value["arches"]) or (
        "minKernel" in value and kernel(value["minKernel"]) <= kernel(MINIMUM_KERNEL))


def normalize(profile, arch):
    if arch not in ARCHES or set(profile) != {"defaultAction", "defaultErrnoRet", "archMap", "syscalls"}:
        raise ValueError("unsupported policy format/architecture")
    if profile["defaultAction"] != "SCMP_ACT_ERRNO" or profile["defaultErrnoRet"] != 1:
        raise ValueError("unexpected default policy action")
    rules = []
    for source in profile["syscalls"]:
        if set(source) - {"names", "action", "args", "errnoRet", "includes", "excludes", "comment"}:
            raise ValueError("unrecognized syscall rule")
        include = condition(source.get("includes", {}), arch, True)
        exclude = condition(source.get("excludes", {}), arch, False)
        if not include or exclude:
            continue
        rule = {key: copy.deepcopy(value) for key, value in source.items()
                if key not in {"includes", "excludes", "comment"}}
        if rule.get("action") not in {"SCMP_ACT_ALLOW", "SCMP_ACT_ERRNO"}:
            raise ValueError("unexpected syscall action")
        names = rule.get("names")
        if not isinstance(names, list) or not names or any(
            not isinstance(x, str) or not x or len(x) > 64 or
            any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in x) for x in names
        ):
            raise ValueError("invalid syscall names")
        rules.append(rule)
    return {"defaultAction": "SCMP_ACT_ERRNO", "defaultErrnoRet": 1,
            "architectures": [ARCHES[arch]], "syscalls": rules}


def locked_source():
    lock = json.loads((ROOT / "source-lock.json").read_text())
    raw = (ROOT / "moby-default.json").read_bytes()
    license_bytes = (ROOT / lock["licenseFile"]).read_bytes()
    if len(raw) != lock["bytes"] or hashlib.sha256(raw).hexdigest() != lock["sha256"] or (
        hashlib.sha256(license_bytes).hexdigest() != lock["licenseSha256"]
    ):
        raise ValueError("pinned source/license mismatch")
    return json.loads(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = locked_source()
    for arch in ARCHES:
        expected = (json.dumps(normalize(source, arch), indent=2) + "\n").encode()
        destination = ROOT / f"{arch}.json"
        if args.check:
            if destination.read_bytes() != expected:
                raise ValueError(f"stale generated {arch} policy")
        else:
            destination.write_bytes(expected)


if __name__ == "__main__":
    main()
