# Developer-only disposable Fedora reference fixture; never an installed runtime dependency.
# Official Fedora 44 build, 2026-11-02; multiarch amd64/arm64 index.
# Source: fedora-cloud/docker-brew-fedora commit 9f8d995b33ecc9756007adfd66b81c18d1301c83.
FROM docker.io/library/fedora@sha256:15da97ed9aa94dd3d92c3925eb89520f6c80e07ea7ee24cba848d1f8b6912b3e
RUN dnf --assumeyes install systemd dbus-broker dbus-daemon shadow-utils \
    shadow-utils-subid python3 util-linux coreutils procps-ng iproute findutils \
    ca-certificates tar gzip acl && dnf clean all
ENV container=container
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
