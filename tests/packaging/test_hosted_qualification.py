"""Hosted qualification never adopts a workstation, foreign run or root product account."""
import importlib.util
import os
import platform
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

def module(name,filename):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tests/packaging'/filename)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

outer=module('hosted_qualification','run-hosted-qualification.py')
inner=module('prepared_qualification','prepare-qualification.py')


class HostedQualification(unittest.TestCase):
    @unittest.skipIf(os.geteuid() == 0 or not shutil.which('git'), 'requires non-root Git fixture tooling')
    def test_public_bundle_setup_escapes_an_inaccessible_runner_working_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve()
            account=root/'fixture';account.mkdir(mode=0o700)
            environment=dict(PATH=os.environ['PATH'],HOME=str(account),LANG='C',
                GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL=os.devnull)
            def git(*arguments,cwd=root):
                return subprocess.check_output(['git',*map(str,arguments)],cwd=cwd,env=environment,stderr=subprocess.PIPE)
            source=root/'source'
            git('init','--initial-branch=main',source)
            git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid',
                'commit','--allow-empty','-m','public fixture',cwd=source)
            revision=git('rev-parse','HEAD',cwd=source).decode().strip()
            bundle=account/'source.bundle';git('bundle','create',bundle,'HEAD',cwd=source)
            runner=root/'runner';runner.mkdir();work=runner/'work';work.mkdir()
            user=['env','-i',*[key+'='+value for key,value in environment.items()]]
            previous=Path.cwd()
            try:
                os.chdir(work);runner.chmod(0)
                broken=subprocess.run([*user,'git','clone','--no-checkout',str(bundle),str(account/'broken')],
                    capture_output=True,timeout=15)
                self.assertNotEqual(broken.returncode,0)
                outer.run_fixture_setup(user,[
                    ['git','clone','--no-checkout',str(bundle),str(account/'source')],
                    ['git','-C',str(account/'source'),'checkout','--detach',revision]],str(account))
            finally:
                runner.chmod(0o700);os.chdir(previous)
            self.assertEqual(git('rev-parse','HEAD',cwd=account/'source').decode().strip(),revision)

    def environment(self):
        return dict(GITHUB_ACTIONS='true',RUNNER_ENVIRONMENT='github-hosted',GITHUB_REPOSITORY_ID='1431005655',
            GITHUB_RUN_ID='123',GITHUB_SHA='a'*40,RUNNER_ARCH='X64')

    def test_supported_native_host_architectures(self):
        environment=self.environment()
        self.assertEqual(outer.host_selection(environment,'linux','x86_64',1001),'x64')
        self.assertEqual(outer.host_selection({**environment,'RUNNER_ARCH':'ARM64'},'darwin','arm64',501),'arm64')
        self.assertEqual(inner.selected(environment,'linux','aarch64',11000),('linux','arm64'))
        self.assertEqual(inner.selected(environment,'darwin','x86_64',501),('darwin','x64'))

    def test_refuses_foreign_persistent_root_and_architecture_mismatches(self):
        environment=self.environment()
        for key,value in [('GITHUB_ACTIONS','false'),('RUNNER_ENVIRONMENT','self-hosted'),
                ('GITHUB_REPOSITORY_ID','1'),('GITHUB_RUN_ID','0'),('GITHUB_SHA','main'),('RUNNER_ARCH','ARM64')]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                outer.host_selection({**environment,key:value},'linux','x86_64',1001)
        with self.assertRaises(ValueError):outer.host_selection(environment,'linux','x86_64',0)
        with self.assertRaises(ValueError):inner.selected(environment,'linux','x86_64',1001)
        with self.assertRaises(ValueError):inner.selected(environment,'darwin','arm64',0)

    def test_unselected_cli_fails_before_input_reads_or_output_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();output=root/'must-not-exist'
            reply=subprocess.run([sys.executable,str(ROOT/'tests/packaging/run-hosted-qualification.py'),
                '--candidate',str(root/'absent'),'--controls',str(root/'absent-controls'),
                '--host','ubuntu:24.04','--tag','v0.1.0-alpha.1','--output',str(output)],
                env={**os.environ,**self.environment(),'RUNNER_ENVIRONMENT':'personal'},
                capture_output=True,text=True,timeout=15)
            self.assertNotEqual(reply.returncode,0)
            self.assertIn('fresh same-repository GitHub-hosted',reply.stderr)
            self.assertFalse(output.exists())

    def test_branch_ref_cannot_enter_tag_qualification_even_with_correct_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();output=root/'must-not-exist'
            environment={**os.environ,**self.environment(),
                'RUNNER_ARCH':'X64' if platform.machine()=='x86_64' else 'ARM64',
                'GITHUB_SHA':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                'GITHUB_REF':'refs/heads/codex/unqualified'}
            reply=subprocess.run([sys.executable,str(ROOT/'tests/packaging/run-hosted-qualification.py'),
                '--candidate',str(root/'absent'),'--controls',str(root/'absent-controls'),
                '--host','ubuntu:24.04','--tag','v0.1.0-alpha.1','--output',str(output)],
                env=environment,capture_output=True,text=True,timeout=15)
            self.assertNotEqual(reply.returncode,0)
            self.assertIn('actual reviewed main/tag source',reply.stderr)
            self.assertFalse(output.exists())


if __name__=='__main__':unittest.main()
