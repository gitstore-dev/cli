"""Fedora review cannot select a local workstation or an unrelated runner."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT=Path(__file__).with_name('run-hosted-fedora.py').resolve()
spec=importlib.util.spec_from_file_location('hosted_fedora',SCRIPT)
hosted=importlib.util.module_from_spec(spec);spec.loader.exec_module(hosted)


class HostedFedora(unittest.TestCase):
    def environment(self):
        return dict(GITHUB_ACTIONS='true',RUNNER_ENVIRONMENT='github-hosted',
            GITHUB_REPOSITORY_ID='1431005655',GITHUB_RUN_ID='123',GITHUB_SHA='a'*40,RUNNER_ARCH='X64')

    def test_actual_architecture_must_match_the_hosted_runner(self):
        environment=self.environment()
        self.assertEqual(hosted.host_selection(environment,'linux','x86_64'),'x64')
        self.assertEqual(hosted.host_selection({**environment,'RUNNER_ARCH':'ARM64'},'linux','aarch64'),'arm64')
        with self.assertRaises(ValueError):
            hosted.host_selection(environment,'linux','aarch64')

    def test_foreign_persistent_and_unpinned_hosts_are_rejected(self):
        environment=self.environment()
        for field,value in (('GITHUB_ACTIONS','false'),('RUNNER_ENVIRONMENT','self-hosted'),
                ('GITHUB_REPOSITORY_ID','1'),('GITHUB_RUN_ID','-1'),('GITHUB_RUN_ID','1;other'),
                ('GITHUB_SHA','main'),('RUNNER_ARCH','ARM64')):
            with self.subTest(field=field,value=value),self.assertRaises(ValueError):
                hosted.host_selection({**environment,field:value},'linux','x86_64')
        with self.assertRaises(ValueError):
            hosted.host_selection(environment,'darwin','x86_64')

    def test_cli_refuses_unselected_host_before_inputs_or_docker_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();output=root/'must-not-exist'
            reply=subprocess.run([sys.executable,str(SCRIPT),'--candidate',str(root/'absent-candidate'),
                '--unsupported-daemon',str(root/'absent-daemon'),'--repository-probe',str(root/'absent-probe'),
                '--producer-run','123','--output',str(output)],capture_output=True,text=True,timeout=10,
                env={**os.environ,**self.environment(),'RUNNER_ENVIRONMENT':'personal'})
            self.assertNotEqual(reply.returncode,0)
            self.assertIn('ephemeral hosted Linux fixture',reply.stderr)
            self.assertFalse(output.exists())


if __name__=='__main__':
    unittest.main()
