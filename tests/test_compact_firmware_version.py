"""Firmware version is shortened only on screen; OTA markers remain usable."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
def read(path): return (ROOT/path).read_text(encoding="utf-8")

class ShortDeviceVersionTests(unittest.TestCase):
    def test_compact_version_renderer(self):
        html=read("gateway/main/gateway_index.html")
        self.assertIn("function shortFirmwareVersion(raw){",html)
        self.assertIn("return match?match[1]+' '+match[2]:raw;",html)
        self.assertIn("firmware.textContent=node.firmware_version?",html)
        self.assertIn("shortFirmwareVersion(node.firmware_version):(node.address?",html)
        self.assertIn("const version=shortFirmwareVersion(node.firmware_version)",html)
        self.assertIn("(shortFirmwareVersion(node.firmware_version)||'версия неизвестна')",html)

    def test_ota_keeps_full_raw_capability_and_chip(self):
        html=read("gateway/main/gateway_index.html")
        gateway=read("gateway/main/automesh_bridge.c")
        self.assertIn("const v=n.firmware_version||'Версия неизвестна';",html)
        self.assertIn("v.includes(' OTA1 ')||v.includes(' OTA2 ')",html)
        self.assertIn("const chip=v.includes(' C3 ')?'C3':v.includes(' S3 ')?'S3':'';",html)
        self.assertIn("automesh_node_ota_block_size",gateway)
        self.assertIn('strstr(c->fw_version," OTA2 ")',gateway)
        self.assertIn('strstr(c->fw_version," OTA1 ")',gateway)
        self.assertIn('cJSON_AddStringToObject(item,"firmware_version",c->fw_version)',gateway)
        self.assertNotIn("c->fw_version=shortFirmwareVersion",gateway)

    def test_release_consistency(self):
        for role in ("gateway","node"):
            self.assertIn('set(PROJECT_VER "0.3.21-dev")',
                          read(role+"/CMakeLists.txt"))
        self.assertIn("AutoMesh-v0.3.21-dev.zip",
                      read(".github/workflows/esp-idf-c3.yml"))
        self.assertIn("v0.3.21-dev",read("docs/PROJECT-STATE.md"))

if __name__=="__main__": unittest.main()
