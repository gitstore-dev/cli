# Data Model

No server resources or datastore are added. These records describe CLI/distribution state.

## CapabilityEvidence

Command IDs/family, milestone, upstream tag/full SHA, schema/resolver paths,
operation/selector/auth mapping, client source, classification (`implemented`,
`client-work`, `validation-work`, `external-contract`, `deferred`), required tests,
results and remaining gate. Submission and waits may have different classifications.
Issue closure or introspection alone cannot establish supported operations.

## ReleaseCandidate

SemVer, v-tag, source SHA, workflow/run identity, required artifact rows, upstream
profiles, changelog/disclaimer, receipts and state. Transitions: proposed →
version-reviewed → built → packaged → validated → attested → producer-verified →
published. Failure leaves an incomplete draft. Retries must match source/digests;
conflicting assets fail rather than replace published bytes.

## Artifact

Unique filename, format, OS/architecture, distro/ABI floor, package/application
versions, source SHA, executable hashes, library closure, notices, compressed and
unpacked sizes, checksum, subject digest, bundle filename and qualification receipt.
Both executables report the same full version. Digest is calculated after all
stripping/relocation/packaging. The real daemon payload is never a symlink/hardlink.

## PackageVersion

Canonical `0.1.0-alpha.1`; tag `v0.1.0-alpha.1`; npm uses canonical SemVer;
Debian `0.1.0~alpha.1-1`; RPM Version `0.1.0`, Release `0.alpha.1.1`.
Test alpha.1 < alpha.2 < stable 0.1.0 with native package comparators.
Homebrew declares the canonical version and tests replacement. Package rebuild
suffixes do not alter the reported application/socket version identity.

## InstalledLayout

Package prefix, real binary directory, private library directory, service template,
config/runtime/state roots, owner UID and application version. Global install owns
code; first local invocation creates user state. Remote commands do not instantiate
the daemon. Explicit socket selection remains connect-only. Running version
mismatch returns an actionable error, never automatic killing/replacement.
User state stays outside the package file inventory; this is layout design, not
a prerelease preservation guarantee.

## WatchCheckpoint

Installation/profile, kind, namespace/selector, opaque journal cursor, last emitted
event, connection state and retry budget. Connecting → streaming → reconnecting →
streaming or expired/failed/stopped. Cursors cannot cross installation/selection.
Expiry requires explicit resnapshot; do not hide event gaps. Bound frames/queues;
duplicates are allowed and no exactly-once delivery is promised.
