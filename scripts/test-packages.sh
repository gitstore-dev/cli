#!/bin/sh
set -eu
package_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$package_repo/tests/packaging/run.py" "$@"
