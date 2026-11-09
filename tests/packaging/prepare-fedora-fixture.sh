#!/bin/sh
# Guest-root setup for one explicitly selected disposable VM; not product code.
set -eu
test "$(uname -s)" = Linux
test "$(id -u)" = 0
test "$(uname -n)" = gs-cli-alpha1-fedora44-native-20261109
test "${GITSTORE_DISPOSABLE_FIXTURE:-}" = gs-cli-alpha1-fedora44-native-20261109
. /etc/os-release
test "$ID:$VERSION_ID" = fedora:44
test -d /run/systemd/system
for tool in getent useradd install loginctl systemctl grep stat; do
    command -v "$tool" >/dev/null
done
! getent passwd gitstorefixture >/dev/null
! getent passwd 11000 >/dev/null
! getent group 11000 >/dev/null
test ! -e /home/gitstorefixture
useradd --uid 11000 --user-group --create-home --shell /bin/bash gitstorefixture
chmod 700 /home/gitstorefixture
install -d -m 700 -o 11000 -g 11000 /home/gitstorefixture/experiments
if test ! -e /dev/net/tun; then
    install -d -m 755 /dev/net
    mknod /dev/net/tun c 10 200
fi
test -c /dev/net/tun
chgrp 11000 /dev/net/tun
chmod 660 /dev/net/tun
loginctl enable-linger gitstorefixture
systemctl start user@11000.service
for controller in cpu memory pids; do
    grep -qw "$controller" /sys/fs/cgroup/user.slice/user-11000.slice/user@11000.service/cgroup.controllers
done
test "$(stat -c %u /run/user/11000)" = 11000
test "$(stat -c %a /run/user/11000)" = 700
test -S /run/user/11000/bus
printf '%s\n' 'Disposable Fedora VM prepared; product commands must run as UID 11000.'
