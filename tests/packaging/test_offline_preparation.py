"""Generated offline fixtures must satisfy the strict private manifest reader."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class OfflinePreparation(unittest.TestCase):
    @unittest.skipIf(os.geteuid()==0,'select a non-root contributor fixture')
    def test_generated_layouts_are_private_with_permissive_umask(self):
        # A subprocess isolates process-wide umask and the synthetic public pins.
        helper=Path(__file__).with_name('prepare-offline-cache.py').resolve()
        program=r'''
import hashlib, importlib.util, json, os, pathlib, sys
root=pathlib.Path(sys.argv[2]); cache=root/'cache'; cache.mkdir(mode=0o700)
image=cache/'image';image.mkdir(mode=0o700);(image/'blobs').mkdir(mode=0o700)
def descriptor(data):
    return dict(digest='sha256:'+hashlib.sha256(data).hexdigest(),size=len(data))
config=b'{}';layer=b'synthetic-public-layer'
manifest=dict(mediaType='application/vnd.oci.image.manifest.v1+json',config=descriptor(config),layers=[descriptor(layer)])
raw=json.dumps(manifest).encode()
for name,data in [('manifest.json',raw),('config.json',config),('blobs/layer-0',layer)]:
    (image/name).write_bytes(data);(image/name).chmod(0o600)
profile=root/'runtime/profiles';profile.mkdir(parents=True)
pin=dict(os='linux',architecture='arm64',digest=descriptor(raw)['digest'],configDigest=descriptor(config)['digest'])
(profile/'core.json').write_text(json.dumps(dict(images=[dict(component=name,platforms=[pin]) for name in ('api','git','controller')])))
spec=importlib.util.spec_from_file_location('offline',sys.argv[1]);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
module.ROOT=root;module.platform.machine=lambda:'aarch64'
sys.argv=['prepare-offline-cache.py','--cache',str(cache),'--output',str(root/'offline')]
os.umask(0o002);module.main()
'''
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();root.chmod(0o700)
            subprocess.run([sys.executable,'-c',program,str(helper),str(root)],check=True,capture_output=True)
            spec=importlib.util.spec_from_file_location('acceptance',helper.with_name('hosted-native-acceptance.py'))
            acceptance=importlib.util.module_from_spec(spec);spec.loader.exec_module(acceptance)
            layouts=acceptance.private_json(root/'offline/layouts.json')
            self.assertEqual(set(layouts),{'api','git','controller'})
            for path in (root/'offline').rglob('*'):
                self.assertEqual(path.stat().st_mode&0o777,0o700 if path.is_dir() else 0o600)


if __name__=='__main__':
    unittest.main()
