#!/usr/bin/env python3
"""Generate deterministic local-protocol seeds; no secrets or external traffic."""
import hashlib
import json
from pathlib import Path
import struct
import sys
root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1])
for name in ("protocol", "framer"):
    (out / name).mkdir(parents=True, exist_ok=True)
def save(name, payload):
    (out / name / hashlib.sha256(payload).hexdigest()).write_bytes(payload)
examples = json.loads((root / "tests/fixtures/daemon/examples.json").read_text())
seeds = [json.dumps(row["message"], separators=(",", ":"), ensure_ascii=False).encode() for row in examples]
seeds += [b"{}", b"[]", b"null", b'{"x":1,"\\u0078":2}', b'{"n":9007199254740992}',
          b'{"n":1.0}', b'{"n":1e0}', b'{"s":"\xff"}', b'{"s":"\\u0000"}',
          b'{"s":"unterminated', b'{"a":' + b'['*32 + b'0' + b']'*32+b'}',
          b'{"a":[' + b'0,'*4096 + b'0]}', b'{"s":"' + b'x'*262145 + b'"}',
          b'{"x":1}\x00', b'{"x":-9007199254740992}', b'{"x":NaN}',
          b'{"s":"\\ud800"}', b'{"x":' + b'9'*4096 + b'}']
for payload in seeds:
    save("protocol", payload)
    framed = struct.pack(">I", len(payload)) + payload
    save("framer", framed)
    save("framer", framed[:-1])
for payload in (b"\0\0\0\0", b"\xff\xff\xff\xff", struct.pack(">I", 1048577),
                b"\0", b"\0\0\0\x02{}\0\0\0\x02{}"):
    save("framer", payload)
print("Deterministic corpus:", {p.name: len(list(p.iterdir())) for p in out.iterdir() if p.is_dir()})
