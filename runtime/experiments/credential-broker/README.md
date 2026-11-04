# Private credential broker checkpoint

The daemon uses the installed `gs` executable's private broker entry point; no
third-party executable or additional distributed broker binary is introduced.
Rust code reuses the existing OS credential store implementation. Registry scope
is an exact HTTPS origin and repository, in a separate versioned key domain from
GraphQL/Git. Bootstrap-user, controller-enrollment and controller-signing purposes
cannot carry registry scope or alias one another. Installation, issuer and account
remain part of every key. No API token is substituted for a missing registry key.

This is an internal interface, not new public registry credential CLI syntax.
Only read brokerage is implemented here; bootstrap generation/persistence and
profile credential-reference wiring belong to the subsequent bootstrap tasks.

## Descriptor and process contract

The Linux worker receives a registered installed broker executable FD, verifies
regular-file/owner/mode/single-link/ELF constraints and executes that FD. The caller
must establish the installed build/source identity; owner/mode alone does not
authenticate an arbitrary executable. PATH is never searched. Unrelated FDs are
closed, standard streams point to `/dev/null`, core dumps are disabled and the
broker is reaped or killed at the parent's monotonic deadline. This worker fork
must occur before threads, with default SIGCHLD handling and no competing
reaper; the control owner does not invoke credential libraries.

FD 3 is a same-UID Unix stream. One big-endian uint32 byte length precedes a JSON
request (1–8192 bytes); the caller half-closes writes before lookup. Fields:

```json
{"identity":{"installation":"local-fixture","issuer":"fixture-issuer","account":"fixture-account","purpose":"registry-pull","registry":"https://ghcr.io","repository":"gitstore-dev/api"},"source":{"kind":"os-store"}}
```

The explicit fallback is `source: {kind: "protected-file", path: "/absolute/path"}`.
The file must be singly linked, regular, user-owned, mode 0600 or stricter and
1–16384 bytes. Symlinks, special files and changed identity/metadata during the read
are rejected. Failures never select another source. All secret inputs are UTF-8
single-line material; a final line ending is removed. An OS entry supplies the
same opaque secret format. Registry auth interpretation belongs to its scoped
consumer, not ambient Docker configuration or a credential helper.

Response: one status byte (0 secret, 1 absent, 2 denied), big-endian uint32 size,
then secret bytes only for status 0 (1–16384 bytes). Errors carry no input or native
diagnostic detail. C secret buffers are explicitly erased on release and Rust uses
zeroizing secret buffers. No secret is sent through argv, URLs or normal output.
Socket operations are bounded; the standalone private process also has a ten-second
absolute alarm, even if a caller trickles bytes.

The launcher copies no ambient environment. Its Linux OS-store session address is
only `/run/user/UID/bus`; other session-bus layouts currently need explicit protected
file input. The existing Linux store refuses locked/ambiguous matches and never
calls Prompt/Unlock. The macOS broker disables Keychain interaction. OS access
denial is an error, with no automatic fallback or prompt.

## Evidence and limits

Four Rust tests pass on macOS and Linux arm64, including a real `gs` process with
synthetic protected input and output solely on its inherited descriptor. C tests
cover malformed/oversized response, timeout, descriptor type, secret release and
Linux executable-FD invocation with unrelated descriptor/environment sentinels.
The Linux end-to-end runner below passes with the actual compiled `gs`, installed
as a single-link copy in a disposable private directory. The initial attempt with
Cargo's hard-linked development artifact was correctly refused; no validation rule
was relaxed. Mode-0644 protected input is refused.

```sh
python3 runtime/experiments/credential-broker/test_broker.py \
  --probe build/daemon-image-sdk/credential_probe \
  --gs /absolute/path/to/gs
```

No real user OS credentials were read/written and native OS-store availability is
not established by these checks. Product worker dispatch, credential reference
configuration and provisioning still need integration. macOS C worker launch is
explicitly unsupported; this task's native runtime launcher targets Linux.
