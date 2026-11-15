"""Firmware build identity, BLE Mesh read-only status and Web-OTA ZIP layout."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

class FirmwareVersionsAndWebOta(unittest.TestCase):
    def test_project_versions_are_embedded(self):
        for role in ("gateway", "node"):
            with self.subTest(role=role):
                self.assertIn('set(PROJECT_VER "0.3.17-dev")',
                              read(role + "/CMakeLists.txt"))

    def test_gateway_reports_running_image_version_and_build_id(self):
        web = read("gateway/main/automesh_web.c")
        self.assertIn("esp_app_get_description()", web)
        self.assertIn('cJSON_AddStringToObject(doc,"version",running->version)', web)
        self.assertIn('cJSON_AddStringToObject(doc,"build_id",build_id)', web)
        self.assertNotIn('cJSON_AddStringToObject(doc,"version","0.3.', web)

    def test_nodes_reply_to_version_request_from_gateway_only(self):
        node = read("node/main/main.c")
        self.assertIn("AUTOMESH_FW_VERSION_GET", node)
        self.assertIn("AUTOMESH_FW_VERSION_STATUS", node)
        self.assertIn("ctx->addr != 0x0001", node)
        self.assertIn("esp_app_get_description()", node)
        self.assertIn("app->app_elf_sha256[0]", node)

    def test_gateway_queries_version_and_does_not_guess(self):
        gateway = read("gateway/main/main.c")
        bridge = read("gateway/main/automesh_bridge.c")
        page = read("gateway/main/gateway_index.html")
        self.assertIn("automesh_gateway_request_version(", gateway)
        self.assertIn("automesh_note_version(", gateway)
        self.assertIn('cJSON_AddStringToObject(item,"firmware_version",c->fw_version)', bridge)
        self.assertIn("Не определена (старый узел или нет ответа)", page)
        self.assertIn('id="gatewayVersion"', page)
        self.assertIn("Прошивка</th>", page)

    def test_release_web_ota_contains_two_images_and_checksums(self):
        ci = read(".github/workflows/esp-idf-c3.yml")
        self.assertIn("New-Item -ItemType Directory -Path _release/Web-OTA", ci)
        self.assertIn("_release/Web-OTA/gw-c3.bin", ci)
        self.assertIn("_release/Web-OTA/gw-s3.bin", ci)
        self.assertIn("_release/Web-OTA/Nodes/node-c3.bin", ci)
        self.assertIn("_release/Web-OTA/Nodes/node-s3.bin", ci)
        self.assertIn("_release/Web-OTA/SHA256SUMS.txt", ci)
        self.assertIn("WEB-OTA-README-RU.txt", ci)
        self.assertIn("ОБНОВЛЕНИЕ ЛЮБОГО УСТРОЙСТВА", read("Windows-Installer/WEB-OTA-README-RU.txt"))

if __name__ == "__main__":
    unittest.main()
