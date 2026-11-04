#!/usr/bin/env python3
"""Normalize ONLY the owned fixture's journal ACL PAX headers; never extract files."""
import copy
import json
import sys
import tarfile

if len(sys.argv) != 3:
    raise SystemExit('usage: normalize-export.py OWNED_EXPORT NEW_NORMALIZED_TAR')
removed = []
with open(sys.argv[2], 'xb') as output:
    with tarfile.open(sys.argv[1], 'r|') as source, tarfile.open(fileobj=output, mode='w|', format=tarfile.PAX_FORMAT) as target:
        for original in source:
            entry = copy.copy(original)
            entry.pax_headers = original.pax_headers.copy()
            for key in ('SCHILY.xattr.system.posix_acl_access', 'SCHILY.xattr.system.posix_acl_default'):
                if key in entry.pax_headers:
                    if entry.name.removeprefix('./').rstrip('/') != 'var/log/journal':
                        raise SystemExit('Unexpected ACL-bearing path; fixture requires review')
                    del entry.pax_headers[key]
                    removed.append({'path': entry.name, 'attribute': key})
            target.addfile(entry, source.extractfile(original) if original.isfile() else None)
print(json.dumps({'removedFixtureJournalAclHeaders': removed}))
