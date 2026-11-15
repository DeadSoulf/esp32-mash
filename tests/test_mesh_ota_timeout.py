"""Regressions: Mesh OTA must not block BLE callback and must diagnose timeouts."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT/path).read_text(encoding="utf-8")

class MeshOtaTimeoutRecoveryTests(unittest.TestCase):
    def test_flash_is_offloaded_from_ble_mesh_callback(self):
        node=read("node/main/automesh_node_ota.c")
        main=read("node/main/main.c")
        self.assertIn('esp_err_t automesh_node_ota_init(void)',node)
        self.assertIn('automesh_node_ota_init();',main)
        self.assertIn('xQueueCreate(4, sizeof(ota_message_t))',node)
        self.assertIn('xTaskCreate(ota_worker_task',node)
        self.assertIn('xQueueSend(ota_requests, &message, 0)',node)
        self.assertIn('process_ota_command(message.model, &message.ctx',node)
        callback=node.split('bool automesh_node_ota_command(',1)[1]
        self.assertNotIn('esp_ota_begin(',callback)
        self.assertNotIn('esp_ota_write(',callback)
        self.assertNotIn('esp_ota_end(',callback)

    def test_sequential_erase_and_idempotent_start(self):
        node=read("node/main/automesh_node_ota.c")
        self.assertIn('esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle)',node)
        self.assertNotIn('esp_ota_begin(part, size, &handle)',node)
        self.assertIn('ota.id == id && ota.size == size',node)
        self.assertIn('memcmp(ota.expected_sha, p + 7, 32) == 0',node)
        self.assertIn('ack(model, ctx, id, ota.received, 0)',node)
        self.assertNotIn('nvs_flash_erase(',node)
        self.assertIn('esp_ota_set_boot_partition(',node)

    def test_mesh_sender_retries_start_and_data_but_not_finish(self):
        gw=read("gateway/main/automesh_gateway_node_ota.c")
        self.assertIn('opcode==AUTOMESH_NODE_OTA_START ||',gw)
        self.assertIn('opcode==AUTOMESH_NODE_OTA_DATA) ? 3u : 1u',gw)
        self.assertIn('attempt<=attempts',gw)
        self.assertIn('ESP_ERR_TIMEOUT',gw)
        self.assertIn('ACK TIMEOUT stage=',gw)
        self.assertIn('transfer.awaiting=false',gw)

    def test_status_contains_failed_stage_and_health_poll_stops(self):
        web=read("gateway/main/automesh_web.c")
        main=read("gateway/main/main.c")
        html=read("gateway/main/gateway_index.html")
        for stage in ("start","data","finish","abort"):
            self.assertIn('node_ota_error(req,result,"'+stage+'")',web)
        self.assertIn('esp_err_to_name(err),stage',web)
        self.assertIn('if (automesh_gateway_node_ota_active())',main)
        self.assertIn('if(reply.stage)',html)

    def test_wifi_and_admin_settings_are_in_dedicated_tab(self):
        html=read("gateway/main/gateway_index.html")
        settings=html.index('<div id="settingsTab"')
        wifi=html.index('<section id="wifiSetup"')
        admin=html.index('<section id="adminChange"')
        self.assertLess(settings,wifi)
        self.assertLess(wifi,admin)
        self.assertLess(admin,html.index("</main>"))
        self.assertNotIn("Радиосвязь и индикация",html)
        self.assertIn('id="wifiForm"',html)
        self.assertIn('id="scanWifi"',html)
        self.assertIn('id="changeAdminForm"',html)

    def test_release_version(self):
        self.assertIn("AutoMesh-v0.3.18-dev.zip",
                      read(".github/workflows/esp-idf-c3.yml"))
        for role in ("gateway","node"):
            self.assertIn('set(PROJECT_VER "0.3.18-dev")',
                          read(role+"/CMakeLists.txt"))

if __name__=="__main__":
    unittest.main()
