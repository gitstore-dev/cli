"""Unsigned review consumes explicit same-repository CI, never foreign artifacts."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('development_review',Path(__file__).resolve().parents[2]/'scripts/prepare-package-review.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DevelopmentReview(unittest.TestCase):
    def test_explicit_successful_public_ci_is_allowed_only_for_unsigned_review(self):
        value=dict(repository=dict(id=1431005655),head_repository=dict(id=1431005655),
            head_sha='a'*40,path='.github/workflows/cli.yml',status='completed',conclusion='success',event='pull_request')
        with patch.object(module.subprocess,'check_output',return_value=json.dumps(value).encode()):
            self.assertEqual(module.selected_run(1,'cli.yml'),value)
        for key,other in [('repository',dict(id=2)),('head_repository',dict(id=2)),
                ('head_sha','main'),('path','.github/workflows/release.yml'),('status','in_progress'),
                ('conclusion','failure'),('event','workflow_dispatch')]:
            changed=copy.deepcopy(value);changed[key]=other
            with self.subTest(key=key),patch.object(module.subprocess,'check_output',return_value=json.dumps(changed).encode()):
                with self.assertRaises(ValueError):
                    module.selected_run(1,'cli.yml')
        with patch.object(module.subprocess,'check_output') as fetch:
            with self.assertRaises(ValueError):
                module.selected_run(0,'cli.yml')
            fetch.assert_not_called()


if __name__=='__main__':
    unittest.main()
