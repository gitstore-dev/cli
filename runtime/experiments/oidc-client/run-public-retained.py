#!/usr/bin/env python3
"""Prove a production candidate on an explicitly owned, stopped public OIDC fixture.

Retains canonical data, material, logs and receipts. Reuses verified images without
provisioning. This maintainer driver never selects ambient user configuration.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import stat
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tests/runtime"))
from support import Fixture
from test_optional import Optional


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("fixture", "root", "daemon", "output-parent"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--standalone", action="store_true")
    args = parser.parse_args()
    name = "identity" if args.standalone else "combined"
    prefix = ("oidc", "serve") if args.standalone else ("serve", "--add", "oidc")
    datastore = "memdb" if args.standalone else "scylladb"
    if not os.geteuid() or not Path("/proc/self/ns/user").exists():
        parser.error("select the dedicated non-root disposable Linux account")
    for path in (args.fixture, args.root, args.daemon, args.output_parent):
        if not path.is_absolute() or path.is_symlink():
            parser.error("select absolute ordinary owned fixture paths")
    info = args.fixture.stat()
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600:
        parser.error("fixture manifest must be private and owned")
    settings = json.loads(args.fixture.read_bytes())
    if settings.get("dedicatedTestAccount") is not True or settings.get("disposableUid") != os.geteuid():
        parser.error("fixture must explicitly select this dedicated test account")
    for path in (args.root, args.output_parent):
        info = path.stat()
        if not path.is_dir() or info.st_uid != os.geteuid() or info.st_mode & 0o022:
            parser.error("fixture roots must be owned ordinary private directories")
    if not args.root.name.startswith("gs-public-runtime-") or args.root.parent != Path(settings["fixtureParent"]):
        parser.error("select an existing explicit public fixture under its owned parent")
    with sqlite3.connect((args.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
        if (db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0] or
                db.execute("SELECT count(*) FROM operations WHERE state IN ('accepted','running')").fetchone()[0] or
                db.execute("SELECT count(*) FROM deployments WHERE deployment_id=? AND profile_id='core' AND profile_version='2' AND desired_state='stopped'", (name,)).fetchone()[0] != 1):
            parser.error("fixture must have a stopped core/2 deployment and no unresolved owners or operations")
    output = Path(tempfile.mkdtemp(prefix="oidc-public-retained-", dir=args.output_parent))
    output.chmod(0o700)
    install = Path(tempfile.mkdtemp(prefix="production-", dir=args.root / "bin"))
    install.chmod(0o700)
    identities = {}
    for binary_name, source in (("gs", Path(settings["gs"])), ("gitstored", args.daemon)):
        info = source.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid not in (0, os.geteuid()) or info.st_mode & 0o022:
            parser.error("candidate executables require ordinary trusted files")
        destination = install / binary_name
        shutil.copyfile(source, destination)
        destination.chmod(0o500)
        identities[binary_name] = Fixture.digest(destination)
        if identities[binary_name] != Fixture.digest(source):
            parser.error("installed candidate identity changed")
    f = Fixture.__new__(Fixture)
    f.root, f.gs, f.daemon = args.root, install / "gs", install / "gitstored"
    f.socket, f.deployments, f.ready_timeout = args.root / "runtime/gitstored.sock", set(), 240
    f.environment = {"PATH": "/nonexistent", "HOME": str(args.root / "home"),
                     "XDG_CONFIG_HOME": str(args.root / "config"), "XDG_STATE_HOME": str(args.root / "state"),
                     "XDG_CACHE_HOME": str(args.root / "cache"), "XDG_RUNTIME_DIR": f"/run/user/{os.geteuid()}",
                     "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{os.geteuid()}/bus", "LANG": "C", "NO_COLOR": "1"}
    result = {"schemaVersion": 1, "status": "running", "root": str(args.root),
              "executables": identities, "retention": "canonical data/material/receipts retained"}
    started = time.monotonic()
    f.start_manager()
    try:
        f.deployments.add(name)
        key = f"production-{time.time_ns()}"
        operation = f.call("--non-interactive", *prefix, "--deployment", name,
                           "--datastore", datastore, "--detach", "--idempotency-key", key)
        check = Optional()
        if not args.standalone:
            check.pause_bridge_during_core_start(f, operation["operationId"])
        deployment = f.ready(name, operation)
        check.assert_endpoints(deployment, not args.standalone)
        terminal = f.call("deployment", "inspect", name, "--operation", operation["operationId"])["operation"]
        fd = os.pidfd_open(f.process.pid)
        try:
            signal.pidfd_send_signal(fd, signal.SIGKILL)
            f.process.wait(timeout=10)
        finally:
            os.close(fd)
        f.start_manager()
        observed = f.call("deployment", "inspect", name)
        assert observed["readiness"] != "ready" and not observed.get("endpoints")
        refused = f.call("--non-interactive", *prefix, "--deployment", name,
                         "--datastore", datastore, "--detach", "--idempotency-key", key + "-replacement", success=False)
        assert b"OWNERSHIP_UNCERTAIN" in refused.stderr or b"DEPLOYMENT_CONFLICT" in refused.stderr
        f.stop(name, key=key + "-stop")
        assert f.call("deployment", "inspect", name)["writerOwnership"] == "released"
        assert f.call("deployment", "inspect", name, "--operation", operation["operationId"])["operation"] == terminal
        result.update(status="passed", services=5 if args.standalone else 9, retainedStart=True, stalledBridgeBlockedReadiness=not args.standalone,
                      resumedReadiness=True, managerLoss=True, readinessNotAdopted=True,
                      replacementRefused=True, ownedStop=True, originalStartUnchanged=True)
    except Exception as error:
        result.update(status="failed", errorType=type(error).__name__)
        raise
    finally:
        f.close()
        result["elapsedSeconds"] = round(time.monotonic() - started, 3)
        path = output / "result.json"
        path.write_text(json.dumps(result, indent=2) + "\n")
        path.chmod(0o600)
        print(json.dumps({"status": result["status"], "receipt": str(path)}), flush=True)


if __name__ == "__main__":
    main()
