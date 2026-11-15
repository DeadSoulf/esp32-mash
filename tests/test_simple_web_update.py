"""Regression tests for the single safe OTA workflow in the gateway web UI."""
from html.parser import HTMLParser
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
def read(path): return (ROOT/path).read_text(encoding="utf-8")

class IdCollector(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids=[]
    def handle_starttag(self, tag, attrs):
        values=dict(attrs)
        if "id" in values:self.ids.append(values["id"])

class SimpleWebUpdateTests(unittest.TestCase):
    def test_one_panel_one_form_no_manual_hash(self):
        page=read("gateway/main/gateway_index.html")
        collector=IdCollector()
        collector.feed(page)
        ids=collector.ids
        self.assertEqual(len(ids),len(set(ids)), "Duplicate HTML id")
        for key in ("otaPanel","otaForm","otaTarget","otaFirmware",
                    "otaInstall","otaCancel","otaProgressBar",
                    "otaFileInfo","otaProgress"):
            self.assertEqual(ids.count(key),1)
        for removed in ("gatewayOtaForm","nodeOtaForm","otaSha",
                        "nodeOtaFirmware","nodeOtaDevice","otaUploadButton"):
            self.assertNotIn(removed,ids)
        self.assertNotIn("id=\"otaSha\"",page)

    def test_embedded_image_compatibility_verification(self):
        page=read("gateway/main/gateway_index.html")
        for marker in (
            "function parseOtaImage(bytes)",
            "d.getUint32(32,true)!==0xabcd5432",
            "d.getUint16(12,true)",
            "const version=ascii(48,32)",
            "const project=ascii(80,32)",
            "function verifyOtaTarget(image,option)",
            "image.chip!==option.dataset.chip",
            "image.project!==requiredProject",
            "preparedOta={...selected,...info,target:option.value,file}",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker,page)

    def test_zip_auto_selection_checks_target_role_and_chip(self):
        page=read("gateway/main/gateway_index.html")
        self.assertIn("async function unzipOtaImage(archive,option)",page)
        self.assertIn("Web-OTA/gw-c3.bin",page)
        self.assertIn("Web-OTA/gw-s3.bin",page)
        self.assertIn("Web-OTA/Nodes/node-c3.bin",page)
        self.assertIn("Web-OTA/Nodes/node-s3.bin",page)
        self.assertIn("new DecompressionStream('deflate-raw')",page)
        # Legacy long names in older release ZIPs still load.
        self.assertIn("const legacy=option.dataset.role",page)
        self.assertIn("const acceptedPaths=[wanted.toLowerCase(),legacy.toLowerCase()]",page)
        self.assertIn("Если браузер",read("Windows-Installer/WEB-OTA-README-RU.txt"))

    def test_digest_is_automatic_and_update_is_manual(self):
        page=read("gateway/main/gateway_index.html")
        self.assertIn("const digest=await sha256Hex(selected.bytes)",page)
        self.assertIn("xhr.setRequestHeader('X-Image-SHA256',digest)",page)
        self.assertIn("sha256:digest",page)
        self.assertIn("$('otaForm').addEventListener('submit'",page)
        self.assertNotIn("if(!confirm('Обновить ТОЛЬКО",page)
        self.assertIn("if(nodeOtaCancelled)",page)
        self.assertIn("otaBusy(true,isNode)",page)
        self.assertIn("missing.disabled=true",page)

    def test_original_gateway_and_node_ota_apis_remain(self):
        server=read("gateway/main/automesh_web.c")
        for endpoint in ("/api/ota/gateway","/api/ota/node/start",
                         "/api/ota/node/chunk","/api/ota/node/finish",
                         "/api/ota/node/abort"):
            self.assertIn(endpoint,server)
        self.assertIn("automesh_ota_upload(req)",server)
        self.assertIn("parse_sha256",server)

    def test_ci_checks_javascript_syntax(self):
        ci=read(".github/workflows/esp-idf-c3.yml")
        self.assertIn("node --check automesh-ui-syntax.js",ci)
        self.assertIn("v0.3.19-dev",ci)

if __name__=="__main__":unittest.main()
