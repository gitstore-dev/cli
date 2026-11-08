"""Single application identity with native package-manager version encodings."""
import re

VERSION = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-alpha\.(0|[1-9][0-9]*))?')


def parse(value):
    if not isinstance(value, str) or not (match := VERSION.fullmatch(value)):
        raise ValueError('expected canonical numeric SemVer with optional alpha.N')
    return match.groups()


def encode(value, rebuild=1):
    major, minor, patch, alpha = parse(value)
    if type(rebuild) is not int or not 1 <= rebuild <= 9999:
        raise ValueError('package rebuild must be an integer from 1 to 9999')
    core = f'{major}.{minor}.{patch}'
    return {
        'application': value, 'npm': value, 'tag': 'v' + value,
        'debian': f'{core}~alpha.{alpha}-{rebuild}' if alpha is not None else f'{core}-{rebuild}',
        'rpmVersion': core,
        'rpmRelease': f'0.alpha.{alpha}.{rebuild}' if alpha is not None else str(rebuild),
        'cmake': core,
    }


def order_key(value):
    major, minor, patch, alpha = parse(value)
    return (int(major), int(minor), int(patch), alpha is None,
            int(alpha) if alpha is not None else 0)


def validate_alpha_proposal(previous, proposed):
    old = parse(previous)
    new = parse(proposed)
    if (old[3] is None or new[3] is None or new[:3] != old[:3] or
            int(new[3]) != int(old[3]) + 1):
        raise ValueError('expected the next alpha of the reviewed version series; stable/core changes require explicit policy review')
    return encode(proposed)
