"""Source-level guards for manual node OTA, relay and admin form placement."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
def load(p):return (ROOT/p).read_text(encoding="utf-8")
class MeshOtaAndResilience(unittest.TestCase):
    def test_relay_and_peer_communications_on_nodes(self):
        config=load("node/sdkconfig.defaults")
        main=load("node/main/main.c")
        self.assertIn("CONFIG_BLE_MESH_RELAY=y",config)
        self.assertIn(".relay = ESP_BLE_MESH_RELAY_ENABLED",main)
        self.assertIn("ESP_BLE_MESH_ADDR_ALL_NODES",main)
        self.assertIn("AUTOMESH_PEER_HELLO",main)
        self.assertIn("automesh_note_peer",main)
        self.assertIn("esp_ble_mesh_server_model_send_msg",main)

    def test_node_ota_checks_inactive_image_chip_digest_and_app(self):
        code=load("node/main/automesh_node_ota.c")
        for token in ("esp_ota_get_next_update_partition(NULL)",
                      "esp_ota_begin", "esp_ota_write", "esp_ota_end",
                      "esp_ota_set_boot_partition", "mbedtls_sha256_finish",
                      "image_header_matches_chip", 'strcmp(app.project_name, "onoff_server")',
                      "AUTOMESH_NODE_OTA_ABORT"):
            self.assertIn(token,code)
        self.assertNotIn("nvs_flash_erase(",code)
        self.assertNotIn("nvs_erase_key(",code)

    def test_gateway_uses_serialized_session_and_admin_web_endpoints(self):
        ui=load("gateway/main/gateway_index.html")
        web=load("gateway/main/automesh_web.c")
        protocol=load("gateway/main/automesh_gateway_node_ota.c")
        for ep in ("/api/ota/node/start","/api/ota/node/chunk",
                   "/api/ota/node/finish","/api/ota/node/abort"):
            self.assertIn(ep,web)
        self.assertIn("if(!authorize(req))return ESP_OK;",web)
        self.assertIn("if(setup_mode || !valid_origin(req))",web)
        self.assertIn("automesh_node_ota_supported(address, chip)",protocol)
        self.assertIn("AUTOMESH_NODE_OTA_BLOCK",protocol)
        self.assertIn("sha256Portable(bytes)",ui)
        self.assertIn("OTA1",ui)

    def test_admin_password_change_at_bottom(self):
        ui=load("gateway/main/gateway_index.html")
        self.assertGreater(ui.find('id="adminChange"'),ui.find('Радиосвязь и индикация'))
        self.assertLess(ui.find('id="adminChange"'),ui.find("</main>"))
        self.assertIn("changeAdminForm",ui)

    def test_ota_zip_includes_node_and_gateway_images(self):
        ci=load(".github/workflows/esp-idf-c3.yml")
        self.assertIn("Nodes/node-c3.bin",ci)
        self.assertIn("Nodes/node-s3.bin",ci)
        self.assertIn("SHA256SUMS.txt",ci)
        self.assertIn('v0.3.20-dev',ci)

    def test_context_document_is_committed(self):
        doc=load("docs/PROJECT-STATE.md")
        for term in ("GitHub Actions","main","OTA1","Provisioner","NVS",
                     "старые", "отключение", "проверить"):
            self.assertIn(term.lower(),doc.lower())

if __name__=="__main__":unittest.main()
