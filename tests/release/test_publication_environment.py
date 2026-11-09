"""Promotion cannot rely on an automatically created unprotected environment."""
import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('publication_environment',ROOT/'scripts/check-publication-environment.py')
policy=importlib.util.module_from_spec(spec);spec.loader.exec_module(policy)


class PublicationEnvironment(unittest.TestCase):
    def test_requires_actual_reviewer_rule_not_just_environment_or_timer(self):
        for rules in [[],[dict(type='wait_timer',wait_timer=30)],
                [dict(type='required_reviewers',reviewers=[])]]:
            with self.subTest(rules=rules),self.assertRaises(ValueError):
                policy.validate(dict(name='release-publication',protection_rules=rules))

    def test_accepts_configured_user_or_team_reviewers(self):
        for kind in ('User','Team'):
            policy.validate(dict(name='release-publication',protection_rules=[dict(type='required_reviewers',
                reviewers=[dict(type=kind,reviewer=dict(id=123))])]))

    def test_rejects_wrong_environment_or_malformed_reviewer(self):
        with self.assertRaises(ValueError):policy.validate(dict(name='other'))
        for kind,identity in [('User',0),('User',True),('Bot',123),('Team','123')]:
            with self.subTest(kind=kind,identity=identity),self.assertRaises(ValueError):
                policy.validate(dict(name='release-publication',protection_rules=[dict(type='required_reviewers',
                    reviewers=[dict(type=kind,reviewer=dict(id=identity))])]))


if __name__=='__main__':
    unittest.main()
