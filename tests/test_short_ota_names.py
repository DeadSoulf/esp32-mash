"""Prevent regressions in the short user-facing Web-OTA filenames."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

def source(path):
    return (ROOT / path).read_text(encoding="utf-8")

class ShortUpdateFilenames(unittest.TestCase):
    def test_all_four_release_images_use_short_names(self):
        flow=source(".github/workflows/esp-idf-c3.yml")
        ui=source("gateway/main/gateway_index.html")
        names=[
            "gw-c3.bin", "gw-s3.bin",
            "Nodes/node-c3.bin", "Nodes/node-s3.bin"
        ]
        for name in names:
            with self.subTest(name=name):
                self.assertIn("Web-OTA/"+name,flow)
                self.assertIn("Web-OTA/"+name,ui)
        self.assertIn("AutoMesh-v0.3.17-dev.zip",flow)
        self.assertIn("$names = @('gw-c3.bin', 'gw-s3.bin',",flow)
        self.assertIn("Nodes/node-c3.bin",flow)
        self.assertIn("Nodes/node-s3.bin",flow)

    def test_old_release_zip_paths_still_work_in_new_browser(self):
        html=source("gateway/main/gateway_index.html")
        self.assertIn("gateway-esp32c3-provisioner.bin",html)
        self.assertIn("gateway-esp32s3-zero-provisioner.bin",html)
        self.assertIn("node-esp32c3-onoff_server.bin",html)
        self.assertIn("node-esp32s3-zero-onoff_server.bin",html)
        self.assertIn("acceptedPaths.some(",html)

    def test_usb_build_filenames_remain_unchanged(self):
        python=source("Windows-Installer/build_release.py")
        workflow=source(".github/workflows/esp-idf-c3.yml")
        self.assertIn('"gateway", "provisioner.bin"',python)
        self.assertIn('"node", "onoff_server.bin"',python)
        self.assertIn("_artifacts/automesh-gateway-esp32c3/provisioner.bin",workflow)
        self.assertIn("_artifacts/automesh-node-esp32s3/onoff_server.bin",workflow)

    def test_short_names_described_for_next_chat(self):
        doc=source("docs/PROJECT-STATE.md")
        for filename in ("gw-c3.bin","gw-s3.bin","node-c3.bin","node-s3.bin"):
            self.assertIn(filename,doc)
        self.assertIn("v0.3.17-dev",doc)

if __name__=="__main__":
    unittest.main()
