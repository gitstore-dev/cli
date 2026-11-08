#!/bin/sh
# Build exact production binaries; never provision services or publish artifacts.
set -eu
candidate_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
candidate_build=""
candidate_prefix=""
candidate_support=""
candidate_source_cache=""
candidate_curl=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) candidate_build=$2; shift 2 ;;
        --prefix) candidate_prefix=$2; shift 2 ;;
        --share-dir) candidate_support=$2; shift 2 ;;
        --source-cache) candidate_source_cache=$2; shift 2 ;;
        --static-curl-prefix) candidate_curl=$2; shift 2 ;;
        *) echo 'Usage: sh scripts/build-release-candidate.sh --build-dir ABS --prefix NEW_ABS --share-dir ABS' >&2; exit 2 ;;
    esac
done
for candidate_path in "$candidate_build" "$candidate_prefix" "$candidate_support"; do
    case "$candidate_path" in /*) ;; *) echo 'Select absolute build, new prefix and share directories' >&2; exit 2 ;; esac
done
if [ -e "$candidate_prefix" ] || [ -L "$candidate_prefix" ]; then
    echo 'Candidate prefix must not exist' >&2
    exit 2
fi
if [ ! -d "$candidate_support/notices" ] || [ ! -d "$candidate_support/reference" ] || [ ! -d "$candidate_support/completions" ]; then
    echo 'Generate reviewed notices, reference and completion directories first' >&2
    exit 2
fi
cd "$candidate_repo"
export LIBZ_SYS_STATIC=1
if [ "$(uname -s)" = Darwin ]; then
    export MACOSX_DEPLOYMENT_TARGET=15.0
fi
cargo build --workspace --release --locked
if [ -n "$candidate_source_cache" ]; then
    case "$candidate_source_cache" in /*) ;; *) echo 'Source cache must be absolute' >&2; exit 2 ;; esac
fi
case "$(uname -s)" in
    Linux)
        case "$candidate_curl" in /*) ;; *) echo 'Select the reviewed static curl prefix' >&2; exit 2 ;; esac
        cmake -S "$candidate_repo/daemon" -B "$candidate_build" \
            -DCMAKE_BUILD_TYPE=MinSizeRel -DGITSTORED_TESTING=OFF \
            -DGITSTORED_IMAGE_SDK=ON -DGITSTORED_LIBCRUN=ON \
            -DGITSTORED_SLIRP=ON -DGITSTORED_BOOTSTRAP=ON \
            -DGITSTORED_STATIC_DISTRIBUTION=ON \
            "-DGITSTORED_STATIC_CURL_PREFIX=$candidate_curl" \
            "-DGITSTORED_SOURCE_CACHE=$candidate_source_cache"
        ;;
    Darwin)
        cmake -S "$candidate_repo/daemon" -B "$candidate_build" \
            -DCMAKE_BUILD_TYPE=MinSizeRel -DGITSTORED_TESTING=OFF \
            -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
            -DGITSTORED_IMAGE_SDK=OFF -DGITSTORED_LIBCRUN=OFF \
            -DGITSTORED_SLIRP=OFF -DGITSTORED_BOOTSTRAP=OFF \
            "-DGITSTORED_SOURCE_CACHE=$candidate_source_cache"
        ;;
    *) echo 'Unsupported production candidate host' >&2; exit 2 ;;
esac
cmake --build "$candidate_build" --parallel 2
install -d -m 755 "$candidate_prefix/bin"
install -m 755 "$candidate_repo/target/release/gs" "$candidate_build/gitstored" "$candidate_prefix/bin/"
for candidate_binary in gs gitstored; do
    python3 "$candidate_repo/runtime/packaging/closure.py" --binary "$candidate_prefix/bin/$candidate_binary"
done
cp -R "$candidate_support" "$candidate_prefix/share"
echo 'Unqualified production staging complete; closure, inventory and package acceptance are required.'
