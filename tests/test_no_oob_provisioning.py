"""Ensure the gateway administrator login authorizes No-OOB Mesh commissioning.

No-OOB Mesh provisioning provides no independent physical device authentication;
manual administrator approval remains mandatory.
"""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]

class NoOOBTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bridge=(ROOT/"gateway/main/automesh_bridge.c").read_text(encoding="utf-8")
        cls.web=(ROOT/"gateway/main/automesh_web.c").read_text(encoding="utf-8")
        cls.html=(ROOT/"gateway/main/gateway_index.html").read_text(encoding="utf-8")
        cls.node=(ROOT/"node/main/main.c").read_text(encoding="utf-8")
        cls.header=(ROOT/"gateway/main/automesh_bridge.h").read_text(encoding="utf-8")

    def test_node_provisioning_has_no_static_oob(self):
        self.assertIn(".static_val = NULL",self.node)
        self.assertIn(".static_val_len = 0",self.node)
        self.assertNotIn("esp_fill_random(automesh_oob",self.node)
        self.assertNotIn("AUTOMESH_NODE_OOB",self.node)
        self.assertNotIn("automesh_prepare_oob()",self.node)

    def test_gateway_approves_only_explicit_selected_node(self):
        self.assertIn("automesh_approve_uuid(const char *uuid_hex)",self.bridge)
        self.assertIn("automesh_approve_uuid(const char *uuid_hex)",self.header)
        self.assertIn("!candidates[i].rejected && !candidates[i].approved",self.bridge)
        self.assertIn("one pending provisioning at a time",self.bridge)
        self.assertIn("esp_ble_mesh_provisioner_add_unprov_dev(&device",self.bridge)
        self.assertNotIn("esp_ble_mesh_provisioner_set_static_oob_value",self.bridge)

    def test_approval_requires_gateway_admin_only(self):
        self.assertIn("if(!authorize(req))return ESP_OK;",self.web)
        self.assertIn('strcmp(req->uri,"/api/approve")',self.web)
        self.assertIn("result=automesh_approve_uuid(u->valuestring)",self.web)
        self.assertNotIn('GetObjectItemCaseSensitive(body,"oob")',self.web)
        self.assertIn("await api('/api/approve','POST',{uuid:node.uuid})",self.html)
        self.assertNotIn("OOB-ключ этой ESP32",self.html)

    def test_existing_mesh_and_admin_nvs_not_erased(self):
        self.assertNotIn('nvs_flash_erase(',self.node)
        self.assertNotIn('nvs_flash_erase(',self.bridge)
        self.assertIn("CONFIG_BLE_MESH_SETTINGS=y",
                      (ROOT/"node/sdkconfig.defaults").read_text(encoding="utf-8"))
        self.assertIn("CONFIG_BLE_MESH_SETTINGS=y",
                      (ROOT/"gateway/sdkconfig.defaults").read_text(encoding="utf-8"))

if __name__=="__main__":
    unittest.main()

