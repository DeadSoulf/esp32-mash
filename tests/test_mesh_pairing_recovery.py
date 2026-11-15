"""Regression checks: saved BLE Mesh AppKeys must survive reflashing."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

def source(path):
    return (ROOT/path).read_text(encoding="utf-8")

class MeshPairingAfterFlashTests(unittest.TestCase):
    def test_restored_key_is_reused_and_not_rotated(self):
        gateway=source("gateway/main/main.c")
        self.assertIn("esp_ble_mesh_provisioner_get_local_app_key(",gateway)
        self.assertIn("memcmp(saved_key,prov_key.app_key,sizeof(prov_key.app_key))!=0",gateway)
        self.assertIn("return ESP_ERR_INVALID_STATE;",gateway)
        self.assertIn("Restored Mesh AppKey found",gateway)
        self.assertIn("automesh_bind_provisioner_models(prov_key.app_idx);",gateway)
        self.assertIn("esp_ble_mesh_provisioner_add_local_app_key(",gateway)
        self.assertIn("ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT",gateway)

    def test_existing_mesh_credentials_are_persisted(self):
        for role in ("gateway","node"):
            self.assertIn("CONFIG_BLE_MESH_SETTINGS=y",
                          source(role+"/sdkconfig.defaults"))
            self.assertNotIn("nvs_flash_erase(",source(role+"/main/main.c"))

    def test_no_automatic_mesh_enrollment_or_nvs_erase(self):
        gateway=source("gateway/main/main.c")
        bridge=source("gateway/main/automesh_bridge.c")
        installer=source("Windows-Installer/installer.py")
        self.assertIn("automesh_candidate(dev_uuid, addr, addr_type, oob_info, bearer)",gateway)
        self.assertIn("ADD_DEV_START_PROV_NOW_FLAG",bridge)
        self.assertIn('if(strcmp(req->uri,"/api/approve")==0)',
                      source("gateway/main/automesh_web.c"))
        self.assertNotIn("read-flash",installer)
        self.assertNotIn("backup_flash(",installer)
        self.assertIn("reset_nvs=args.erase_nvs",installer)

    def test_wifi_setup_does_not_claim_mesh_enabled(self):
        gateway=source("gateway/main/main.c")
        self.assertIn('if (!usb_only && !automesh_web_has_network())',gateway)
        self.assertIn("Setup AP mode; Mesh paused",gateway)
        self.assertIn("if(usb_only)",gateway)
        self.assertIn("USB-only Mesh provisioner active; Wi-Fi disabled",gateway)

if __name__=="__main__":
    unittest.main()
