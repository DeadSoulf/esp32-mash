"""Source-level regression guards for first-boot open AP and admin passwords.

Firmware behavior must also be checked on a spare gateway (C3 and S3).
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
GW = (ROOT / "gateway" / "main" / "automesh_web.c").read_text("utf-8")
ADMIN = (ROOT / "gateway" / "main" / "automesh_admin.c").read_text("utf-8")
HTML = (ROOT / "gateway" / "main" / "gateway_index.html").read_text("utf-8")
CMAKE = (ROOT / "gateway" / "main" / "CMakeLists.txt").read_text("utf-8")


class FirstBootAdminTests(unittest.TestCase):
    def test_setup_ap_is_open_without_router(self):
        self.assertIn("setup_mode=!load_home_network()", GW)
        self.assertIn("cfg.ap.authmode=WIFI_AUTH_OPEN", GW)
        self.assertIn("cfg.ap.password[0] = '\\0'", GW)
        self.assertNotIn("cfg.ap.authmode=WIFI_AUTH_WPA2_PSK", GW)
        self.assertIn("WIFI_MODE_APSTA:WIFI_MODE_STA", GW)

    def test_new_admin_is_only_created_during_setup(self):
        self.assertRegex(GW, r"if\s*\(!setup_mode\s*\|\|\s*automesh_admin_is_configured\(\)\)")
        self.assertIn('"/api/admin/setup"', GW)
        self.assertIn('"/api/bootstrap"', GW)
        self.assertIn('"/api/admin/change"', GW)
        self.assertIn("if (!authorize(req)) return ESP_OK;", GW)
        self.assertIn("auth_lock_until_us", GW)

    def test_admin_hash_only_in_nonvolatile_storage(self):
        self.assertIn('nvs_open("meshweb", NVS_READWRITE', ADMIN)
        self.assertIn('nvs_set_blob(h, "admin_v1"', ADMIN)
        self.assertIn("mbedtls_pkcs5_pbkdf2_hmac_ext", ADMIN)
        self.assertIn("esp_fill_random", ADMIN)
        self.assertIn("count >= 6", ADMIN)
        self.assertIn('nvs_erase_key(h, "admin")', ADMIN)
        self.assertNotIn('nvs_set_str(h, "admin"', ADMIN)
        self.assertNotIn('ESP_LOGI(TAG,"Admin token:', GW)
        self.assertNotIn('ESP_LOGI(TAG,"WiFi password:', GW)

    def test_existing_admin_remains_usable(self):
        self.assertIn('nvs_get_str(h, "admin"', ADMIN)
        self.assertIn("ADMIN_LEGACY", ADMIN)
        self.assertIn("automesh_admin_change", ADMIN)
        self.assertIn("if (automesh_admin_is_configured()) return ESP_ERR_INVALID_STATE;", ADMIN)

    def test_first_boot_form_and_settings(self):
        for expected in ("firstAdmin", "createAdminForm", "changeAdminForm",
                         "newAdminKey", "newAdminConfirm", "bootstrapAdmin",
                         "validAdminKey", "Сканировать Wi-Fi"):
            self.assertIn(expected, HTML)
        self.assertIn("automesh_admin.c", CMAKE)
        self.assertIn(" mbedtls ", CMAKE)
        self.assertIn("esp_driver_rmt)", CMAKE)


if __name__ == "__main__":
    unittest.main()

