#!/usr/bin/env python3
"""Build private HTTP(S)-only curl from an explicitly acquired patched source set.

Producer tooling on Ubuntu; never installed or run on a consumer's machine.
Acquire with apt-get source curl=8.5.0-2ubuntu10.16 using authenticated Ubuntu
Sources metadata. No unpatched upstream fallback and no automatic downloads.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-cache', required=True, type=Path)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--prefix', required=True, type=Path)
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('build as the dedicated non-root producer account')
    for path in [args.source_cache, args.build_dir, args.prefix]:
        if not path.is_absolute() or path.is_symlink():
            parser.error('select absolute ordinary paths')
    if args.build_dir.exists() or args.prefix.exists():
        parser.error('build and prefix must be new directories')
    lock = json.loads((ROOT / 'packaging/curl-source.json').read_text())
    for name, digest in lock['files'].items():
        path = args.source_cache / name
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            parser.error('source identity mismatch: ' + name)
    args.build_dir.mkdir(mode=0o700)
    for name in lock['files']:
        shutil.copyfile(args.source_cache / name, args.build_dir / name)
    source = args.build_dir / 'source'
    subprocess.run(['dpkg-source', '--no-check', '-x',
                    str(args.build_dir / f"curl_{lock['version']}.dsc"), str(source)], check=True)
    # Source bytes are pinned above; dpkg-source applies the reviewed Ubuntu
    # security patch series. Disable protocols and libraries the daemon never uses.
    options = ['--disable-shared', '--enable-static', '--with-openssl',
        '--disable-ldap', '--disable-ldaps', '--without-libpsl', '--without-libidn2',
        '--without-libssh2', '--without-libssh', '--without-librtmp', '--without-gssapi',
        '--without-nghttp2', '--without-nghttp3', '--without-ngtcp2', '--without-brotli',
        '--without-zstd', '--without-zlib', '--disable-docs', '--disable-manual',
        '--disable-ntlm', '--disable-negotiate']
    options += ['--disable-' + protocol for protocol in
                ['ftp', 'file', 'dict', 'telnet', 'tftp', 'pop3', 'imap', 'smb',
                 'smtp', 'gopher', 'mqtt', 'rtsp']]
    subprocess.run([str(source / 'configure'), '--prefix=' + str(args.prefix),
                    *options], cwd=source, check=True)
    for command in [['make', '-C', 'lib', '-j2'], ['make', '-C', 'lib', 'install'],
                    ['make', '-C', 'include', 'install'], ['make', 'install-pkgconfigDATA']]:
        subprocess.run(command, cwd=source, check=True)
    library = args.prefix / 'lib'
    (library / 'libcurl.a').rename(library / 'libgitstored_curl.a')
    pc = library / 'pkgconfig/libcurl.pc'
    pc.write_text(pc.read_text().replace('-lcurl', '-lgitstored_curl'))
    shutil.copyfile(source / 'COPYING', args.prefix / 'COPYING')
    shutil.copyfile(ROOT / 'packaging/curl-source.json', args.prefix / 'source-lock.json')


if __name__ == '__main__':
    main()
