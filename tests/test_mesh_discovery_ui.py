"""Regression checks for the administrator panel and manual Mesh discovery UI."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
UI = (ROOT / "gateway/main/gateway_index.html").read_text(encoding="utf-8")
GATEWAY = (ROOT / "gateway/main/main.c").read_text(encoding="utf-8")


class MeshDiscoveryUiTests(unittest.TestCase):
    def test_login_hides_auth_panel_only_after_success(self):
        self.assertIn("const state=await api('/api/status');", UI)
        self.assertIn("const list=await api('/api/devices');", UI)
        self.assertIn(" $('authPanel').hidden=true;", UI)
        self.assertIn(" $('authPanel').hidden=false;", UI)
        self.assertIn(" $('logout').hidden=false;", UI)
        self.assertIn(" $('logout').hidden=true;", UI)

    def test_search_action_is_explicit_and_requires_login(self):
        self.assertIn('id="scanMesh"', UI)
        self.assertIn('id="meshScanStatus"', UI)
        self.assertIn("$('scanMesh').addEventListener('click',async()=>{", UI)
        self.assertIn("if(meshSearchBusy || !token)return;", UI)
        self.assertIn("await update();", UI)

    def test_discovery_does_not_automatically_approve(self):
        self.assertIn("esp_ble_mesh_provisioner_prov_enable(", GATEWAY)
        self.assertIn("automesh_candidate(dev_uuid, addr, addr_type, oob_info, bearer)", GATEWAY)
        search_handler = UI.split("$('scanMesh').addEventListener('click',async()=>{", 1)[1].split("\nlet scanActive=false;", 1)[0]
        self.assertNotIn("/api/approve", search_handler)
        self.assertIn("action('Разрешить'", UI)


if __name__ == "__main__":
    unittest.main()
