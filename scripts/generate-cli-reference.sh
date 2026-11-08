#!/bin/sh
set -eu
test "$#" -eq 2 || { echo 'Usage: sh scripts/generate-cli-reference.sh ABS_GS NEW_ABS_SHARE' >&2; exit 2; }
case "$1" in /*) ;; *) exit 2 ;; esac
case "$2" in /*) ;; *) exit 2 ;; esac
test -x "$1"
test ! -e "$2"
test ! -L "$2"
install -d -m 755 "$2/reference" "$2/completions"
"$1" reference > "$2/reference/gs.md"
for shell in bash zsh fish; do
    "$1" completion "$shell" > "$2/completions/gs.$shell"
done
