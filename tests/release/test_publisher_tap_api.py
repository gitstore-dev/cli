"""The formula adapter uses the scoped App token and rejects ambiguous API errors."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('publish_release',ROOT/'scripts/publish-release.py')
publisher=importlib.util.module_from_spec(spec);spec.loader.exec_module(publisher)


class PublisherTapAPI(unittest.TestCase):
    def test_app_token_stays_in_environment_and_request_is_structured_json(self):
        environment={'GH_TOKEN':'synthetic-release-token','GITSTORE_TAP_TOKEN':'synthetic-app-token'}
        with patch.dict(os.environ,environment),patch.object(publisher.subprocess,'run',
                return_value=subprocess.CompletedProcess([],0,b'{"sha":"fixture"}',b'')) as run:
            result=publisher.GitHub('v0.1.0-alpha.5').tap_api('git/refs',method='POST',payload={'ref':'refs/heads/fixture'})
            self.assertEqual(result,{'sha':'fixture'})
            args,kw=run.call_args
            self.assertEqual(kw['env']['GH_TOKEN'],'synthetic-app-token')
            self.assertNotIn('synthetic-app-token',str(args))
            self.assertNotIn('synthetic-release-token',str(args))
            self.assertEqual(json.loads(kw['input']),{'ref':'refs/heads/fixture'})
            self.assertEqual(os.environ['GH_TOKEN'],'synthetic-release-token')

    def test_only_explicit_missing_resource_returns_none(self):
        for status in [404,403,500]:
            with self.subTest(status=status),patch.dict(os.environ,{'GITSTORE_TAP_TOKEN':'synthetic'}),patch.object(
                    publisher.subprocess,'run',return_value=subprocess.CompletedProcess([],1,b'',f'failure (HTTP {status})'.encode())):
                if status==404:self.assertIsNone(publisher.GitHub('v0.1.0-alpha.5').tap_api('git/ref/heads/fixture',missing=True))
                else:
                    with self.assertRaises(RuntimeError):publisher.GitHub('v0.1.0-alpha.5').tap_api('git/ref/heads/fixture',missing=True)

    def test_missing_app_token_and_older_release_do_not_submit_a_proposal(self):
        client=publisher.GitHub('v0.1.0-alpha.5')
        with patch.dict(os.environ,{},clear=True),patch.object(publisher.subprocess,'run') as run:
            with self.assertRaises(ValueError):client.tap_api('git/refs',method='POST',payload={})
            run.assert_not_called()
        with patch.object(client,'api',return_value=[dict(tag_name='v0.1.0-alpha.6',draft=False,prerelease=True)]),patch.object(client,'tap_api') as api:
            with self.assertRaises(ValueError):client.update_tap('v0.1.0-alpha.5',b'formula')
            api.assert_not_called()


if __name__=='__main__':unittest.main()
