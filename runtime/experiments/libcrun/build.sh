#!/bin/sh
set -eu
# Run in the isolated guest's experiments directory with pinned crun source built.
source_root=$PWD/crun-1.30.1
cc -std=c17 -Wall -Wextra -Werror -static payload.c -o payload-static
cd "$source_root"
./libtool --mode=link cc -std=c17 -Wall -Wextra -Werror -I. -Isrc -Ilibocispec/src ../probe.c libcrun.la -o ../probe-shared
./libtool --mode=link cc -std=c17 -Wall -Wextra -Werror -I. -Isrc -Ilibocispec/src ../probe.c libcrun.la -static -o ../probe-static
if [ "${BUILD_DELEGATED:-0}" = 1 ]; then
  ./libtool --mode=link cc -std=c17 -Wall -Wextra -Werror -DGSTD_PROBE_CGROUPS=1 -I. -Isrc -Ilibocispec/src ../probe.c libcrun.la -static -o ../probe-delegated
fi
