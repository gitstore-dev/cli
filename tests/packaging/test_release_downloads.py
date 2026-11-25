"""Release-build source downloads retry transient failures only and keep the digest check."""
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts'))
SPEC = importlib.util.spec_from_file_location('release_platform', ROOT/'scripts/build-release-platform.py')
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)
PAYLOAD = b'pinned source bytes'
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


class Downloads(unittest.TestCase):
    url = 'https://example.invalid/source.tar.gz'

    def response(self, url=None, payload=PAYLOAD):
        response = io.BytesIO(payload)
        response.url = url or self.url
        return response

    def error(self, code):
        return HTTPError(self.url, code, 'synthetic fixture failure', {}, io.BytesIO())

    def run_download(self, results, digest=DIGEST):
        with patch.object(build.urllib.request, 'urlopen', side_effect=results) as request, \
                patch.object(build.time, 'sleep') as sleep:
            try:
                return build.download(self.url, digest), request, sleep
            except Exception as error:
                return error, request, sleep

    def test_transient_failures_recover_with_growing_backoff(self):
        payload, request, sleep = self.run_download([self.error(503), URLError('dns'), self.response()])
        self.assertEqual(payload, PAYLOAD)
        self.assertEqual(request.call_count, 3)
        self.assertEqual([c.args[0] for c in sleep.call_args_list], [2, 4])
        self.assertEqual({c.args[0].full_url for c in request.call_args_list}, {self.url})

    def test_persistent_failure_is_bounded_and_names_the_url(self):
        error, request, sleep = self.run_download([self.error(503) for _ in range(9)])
        self.assertIsInstance(error, RuntimeError)
        self.assertIn(self.url, str(error))
        self.assertIn('HTTP 503', str(error))
        self.assertEqual(request.call_count, build.DOWNLOAD_ATTEMPTS)
        self.assertEqual(sleep.call_count, build.DOWNLOAD_ATTEMPTS - 1)

    def test_permanent_errors_and_https_downgrade_are_not_retried(self):
        for result, expected in ((self.error(403), RuntimeError), (self.error(404), RuntimeError),
                                 (self.response('http://example.invalid/source'), ValueError)):
            with self.subTest(result=result):
                error, request, sleep = self.run_download([result])
                self.assertIsInstance(error, expected)
                request.assert_called_once()
                sleep.assert_not_called()

    def test_digest_mismatch_is_fatal_and_never_retried(self):
        error, request, sleep = self.run_download([self.response(), self.response()], digest='0'*64)
        self.assertIsInstance(error, ValueError)
        self.assertIn('digest mismatch', str(error))
        request.assert_called_once()
        sleep.assert_not_called()

    def test_non_https_source_is_rejected_before_any_request(self):
        with patch.object(build.urllib.request, 'urlopen') as request:
            with self.assertRaises(ValueError):
                build.download('http://example.invalid/source', DIGEST)
            request.assert_not_called()


if __name__ == '__main__':
    unittest.main()
