"""Regression guards for Waveshare ESP32-S3-Zero status LED support."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ZeroRgbTests(unittest.TestCase):
    def test_rmt_rgb_driver_for_both_roles(self):
        for role in ("gateway", "node"):
            with self.subTest(role=role):
                led = (ROOT/role/"main"/"automesh_status_led.c").read_text("utf-8")
                cmake = (ROOT/role/"main"/"CMakeLists.txt").read_text("utf-8")
                self.assertIn("#if CONFIG_IDF_TARGET_ESP32S3", led)
                self.assertIn("GPIO_NUM_21", led)
                self.assertIn("rmt_new_tx_channel", led)
                self.assertIn("rmt_new_bytes_encoder", led)
                self.assertIn("rmt_tx_wait_all_done", led)
                self.assertIn("automesh_rgb_init();", led)
                self.assertIn("automesh_rgb_show(", led)
                self.assertIn("esp_driver_rmt", cmake)
                self.assertIn("GPIO_NUM_8", led)  # C3 indicator unchanged

    def test_installer_describes_exact_s3_zero_board(self):
        stage = (ROOT/"Windows-Installer"/"build_release.py").read_text("utf-8")
        self.assertIn("Waveshare ESP32-S3-Zero 4MB", stage)
        self.assertIn("WS2812 RGB on GPIO21", stage)
        self.assertIn('f"{chip}-zero-4mb"', stage)


if __name__ == "__main__":
    unittest.main()
