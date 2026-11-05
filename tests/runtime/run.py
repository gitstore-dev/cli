#!/usr/bin/env python3
"""Opt-in real runtime scenarios; unavailable fixture exits 77, failed checks exit 1.
No fixture is synthesized, no container engine is selected, and all owned evidence
is retained by the scenario harness. This is development tooling, not packaging.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--scenario", required=True, choices=("core-memdb", "exp-003b", "exp-003b-sdk", "core-offline", "host-unmet", "exp-004b-local", "exp-004b-disk", "cli-tls", "clean-install", "optional", "recovery", "scylla-native", "persistent-scylla"))
parser.add_argument("--fixture", required=True, type=Path)
parser.add_argument("--case", help="One saved test method in the selected scenario")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
scenarios = {"persistent-scylla": ("test_scylla_public.py", "PersistentScylla"), "scylla-native": ("test_scylla.py", "ScyllaNative"), "recovery": ("test_recovery.py", "Recovery"), "optional": ("test_optional.py", "Optional"),"core-memdb": ("test_core.py", "Core"),
             "exp-003b": ("test_assets.py", "Assets"),
             "exp-003b-sdk": ("test_image_sdk.py", "ImageSdk"),
             "exp-004b-disk": ("test_disk.py", "Disk"),
             "cli-tls": ("test_tls.py", "Tls"),
             "clean-install": ("test_clean_install.py", "CleanInstall"),
             "core-offline": ("test_assets.py", "Assets"),
             "host-unmet": ("test_host.py", "Host"),
             "exp-004b-local": ("test_volumes.py", "Volumes")}
script, case_class = scenarios[args.scenario]

def unavailable(message):
    print(json.dumps({"schemaVersion": 1, "scenario": args.scenario,
                      "status": "unavailable", "reason": message}), flush=True)
    raise SystemExit(77)

uid = os.geteuid()
if not uid or not Path("/proc/self/ns/user").exists():
    unavailable("Select an explicitly dedicated non-root Linux fixture")
source = args.fixture
if not source.is_absolute():
    unavailable("Fixture manifest must be an absolute owner-selected path")
try:
    metadata = source.lstat()
    if (not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != uid or
            stat.S_IMODE(metadata.st_mode) != 0o600 or metadata.st_nlink != 1 or
            not 0 < metadata.st_size <= 65536):
        unavailable("Fixture manifest must be an owned mode-0600 ordinary single-link file")
    source = source.resolve(strict=True)
    settings = json.loads(source.read_bytes())
except (OSError, ValueError):
    unavailable("Fixture manifest cannot be read as bounded JSON")
if settings.get("disposableUid") != uid or settings.get("dedicatedTestAccount") is not True:
    unavailable("Fixture must explicitly select the invoking dedicated test account")
try:
    parent = Path(settings["fixtureParent"])
    if not parent.is_absolute() or parent.is_symlink():
        unavailable("Fixture parent must be an absolute ordinary directory")
    parent = parent.resolve(strict=True)
    info = parent.stat()
    if info.st_uid != uid or info.st_mode & 0o022:
        unavailable("Fixture parent must be owned and not group/other writable")
except (KeyError, OSError, TypeError):
    unavailable("An owned disposable fixture parent is required")
methods = []
for item in ast.parse((repo / "tests/runtime" / script).read_text()).body:
    if isinstance(item, ast.ClassDef) and item.name == case_class:
        methods = [f.name for f in item.body if isinstance(f, ast.FunctionDef) and f.name.startswith("test_")]
selected = args.case
if args.scenario == "core-offline":
    selected = selected or "test_offline_layouts_authorize_only_the_selected_verified_bytes"
if selected and selected not in methods:
    parser.error("--case must name one saved method in this scenario")
migration_preparation = args.scenario == "scylla-native" and selected == "test_private_standalone_migration_preparation"
migration_execution = args.scenario == "scylla-native" and selected in ("test_private_standalone_migration_execution", "test_private_surviving_migration_helper_recovery")
migration_recovery = args.scenario == "scylla-native" and selected == "test_private_migration_preparation_recovery"
if migration_preparation or migration_execution or (args.scenario == "scylla-native" and not selected):
    target_root = settings.get("persistentPreparationRoot")
    if not isinstance(target_root, str) or not Path(target_root).is_absolute():
        unavailable("Select an explicit stopped persistentPreparationRoot for private migration preparation")
if migration_recovery:
    recovery_root = settings.get("persistentRecoveryRoot")
    if not isinstance(recovery_root, str) or not Path(recovery_root).is_absolute():
        unavailable("Select an explicit interrupted persistentRecoveryRoot for observed cleanup")
managed_scylla = args.scenario == "scylla-native" and selected in (
    "test_private_journaled_persistent_bootstrap", "test_private_persistent_actor_retained_restart",
    "test_private_persistent_observer_restoration", "test_private_persistent_manager_loss")
required = (["migrationProbe"] if migration_preparation or migration_execution or migration_recovery
            else ["gs", "datastoreProbe"] if managed_scylla
            else ["scyllaNativeProbe", "scyllaBackupProbe"] if args.scenario == "scylla-native"
            else ["imageSdkTest", "registryFixture"] if args.scenario == "exp-003b-sdk"
            else ["volumeDiskProbe"] if args.scenario == "exp-004b-disk"
            else ["gs", "registryFixture"] if args.scenario == "cli-tls"
            else ["gs", "daemon"])
if args.scenario == "scylla-native" and not selected:
    required.extend(["gs", "datastoreProbe", "migrationProbe"])
if args.scenario == "persistent-scylla" and (not selected or selected == "test_private_backup_restore_and_unsafe_cancellation"):
    required.extend(["scyllaNativeProbe", "scyllaBackupProbe"])
if args.scenario == "core-memdb" and (not selected or selected == "test_missing_execution_capability_fails_before_effects"):
    required.append("unsupportedDaemon")
if args.scenario == "clean-install":
    if not selected or selected == "test_missing_runtime_and_optional_mapping":
        required.append("unsupportedDaemon")
    if not selected or selected == "test_public_graphql_git_and_retained_restart":
        required.append("repositoryProbe")
identities = {}
for name in required:
    try:
        path = Path(settings[name])
        if not path.is_absolute() or path.is_symlink():
            unavailable("Selected test binaries must be absolute ordinary executable files")
        info = path.stat()
        if (not stat.S_ISREG(info.st_mode) or info.st_uid not in (0, uid) or
                info.st_mode & 0o022 or not info.st_mode & 0o111):
            unavailable("Selected test binaries must have trusted owner/executable modes")
        with path.open("rb") as stream:
            identities[name] = {"path": str(path), "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
    except (KeyError, OSError, TypeError):
        unavailable("Select existing own gs and daemon fixture executables")
checks = []
if args.scenario in ("core-offline", "exp-004b-local", "clean-install", "recovery") or (args.scenario == "exp-003b" and
        (not selected or selected in ("test_offline_layouts_authorize_only_the_selected_verified_bytes", "test_interrupted_offline_blob_never_activates",
                                     "test_warm_restart_reuses_assets_and_existing_private_material",
                                     "test_tampered_warm_blob_cannot_be_reused_for_execution"))):
    checks.append("offlineLayouts")
if args.scenario == "exp-003b" and (not selected or selected == "test_wrong_platform_offline_input_never_activates"):
    checks.append("wrongPlatformLayouts")
for name in checks:
    layouts = settings.get(name)
    if not isinstance(layouts, dict) or set(layouts) != {"api", "git", "controller"}:
        unavailable("Select all three verified offline layout inputs required by this scenario")
    for value in layouts.values():
        try:
            path = Path(value)
            if not path.is_absolute() or path.is_symlink() or not (path / "oci-layout").is_file() or not (path / "index.json").is_file():
                unavailable("Selected offline layout is absent; no registry fallback is allowed")
        except (OSError, TypeError):
            unavailable("Select ordinary offline OCI layout directories")
if args.scenario == "host-unmet" and os.environ.get("GITSTORE_EXPECT_MISSING_TUN") != "1":
    unavailable("Explicitly establish the disposable host's TUN denial before this scenario")
# Pin the exact tested bytes for the whole batch. A developer build may replace
# the original executable while later cases are still creating their fixtures.
snapshot = Path(tempfile.mkdtemp(prefix="runtime-inputs-", dir=parent))
snapshot.chmod(0o700)
for name in required:
    destination = snapshot / name
    shutil.copyfile(settings[name], destination)
    destination.chmod(0o500)
    with destination.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != identities[name]["sha256"]:
        unavailable("Selected binary changed during snapshot; retained inputs must be inspected")
    settings[name] = str(destination)
snapshot_manifest = snapshot / "fixture.json"
snapshot_fd = os.open(snapshot_manifest, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(snapshot_fd, "w") as stream:
    json.dump(settings, stream)
    stream.flush()
    os.fsync(stream.fileno())
fd, record_name = tempfile.mkstemp(prefix=f"runtime-run-{args.scenario}-", suffix=".json", dir=parent)
record = {"schemaVersion": 1, "recordedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "scenario": args.scenario, "case": selected,
          "adapter": ("private standalone migration preparation; no native/schema dispatch"
                      if migration_preparation else "private standalone image-native migration; not public migration execution"
                      if migration_execution else "private interrupted migration preparation recovery; no CREATE/START"
                      if migration_recovery else "private SQLite-journaled persistent bootstrap with real native workloads; not public deployment execution"
                      if managed_scylla
                      else "developer-only native pinned Scylla/CQL/migration probe; not public deployment execution"
                      if args.scenario == "scylla-native"
                      else "private schema/backup/restore/cancellation; not public restore support"
                      if args.scenario == "persistent-scylla" and selected == "test_private_backup_restore_and_unsafe_cancellation"
                      else "maintained image code library with isolated synthetic registry"
                      if args.scenario == "exp-003b-sdk"
                      else "actual tmpfs capacity/full-disk with native adapter primitives"
                      if args.scenario == "exp-004b-disk"
                      else "actual CLI with explicitly selected synthetic HTTPS peers"
                      if args.scenario == "cli-tls"
                      else "public CLI with selected native SDK/runtime/network implementations"),
          "status": "running", "fixture": str(source), "executables": identities,
          "snapshotFixture": str(snapshot_manifest),
          "testAccountUid": uid, "retention": "all scenario roots retained"}
started = time.monotonic()

def save():
    encoded = (json.dumps(record, indent=2) + "\n").encode()
    os.lseek(fd, 0, os.SEEK_SET)
    os.ftruncate(fd, 0)
    os.write(fd, encoded)
    os.fsync(fd)

save()
print(json.dumps({"scenario": args.scenario, "record": record_name}), flush=True)
command = [sys.executable, str(repo / "tests/runtime" / script)]
if selected:
    command.append(f"{case_class}.{selected}")
environment = {"PATH": "/usr/bin:/bin", "GITSTORE_RUNTIME_FIXTURE": str(snapshot_manifest)}
if args.scenario == "host-unmet":
    environment["GITSTORE_EXPECT_MISSING_TUN"] = "1"
try:
    result = subprocess.run(command, cwd=repo, env=environment, timeout=1800, check=False)
    record.update(status="passed" if result.returncode == 0 else "failed", exitCode=result.returncode)
except subprocess.TimeoutExpired:
    record.update(status="failed", exitCode=1, reason="Scenario deadline expired; inspect retained ownership before cleanup")
finally:
    record["elapsedSeconds"] = round(time.monotonic() - started, 3)
    save()
    os.close(fd)
print(json.dumps({"scenario": args.scenario, "status": record["status"], "record": record_name}), flush=True)
raise SystemExit(0 if record["status"] == "passed" else 1)
