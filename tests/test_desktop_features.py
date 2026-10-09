from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_desktop_features import Desktop


class DesktopBootTests(unittest.TestCase):
    def desktop(self):
        desktop = Desktop.__new__(Desktop)
        desktop.process = Mock()
        desktop.process.poll.return_value = None
        desktop.log = Mock()
        return desktop

    def test_slow_cold_boot_can_reach_shell_after_24_seconds(self):
        desktop = self.desktop()
        desktop.log.side_effect = ["[minifs] formatting", "[boot] user shell started", "buzzos:/> "]
        with patch("check_desktop_features.time.monotonic", side_effect=[0, 24, 29]), \
                patch("check_desktop_features.time.sleep") as sleep:
            desktop.wait_for_shell()
        self.assertEqual(sleep.call_count, 2)

    def test_missing_shell_times_out(self):
        desktop = self.desktop()
        desktop.log.return_value = "[boot] starting"
        with patch("check_desktop_features.time.monotonic", side_effect=[0, 0, 60]), \
                patch("check_desktop_features.time.sleep"), \
                self.assertRaisesRegex(RuntimeError, "Boot timeout"):
            desktop.wait_for_shell()

    def test_exited_qemu_is_not_treated_as_slow_boot(self):
        desktop = self.desktop()
        desktop.log.return_value = "[boot] starting"
        desktop.process.poll.return_value = 1
        with patch("check_desktop_features.time.sleep") as sleep, \
                self.assertRaisesRegex(RuntimeError, "QEMU exited during boot"):
            desktop.wait_for_shell()
        sleep.assert_not_called()

    def test_interleaved_shell_prompt_is_recognized(self):
        desktop = self.desktop()
        desktop.log.return_value = "buzzos:/> [timer] tick\n"
        with patch("check_desktop_features.time.sleep") as sleep:
            desktop.wait_for_shell()
        sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main()
