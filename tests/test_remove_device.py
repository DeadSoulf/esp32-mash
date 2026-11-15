"""Delete a provisioned ESP32 from the gateway, without showing UUID on the page."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")

class DeleteDeviceUiAndMeshTests(unittest.TestCase):
    def test_uuid_is_hidden_but_available_for_internal_requests(self):
        html = read("gateway/main/gateway_index.html")
        self.assertIn("<th>Устройство</th>", html)
        self.assertNotIn("<th>Устройство / UUID</th>", html)
        self.assertNotIn("textContent=node.uuid", html)
        self.assertNotIn("||node.uuid", html)
        self.assertNotIn("||n.uuid", html)
        self.assertIn("uuid:node.uuid", html)
        self.assertIn("action('Удалить устройство'", html)
        self.assertIn("сбросить Mesh на самом устройстве", html)
        self.assertIn("await api('/api/remove','POST',{uuid:node.uuid})", html)

    def test_auth_required_for_delete_route(self):
        web = read("gateway/main/automesh_web.c")
        self.assertIn('httpd_uri_t remove={.uri="/api/remove"', web)
        self.assertIn('httpd_register_uri_handler(server,&remove)', web)
        self.assertIn('if(strcmp(req->uri,"/api/remove")==0) result=automesh_remove_uuid', web)
        self.assertIn("if(!authorize(req))return ESP_OK;", web)
        self.assertIn("if(!valid_origin(req))", web)

    def test_async_sdk_delete_with_callback_before_registry_removal(self):
        bridge = read("gateway/main/automesh_bridge.c")
        main = read("gateway/main/main.c")
        self.assertIn("esp_ble_mesh_provisioner_delete_node_with_uuid(uuid)", bridge)
        self.assertIn("ESP_BLE_MESH_PROVISIONER_DELETE_NODE_WITH_UUID_COMP_EVT", main)
        self.assertIn("automesh_remove_completed(uuid,result)", main)
        self.assertIn("automesh_gateway_forget_node(uuid)", main)
        self.assertIn("automesh_store_nodes();", main)
        self.assertIn("registry_save_locked()", bridge)
        self.assertIn("if (automesh_gateway_node_ota_active())", bridge)
        self.assertIn("candidates[slot].removing=true", bridge)
        self.assertIn('c->removing?"removing"', bridge)

    def test_per_device_not_whole_network(self):
        bridge = read("gateway/main/automesh_bridge.c")
        self.assertIn("memcmp(c->device.uuid,uuid,sizeof(uuid))==0", bridge)
        self.assertNotIn("nvs_flash_erase(", bridge)
        self.assertNotIn("esp_ble_mesh_provisioner_erase_settings(", bridge)

    def test_release_version_and_handoff(self):
        for folder in ("gateway", "node"):
            self.assertIn('set(PROJECT_VER "0.3.23-dev")',
                          read(folder + "/CMakeLists.txt"))
        workflow = read(".github/workflows/esp-idf-c3.yml")
        self.assertIn("v0.3.23-dev", workflow)
        # Markdown wraps the button label across lines; normalize whitespace.
        self.assertIn("Удалить устройство",
                      " ".join(read("docs/PROJECT-STATE.md").split()))

if __name__ == "__main__":
    unittest.main()

# Device-version display is compact in UI; Mesh removal and OTA data remain untouched.
