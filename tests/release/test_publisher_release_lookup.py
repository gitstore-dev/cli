"""Draft discovery uses GitHub's documented list/by-ID contract, never tag lookup."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('draft_publisher', ROOT / 'scripts/publish-release.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class ReleaseLookup(unittest.TestCase):
    def setUp(self):
        self.tag = 'v0.1.0-alpha.5'
        self.release = dict(id=42, tag_name=self.tag, draft=True, prerelease=True, assets=[], body='notice')
        self.client = publisher.GitHub(self.tag)

    def test_draft_and_published_releases_resolve_by_id_and_stay_pinned(self):
        for draft in (True, False):
            client = publisher.GitHub(self.tag)
            release = {**self.release, 'draft': draft}
            replies = [[release], release, {'sha': 'a' * 40}, release, {'sha': 'a' * 40}]
            with self.subTest(draft=draft), patch.object(client, 'api', side_effect=replies) as api:
                self.assertEqual(client.inspect()['draft'], draft)
                client.inspect()
                self.assertEqual([c.args[0] for c in api.call_args_list], [
                    'releases?per_page=100&page=1', 'releases/42', 'commits/' + self.tag,
                    'releases/42', 'commits/' + self.tag])

    def test_draft_on_a_later_page_is_found(self):
        previous = [dict(id=i + 100, tag_name='other-' + str(i)) for i in range(100)]
        with patch.object(self.client, 'api', side_effect=[previous, [self.release], self.release,
                {'sha': 'a' * 40}]) as api:
            self.client.inspect()
            self.assertEqual(api.call_args_list[1].args[0], 'releases?per_page=100&page=2')

    def test_missing_ambiguous_and_invalid_id_never_select_a_release(self):
        for listing in ([], [self.release, {**self.release, 'id': 43}],
                [{**self.release, 'id': True}], [{**self.release, 'id': 0}]):
            with self.subTest(listing=listing), patch.object(self.client, 'api', return_value=listing) as api:
                with self.assertRaises(ValueError):
                    self.client.inspect()
                self.assertEqual(api.call_count, 1)
                self.assertFalse(hasattr(self.client, 'release_id'))

    def test_incomplete_bounded_listing_fails_without_guessing(self):
        page = [dict(id=i + 100, tag_name='other-' + str(i)) for i in range(100)]
        with patch.object(self.client, 'api', return_value=page) as api:
            with self.assertRaises(ValueError):
                self.client.inspect()
            self.assertEqual(api.call_count, 10)
            self.assertFalse(hasattr(self.client, 'release_id'))

    def test_upload_uses_pinned_id_and_an_exact_binary_body(self):
        self.client.release_id = 42

        def upload(*args):
            self.assertEqual(args[:6], ('api',
                'https://uploads.github.com/repos/gitstore-dev/cli/releases/42/assets?name=SHA256SUMS',
                '--method', 'POST', '--header', 'Content-Type: application/octet-stream'))
            self.assertEqual(args[6], '--input')
            self.assertEqual(Path(args[7]).read_bytes(), b'exact approved bytes\x00\xff')
            self.assertNotIn('--clobber', args)
            return b'{}'

        with patch.object(self.client, 'gh', side_effect=upload) as gh:
            self.client.upload('SHA256SUMS', b'exact approved bytes\x00\xff')
            gh.assert_called_once()

    def test_upload_cannot_guess_an_uninspected_draft(self):
        with patch.object(self.client, 'gh') as gh:
            with self.assertRaises(ValueError):
                self.client.upload('SHA256SUMS', b'bytes')
            gh.assert_not_called()

    def test_notice_and_promotion_explicitly_retain_the_reviewed_tag(self):
        self.client.release_id = 42
        release = {**self.release, 'body': 'original release notes'}
        with patch.object(self.client, 'api', side_effect=[release, {}, {}]) as api:
            self.client.ensure_notice('required notice')
            self.client.promote()
            self.assertEqual(api.call_args_list[1].args, ('releases/42', dict(
                tag_name=self.tag, draft=True, prerelease=True,
                body='required notice\n\noriginal release notes')))
            self.assertEqual(api.call_args_list[2].args, ('releases/42', dict(
                tag_name=self.tag, draft=False, prerelease=True, make_latest='false')))


if __name__ == '__main__':
    unittest.main()
