"""Real debug-bearing objects must not inflate allocated linker attribution."""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('footprint', ROOT / 'scripts/measure-release-footprint.py')
footprint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(footprint)


@unittest.skipUnless(sys.platform == 'linux' and all(shutil.which(t) for t in ('cc', 'objcopy', 'ld')),
                     'requires Linux ELF/GNU map producer')
class Attribution(unittest.TestCase):
    def test_two_debug_objects_and_wrapped_map_headers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'objects').mkdir()
            expected = {}
            for name in ('first', 'second'):
                path = root / (name + '.c')
                path.write_text('int long_function_section_name_' + name + '(void) { return 42; }\n')
                obj = 'objects/' + name + '.o'
                subprocess.run(['cc', '-g', '-ffunction-sections', '-fno-asynchronous-unwind-tables',
                                '-fno-unwind-tables', '-c', str(path), '-o', obj], cwd=root, check=True)
                # Exclude linker-merged property notes from the expected
                # object-byte oracle; leave real compiler DWARF untouched.
                subprocess.run(['objcopy', '--remove-section=.note.gnu.property', obj], cwd=root, check=True)
                expected[obj] = sum(section[3] for section in footprint.allocated_sections(root / obj).values())
                self.assertGreater(expected[obj], 0)
            subprocess.run(['ld', '--build-id=none', '-e', 'long_function_section_name_first', '-Map=fixture.map',
                            *expected, '-o', 'fixture'], cwd=root, check=True)
            map_text = (root / 'fixture.map').read_text()
            actual = footprint.allocated_contributors(map_text, root / 'fixture')
            self.assertEqual(actual, expected)
            # Reproduce the old bug: second object's .debug_* contributions
            # have nonzero offsets despite lacking SHF_ALLOC.
            old = {}
            for line in map_text.split('Linker script and memory map', 1)[1].splitlines():
                m = re.match(r'^\s*(?:\.[^\s]+\s+)?0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(objects/\S+)', line)
                if m and int(m[1], 16):
                    old[m[3]] = old.get(m[3], 0) + int(m[2], 16)
            self.assertGreater(old['objects/second.o'], expected['objects/second.o'], map_text)
            # Explicitly exercise output wrapping as well as the long input
            # section wrapping emitted by the real GNU linker above.
            wrapped = re.sub(r'^(\.text)\s+(0x[0-9a-f]+\s+0x[0-9a-f]+)',
                             r'\1\n                \2', map_text, flags=re.M)
            self.assertNotEqual(wrapped, map_text)
            self.assertEqual(footprint.allocated_contributors(wrapped, root / 'fixture'), expected)


if __name__ == '__main__':
    unittest.main()
