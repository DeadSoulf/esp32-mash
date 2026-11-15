"""UI layout: Devices, Map-only, and Settings tabs.

These checks prevent hidden OTA/Wi-Fi panels from leaking into the Mesh map,
and preserve first-boot Wi-Fi access, authentication and admin feedback.
"""
from html.parser import HTMLParser
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / "gateway/main/gateway_index.html").read_text(encoding="utf-8")

class Structure(HTMLParser):
    VOID = {"input", "meta", "link", "br", "img", "hr", "source", "area"}
    def __init__(self):
        super().__init__()
        self.path = []
        self.ids = {}
        self.children = {}
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        ident = attrs.get("id")
        if ident:
            self.ids[ident] = {"tag": tag, "attrs": attrs,
                               "ancestors": tuple(self.path)}
        for ancestor in self.path:
            self.children.setdefault(ancestor, []).append((tag, ident))
        if tag not in self.VOID:
            self.path.append(ident or "")
    def handle_endtag(self, tag):
        if self.path:
            self.path.pop()

class TabLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = Structure()
        cls.doc.feed(HTML)
    def test_exactly_three_tabs(self):
        tabs = [name for name, item in self.doc.ids.items()
                if item["attrs"].get("role") == "tab"]
        self.assertEqual(set(tabs),
                         {"showDevices", "showMeshMap", "showSettings"})
        expected = {"showDevices": "devicesTab",
                    "showMeshMap": "meshMapPanel",
                    "showSettings": "settingsTab"}
        for tab, panel in expected.items():
            with self.subTest(tab=tab):
                self.assertEqual(self.doc.ids[tab]["attrs"]["aria-controls"], panel)
                self.assertEqual(self.doc.ids[panel]["attrs"]["role"], "tabpanel")

    def test_map_tab_contains_only_svg_and_context_menu(self):
        children = self.doc.children["meshMapPanel"]
        # Only the SVG map and its transient device popup; no static
        # forms, extra panels or management sections in the map tab.
        ids = [ident for _,ident in children]
        self.assertIn("meshSvg", ids)
        self.assertIn("meshScene", ids)
        self.assertIn("meshNodeMenu", ids)
        self.assertIn("meshMenuLocate", ids)
        self.assertIn("hidden", self.doc.ids["meshNodeMenu"]["attrs"])
        self.assertFalse(any(tag in ("form", "h2", "section") for tag,_ in children))
        self.assertNotIn("Радиосвязь и индикация", HTML)
        for old in ("meshMapRefresh", "meshMapZoomIn", "meshMapZoomOut",
                    "meshMapReset", "meshMapStatus"):
            self.assertNotIn(old, self.doc.ids)
        self.assertIn("meshSvg.addEventListener('wheel'", HTML)
        self.assertIn("meshSvg.addEventListener('pointermove'", HTML)
        self.assertIn("meshSvg.addEventListener('dblclick'", HTML)

    def test_wifi_and_admin_are_both_inside_settings(self):
        ids=self.doc.ids
        for ident in ("wifiSetup", "wifiForm", "scanWifi", "ssid",
                      "webAddress", "adminChange", "changeAdminForm",
                      "settingsMessage"):
            with self.subTest(ident=ident):
                self.assertIn("settingsTab", ids[ident]["ancestors"])
                self.assertNotIn("meshMapPanel", ids[ident]["ancestors"])
        self.assertNotIn("settingsTab", ids["firstAdmin"]["ancestors"])
        self.assertIn("settingsTab", ids["settingsMessage"]["ancestors"])
        self.assertIn("$('settingsMessage').textContent=s;", HTML)

    def test_overview_and_ota_are_not_in_map_or_settings(self):
        ids=self.doc.ids
        for ident in ("found", "live", "waiting", "devicesPanel", "devices",
                      "otaPanel", "otaForm", "otaTarget", "otaCancel"):
            with self.subTest(ident=ident):
                self.assertIn("devicesTab", ids[ident]["ancestors"])
                self.assertNotIn("meshMapPanel", ids[ident]["ancestors"])
        self.assertIn("if(state.setup_mode && !settingsSetupShown)", HTML)
        self.assertIn("activeMeshTab='settings';", HTML)
        self.assertIn("hideMainTabs();", HTML)
        self.assertIn("applyMeshTabs();", HTML)
        self.assertIn("[hidden]{display:none!important}", HTML)

    def test_no_dead_javascript_dom_id_references(self):
        # A removed visual control must not cause a JS null dereference at load.
        refs = set(re.findall(r"\$\('([A-Za-z][\w-]*)'\)", HTML))
        missing = refs - set(self.doc.ids)
        self.assertFalse(missing, f"Unknown DOM identifiers: {sorted(missing)}")

    def test_release_version(self):
        for role in ("gateway", "node"):
            project=(ROOT/role/"CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn('set(PROJECT_VER "0.3.26-dev")', project)

if __name__ == "__main__":
    unittest.main()
