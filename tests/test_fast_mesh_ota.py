"""OTA2 compatibility and size guards: fast blocks only for advertised OTA2."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
def src(path): return (ROOT/path).read_text(encoding="utf-8")

class FastMeshOtaTests(unittest.TestCase):
    def test_protocol_preserves_ota1_and_limits_ota2(self):
        gateway=src("gateway/main/automesh_ota_protocol.h")
        node=src("node/main/automesh_ota_protocol.h")
        self.assertEqual(gateway,node)
        self.assertIn("#define AUTOMESH_NODE_OTA_LEGACY_BLOCK 96u",gateway)
        self.assertIn("#define AUTOMESH_NODE_OTA_BLOCK 224u",gateway)
        self.assertIn("AUTOMESH_NODE_OTA_MAX_IMAGE 0x1c0000u",gateway)
        for name in ("START","DATA","FINISH","ABORT","STATUS"):
            self.assertIn("AUTOMESH_NODE_OTA_"+name,gateway)

    def test_explicit_version_negotiation_no_oversized_legacy_packet(self):
        bridge=src("gateway/main/automesh_bridge.c")
        sender=src("gateway/main/automesh_gateway_node_ota.c")
        header=src("gateway/main/automesh_gateway_node_ota.h")
        for feature in ('" OTA1 "','" OTA2 "'):
            self.assertIn(feature,bridge)
        self.assertIn("automesh_node_ota_block_size(uint16_t address)",bridge)
        self.assertIn("max_chunk!=AUTOMESH_NODE_OTA_LEGACY_BLOCK",sender)
        self.assertIn("max_chunk!=AUTOMESH_NODE_OTA_BLOCK",sender)
        self.assertIn(".chunk_len=max_chunk",sender)
        self.assertIn("if(len>transfer.chunk_len) return ESP_ERR_INVALID_ARG;",sender)
        self.assertIn("automesh_gateway_node_ota_max_chunk",header)
        self.assertIn("AUTOMESH_NODE_OTA_LEGACY_BLOCK",bridge)

    def test_node_advertises_ota2_and_supports_both_data_sizes(self):
        node=src("node/main/main.c")
        ota=src("node/main/automesh_node_ota.c")
        self.assertIn(" OTA3 %02x",node)
        self.assertIn("AUTOMESH_NODE_OTA_BLOCK",ota)
        self.assertIn("AUTOMESH_NODE_OTA_STREAM",ota)
        self.assertIn("AUTOMESH_NODE_OTA_CHECKPOINT",ota)
        self.assertIn("esp_ota_write(ota.handle",ota)
        self.assertIn("mbedtls_sha256_update(&ota.sha",ota)
        self.assertIn("memcmp(actual, ota.expected_sha, 32)",ota)
        self.assertIn("esp_ota_set_boot_partition",ota)
        self.assertNotIn("nvs_flash_erase(",ota)

    def test_web_advertises_max_chunk_and_client_never_guesses_fast(self):
        server=src("gateway/main/automesh_web.c")
        html=src("gateway/main/gateway_index.html")
        self.assertIn('max_chunk',server)
        self.assertIn("automesh_gateway_node_ota_max_chunk()",server)
        self.assertIn("[96,224].includes(Number(session.max_chunk))",html)
        self.assertIn("Number(session.max_chunk):96",html)
        self.assertIn("offset+=batchBytes",html)
        self.assertIn("session.transport==='OTA3'",html)
        self.assertIn("offset+block.length",html)
        self.assertIn("const batchBytes=blockBytes*4",html)
        self.assertIn("/api/ota/node/batch",html)
        self.assertIn("req->content_len>4*step",server)
        self.assertIn("automesh_gateway_node_ota_chunk(",server)
        self.assertIn("const speed=Math.round(done/elapsed)",html)
        self.assertIn("const timeLeft=",html)
        self.assertIn("v.includes(' OTA1 ')||v.includes(' OTA2 ')",html)

    def test_all_versions_match(self):
        for role in ("gateway","node"):
            self.assertIn('set(PROJECT_VER "0.3.23-dev")',src(role+"/CMakeLists.txt"))
        self.assertIn("AutoMesh-v0.3.23-dev.zip",src(".github/workflows/esp-idf-c3.yml"))

if __name__=="__main__":unittest.main()
