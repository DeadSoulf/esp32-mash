"""ESP32 AutoMesh local-only web gateway. Python 3.10+; pyserial required.

Security: loopback bind only, per-process CSRF token, explicit OOB approval.
This is an experimental gateway, not a production deployment.
"""
from __future__ import annotations

import hmac
import json
import os
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit("Missing pyserial: py -m pip install pyserial") from exc

COM_PORT = os.environ.get("AUTOMESH_PORT", "COM12")
HTTP_PORT = int(os.environ.get("AUTOMESH_WEB_PORT", "8765"))
BIND_HOST = "127.0.0.1"
CSRF_TOKEN = secrets.token_urlsafe(32)
UUID = re.compile(r"^[0-9a-fA-F]{32}$")
OOB = re.compile(r"^[0-9a-fA-F]{32}$")


class Gateway:
    def __init__(self):
        self.lock = threading.RLock()
        self.serial_port = None
        self.status = "disconnected"
        self.last_error = ""
        self.devices = {}

    def event(self, obj):
        event = obj.get("event", "")
        uuid = obj.get("uuid", "")
        with self.lock:
            if event == "candidate" and UUID.fullmatch(uuid):
                old = self.devices.get(uuid, {})
                self.devices[uuid] = {
                    "uuid": uuid,
                    "address": obj.get("addr", old.get("address", "")),
                    "state": old.get("state", "awaiting_confirmation"),
                    "last_seen": int(time.time()),
                }
            elif event == "approval_started" and uuid in self.devices:
                self.devices[uuid]["state"] = "provisioning"
            elif event == "denied" and uuid in self.devices:
                self.devices[uuid]["state"] = "denied"
            elif event == "provisioned" and uuid in self.devices:
                self.devices[uuid]["state"] = "provisioned"
                self.devices[uuid]["mesh_address"] = obj.get("address")
            elif event == "error" and uuid in self.devices:
                self.devices[uuid]["state"] = "error"
                self.devices[uuid]["error"] = obj.get("reason", "unknown")

    def send(self, command):
        with self.lock:
            if not self.serial_port or not self.serial_port.is_open:
                raise ConnectionError("COM-port gateway is not connected")
            self.serial_port.write((command + "\n").encode("ascii"))
            self.serial_port.flush()

    def snapshot(self):
        with self.lock:
            return {
                "connection": self.status,
                "port": COM_PORT,
                "error": self.last_error,
                "devices": sorted(self.devices.values(), key=lambda d: d["uuid"]),
                "phase": "Provisioning prototype; Mesh OTA not implemented yet",
            }


gateway = Gateway()


def serial_reader():
    while True:
        try:
            with serial.Serial(COM_PORT, 115200, timeout=1, write_timeout=2) as port:
                with gateway.lock:
                    gateway.serial_port = port
                    gateway.status = "connected"
                    gateway.last_error = ""
                time.sleep(1)
                gateway.send("HELLO")
                gateway.send("LIST")
                while True:
                    data = port.readline()
                    if not data:
                        continue
                    text = data.decode("utf-8", errors="replace").strip()
                    if "AUTOMESH {" not in text:
                        continue
                    raw = text.split("AUTOMESH ", 1)[1]
                    try:
                        obj = json.loads(raw)
                    except (ValueError, TypeError):
                        continue
                    if isinstance(obj, dict):
                        gateway.event(obj)
        except (serial.SerialException, OSError, ConnectionError) as exc:
            with gateway.lock:
                gateway.status = "disconnected"
                gateway.last_error = str(exc)
        finally:
            with gateway.lock:
                gateway.serial_port = None
                gateway.status = "disconnected"
        time.sleep(3)


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, data):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/status":
            self.reply(200, gateway.snapshot())
        elif self.path == "/":
            template = Path(__file__).with_name("index.html").read_text(encoding="utf-8")
            body = template.replace("__CSRF_TOKEN__", CSRF_TOKEN).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Frame-Options", "DENY")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.reply(404, {"error": "not_found"})

    def do_POST(self):
        if self.path not in ("/api/approve", "/api/deny"):
            return self.reply(404, {"error": "not_found"})
        if not hmac.compare_digest(self.headers.get("X-CSRF-Token", ""), CSRF_TOKEN):
            return self.reply(403, {"error": "csrf_denied"})
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 512:
                raise ValueError("invalid request size")
            obj = json.loads(self.rfile.read(size))
            uuid = str(obj.get("uuid", "")).lower()
            if not UUID.fullmatch(uuid):
                raise ValueError("bad UUID")
            snapshot = gateway.snapshot()
            matches = [d for d in snapshot["devices"] if d["uuid"].lower() == uuid]
            if not matches or matches[0]["state"] not in ("awaiting_confirmation", "error"):
                raise ValueError("device is not awaiting confirmation")
            if self.path == "/api/approve":
                oob = str(obj.get("oob", "")).lower()
                if not OOB.fullmatch(oob):
                    raise ValueError("16-byte OOB key must be exactly 32 hex characters")
                gateway.send("APPROVE " + uuid + " " + oob)
            else:
                gateway.send("DENY " + uuid)
            self.reply(202, {"queued": True})
        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            self.reply(400, {"error": str(exc)})
        except (ConnectionError, serial.SerialException, OSError) as exc:
            self.reply(503, {"error": str(exc)})

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    threading.Thread(target=serial_reader, daemon=True).start()
    server = ThreadingHTTPServer((BIND_HOST, HTTP_PORT), Handler)
    print(f"ESP32 AutoMesh web panel: http://{BIND_HOST}:{HTTP_PORT}/")
    print("Localhost-only. Remote access must be via authenticated HTTPS/VPN reverse proxy.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

