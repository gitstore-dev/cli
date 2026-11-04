#!/bin/sh
set -eu
runtime_test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$runtime_test_root/tests/runtime/run.py" "$@"
