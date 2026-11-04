#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
daemon=""
gs=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --daemon) daemon=$2; shift 2 ;;
    --gs) gs=$2; shift 2 ;;
    *) echo "Usage: scripts/test-daemon.sh --daemon PATH --gs PATH" >&2; exit 2 ;;
  esac
done
[ -n "$daemon" ] && [ -x "$daemon" ] && [ -n "$gs" ] && [ -x "$gs" ] || { echo "Select built gs and gitstored executables" >&2; exit 2; }
daemon=$(python3 -c 'import os,sys;print(os.path.abspath(sys.argv[1]))' "$daemon")
gs=$(python3 -c 'import os,sys;print(os.path.abspath(sys.argv[1]))' "$gs")
# No user config or secrets are inherited by either fixture process.
python3 - "$daemon" "$gs" <<'PY'
import os, pathlib, subprocess, sys, tempfile
with tempfile.TemporaryDirectory(prefix='gitstored-process-') as tmp:
    os.chmod(tmp, 0o700)
    env={'PATH':'/nonexistent','HOME':tmp,'XDG_CONFIG_HOME':tmp,'XDG_STATE_HOME':tmp,'XDG_RUNTIME_DIR':tmp,'XDG_CACHE_HOME':tmp}
    for binary in sys.argv[1:]:
        for option in ('--help','--version'):
            result=subprocess.run([binary,option],cwd=tmp,env=env,capture_output=True,timeout=10)
            if result.returncode or not result.stdout or result.stderr:
                raise SystemExit(f'FAIL: {pathlib.Path(binary).name} {option}')
    if list(pathlib.Path(tmp).iterdir()): raise SystemExit('Unexpected setup side effects')
print('PASS: isolated help/version; no configuration or runtime side effects')
PY
cd "$root"
GITSTORED_TEST_BINARY="$daemon" cargo test --locked -p gs --test daemon_setup -- --ignored

GITSTORED_TEST_BINARY="$daemon" cargo test --locked -p gs --test daemon_wire -- --include-ignored

GITSTORED_TEST_BINARY="$daemon" cargo test --locked -p gs --test daemon_validate -- --ignored

GITSTORED_TEST_BINARY="$daemon" cargo test --locked -p gs --test daemon_inspect -- --ignored

cargo test --locked -p gs --test runtime_credentials
if [ "$(uname -s)" = Linux ]; then
  python3 runtime/experiments/credential-broker/test_broker.py \
    --probe "$(dirname "$daemon")/credential_probe" --gs "$gs"
fi
