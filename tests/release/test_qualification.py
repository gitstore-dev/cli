"""Qualification cannot silently use developer machines or foreign OS tuples."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('qualification',ROOT/'scripts/qualify-release.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Qualification(unittest.TestCase):
    def fixture(self):
        return dict(schemaVersion=1,dedicatedTestAccount=True,cleanHost=True,
            disposableHost=True,disposableUid=11000,selection=['linux','arm64','ubuntu:24.04'])

    def test_exact_clean_host_and_distribution_channels(self):
        with patch.object(module.sys,'platform','linux'), patch.object(module.platform,'machine',return_value='aarch64'), \
                patch.object(module.os,'geteuid',return_value=11000), \
                patch.object(module.platform,'freedesktop_os_release',return_value={'ID':'ubuntu','VERSION_ID':'24.04'}):
            self.assertEqual(module.selection('ubuntu:24.04',self.fixture()),
                ('linux','arm64',['archive','deb','homebrew','npm']))
            for key, value in [('cleanHost',False),('dedicatedTestAccount',False),
                    ('disposableHost',False),('disposableUid',1000),('selection',['linux','x64','ubuntu:24.04'])]:
                fixture = self.fixture()
                fixture[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError):
                    module.selection('ubuntu:24.04',fixture)
            with self.assertRaises(ValueError):
                module.selection('fedora:44',self.fixture())

    def test_newer_macos_is_not_evidence_for_macos15(self):
        fixture = self.fixture()
        fixture['selection'] = ['darwin','arm64','macos-15']
        with patch.object(module.sys,'platform','darwin'), patch.object(module.platform,'machine',return_value='arm64'), \
                patch.object(module.os,'geteuid',return_value=11000), \
                patch.object(module.platform,'mac_ver',return_value=('26.6.1',(),'')):
            with self.assertRaises(ValueError):
                module.selection('macos-15',fixture)

    def test_every_selected_host_channel_is_explicit(self):
        rows = module.qualification_rows()
        self.assertEqual(len(rows),22)
        self.assertIn(('linux','x64','fedora:44','rpm'),rows)
        self.assertNotIn(('linux','x64','fedora:44','deb'),rows)
        self.assertNotIn(('linux','x64','ubuntu:24.04','rpm'),rows)
