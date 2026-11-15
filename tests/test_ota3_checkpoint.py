"""OTA3 checks. Live radio throughput requires actual ESP32 boards."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
def source(path):
    return (ROOT / path).read_text(encoding="utf-8")

class Ota3CheckpointTests(unittest.TestCase):
    def test_same_protocol_on_gateway_and_node(self):
        a=source("gateway/main/automesh_ota_protocol.h")
        b=source("node/main/automesh_ota_protocol.h")
        self.assertEqual(a,b)
        for name in ("STREAM","CHECKPOINT","STATUS"):
            self.assertIn("AUTOMESH_NODE_OTA_"+name,a)
        self.assertIn("AUTOMESH_NODE_OTA_STREAM_WINDOW 4u",a)

    def test_capability_negotiated_not_inferred(self):
        bridge=source("gateway/main/automesh_bridge.c")
        send=source("gateway/main/automesh_gateway_node_ota.c")
        self.assertIn('strstr(c->fw_version," OTA3 ")!=NULL',bridge)
        self.assertIn("automesh_node_ota_stream_supported(address)",send)
        self.assertIn(".stream=use_stream",send)
        self.assertIn("if(!transfer.stream)",send)
        self.assertIn("automesh_gateway_node_ota_chunk(pos,data+used,n,confirmed)",send)

    def test_checkpoint_controls_flash_progress(self):
        send=source("gateway/main/automesh_gateway_node_ota.c")
        callback=source("gateway/main/main.c")
        self.assertIn("ESP_BLE_MESH_MODEL_SEND_COMP_EVT",callback)
        self.assertIn("automesh_gateway_node_ota_on_send_complete(",callback)
        self.assertIn("xSemaphoreTake(stream_event,pdMS_TO_TICKS(20000))",send)
        self.assertIn("AUTOMESH_NODE_OTA_CHECKPOINT,query,sizeof(query),goal",send)
        self.assertIn("resume=received",send)
        self.assertIn("transfer.allow_short_ack",send)
        self.assertIn("received<resume || received>goal",send)

    def test_worker_write_and_image_checks_preserved(self):
        node=source("node/main/automesh_node_ota.c")
        main=source("node/main/main.c")
        self.assertIn("ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_STREAM, 7)",main)
        self.assertIn("ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_CHECKPOINT, 2)",main)
        self.assertIn("if (!streamed) ack(model, ctx, id, ota.received, 0)",node)
        self.assertIn("esp_ota_write(ota.handle",node)
        self.assertIn("memcmp(actual, ota.expected_sha, 32)",node)
        self.assertIn("esp_ota_set_boot_partition",node)
        self.assertNotIn("nvs_flash_erase(",node)

    def test_web_api_and_user_interface_agree(self):
        web=source("gateway/main/automesh_web.c")
        ui=source("gateway/main/gateway_index.html")
        self.assertIn("automesh_gateway_node_ota_streaming()",web)
        self.assertIn("session.transport===\'OTA3\'",ui)
        self.assertIn("automesh_gateway_node_ota_batch(",web)
        self.assertIn("req->content_len>4*step",web)

if __name__=="__main__": unittest.main()
