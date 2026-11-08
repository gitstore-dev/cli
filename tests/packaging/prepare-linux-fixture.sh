#!/bin/sh
# Guest-root preparation ONLY for the explicitly authorized disposable Apple VM.
# Installed GitStore does not perform any of these host-administration actions.
set -eu
test "$(uname -s)" = Linux
test "$(id -u)" = 0
test "$(hostname)" = gs-cli-alpha1-linux-20261108
test "${GITSTORE_DISPOSABLE_FIXTURE:-}" = gs-cli-alpha1-linux-20261108
test -d /run/systemd/system
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install --yes build-essential pkg-config meson ninja-build cmake clang-18 \
    autoconf automake libtool python3 python3-venv python3-pip curl ca-certificates \
    libsystemd-dev libseccomp-dev libcap-dev libjson-c-dev libglib2.0-dev \
    libssl-dev libcurl4-openssl-dev libcrypt-dev uidmap dbus-user-session \
    libacl1-dev zlib1g-dev git
if getent passwd gitstorefixture >/dev/null; then
    test "$(id -u gitstorefixture)" = 11000
else
    ! getent passwd 11000 >/dev/null
    useradd --uid 11000 --user-group --create-home --shell /bin/bash gitstorefixture
fi
install -d -m 700 -o 11000 -g 11000 /home/gitstorefixture/experiments
test -c /dev/net/tun
chgrp 11000 /dev/net/tun
chmod 660 /dev/net/tun
if test -f /proc/sys/kernel/apparmor_restrict_unprivileged_userns; then
    sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
fi
loginctl enable-linger gitstorefixture
systemctl start user@11000.service
for controller in cpu memory pids; do
    grep -qw "$controller" /sys/fs/cgroup/user.slice/user-11000.slice/user@11000.service/cgroup.controllers
done
test "$(stat -c %u /run/user/11000)" = 11000
test "$(stat -c %a /run/user/11000)" = 700
echo 'Disposable guest preparation complete; product processes must run as UID 11000.'
