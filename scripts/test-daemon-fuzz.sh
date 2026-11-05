#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build/daemon-fuzz"
seconds=60
cache=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir) build=$2; shift 2 ;;
    --seconds) seconds=$2; shift 2 ;;
    --source-cache) cache=$2; shift 2 ;;
    *) echo 'Usage: scripts/test-daemon-fuzz.sh [--build-dir PATH] [--seconds 2..3600] [--source-cache PATH]' >&2; exit 2 ;;
  esac
done
case "$seconds" in ''|*[!0-9]*) exit 2 ;; esac
[ "$seconds" -ge 2 ] && [ "$seconds" -le 3600 ] || exit 2
: "${CC:=clang-18}"
cmake -S "$root/daemon" -B "$build" -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_BUILD_TYPE=Debug -DGITSTORED_TESTING=ON -DGITSTORED_SANITIZERS=ON \
  -DGITSTORED_FUZZING=ON -DGITSTORED_SOURCE_CACHE="$cache"
cmake --build "$build" --target fuzz_protocol fuzz_framer --parallel 2
python3 "$root/daemon/fuzz/make_corpus.py" "$build/corpus"
# The requested duration is divided between both harnesses. Inputs/findings and
# logs remain in the ignored build directory, never the source seed directory.
first=$((seconds / 2))
second=$((seconds - first))
mkdir -p "$build/findings"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
"$build/fuzz_protocol" "$build/corpus/protocol" -seed=1 -max_total_time="$first" \
  -max_len=1048576 -timeout=5 -rss_limit_mb=1024 -malloc_limit_mb=64 \
  -artifact_prefix="$build/findings/" > "$build/protocol.log" 2>&1
"$build/fuzz_framer" "$build/corpus/framer" -seed=1 -max_total_time="$second" \
  -max_len=1048580 -timeout=5 -rss_limit_mb=1024 -malloc_limit_mb=64 \
  -artifact_prefix="$build/findings/" > "$build/framer.log" 2>&1
tail -n 3 "$build/protocol.log" "$build/framer.log"
