"""Native acceptance must not silently adopt a different host or account."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('native_acceptance',Path(__file__).with_name('hosted-native-acceptance.py'))
acceptance=importlib.util.module_from_spec(spec);spec.loader.exec_module(acceptance)


class NativeFixtureSelection(unittest.TestCase):
    def settings(self,name):
        return dict(fixtureName=name,dedicatedTestAccount=True,disposableUid=11000)

    def test_supported_hosts_require_their_matching_explicit_selection(self):
        for kind,name,distribution,label in (
                ('hosted-ubuntu24','github-hosted-ubuntu24-123',dict(ID='ubuntu',VERSION_ID='24.04'),'Ubuntu'),
                ('apple-fedora44','apple-fedora44-native-arm64-20261109',dict(ID='fedora',VERSION_ID='44'),'Fedora'),
                ('hosted-fedora44','github-hosted-fedora44-123',dict(ID='fedora',VERSION_ID='44'),'Fedora')):
            with self.subTest(kind=kind):
                self.assertEqual(acceptance.select_fixture(self.settings(name),kind,distribution),label)

    def test_fedora_cannot_enter_through_the_default_ubuntu_lane(self):
        with self.assertRaises(ValueError):
            acceptance.select_fixture(self.settings('apple-fedora44-native-arm64-20261109'),
                'hosted-ubuntu24',dict(ID='fedora',VERSION_ID='44'))

    def test_actual_distribution_and_version_override_manifest_claims(self):
        settings=self.settings('github-hosted-fedora44-123')
        for distribution in (dict(ID='ubuntu',VERSION_ID='24.04'),dict(ID='fedora',VERSION_ID='43'),
                dict(ID='fedora',VERSION_ID='45'),{}):
            with self.subTest(distribution=distribution),self.assertRaises(ValueError):
                acceptance.select_fixture(settings,'hosted-fedora44',distribution)

    def test_unselected_accounts_and_unknown_kinds_are_rejected(self):
        settings=self.settings('github-hosted-fedora44-123')
        for invalid in ({**settings,'disposableUid':0},{**settings,'disposableUid':1000},
                {**settings,'dedicatedTestAccount':False},{**settings,'fixtureName':None}):
            with self.subTest(invalid=invalid),self.assertRaises(ValueError):
                acceptance.select_fixture(invalid,'hosted-fedora44',dict(ID='fedora',VERSION_ID='44'))
        with self.assertRaises(ValueError):
            acceptance.select_fixture(settings,'automatic',dict(ID='fedora',VERSION_ID='44'))


if __name__=='__main__':
    unittest.main()
