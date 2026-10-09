"""Write the user linker script without shell-dependent echo quoting."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(
    'ENTRY(_start)\n'
    'SECTIONS { . = 0x0000000100000000; .text : { *(.text.entry) *(.text*) } '
    '.rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n',
    encoding='ascii', newline='\n')
