"""Guard against Mesh OTA ACKs disappearing in the ESP-IDF vendor client.

When esp_ble_mesh_client_model_send_msg(..., need_rsp=false) is used, a
STATUS is unsolicited and is delivered via CLIENT_MODEL_RECV_PUBLISH_MSG_EVT,
not MODEL_OPERATION_EVT. Old firmware ignored every such ACK and timed out.
"""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
def read(p): return (ROOT/p).read_text(encoding="utf-8")

class MeshOtaAckDelivery(unittest.TestCase):
    def test_gateway_handles_unsolicited_client_status(self):
        main=read("gateway/main/main.c")
        protocol=read("gateway/main/automesh_gateway_node_ota.c")
        self.assertIn("ESP_BLE_MESH_CLIENT_MODEL_RECV_PUBLISH_MSG_EVT",main)
        self.assertIn("param->client_recv_publish_msg.opcode",main)
        self.assertIn("param->client_recv_publish_msg.model",main)
        self.assertIn("param->client_recv_publish_msg.ctx",main)
        self.assertIn("param->client_recv_publish_msg.msg",main)
        self.assertIn("opcode==AUTOMESH_NODE_OTA_STATUS && len==7",main)
        self.assertIn("automesh_gateway_node_ota_on_status(ctx->addr,payload,len)",main)
        self.assertIn("ESP_BLE_MESH_MODEL_OPERATION_EVT",main)
        self.assertIn("automesh_gateway_node_ota_on_status(param->model_operation.ctx->addr",main)
        self.assertRegex(protocol,r"\(uint16_t\)len, \(uint8_t \*\)payload, 0, false, ROLE_PROVISIONER")
        self.assertIn("transfer.address==source && transfer.id==id",protocol)
        self.assertIn("transfer.expected_ack=expected_offset",protocol)
        self.assertIn("xSemaphoreGive(reply_event)",protocol)

    def test_status_returns_through_relays(self):
        node=read("node/main/automesh_node_ota.c")
        self.assertRegex(node,r"reply\.send_ttl\s*=\s*5")
        self.assertIn("esp_ble_mesh_server_model_send_msg(",node)
        self.assertIn("AUTOMESH_NODE_OTA_STATUS",node)
        self.assertIn("Status ACK sent",node)
        self.assertIn("Status reply FAILED",node)
        self.assertIn("ctx->addr != 0x0001",node)
        self.assertNotIn("esp_ble_mesh_node_local_reset(",node)
        self.assertNotIn("nvs_flash_erase(",node)

    def test_no_automatic_firmware_install(self):
        page=read("gateway/main/gateway_index.html")
        self.assertIn("$('otaForm').addEventListener('submit'",page)
        self.assertIn("if(!file||!option||option.disabled)",page.replace(" ","").replace("\\n",""))
        self.assertIn("image.project!==requiredProject",page)
        self.assertIn("sha256:digest",page)

    def test_version_and_build_pipeline(self):
        for p in ("gateway/CMakeLists.txt","node/CMakeLists.txt"):
            self.assertIn('set(PROJECT_VER "0.3.24-dev")',read(p))
        workflow=read(".github/workflows/esp-idf-c3.yml")
        self.assertIn("AutoMesh-v0.3.24-dev.zip",workflow)
        self.assertIn("node --check automesh-ui-syntax.js",workflow)
        self.assertIn("OTA",read("docs/PROJECT-STATE.md"))

if __name__=="__main__":
    unittest.main()
