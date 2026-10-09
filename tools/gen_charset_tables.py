"""Generate deterministic GBK conversion tables using Python's bundled codec."""

import argparse
from pathlib import Path


def generate(output: Path) -> None:
    """Write a complete two-byte GBK decode table and sorted reverse map."""
    decoded = []
    encoded = {}
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            if trail == 0x7F:
                continue
            raw = bytes((lead, trail))
            try:
                character = raw.decode('gbk')
            except UnicodeDecodeError:
                decoded.append(0)
                continue
            codepoint = ord(character)
            if codepoint > 0xFFFF:
                raise ValueError('GBK table requires a BMP mapping')
            decoded.append(codepoint)
            encoded[codepoint] = int.from_bytes(character.encode('gbk'), 'big')
    lines = ['#ifndef BUZZOS_CHARSET_GBK_TABLES_H',
             '#define BUZZOS_CHARSET_GBK_TABLES_H',
             'static const uint16_t charset_gbk_decode_table[] = {']
    for start in range(0, len(decoded), 12):
        lines.append('    ' + ','.join(f'0x{value:04x}' for value in decoded[start:start + 12]) + ',')
    lines.extend(['};', 'static const uint32_t charset_gbk_encode_table[] = {'])
    reverse = [(codepoint << 16) | encoded[codepoint] for codepoint in sorted(encoded)]
    for start in range(0, len(reverse), 8):
        lines.append('    ' + ','.join(f'0x{value:08x}u' for value in reverse[start:start + 8]) + ',')
    lines.extend(['};', '#endif', ''])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines), encoding='ascii')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    generate(parser.parse_args().output)


if __name__ == '__main__':
    main()
