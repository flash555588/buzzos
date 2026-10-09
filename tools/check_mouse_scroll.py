#!/usr/bin/env python3
"""Check mouse motion, wheel direction and captured Start scrolling in QEMU."""
import argparse
import json
from pathlib import Path
import shutil
import time

from check_desktop_features import Desktop


def cursor(frame):
    width, height, rgb = frame
    positions, offset = [], 0
    while True:
        offset = rgb.find(b'\0\0\0', offset)
        if offset < 0:
            break
        if offset % 3 == 0:
            positions.append((offset//3 % width, offset//3 // width))
        offset += 3
    assert positions, 'Software cursor outline missing'
    return min(x for x, _ in positions), min(y for _, y in positions)


def paint_top(frame):
    width, height, rgb = frame
    offset = rgb.find(b'\xa2\x00\xff')
    while offset >= 0 and offset % 3:
        offset = rgb.find(b'\xa2\x00\xff', offset+1)
    assert offset >= 0, 'Paint tile/badge not visible during reversal'
    return offset // (width * 3)


def run(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / 'mouse-test.img'
    if image == args.image.resolve():
        raise ValueError('Test copy must differ from source image')
    shutil.copyfile(args.image, image)
    desktop = Desktop(args.qemu, image, out, 'mouse')
    checks = []
    try:
        desktop.move(800, 300)
        expected = [800, 300]
        assert cursor(desktop.capture('motion-start')) == tuple(expected)
        for step, (dx, dy) in enumerate(((0, 200), (0, -200), (200, 0), (-200, 0))):
            desktop.command(f'mouse_move {dx} {dy}')
            time.sleep(.2)
            expected[0] += dx
            expected[1] += dy
            assert cursor(desktop.capture(f'motion-{step}')) == tuple(expected)
        desktop.pointer = expected
        checks.append('raw +/-200px movement on both axes')
        for mode, size in ((11, (800, 600)), (12, (640, 960))):
            desktop.key('s')
            desktop.key('1')
            for _ in range(mode):
                desktop.key('bracket_right')
            desktop.key('ctrl-w')
            desktop.key('home')
            time.sleep(.3)
            desktop.pointer = [min(desktop.pointer[0], size[0]-1),
                               min(desktop.pointer[1], size[1]-1)]
            desktop.move(size[0]-4, 100)
            rest = desktop.capture(f'{mode}-wheel-rest')
            for _ in range(3):
                desktop.command('mouse_move 0 0 1')
                time.sleep(.07)
            time.sleep(.25)
            assert desktop.capture(f'{mode}-wheel-down') != rest
            for _ in range(3):
                desktop.command('mouse_move 0 0 -1')
                time.sleep(.07)
            time.sleep(.25)
            assert desktop.capture(f'{mode}-wheel-return') == rest
            if mode == 11:
                # Reverse before pending downward travel has completed.
                # The visible Paint row should move down on screen (scroll
                # up), rather than keep chasing the previous down target.
                for _ in range(4):
                    desktop.command('mouse_move 0 0 1')
                    time.sleep(.02)
                desktop.command('mouse_move 0 0 -1')
                time.sleep(.035)
                reversing = desktop.capture('wheel-reversing')
                time.sleep(.25)
                assert paint_top(desktop.capture('wheel-reversed')) >= paint_top(reversing), 'Upward wheel continued downward travel'
                desktop.key('home')
                time.sleep(.25)
                desktop.key('ctrl-p')
                time.sleep(.3)
                for _ in range(4):
                    desktop.command('mouse_move 0 0 1')
                    time.sleep(.02)
                desktop.command('mouse_move 0 0 -1')
                time.sleep(.035)
                reversing = desktop.capture('search-reversing')
                time.sleep(.25)
                assert paint_top(desktop.capture('search-reversed')) >= paint_top(reversing), 'Search reversed wheel kept moving down'
                desktop.key('esc')
                time.sleep(.3)
            # Capture the thumb, drag a little, then turn the wheel while
            # holding still. Every frame must stay at the drag's position.
            desktop.move(size[0]-15, 180)
            desktop.command('mouse_button 1')
            time.sleep(.1)
            desktop.move(size[0]-15, 228)
            held = desktop.capture(f'{mode}-thumb-held')
            for direction in (1, -1):
                desktop.command(f'mouse_move 0 0 {direction}')
                for step in range(8):
                    time.sleep(.025)
                    assert desktop.capture(f'{mode}-held-{direction}-{step}') == held, 'Wheel overwrote a captured thumb'
            desktop.command('mouse_button 0')
            desktop.move(10, 10)
            time.sleep(.25)
            settled = desktop.capture(f'{mode}-released')
            time.sleep(.35)
            assert desktop.capture(f'{mode}-released-stable') == settled, 'Drag resumed stale scroll motion'
            # The final 16px of the viewport are below the app work area;
            # clicking that portion of the track must still scroll Start.
            desktop.key('home')
            time.sleep(.2)
            desktop.move(size[0]-15, size[1]-70)
            before = desktop.capture(f'{mode}-bottom-before')
            desktop.command('mouse_button 1')
            time.sleep(.1)
            desktop.command('mouse_button 0')
            time.sleep(.2)
            assert desktop.capture(f'{mode}-bottom-after') != before, 'Bottom scrollbar track ignored the click'
            assert 'launch /fs/apps/' not in desktop.log(), 'Scrollbar click launched a tile'
            desktop.move(10, 10)
            desktop.key('home')
            time.sleep(.3)
            checks.append(f'{size}: wheel reversal, drag capture, lower track hit and release stability')
            print(checks[-1], flush=True)
        assert 'EXCEPTION' not in desktop.log()
        (out / 'result.json').write_text(json.dumps({'checks': checks}, indent=2) + '\n')
    finally:
        desktop.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, default=Path('build/mouse-scroll'))
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    run(parser.parse_args())
