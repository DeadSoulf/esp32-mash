"""USB host proof-of-concept safety and interface regression tests."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
def load(path):
    return (ROOT/path).read_text(encoding="utf-8")

class UsbServerTests(unittest.TestCase):
    def test_pc_project_is_cross_platform(self):
        csproj=load("server/AutoMesh.Server.csproj")
        self.assertIn("<TargetFramework>net10.0</TargetFramework>",csproj)
        self.assertIn("System.IO.Ports",csproj)
        source=load("server/UsbMeshBridge.cs")
        self.assertIn("SerialPort.GetPortNames()",source)
        self.assertIn("port.ReadLine()",source)
        self.assertIn('port.WriteLine("HELLO")',source)
        self.assertIn('port.WriteLine("LIST")',source)
        self.assertIn("await Task.Delay(1200",source)

    def test_usb_gateway_mode_is_explicit_and_persisted(self):
        bridge=load("gateway/main/automesh_bridge.c")
        main=load("gateway/main/main.c")
        self.assertIn("nvs_set_u8(h,\"usb_mode\"",bridge)
        self.assertIn("automesh_bridge_usb_mode()",main)
        self.assertIn("if(usb_only)",main)
        self.assertIn("if (!usb_only && !automesh_web_has_network())",main)
        self.assertIn("esp_restart()",bridge)
        self.assertIn("automesh_bridge_start();",main)
        self.assertIn("USBMODE",bridge)
        self.assertIn("snapshot_end",bridge)
        self.assertIn("snapshot_begin",bridge)
        self.assertIn('cJSON_AddStringToObject(obj,"event","device")',bridge)
        self.assertNotIn("nvs_flash_erase(",bridge)

    def test_unknown_nodes_require_manual_approval(self):
        main=load("gateway/main/main.c")
        bridge=load("gateway/main/automesh_bridge.c")
        server=load("server/Program.cs")
        ui=load("server/wwwroot/index.html")
        self.assertIn("Discovery NEVER queues a device for provisioning",main)
        self.assertIn('strcmp(verb,"APPROVE")',bridge)
        self.assertIn("ADD_DEV_START_PROV_NOW_FLAG",bridge)
        self.assertIn("/api/devices/{uuid}/approve",server)
        self.assertIn("Authorized(request, secret)",server)
        self.assertIn("confirm(",ui)

    def test_local_only_default_and_token(self):
        server=load("server/Program.cs")
        self.assertIn("http://127.0.0.1:8765",server)
        self.assertIn("CryptographicOperations.FixedTimeEquals",server)
        self.assertIn("AUTOMESH_SERVER_TOKEN",load("server/ServerSecret.cs"))
        self.assertNotIn("app.UseCors(",server)

    def test_release_has_windows_and_linux_packages(self):
        workflow=load(".github/workflows/esp-idf-c3.yml")
        self.assertIn("win-x64",workflow)
        self.assertIn("linux-x64",workflow)
        self.assertIn("AutoMesh-Server-windows-x64.zip",workflow)
        self.assertIn("AutoMesh-Server-linux-x64.tar.gz",workflow)

if __name__=="__main__":
    unittest.main()
