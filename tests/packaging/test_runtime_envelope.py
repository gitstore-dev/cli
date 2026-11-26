"""The release runtime fixture reads CLI v2 results; it only runs at release time, so pin it here."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('runtime_support',
    Path(__file__).resolve().parents[2]/'tests/runtime/support.py')
support = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(support)


class EnvelopeUnwrapping(unittest.TestCase):
    def test_v2_envelope_yields_its_data(self):
        accepted = {'deploymentId': 'cold', 'operationId': 'op-1'}
        envelope = {'apiVersion': 'cli.gitstore.dev/v2', 'kind': 'OperationResult',
                    'summary': {'operationId': 'op-1'}, 'data': accepted}
        self.assertEqual(support.v2_data(envelope), accepted)

    def test_raw_documents_pass_through_unchanged(self):
        for raw in ({'data': {'__typename': 'Query'}},          # raw GraphQL response
                    {'operationId': 'op-1'},                    # pre-v2 result
                    {'apiVersion': 'other/v1', 'data': {}},     # not this CLI's envelope
                    {'apiVersion': 'cli.gitstore.dev/v2'},      # no data member
                    [], None):
            with self.subTest(raw=raw):
                self.assertIs(support.v2_data(raw), raw)


if __name__ == '__main__':
    unittest.main()
