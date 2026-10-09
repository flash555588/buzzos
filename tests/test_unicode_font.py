"""Verify the generated kernel fallback covers GBK and traditional Chinese."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'src/kernel/drv/font_unicode_data.h'
FIXTURE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "font_unicode.c"

int main(void) {
    const uint32_t characters[] = {0x7e41, 0x9ad4, 0x4e2d, 0x6587, 0x9f8d, 0x4e02};
    uint8_t bits[UFONT_BYTES], previous[UFONT_BYTES];
    for (unsigned int index = 0; index < sizeof(characters)/sizeof(characters[0]); index++) {
        assert(font_unicode_lookup(characters[index], bits) == 30);
        unsigned int ink = 0;
        for (unsigned int byte = 0; byte < UFONT_BYTES; byte++) ink |= bits[byte];
        assert(ink);
        if (index) assert(memcmp(bits, previous, sizeof(bits)));
        memcpy(previous, bits, sizeof(bits));
    }
    assert(!font_unicode_lookup(0x1f600, bits));
    for (unsigned int index = 1; index < KFONT_UNICODE_COUNT; index++)
        assert(kfont_unicode_codepoints[index - 1] < kfont_unicode_codepoints[index]);
    printf("PASS actual lookup, nonempty distinct glyphs, sorted coverage: %u\n", KFONT_UNICODE_COUNT);
    return 0;
}
'''


class UnicodeFontTests(unittest.TestCase):
    """Check generated data and the production binary-search lookup."""

    def test_gbk_coverage(self) -> None:
        """All strict Python GBK mappings must occur in the ordered table."""
        source = HEADER.read_text(encoding='ascii')
        table = source.split('kfont_unicode_codepoints', 1)[1].split('};', 1)[0]
        points = [int(value, 16) for value in re.findall(r'0x([0-9A-Fa-f]+)u', table)]
        count = int(re.search(r'KFONT_UNICODE_COUNT = (\d+)', source).group(1))
        self.assertEqual(len(points), count)
        self.assertEqual(points, sorted(set(points)))
        self.assertNotIn(0xfffd, points)
        self.assertTrue(all(point > 0x7f for point in points))
        expected = set()
        for lead in range(0x81, 0xff):
            for trail in range(0x40, 0xff):
                if trail == 0x7f:
                    continue
                try:
                    character = bytes((lead, trail)).decode('gbk', errors='strict')
                except UnicodeDecodeError:
                    continue
                expected.add(ord(character))
        self.assertTrue(expected.issubset(points),
                        f'Missing GBK codepoints: {sorted(expected.difference(points))}')
        self.assertTrue(set(map(ord, '繁體中文龍丂')).issubset(points))

    def test_production_lookup(self) -> None:
        """Compile production lookup and ensure the requested glyphs contain ink."""
        compiler = shutil.which('zig.exe') or shutil.which('cc')
        if not compiler:
            self.skipTest('A host C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            fixture = temporary / 'font-test.c'
            executable = temporary / 'font-test.exe'
            fixture.write_text(FIXTURE, encoding='ascii')
            command = [compiler]
            if Path(compiler).name == 'zig.exe':
                command.append('cc')
            compiled = subprocess.run(command + [
                '-std=gnu11', '-O2', '-I', str(ROOT / 'src/kernel/drv'),
                str(fixture), '-o', str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS actual lookup', result.stdout)


if __name__ == '__main__':
    unittest.main()
