"""Actual GNU ELF finalization controls; synthetic executable, no release claim."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
from finalize_elf import finalize, finalize_record

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'linux' and all(shutil.which(t) for t in ('cc', 'strip', 'readelf')),
                     'requires Linux C compiler and GNU binutils')
class Finalization(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'fixture.c'
        self.source.write_text('#include <stdio.h>\nint main(void) { puts("fixture version 1"); return 0; }\n')
        self.obj = self.root / 'fixture.o'
        self.binary = self.root / 'fixture'
        subprocess.run(['cc', '-g', '-O0', '-c', str(self.source), '-o', str(self.obj)], check=True)
        subprocess.run(['cc', str(self.obj), '-Wl,-z,relro,-z,now', '-o', str(self.binary)], check=True)

    def inspect(self, *arguments):
        return subprocess.check_output(['readelf', *arguments, str(self.binary)])

    def test_debug_removed_but_execution_dynamic_symbols_hardening_and_objects_preserved(self):
        obj = self.obj.read_bytes()
        original = self.binary.read_bytes()
        dynamic = self.inspect('--dynamic')
        dynsym = self.inspect('--dyn-syms')
        headers = self.inspect('--program-headers')
        before = subprocess.check_output([str(self.binary)])
        self.assertIn(b'.debug_info', self.inspect('--sections'))
        result = finalize(self.binary)
        self.assertNotIn(b'.debug_', self.inspect('--sections'))
        self.assertIn(b'.symtab', self.inspect('--sections'))
        self.assertEqual(self.inspect('--dynamic'), dynamic)
        self.assertEqual(self.inspect('--dyn-syms'), dynsym)
        # Program-header flags/sizes and segment layout include RELRO/NX policy.
        self.assertEqual(self.inspect('--program-headers'), headers)
        self.assertEqual(subprocess.check_output([str(self.binary)]), before)
        self.assertEqual(self.obj.read_bytes(), obj)
        self.assertLess(result['bytes'], result['inputBytes'])
        self.assertEqual(result['inputSha256'], hashlib.sha256(original).hexdigest())
        self.assertEqual(result['sha256'], hashlib.sha256(self.binary.read_bytes()).hexdigest())
        # The retained object still builds and finalizes into a working ELF.
        rebuilt = self.root / 'rebuilt'
        subprocess.run(['cc', str(self.obj), '-Wl,-z,relro,-z,now', '-o', str(rebuilt)], check=True)
        finalize(rebuilt)
        self.assertEqual(subprocess.check_output([str(rebuilt)]), before)

    def test_symlinks_and_non_elf_inputs_reject(self):
        alias = self.root / 'alias'
        alias.symlink_to(self.binary)
        with self.assertRaises(OSError):
            finalize(alias)
        with self.assertRaises(ValueError):
            finalize(self.source)

    def test_incremental_build_reuses_unchanged_identity_and_updates_after_relink(self):
        record = self.root / 'finalization.json'
        first = finalize_record(self.binary, record)
        self.assertEqual(finalize_record(self.binary, record), first)
        self.source.write_text('int main(void) { return 42; }\n')
        subprocess.run(['cc', '-g', str(self.source), '-o', str(self.binary)], check=True)
        second = finalize_record(self.binary, record)
        self.assertNotEqual(first['sha256'], second['sha256'])
        self.assertEqual(json.loads(record.read_text()), second)
        self.assertEqual(subprocess.run([str(self.binary)]).returncode, 42)

    @unittest.skipIf(os.geteuid() == 0, 'relink producer requires a non-root account')
    def test_relink_kit_retains_objects_and_identifies_final_distributed_elf(self):
        build = self.root / 'build'
        build.mkdir()
        main = build / 'main.c.o'
        shutil.copyfile(self.obj, main)
        core = build / 'libgitstored_core.a'
        subprocess.run(['ar', 'cr', str(core), str(self.obj)], check=True)
        (build / 'CMakeCache.txt').write_text('GITSTORED_TESTING:BOOL=OFF\nGITSTORED_STATIC_DISTRIBUTION:BOOL=ON\n')
        link = build / 'CMakeFiles/gitstored.dir'
        link.mkdir(parents=True)
        (link / 'link.txt').write_text('/usr/bin/cc main.c.o libgitstored_core.a -o gitstored\n')
        subprocess.run(['/usr/bin/cc', 'main.c.o', 'libgitstored_core.a', '-o', 'gitstored'], cwd=build, check=True)
        binary = build / 'gitstored'
        finalization = finalize(binary)
        (build / 'finalization.json').write_text(json.dumps(finalization))
        output = self.root / 'kit'
        subprocess.run([sys.executable, str(ROOT / 'scripts/prepare-relink-kit.py'),
                        '--build-dir', str(build), '--output', str(output)], check=True)
        manifest = json.loads((output / 'relink.json').read_text())
        self.assertEqual(manifest['originalSha256'], finalization['sha256'])
        self.assertEqual(manifest['finalization'], finalization)
        for name, material in manifest['materials'].items():
            self.assertEqual((output / name).read_bytes(), Path(material['input']).read_bytes())
        rebuilt = output / 'gitstored-relinked'
        self.assertEqual(subprocess.check_output([str(rebuilt)]), b'fixture version 1\n')
        sections = subprocess.check_output(['readelf', '--sections', str(rebuilt)])
        self.assertNotIn(b'.debug_', sections)
        self.assertIn(b'.symtab', sections)


if __name__ == '__main__':
    unittest.main()
