#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cargo build --locked -p gs
exec python3 scripts/test_remote.py
