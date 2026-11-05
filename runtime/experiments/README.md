# Runtime experiment sources

These are reproducible contributor probes and evidence, not installed runtime
assets or end-user commands. Generated binaries, image layers and raw logs belong
under ignored build/fixture directories. Only source, locks and bounded redacted
receipts belong in Git. For product use, read [the local runtime guide](../../docs/local-runtime.md).

| Experiment | Source and scope |
| --- | --- |
| EXP-003A | [libcrun](libcrun.md): ordinary C API, worker ownership and native lifecycle. |
| EXP-003B | [Image libraries](image-library.md), [image acquisition](images.md): maintained transfer/unpack, private credentials and trusted offline assets. |
| EXP-003C | [CLI provenance](cli-provenance/README.md): offline maintainer release-verification controls; no bundled gh. |
| EXP-004A | [Network library](network-library.md): rootless libslirp; CNI is a separate follow-up. |
| EXP-004B-local | [Volumes](local-volumes.md): owned retained local storage, exclusivity and disk failure. |
| EXP-005A | [Apple libraries](apple.md): bounded existing-service/guest feasibility and unsupported clean-host combinations. |
| EXP-005B | [Managed guest](macos-vm.md): Linux guest reuse and unqualified host delivery/control boundaries. |
| Native acceptance | [Recovery](recovery.md), [Scylla](scylladb.md): actual workload, migration and retained-writer evidence. |

Results apply to their exact source/artifact/host identities. An experiment pass
does not qualify another architecture, a clean end-user installation or a published
release. See [reference host qualification](../packaging/reference-hosts.md) for
fixture requirements and the [validation archive](../../docs/contributing/validation/local-runtime.md)
for controlled versus native results.
