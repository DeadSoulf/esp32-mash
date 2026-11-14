"""Regression coverage: invalid admin credentials must never flood gateway HTTP APIs."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
UI = (ROOT / "gateway" / "main" / "gateway_index.html").read_text("utf-8")
WEB = (ROOT / "gateway" / "main" / "automesh_web.c").read_text("utf-8")


class AuthenticationPollingTests(unittest.TestCase):
    def test_failed_auth_clears_saved_token(self):
        self.assertIn("function invalidateAuth(reason)", UI)
        self.assertIn("sessionStorage.removeItem('automesh_admin')", UI)
        self.assertIn("resp.status===401", UI)
        self.assertIn("invalidateAuth('Неверный ключ администратора.", UI)

    def test_429_stops_retry_and_displays_cooldown(self):
        self.assertIn("resp.status===429", UI)
        self.assertIn("resp.headers.get('Retry-After')", UI)
        self.assertIn("authBlockedUntil=Date.now()+seconds*1000", UI)
        self.assertIn("Date.now()<authBlockedUntil", UI)
        self.assertIn('httpd_resp_set_hdr(req, "Retry-After", retry)', WEB)

    def test_status_checks_are_serial_not_concurrent(self):
        self.assertIn("if(!token || updateBusy || uploadingOta || Date.now()<authBlockedUntil)return;", UI)
        self.assertIn("updateBusy=true;", UI)
        self.assertIn("const state=await api('/api/status');", UI)
        self.assertIn("const list=await api('/api/devices');", UI)
        self.assertIn("}finally{updateBusy=false;}", UI)
        self.assertNotIn("Promise.all([api('/api/status'),api('/api/devices')])", UI)


if __name__ == "__main__":
    unittest.main()


