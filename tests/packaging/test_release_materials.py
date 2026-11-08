"""Generated license subjects bind real inputs without prepublishing own kits."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'runtime/packaging'))
import release_materials


class ReleaseMaterials(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.inputs = self.root/'inputs'
        self.inputs.mkdir(mode=0o700)
        self.project = self.root/'project'
        env = dict(PATH=os.environ['PATH'],HOME=str(self.root),GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null')
        subprocess.run(['git','-c','init.templateDir=','init','--quiet',str(self.project)],env=env,check=True)
        (self.project/'source.c').write_text('/* synthetic tracked application */\n')
        subprocess.run(['git','-C',str(self.project),'add','source.c'],env=env,check=True)
        subprocess.run(['git','-C',str(self.project),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid',
            '-c','commit.gpgsign=false','commit','--quiet','-m','Source fixture'],env=env,check=True)
        (self.project/'untracked-private-config').write_text('synthetic-private-fixture\n')
        self.data = b'complete synthetic upstream source archive'
        (self.inputs/'source.tar').write_bytes(self.data)
        self.materials = [dict(path='source.tar',sha256=hashlib.sha256(self.data).hexdigest())]
        self.manifest = dict(components=[dict(id='library@1',correspondingSource=dict(generated='corresponding-source',
            releaseAsset='source-@VERSION@-@OS@-@ARCH@.tar.gz'),relinkKit=dict(generated='daemon-relink',
            releaseAsset='relink-@VERSION@-@OS@-@ARCH@.tar.gz'))])

    def generate(self,manifest=None,name='generated',relink=b'synthetic relink objects'):
        return release_materials.generate(manifest or self.manifest,self.materials,self.inputs,project=self.project,
            output=self.inputs/name,version='0.1.0-alpha.1',os_name='linux',arch='arm64',relink_bytes=relink)

    def test_source_and_objects_are_final_subjects_and_private_worktree_files_are_excluded(self):
        manifest,assets = self.generate()
        source = manifest['components'][0]['correspondingSource']
        data = assets[source['releaseAsset']][1]
        self.assertEqual(hashlib.sha256(data).hexdigest(),source['sha256'])
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            self.assertEqual(archive.extractfile('source.tar').read(),self.data)
            name = next(m.name for m in archive if m.name.startswith('project/'))
            with tarfile.open(fileobj=io.BytesIO(archive.extractfile(name).read())) as project:
                self.assertEqual(project.getnames(),['source.c'])
        _,again = self.generate(name='reproduced')
        self.assertEqual(assets,again)
        self.assertIn('generated',self.manifest['components'][0]['correspondingSource'])

    def test_missing_objects_unknown_selectors_and_name_collisions_fail_before_outputs(self):
        with self.assertRaises(ValueError):
            self.generate(relink=None)
        self.assertFalse((self.inputs/'generated').exists())
        for change in ('selector','collision'):
            manifest = copy.deepcopy(self.manifest)
            if change=='selector':
                manifest['components'][0]['relinkKit']['generated']='unknown'
            else:
                manifest['components'][0]['relinkKit']['releaseAsset']=manifest['components'][0]['correspondingSource']['releaseAsset']
            with self.subTest(change=change),self.assertRaises(ValueError):
                self.generate(manifest)

    def test_actual_archive_and_source_identity_are_both_required(self):
        native = self.root/'native'
        native.mkdir()
        (native/'library.a').write_bytes(b'!<arch>\nsynthetic library objects')
        definition = dict(name='library@1',sha256='@BUILD_SHA256@',license='MIT',source='https://example.invalid/pinned',
            delivery='static',buildArchive='library.a',sourceMaterial='source.tar',sourceSha256=self.materials[0]['sha256'])
        row = release_materials.resolve_closure([definition],native,self.inputs,sysroot=self.root,target='fixture')[0]
        self.assertEqual(row['sha256'],hashlib.sha256((native/'library.a').read_bytes()).hexdigest())
        definition['sha256']='0'*64
        with self.assertRaises(ValueError):
            release_materials.resolve_closure([definition],native,self.inputs,sysroot=self.root,target='fixture')
        definition['sha256']='@BUILD_SHA256@'
        (self.inputs/'source.tar').write_bytes(b'changed source')
        with self.assertRaises(ValueError):
            release_materials.resolve_closure([definition],native,self.inputs,sysroot=self.root,target='fixture')

    def test_changed_build_pin_and_unreviewed_link_input_cannot_reuse_old_notices(self):
        runtime=self.project/'runtime'
        runtime.mkdir()
        pin=dict(version='2.15.1',sha256=self.materials[0]['sha256'],url='https://example.invalid/pinned-source')
        lock=runtime/'dependencies.lock.json'
        lock.write_text(json.dumps(dict(dependencies=dict(jansson=pin))))
        definition=dict(name='jansson@2.15.1',sha256='@BUILD_SHA256@',license='MIT',source=pin['url'],delivery='static',
            dependencyPin='jansson',buildArchive='library.a',sourceMaterial='source.tar',sourceSha256=pin['sha256'])
        native=self.root/'native'
        native.mkdir()
        (native/'library.a').write_bytes(b'!<arch>\nsynthetic objects')
        (native/'CMakeFiles/gitstored.dir').mkdir(parents=True)
        link=native/'CMakeFiles/gitstored.dir/link.txt'
        link.write_text('cc -o gitstored library.a\n')
        release_materials.resolve_closure([definition],native,self.inputs,sysroot=self.root,target='fixture',project=self.project)
        link.write_text('cc -o gitstored library.a unexpected.a\n')
        with self.assertRaisesRegex(ValueError,'every actual linked archive'):
            release_materials.resolve_closure([definition],native,self.inputs,sysroot=self.root,target='fixture',project=self.project)
        pin['sha256']='0'*64
        lock.write_text(json.dumps(dict(dependencies=dict(jansson=pin))))
        with self.assertRaisesRegex(ValueError,'actual committed build pin'):
            release_materials.validate_source_pin(definition,self.project)
