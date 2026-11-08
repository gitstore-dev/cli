#!/bin/sh
# Test infrastructure only: NEVER run on a personal or persistent runner.
set -eu
test "$(uname -s)" = Linux
test "$(id -u)" = 0
test "${GITHUB_ACTIONS:-}" = true
test "${RUNNER_ENVIRONMENT:-}" = github-hosted
test "${GITHUB_REPOSITORY_ID:-}" = 1431005655
test "${GITSTORE_DISPOSABLE_FIXTURE:-}" = github-hosted-ubuntu24
. /etc/os-release
test "$ID:$VERSION_ID" = ubuntu:24.04
test -d /run/systemd/system
test "$#" = 2
case "$1:$2" in /*:/*) ;; *) exit 2 ;; esac
test -x "$1/gs"
test -x "$1/gitstored"
# Refuse collisions rather than adopting an existing account or experiment.
! getent passwd gitstorefixture >/dev/null
! getent passwd 11000 >/dev/null
! getent group 11000 >/dev/null
test ! -e /home/gitstorefixture
export DEBIAN_FRONTEND=noninteractive
apt-get install --yes uidmap dbus-user-session
useradd --uid 11000 --user-group --create-home --shell /bin/bash gitstorefixture
chmod 700 /home/gitstorefixture
install -d -m 700 -o 11000 -g 11000 /home/gitstorefixture/experiments /home/gitstorefixture/bin
install -m 500 -o 11000 -g 11000 "$1/gs" "$1/gitstored" /home/gitstorefixture/bin/
# The hosted checkout's ancestor directories can be private to the runner user.
# Copy committed public test inputs; never widen runner-home permissions or copy
# its .git directory, token configuration, environment or arbitrary build tree.
test "$(git -c safe.directory="$2" -C "$2" rev-parse HEAD)" = "$GITHUB_SHA"
install -d -m 700 /home/gitstorefixture/source
git -c safe.directory="$2" -C "$2" archive --format=tar \
    --output=/home/gitstorefixture/test-inputs.tar HEAD \
    tests/runtime tests/packaging/check-linux-fixture.py runtime/profiles
tar -xf /home/gitstorefixture/test-inputs.tar -C /home/gitstorefixture/source
chown -Rh 11000:11000 /home/gitstorefixture/source
if test ! -e /dev/net/tun; then
    install -d -m 755 /dev/net
    mknod /dev/net/tun c 10 200
fi
test -c /dev/net/tun
chgrp 11000 /dev/net/tun
chmod 660 /dev/net/tun
if test -f /proc/sys/kernel/apparmor_restrict_unprivileged_userns; then
    sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
fi
loginctl enable-linger gitstorefixture
systemctl start user@11000.service
python3 - <<'PY'
import json, os, pathlib, sys
source=pathlib.Path('/home/gitstorefixture/source').resolve(strict=True)
assert (source/'tests/runtime/run.py').is_file()
revision=os.environ['GITHUB_SHA']
assert len(revision)==40 and all(c in '0123456789abcdef' for c in revision)
value=dict(schemaVersion=1,fixtureName='github-hosted-ubuntu24-'+os.environ['GITHUB_RUN_ID'],
    disposableUid=11000,dedicatedTestAccount=True,fixtureParent='/home/gitstorefixture/experiments',
    gs='/home/gitstorefixture/bin/gs',daemon='/home/gitstorefixture/bin/gitstored',
    sourceRoot=str(source),sourceRevision=revision,
    purpose='Ephemeral hosted core smoke only; not full package or release qualification')
path=pathlib.Path('/home/gitstorefixture/experiments/hosted.json')
fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
os.fchown(fd,11000,11000)
with os.fdopen(fd,'w') as stream:
    json.dump(value,stream,indent=2)
    stream.write('\n')
PY
printf '%s\n' 'Ephemeral hosted fixture prepared; product commands must run as UID 11000.'
