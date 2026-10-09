import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source_block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]


FIXTURE = r'''
#include <stdio.h>
struct rect { int unused; };
static struct { int used; } app_sessions[1] = {{1}};
static int context_open, start_open, pointer_x = 50, pointer_y = 50;
static int hover_app = -1, mouse_events, leave_events;
static int hit_window(int horizontal, int vertical) {
    return horizontal < 100 && vertical < 100 ? 7 : -1;
}
static int app_slot_for_win(int window_id) { return window_id == 7 ? 0 : -1; }
static struct rect content_rect(int window_id) { (void)window_id; return (struct rect){0}; }
static struct rect start_menu_rect(void) { return (struct rect){0}; }
static int inside(int horizontal, int vertical, struct rect area) {
    (void)area; return horizontal < 100 && vertical < 100;
}
static void send_mouse_to_app(int window_id, int buttons, int wheel) {
    (void)window_id; (void)buttons; (void)wheel; mouse_events++;
}
static void send_mouse_leave_to_app(int window_id) { (void)window_id; leave_events++; }
FUNCTION_SOURCE
int main(void) {
    RELEASE_CALL
    int entered = mouse_events;
    for (int iteration = 0; iteration < 10000; iteration++) { RELEASE_CALL }
    int idle_extra = mouse_events - entered;
    int before_move = mouse_events;
    update_hover_app(1);
    RELEASE_CALL
    int move_extra = mouse_events - before_move;
    pointer_x = 150;
    RELEASE_CALL
    RELEASE_CALL
    int left_once = leave_events;
    pointer_x = 50;
    RELEASE_CALL
    context_open = 1;
    RELEASE_CALL
    int before_context_close = mouse_events;
    context_open = 0;
    RELEASE_CALL
    int context_enter = mouse_events - before_context_close;
    int before_release = mouse_events;
    send_mouse_to_app(7, 0, 0);
    RELEASE_CALL
    printf("{\"entered\":%d,\"idle_extra\":%d,\"move_extra\":%d,"
           "\"left_once\":%d,\"context_enter\":%d,\"release_extra\":%d}\n",
           entered, idle_extra, move_extra, left_once, context_enter,
           mouse_events - before_release);
    return 0;
}
'''


class GuiHoverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('zig.exe') or shutil.which('zig') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('A host C compiler is required')
        source = (ROOT / 'src/user/bin/gui.c').read_text(encoding='utf-8')
        function = source_block(source, 'static void update_hover_app(int force) {')
        release = source_block(source_block(source, 'static void handle_mouse(void) {'), 'if (!left) {')
        calls = re.findall(r'update_hover_app\(\d+\);', release)
        if len(calls) != 1 or 'send_mouse_to_app(app_mouse_capture, ms.buttons, 0);' not in release:
            raise AssertionError('Mouse release integration changed; update the behavioral fixture')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        program = Path(cls.directory.name) / 'gui_hover.c'
        executable = program.with_suffix('.exe')
        program.write_text(FIXTURE.replace('FUNCTION_SOURCE', function).replace('RELEASE_CALL', calls[0]),
                           encoding='utf-8')
        command = [compiler]
        if Path(compiler).stem == 'zig':
            command.append('cc')
        subprocess.run(command + ['-std=c11', '-O1', '-UNDEBUG', str(program), '-o', str(executable)],
                       check=True, capture_output=True, text=True, timeout=60)
        result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=10)
        cls.result = json.loads(result.stdout)

    def test_idle_poll_does_not_repeat_hover(self):
        self.assertEqual(self.result['entered'], 1)
        self.assertEqual(self.result['idle_extra'], 0)

    def test_actual_mouse_move_is_not_duplicated(self):
        self.assertEqual(self.result['move_extra'], 1)

    def test_hover_leave_and_context_reenter_are_preserved(self):
        self.assertEqual(self.result['left_once'], 1)
        self.assertEqual(self.result['context_enter'], 1)

    def test_explicit_capture_release_is_not_duplicated(self):
        self.assertEqual(self.result['release_extra'], 1)


if __name__ == '__main__':
    unittest.main()
