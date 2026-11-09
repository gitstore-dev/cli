"""Transient source-host failures are bounded; policy failures remain fatal."""
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

SPEC = importlib.util.spec_from_file_location('native_materials',
    Path(__file__).resolve().parents[2]/'scripts/collect-native-materials.py')
materials = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(materials)


class Downloads(unittest.TestCase):
    url = 'https://example.invalid/source.tar.gz'

    def response(self, url=None):
        response = io.BytesIO(b'pinned source bytes')
        response.url = url or self.url
        return response

    def error(self, code):
        return HTTPError(self.url, code, 'synthetic fixture failure', {}, io.BytesIO())

    def test_transient_failure_recovers_without_changing_selected_url(self):
        with patch.object(materials, 'urlopen', side_effect=[self.error(503), self.response()]) as request, \
                patch.object(materials.time, 'sleep') as sleep:
            self.assertEqual(materials.download(self.url), b'pinned source bytes')
            self.assertEqual(request.call_args_list, [unittest.mock.call(self.url, timeout=60)]*2)
            sleep.assert_called_once_with(1)

    def test_persistent_transient_failure_is_bounded(self):
        with patch.object(materials, 'urlopen', side_effect=[self.error(503) for _ in range(3)]) as request, \
                patch.object(materials.time, 'sleep') as sleep:
            with self.assertRaises(HTTPError):
                materials.download(self.url)
            self.assertEqual(request.call_count, 3)
            self.assertEqual(sleep.call_args_list, [unittest.mock.call(1), unittest.mock.call(2)])

    def test_permanent_error_and_https_downgrade_are_not_retried(self):
        for result, expected in ((self.error(404), HTTPError),
                                 (self.response('http://example.invalid/source'), ValueError)):
            with self.subTest(result=result), patch.object(materials, 'urlopen', side_effect=[result]) as request, \
                    patch.object(materials.time, 'sleep') as sleep:
                with self.assertRaises(expected):
                    materials.download(self.url)
                request.assert_called_once()
                sleep.assert_not_called()
