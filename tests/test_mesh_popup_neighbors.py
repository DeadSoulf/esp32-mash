"""Regression checks for real direct-neighbor links and interactive node popup.

The BLE access context may have unknown RSSI (0 / -128). Such packets
are still authenticated TTL=1 neighbor observations; do not drop them.
Only mark quality unknown; never fabricate a dBm number or parent route.
"""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
def read(p): return (ROOT/p).read_text(encoding="utf-8")

class MeshPopupAndNeighborsTests(unittest.TestCase):
    def test_beacons_with_missing_rssi_are_still_link_observations(self):
        gateway=read("gateway/main/automesh_topology.c")
        node=read("node/main/main.c")
        self.assertIn("static int8_t reliable_rssi",gateway)
        self.assertIn("record_locked(0x0001,neighbor,reliable_rssi(rssi),now)",gateway)
        self.assertIn("record_locked(reporter,neighbor,reliable_rssi(rssi),now)",gateway)
        self.assertIn('if(link.rssi==-128)strcpy(rssi_text,"null")',gateway)
        self.assertIn("rssi >= 0 || rssi == -128)rssi=-128",node)
        self.assertIn("automesh_note_direct_peer(ctx->addr,ctx->recv_rssi)",node)
        self.assertIn("ctx.send_ttl=1;",node)
        self.assertNotIn("nvs_flash_erase(",gateway)
        self.assertNotIn("nvs_commit(",gateway)

    def test_live_topology_api_includes_radio_diagnostics(self):
        topo=read("gateway/main/automesh_topology.c")
        web=read("gateway/main/automesh_web.c")
        self.assertIn("direct_beacon_count++",topo)
        self.assertIn("neighbor_report_count++",topo)
        self.assertIn("last_beacon_age",topo)
        self.assertIn("last_report_age",topo)
        self.assertIn("automesh_mesh_address_approved(link.source)",topo)
        self.assertIn("automesh_mesh_address_approved(link.target)",topo)
        self.assertIn("if(!authorize(req))return ESP_OK;",web)
        self.assertIn('"/api/mesh/topology"',web)

    def test_menu_is_only_on_demand_map_overlay(self):
        html=read("gateway/main/gateway_index.html")
        for markup in ('id="meshNodeMenu"','id="meshMenuTitle"',
                       'id="meshMenuInfo"','id="meshMenuNeighbors"',
                       'id="meshMenuActions"','id="meshMenuFeedback"',
                       'id="meshMenuLocate"','id="meshMenuClose"'):
            with self.subTest(markup=markup): self.assertIn(markup,html)
        self.assertIn('role="dialog"',html)
        self.assertIn('id="meshNodeMenu" class="mesh-popup" role="dialog" aria-label="Устройство Mesh" hidden',html)
        self.assertIn("function openMeshNodeMenu(address,clientX,clientY)",html)
        self.assertIn("function closeMeshNodeMenu()",html)
        self.assertIn("function refreshMeshNodeMenu()",html)
        self.assertIn("document.addEventListener('keydown'",html)
        self.assertIn("event.key==='Escape'",html)
        self.assertIn("Math.hypot(",html)
        self.assertIn("if(!cancelled&&!drag.moved&&drag.address!==null)",html)
        self.assertIn("openMeshNodeMenu(drag.address,event.clientX,event.clientY)",html)
        self.assertIn("openMeshNodeMenu(Number(node.getAttribute('data-address'))",html)
        self.assertIn("'mesh-node'",html)
        self.assertIn("role:'button'",html)

    def test_locate_reuses_existing_mesh_command_and_shows_feedback(self):
        html=read("gateway/main/gateway_index.html")
        self.assertIn("api('/api/locate','POST',{address,seconds:15})",html)
        self.assertIn("api('/api/locate/status')",html)
        self.assertIn("result.status==='confirmed'",html)
        self.assertIn("result.status==='failed'||result.status==='timeout'",html)
        self.assertIn("meshMapLocateBusy",html)
        self.assertIn("button.hidden=gateway",html)
        self.assertIn("meshMapSelectedAddress===address",html)
        self.assertIn("textContent=str",html)

    def test_link_quality_never_invents_rssi_or_fixed_parent(self):
        html=read("gateway/main/gateway_index.html")
        self.assertIn("link.rssi===null||link.rssi===undefined",html)
        self.assertIn("rssi===null?'#91a1b2':meshSignalColor(rssi)",html)
        self.assertIn("rssi===null?'RSSI —':rssi+' dBm'",html)
        self.assertIn("meshRssiText(measured)",html)
        self.assertIn("meshMapDiagnostics=result.diagnostics||{}",html)
        self.assertIn("Прямые радиососеди пока не обнаружены.",html)
        self.assertNotIn("meshParent=",html)

    def test_release_version(self):
        for role in ("gateway","node"):
            self.assertIn('set(PROJECT_VER "0.3.21-dev")',read(role+"/CMakeLists.txt"))
        self.assertIn("AutoMesh-v0.3.21-dev.zip",read(".github/workflows/esp-idf-c3.yml"))

if __name__=="__main__": unittest.main()
