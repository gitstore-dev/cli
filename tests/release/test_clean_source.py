"""A release must not attribute uncommitted executable source to a clean Git SHA."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('platform_producer',ROOT/'scripts/build-release-platform.py')
producer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(producer)


class CleanSource(unittest.TestCase):
    def test_untracked_source_is_refused_before_build_or_material_download(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            environment = dict(PATH=os.environ['PATH'],HOME=str(root),GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null')
            subprocess.run(['git','-c','init.templateDir=','init','--quiet',str(root)],env=environment,check=True)
            (root/'untracked-production-source.c').write_text('/* synthetic source fixture */\n')
            output = root/'candidate'
            with patch.object(producer,'ROOT',root), patch.object(sys,'argv',['producer','--output',str(output)]), \
                    patch.dict(os.environ,environment,clear=True):
                with self.assertRaisesRegex(ValueError,'clean committed checkout'):
                    producer.main()
            self.assertFalse(output.exists())
