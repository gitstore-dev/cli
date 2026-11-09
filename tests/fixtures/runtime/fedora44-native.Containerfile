# Developer-only disposable Apple Linux VM; never an installed runtime dependency.
FROM quay.io/fedora/fedora@sha256:526646a990bb8442f936827d98253677dd7a4c28fad33f938e32c7ffdadb72c1
RUN dnf --assumeyes install systemd dbus-broker dbus-daemon shadow-utils \
    shadow-utils-subid python3 util-linux coreutils procps-ng iproute findutils \
    ca-certificates tar gzip acl && dnf clean all
ENV container=container
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
