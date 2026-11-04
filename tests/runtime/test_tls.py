"""Actual release CLI TLS checks against explicitly owned synthetic HTTPS peers."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import unittest


class Tls(unittest.TestCase):
    def test_release_client_accepts_trusted_ca_and_rejects_unknown_ca(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_text())
        root = Path(tempfile.mkdtemp(prefix="runtime-cli-tls-", dir=settings["fixtureParent"]))
        root.chmod(0o700)
        print(f"Retained CLI TLS fixture: {root}", flush=True)
        empty = root / "empty-certs"
        empty.mkdir(mode=0o700)
        environment = {"PATH": "/nonexistent", "HOME": str(root), "LANG": "C",
                       "SSL_CERT_DIR": str(empty)}
        peer = subprocess.Popen([settings["registryFixture"], "serve-cli-tls-fixture"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, env=environment)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(peer.stdout, selectors.EVENT_READ)
                self.assertTrue(ready.select(timeout=5), "TLS fixture startup deadline")
                line = peer.stdout.readline(16385)
            self.assertLessEqual(len(line), 16384)
            metadata = json.loads(line)
            def protected(path, text):
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                with os.fdopen(fd, "w") as stream:
                    stream.write(text)
            protected(root / "ca.pem", "".join(metadata["certificates"]))
            protected(root / "untrusted.pem", metadata["certificates"][1])
            protected(root / "token", "synthetic-cli-tls-only")
            protected(root / "config.toml", f'''version = 1
current_context = "tls"
[contexts.tls]
endpoint = "{metadata['source']}/graphql"
contract_profile = "m0-2026-10-27"
account = "fixture"
[contexts.tls.accounts.fixture]
issuer = "synthetic"
''')
            arguments = [settings["gs"], "--config", str(root / "config.toml"),
                         "--credential-file", str(root / "token"), "--non-interactive",
                         "--timeout", "5s", "-o", "json", "api", "graphql", "-f", "query={__typename}"]
            environment["SSL_CERT_FILE"] = str(root / "ca.pem")
            trusted = subprocess.run(arguments, env=environment, capture_output=True, timeout=10)
            self.assertEqual(trusted.returncode, 0, "Trusted fixture CA was rejected")
            self.assertEqual(json.loads(trusted.stdout), {"data": {"__typename": "Query"}})
            environment["SSL_CERT_FILE"] = str(root / "untrusted.pem")
            untrusted = subprocess.run(arguments, env=environment, capture_output=True, timeout=10)
            self.assertNotEqual(untrusted.returncode, 0, "Unknown CA was accepted")
            self.assertEqual(untrusted.stdout, b"")
            self.assertNotIn(b"synthetic-cli-tls-only", untrusted.stderr)
        finally:
            peer.stdin.close()
            try:
                status = peer.wait(timeout=3)
            except subprocess.TimeoutExpired:
                peer.kill()
                peer.wait(timeout=3)
                raise AssertionError("Owned TLS fixture shutdown deadline") from None
            self.assertEqual(status, 0, "TLS fixture did not observe the expected request")
            peer.stdout.close()


if __name__ == "__main__":
    unittest.main()
