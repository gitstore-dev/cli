#!/usr/bin/env python3
"""Validate a source receipt, without network access or capability inference."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/packaging'))
from inventory import validate_capabilities


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('receipt', type=Path)
    args = parser.parse_args()
    try:
        value = json.loads(args.receipt.read_text())
        validate_capabilities(value)
    except (ValueError, OSError) as error:
        parser.exit(1, f'Invalid capability evidence: {error}\n')
    print(f'PASS: {len(value["commands"])} command records; source review only')


if __name__ == '__main__':
    main()
