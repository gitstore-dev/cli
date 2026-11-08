"""Release credentials must never consume PR/foreign/wrong-source build inputs."""
import copy
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('input_runs', ROOT / 'scripts/check-release-run.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InputRuns(unittest.TestCase):
    def setUp(self):
        self.run = dict(repository=dict(id=1431005655), head_repository=dict(id=1431005655),
            head_sha='a' * 40, path='.github/workflows/release.yml', status='completed',
            conclusion='success', event='push', head_branch='v0.1.0-alpha.1')

    def test_exact_tagged_success_only(self):
        module.validate(self.run, 'release.yml', 'a' * 40, 'v0.1.0-alpha.1')
        for field, value in [('repository', dict(id=1)), ('head_repository', dict(id=1)),
            ('head_sha', 'b' * 40), ('path', '.github/workflows/cli.yml'),
            ('status', 'in_progress'), ('conclusion', 'failure'),
            ('event', 'pull_request'), ('head_branch', 'main')]:
            run = copy.deepcopy(self.run)
            run[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                module.validate(run, 'release.yml', 'a' * 40, 'v0.1.0-alpha.1')

    def test_reference_qualification_requires_manual_run_same_source(self):
        self.run.update(path='.github/workflows/runtime.yml', head_branch='main')
        with self.assertRaises(ValueError):
            module.validate(self.run, 'runtime.yml', 'a' * 40, 'v0.1.0-alpha.1')
        self.run['event'] = 'workflow_dispatch'
        module.validate(self.run, 'runtime.yml', 'a' * 40, 'v0.1.0-alpha.1')
        self.run['head_branch'] = 'v0.1.0-alpha.1'
        module.validate(self.run, 'runtime.yml', 'a' * 40, 'v0.1.0-alpha.1')
        self.run['head_sha'] = 'b' * 40
        with self.assertRaises(ValueError):
            module.validate(self.run, 'runtime.yml', 'a' * 40, 'v0.1.0-alpha.1')
        self.run['head_sha'] = 'a' * 40
        self.run['head_branch'] = 'other'
        with self.assertRaises(ValueError):
            module.validate(self.run, 'runtime.yml', 'a' * 40, 'v0.1.0-alpha.1')

    def test_complete_alpha_tag_and_revision_are_required(self):
        for tag,revision in [('main','a'*40),('v0.1.0','a'*40),('v0.1.0-alpha.1','main')]:
            with self.subTest(tag=tag,revision=revision), self.assertRaises(ValueError):
                module.validate(self.run,'release.yml',revision,tag)


if __name__ == '__main__':
    unittest.main()
