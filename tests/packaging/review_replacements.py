"""Select exact local-only next-alpha inputs for controlled package-manager checks."""
import hashlib
from pathlib import Path


def arguments(root, record, os_name, arch, channel):
    version = record.get('replacementVersion')
    if version is None:
        return []
    if os_name not in ('linux','darwin') or arch not in ('x64','arm64'):
        raise ValueError('unexpected replacement platform')
    if channel not in ('archive','npm','homebrew','deb','rpm'):
        raise ValueError('unexpected replacement channel')
    # Versions come from the same strict policy as all package names. Reject
    # traversal before constructing or reading any selected fixture path.
    from versions import validate_alpha_proposal
    validate_alpha_proposal(record['version'],version)
    suffix = {'archive':'tar.gz','homebrew':'tar.gz','npm':'tgz','deb':'deb','rpm':'rpm'}[channel]
    artifact = Path(root)/'replacements'/(os_name+'-'+arch)/'packages'/f'UNQUALIFIED-ONLY-{version}-{os_name}-{arch}.{suffix}'
    if artifact.is_symlink() or not artifact.is_file() or artifact.stat().st_size > 512*1024*1024:
        raise ValueError('select an ordinary bounded replacement package')
    result = ['--replacement',str(artifact),'--replacement-sha256',hashlib.sha256(artifact.read_bytes()).hexdigest(),
              '--replacement-version',version]
    if channel=='homebrew':
        formula = Path(root)/'gitstore-next.rb'
        if formula.is_symlink() or not formula.is_file() or formula.stat().st_size > 65536:
            raise ValueError('select an ordinary bounded replacement formula')
        result += ['--replacement-formula',str(formula),'--replacement-formula-sha256',hashlib.sha256(formula.read_bytes()).hexdigest()]
    return result
