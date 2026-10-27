# ADR 0001: Bounded JSON Protocol over a Local Socket

**Status**: accepted (design). **Date**: 2026-10-27.
**Owner**: CLI/daemon maintainers. **Evidence**: EVD-001, EVD-002, EVD-022.
**Dependencies**: DEP-008 resolved by this design; DEP-012 remains open.

## Context

Rust gs and C gitstored need a versioned per-user lifecycle interface with durable
operation identity, bounded parsing/streams, observable failure and explicit capability
gates. The socket grants host-profile authority and is separate from catalogue roles.

## Decision

Accept [local protocol 1.0](../contracts/daemon-protocol.md) as the canonical wire contract:
four-byte big-endian byte length and UTF-8 JSON objects over an owner-only Unix stream
socket. Authenticate OS peer identity before requests; no unauthenticated TCP transport.
Negotiate versions and capabilities before mutations, with independent error categories.

Use typed, hand-maintained C and Rust boundary structures derived from the canonical
method table, with shared request/response fixtures. No shared-memory C/Rust ABI is
required between client and daemon. Parser implementations/versions are selected and
audited during M2; they must reject duplicate keys, enforce limits and preserve exact
integer/identifier semantics. Bindings must own decoded memory within each process and
document every allocation/free and event-buffer lifetime. Sanitizers/fuzzing and cross-
language fixtures are required before considering generated bindings or claiming safety.

Keep protocol details solely in the canonical document. Its 22 fixtures, numeric limits,
quota/retention policy, method parameters, idempotency fingerprint, state model and error
mapping are normative design. Accept all planning bounds plus daemon-global queue/log
and history quotas: per-connection limits alone otherwise multiply unchecked. Minor
changes are additive/capability-gated; breaking semantics require a protocol major.

Request IDs correlate exchanges; dedupe keys correlate durable mutations. A wait deadline
does not cancel detached work. Persist intent and dedupe before acknowledgment, return
original identity for same-key replay, and conflict on changed parameters. Retention
expiry cannot permit unsafe writer replacement or eviction of active operation identity.
Canonical normalization excludes observation-only deadlines and includes semantic defaults.

## Alternatives

- Newline-delimited JSON: readable, but delimiter handling and stream framing remain a
  separate discipline. Length prefix makes the pre-allocation bound explicit.
- Protobuf with generated C/Rust bindings: a viable later alternative, but adds toolchain,
  schema generation and compatibility work without an existing executable consumer.
- Local HTTP/gRPC: supplies established transports but adds a service stack and does not
  remove peer authorization, quotas, durable idempotency or ownership obligations.
- Unversioned ad hoc strings: rejected because error, compatibility and framing behavior
  would be ambiguous across Rust/C and daemon upgrades.

## Consequences and feasibility limits

Human-readable JSON helps inspect fixtures; performance and small dependency footprint
are not measured. Hand-maintained mappings risk drift and require identical C/Rust fixture
coverage. Numeric bounds are selected safety policy, not proven throughput. Exactly-once
external side effects are not promised; the durable operation/ownership protocol prevents
blind replay and makes uncertain outcomes inspectable.

## Validation gate

DEP-008 is resolved for M0 design because operations, types, limits, versions, recovery
and examples are fixed. DEP-012 remains open: prove partial-frame reads/writes, invalid
UTF-8/JSON/duplicate keys, numeric bounds, unauthorized peers, mismatched versions, slow
consumer/global quota behavior and cursor expiry. Round-trip every fixture through both
languages, fuzz parsers, run C sanitizers, and interrupt acceptance/acknowledgment/restart
to verify identity and ownership. No such runtime test was executed in M0.
