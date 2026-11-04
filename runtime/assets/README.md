# Registered asset manifests

This is a non-secret internal descriptor contract, not a daemon method or a
user-selected registry/mount/command format. The JSON schema documents shapes;
native validation additionally compares every descriptor with the compiled
profile catalogue, selected component set, platform and exact digests. A caller
cannot authenticate a different source merely by recomputing its manifest hash.

`profileDigest` fingerprints the complete compiled profile input. `manifestDigest`
fingerprints the canonical descriptor object with that field omitted, including
platform, selected images and origin policy. Consent/cache activation must bind
this immutable identity. Unknown components/fields, duplicate selections, changed
source/digest/platform and unverified trust policy are refused before effects.
CLI-published manifests/packages require their own release evidence; these
upstream GitStore image descriptors require source/platform/digest integrity and
do not invent an upstream attestation prerequisite.

The core origin policy permits its exact GHCR registry and the explicitly
registered blob-only HTTPS origin `https://pkg-containers.githubusercontent.com`.
Anonymous metadata/first-layer routing observations for all six pinned core
component/platform references are recorded in
[the image experiments](../experiments/image-library/). No signed paths, query
strings or tokens are recorded. Blob origins never receive registry credentials.
Unknown token/blob destinations still fail closed; redirects do not authorize
additional origins. Policy is compiled into the profile and manifest digest, so
changing it invalidates prior consent identity. Read-only validation never
authorizes downloads or activation.

Declared code dependencies use ordinary build manifests and `origin=code`; they
are not independently discovered/downloaded libraries. External host/cache
components require a registered verifier and actual trusted identity/platform/ABI
observations. Metadata claiming compatibility is insufficient. No external network
executable/provider is selected. Network code uses the selected libslirp source
with its precise queue-access correction; runtime integration is still pending.

`gstd_dependency_asset_fd` can verify already-present **manifest bytes** from a
borrowed host/cache descriptor against a registered source/platform digest. It
requires a read-only regular file, a single link and trusted ownership/modes,
hashes at most 1 MiB through maintained SHA-256, and checks unchanged identity,
size/timestamps/ownership around the read. It records device/inode identity rather
than reopening a supplied pathname. `verified-manifest-content` is not complete
image verification or permission to execute; config/layers and activation remain
separate. Metadata alone cannot forge that observation: checking it re-observes
the same descriptor. No generic executable or native-library provider is selected.
