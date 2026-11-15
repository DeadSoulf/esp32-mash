"""Static guards for the live authenticated, RAM-only BLE Mesh radio map."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]

def source(relative):
    return (ROOT/relative).read_text(encoding="utf-8")

class MeshMapTests(unittest.TestCase):
    def test_direct_one_hop_beacons_are_separate_from_peer_heartbeat(self):
        node=source("node/main/main.c")
        gateway=source("gateway/main/main.c")
        self.assertIn("ctx.send_ttl=4;",node)
        self.assertIn("ctx.send_ttl=1;",node)
        self.assertIn("ctx.send_ttl=1;",gateway)
        self.assertIn("AUTOMESH_NEIGHBOR_BEACON",node)
        self.assertIn("AUTOMESH_NEIGHBOR_BEACON",gateway)
        self.assertIn("automesh_note_direct_peer(ctx->addr,ctx->recv_rssi)",node)
        self.assertIn("automesh_topology_note_direct(ctx->addr,ctx->recv_rssi)",gateway)
        self.assertIn("AUTOMESH_PEER_HELLO",node)

    def test_compact_authenticated_report_and_expiring_links(self):
        node=source("node/main/main.c")
        gateway=source("gateway/main/main.c")
        topo=source("gateway/main/automesh_topology.c")
        self.assertIn("AUTOMESH_NEIGHBOR_REPORT",node)
        self.assertIn("AUTOMESH_NEIGHBOR_REPORT",gateway)
        self.assertIn("automesh_report_direct_neighbors();",node)
        self.assertIn("automesh_topology_note_report",gateway)
        self.assertIn("AUTOMESH_TOPOLOGY_TTL_US 210000000LL",topo)
        self.assertIn("automesh_mesh_address_approved(link.source)",topo)
        self.assertIn("automesh_mesh_address_approved(link.target)",topo)
        self.assertIn('"source"',topo.replace('\\',''))
        self.assertIn('record_locked(',topo)
        for forbidden in ("nvs_open(", "nvs_set_blob(", "nvs_commit("):
            self.assertNotIn(forbidden,topo)

    def test_topology_endpoint_requires_administrator(self):
        web=source("gateway/main/automesh_web.c")
        self.assertIn('if(!authorize(req))return ESP_OK;',web)
        self.assertIn('"/api/mesh/topology"',web)
        self.assertIn('httpd_register_uri_handler(server,&topology)',web)
        self.assertIn('automesh_topology_json(body,capacity)',web)
        self.assertIn('hc.max_uri_handlers=28;',web)

    def test_svg_map_is_interactive_and_hides_uuid(self):
        html=source("gateway/main/gateway_index.html")
        for marker in ('id="meshTabs"', 'id="showMeshMap"',
                       'id="meshMapPanel"','id="meshSvg"',
                       'id="showSettings"','id="settingsTab"',
                       'function renderMeshMap()', 'meshSvg.addEventListener(\'pointermove\'',
                       "meshSvg.addEventListener('wheel'",
                       "api('/api/mesh/topology')",
                       "meshMapPositions=new Map()",
                       "meshMapLastFetch>=15000",
                       "meshMapPositions.clear()"):
            with self.subTest(marker=marker):
                self.assertIn(marker,html)
        # The map is intentionally the only visual content of the tab.
        for removed in ('meshMapRefresh','meshMapZoomIn','meshMapZoomOut',
                        'meshMapReset','meshMapStatus'):
            self.assertNotIn('id="'+removed+'"',html)
        self.assertIn("meshSvg.addEventListener('dblclick'",html)
        self.assertNotIn('textContent=node.uuid',html)
        self.assertIn("activeMeshTab==='map'",html)
        self.assertIn("const bidirectional=",html)

    def test_ci_and_firmware_version_match(self):
        workflow=source(".github/workflows/esp-idf-c3.yml")
        self.assertIn("AutoMesh-v0.3.23-dev.zip",workflow)
        for name in ("gateway","node"):
            self.assertIn('set(PROJECT_VER "0.3.23-dev")',
                          source(name+"/CMakeLists.txt"))
        self.assertIn("automesh_topology.c",
                      source("gateway/main/CMakeLists.txt"))
        self.assertIn("v0.3.23-dev",source("docs/PROJECT-STATE.md"))

if __name__=="__main__": unittest.main()
