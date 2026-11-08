#!/bin/sh
set -eu
capability_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$capability_repo"
cargo test --locked -p gs --test alpha9_resources --test authoring --test export \
    --test auth_remote --test resource_catalog --test watch --test gates
cargo build --locked -p gs
exec python3 scripts/test_remote.py
