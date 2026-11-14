from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

class AutoMeshSourceSafetyTests(unittest.TestCase):
    def test_mesh_persistence_enabled_in_both_firmwares(self):
        for role in ("gateway","node"):
            with self.subTest(role=role):
                settings=(ROOT/role/"sdkconfig.defaults").read_text("utf-8")
                self.assertIn("CONFIG_BLE_MESH_SETTINGS=y",settings)
                self.assertIn("CONFIG_BLE_MESH_STORE_TIMEOUT=0",settings)

    def test_no_automatic_nvs_erase(self):
        for role in ("gateway","node"):
            with self.subTest(role=role):
                source=(ROOT/role/"main"/"main.c").read_text("utf-8")
                self.assertNotIn("nvs_flash_erase(",source)

    def test_locate_is_acknowledged(self):
        gw=(ROOT/"gateway"/"main"/"main.c").read_text("utf-8")
        ui=(ROOT/"gateway"/"main"/"gateway_index.html").read_text("utf-8")
        self.assertIn("esp_ble_mesh_client_model_init(&locate_models[0])",gw)
        self.assertIn("LOC_CONFIRMED",gw)
        self.assertIn("/api/locate/status",ui)

    def test_device_names_are_persistent(self):
        bridge=(ROOT/"gateway"/"main"/"automesh_bridge.c").read_text("utf-8")
        web=(ROOT/"gateway"/"main"/"automesh_web.c").read_text("utf-8")
        self.assertIn('nvs_set_blob(h,"registered"',bridge)
        self.assertIn('nvs_get_blob(h,"registered"',bridge)
        self.assertIn('"/api/name"',web)

if __name__=="__main__":
    unittest.main()

