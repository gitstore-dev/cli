# Developer-only disposable Fedora reference fixture; never an installed runtime dependency.
FROM quay.io/fedora/fedora@sha256:17c99e30075386f9385685a22ad1924e2c22faee5863272c0379c81b80b9d258
RUN dnf --assumeyes install systemd dbus-broker dbus-daemon shadow-utils \
    shadow-utils-subid python3 util-linux coreutils procps-ng iproute findutils \
    ca-certificates tar gzip acl && dnf clean all
ENV container=container
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
