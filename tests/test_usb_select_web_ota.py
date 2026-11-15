"""Regression tests for USB controller selection and gateway web OTA safety."""
from pathlib import Path
import importlib.util
import io
import sys
import types
from contextlib import redirect_stdout
from unittest.mock import patch
import unittest

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "Windows-Installer" / "installer.py"
OTA = (ROOT / "gateway" / "main" / "automesh_ota.c").read_text(encoding="utf-8")
WEB = (ROOT / "gateway" / "main" / "automesh_web.c").read_text(encoding="utf-8")
HTML = (ROOT / "gateway" / "main" / "gateway_index.html").read_text(encoding="utf-8")
MAIN = (ROOT / "gateway" / "main" / "main.c").read_text(encoding="utf-8")

DEVICES = [
    {"port":"COM12","chip":"ESP32-C3","mac":"a0:85:e3:49:71:8c","flash":4194304},
    {"port":"COM16","chip":"ESP32-S3","mac":"50:78:7d:18:ba:20","flash":4194304},
]


class USBInstallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Installer requires esptool + pyserial, but these modules are only
        # imported, never used to access USB in these unit tests.
        spec=importlib.util.spec_from_file_location("automesh_usb_installer",INSTALLER)
        cls.installer=importlib.util.module_from_spec(spec)
        fake_esptool=types.ModuleType("esptool")
        fake_serial=types.ModuleType("serial")
        fake_tools=types.ModuleType("serial.tools")
        fake_list_ports=types.ModuleType("serial.tools.list_ports")
        fake_tools.list_ports=fake_list_ports
        fake_serial.tools=fake_tools
        # Tests exercise choice and validation only: no COM access needed.
        with patch.dict(sys.modules,{
            "esptool":fake_esptool,
            "serial":fake_serial,
            "serial.tools":fake_tools,
            "serial.tools.list_ports":fake_list_ports,
        }):
            spec.loader.exec_module(cls.installer)

    def test_controller_selection_never_defaults_even_if_only_one(self):
        with patch("builtins.input",return_value="0"):
            with self.assertRaises(RuntimeError):
                self.installer.prompt_device(DEVICES[:1],None)

    def test_user_selects_s3_and_explicit_gateway_role(self):
        with patch("builtins.input",side_effect=["2","1"]):
            selected=self.installer.prompt_device(DEVICES,None)
            role=self.installer.choose_role(None,None)
        self.assertEqual(selected["port"],"COM16")
        self.assertEqual(role,"gateway")

    def test_explicit_port_can_select_only_that_controller(self):
        selected=self.installer.prompt_device(DEVICES,"COM12")
        self.assertEqual(selected["mac"],DEVICES[0]["mac"])
        with self.assertRaises(RuntimeError):
            self.installer.prompt_device(DEVICES,"COM31")

    def test_already_detected_role_still_requires_selection(self):
        with patch("builtins.input",return_value="1"):
            self.assertEqual(self.installer.choose_role("gateway",None),"gateway")

    def test_no_flash_when_selection_cancelled(self):
        with patch("builtins.input",return_value="0"):
            with self.assertRaises(RuntimeError):
                self.installer.prompt_device(DEVICES,None)

    def test_install_uses_only_role_and_one_confirmation_with_explicit_port(self):
        from pathlib import Path
        dev=DEVICES[0]
        manifest={"schema":2,"profiles":{"esp32c3-supermini-4mb":{"roles":{"gateway":{},"node":{}}}}}
        profile={"board_profile":"ESP32-C3 SuperMini", "led_profile":"GPIO8"}
        with patch.object(self.installer,"read_manifest",return_value=manifest), \
             patch.object(self.installer,"identify",return_value=dev) as identify, \
             patch.object(self.installer,"matching_profile",return_value=("esp32c3-supermini-4mb",profile)), \
             patch.object(self.installer,"role_assets",return_value={"application.bin":Path("app.bin")}), \
             patch.object(self.installer,"backup_flash",return_value=Path("saved.bin")) as backup, \
             patch.object(self.installer,"existing_role",return_value="node"), \
             patch.object(self.installer,"flash_device") as flash, \
             patch("builtins.input",side_effect=["1","1"]) as question, \
             patch.object(sys,"argv",["installer.py","--port","COM12"]):
            self.assertEqual(self.installer.main(),0)
        self.assertEqual(question.call_count,2)  # Role + one final confirmation
        self.assertEqual(identify.call_count,2)   # Before and after backing up
        backup.assert_called_once_with(dev)
        flash.assert_called_once()
        self.assertFalse(flash.call_args.kwargs["reset_nvs"])

    def test_cancel_last_step_does_not_flash(self):
        from pathlib import Path
        dev=DEVICES[0]
        manifest={"schema":2,"profiles":{"c3":{"roles":{"gateway":{},"node":{}}}}}
        with patch.object(self.installer,"read_manifest",return_value=manifest), \
             patch.object(self.installer,"identify",return_value=dev), \
             patch.object(self.installer,"matching_profile",return_value=("c3",{"board_profile":"C3","led_profile":"GPIO8"})), \
             patch.object(self.installer,"role_assets",return_value={"application.bin":Path("app.bin")}), \
             patch.object(self.installer,"backup_flash",return_value=Path("saved.bin")), \
             patch.object(self.installer,"existing_role",return_value="node"), \
             patch.object(self.installer,"flash_device") as flash, \
             patch("builtins.input",side_effect=["1","0"]) as question, \
             patch.object(sys,"argv",["installer.py","--port","COM12"]):
            self.assertEqual(self.installer.main(),5)
        self.assertEqual(question.call_count,2)
        flash.assert_not_called()

    def test_nvs_erasure_requires_explicit_cli_flag(self):
        from pathlib import Path
        dev=DEVICES[0]
        manifest={"schema":2,"profiles":{"c3":{"roles":{"gateway":{},"node":{}}}}}
        with patch.object(self.installer,"read_manifest",return_value=manifest), \
             patch.object(self.installer,"identify",return_value=dev), \
             patch.object(self.installer,"matching_profile",return_value=("c3",{"board_profile":"C3","led_profile":"GPIO8"})), \
             patch.object(self.installer,"role_assets",return_value={"application.bin":Path("app.bin")}), \
             patch.object(self.installer,"backup_flash",return_value=Path("saved.bin")), \
             patch.object(self.installer,"existing_role",return_value="node"), \
             patch.object(self.installer,"flash_device") as flash, \
             patch("builtins.input",side_effect=["1","1"]), \
             patch.object(sys,"argv",["installer.py","--port","COM12","--erase-nvs"]):
            self.assertEqual(self.installer.main(),0)
        self.assertTrue(flash.call_args.kwargs["reset_nvs"])


class WebGatewayOTATests(unittest.TestCase):
    def test_ota_only_runs_with_gateway_admin_and_valid_origin(self):
        self.assertIn('if(!authorize(req))return ESP_OK;',WEB)
        self.assertIn('if(!valid_origin(req))',WEB)
        self.assertIn('"/api/ota/gateway"',WEB)
        self.assertIn("if(setup_mode)",WEB)
        self.assertIn("return automesh_ota_upload(req);",WEB)

    def test_only_inactive_app_partition_is_written(self):
        self.assertIn("esp_ota_get_next_update_partition(NULL)",OTA)
        self.assertIn("target->address==running->address",OTA)
        self.assertIn("esp_ota_begin(target,req->content_len,&handle)",OTA)
        self.assertIn("esp_ota_write(handle",OTA)
        self.assertIn("esp_ota_end(handle)",OTA)
        self.assertIn("esp_ota_set_boot_partition(target)",OTA)
        self.assertNotIn("nvs_flash_erase(",OTA)

    def test_validates_image_chip_integrity_and_project(self):
        for phrase in ("ESP_CHIP_ID_ESP32C3","ESP_CHIP_ID_ESP32S3",
                       "ESP_IMAGE_HEADER_MAGIC", "sha_matches(digest,sha_expected)",
                       "esp_ota_get_partition_description(target,&new_app)",
                       "esp_app_get_description()->project_name"):
            self.assertIn(phrase,OTA)

    def test_ota_confirmation_waits_for_router_ip(self):
        self.assertIn("if(server)automesh_mark_boot_healthy();",WEB)
        self.assertIn("if(!setup_mode && got_ip)",WEB)
        self.assertIn("void automesh_mark_boot_healthy(void)",MAIN)
        self.assertIn("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
                      (ROOT/"gateway/sdkconfig.defaults").read_text("utf-8"))

    def test_unified_web_update_checks_gateway_binary_before_upload(self):
        # The former gatewayOtaForm was replaced by a single OTA form.
        # Chip and project are validated in the browser; the gateway's
        # existing OTA implementation independently validates the image.
        self.assertIn('id="otaForm"',HTML)
        self.assertIn('id="otaTarget"',HTML)
        self.assertIn('id="otaFirmware"',HTML)
        self.assertIn("new XMLHttpRequest()",HTML)
        self.assertIn("'/api/ota/gateway'",HTML)
        self.assertIn("function parseOtaImage(bytes)",HTML)
        self.assertIn("image.project!==requiredProject",HTML)
        self.assertIn("bytes.length>0x1c0000",HTML)
        self.assertIn("host_ota_ready",HTML)


if __name__=="__main__":
    unittest.main()

