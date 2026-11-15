#!/usr/bin/env python3
"""Stage verified ESP32-C3 and ESP32-S3 firmware for the Windows installer.

Input: GitHub Actions download-artifact output; a distinct directory per
  automesh-{gateway,node}-{esp32c3,esp32s3}.
Output: manifest.json with SHA256, per-target firmware files, no secrets.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

CHIPS = [
    ("esp32c3", "ESP32-C3", "ESP32-C3 SuperMini 4MB; GPIO8 active-low status LED", "GPIO8 (active-low)"),
    ("esp32s3", "ESP32-S3", "Waveshare ESP32-S3-Zero 4MB; WS2812 on GPIO21", "WS2812 RGB on GPIO21 (Waveshare S3-Zero)"),
]
ROLES = (("gateway", "provisioner.bin"), ("node", "onoff_server.bin"))
IMAGES = {
    "bootloader.bin": Path("bootloader/bootloader.bin"),
    "partitions.bin": Path("partition_table/partition-table.bin"),
    "otadata.bin": Path("ota_data_initial.bin"),
}
MAX_APP_BYTES = 0x1C0000


def stage(artifacts: Path, target: Path) -> dict:
    manifest = {"schema": 2, "version": "0.3.26-dev", "profiles": {}}
    for chip, full_chip, description, led_profile in CHIPS:
        key = f"{chip}-zero-4mb" if chip == "esp32s3" else f"{chip}-supermini-4mb"
        profile = {
            "chip": full_chip,
            "esptool_chip": chip,
            "flash_size_bytes": 4 * 1024 * 1024,
            "flash_mode": "dio",
            "flash_freq": "80m",
            "board_profile": description,
            "led_profile": led_profile,
            "roles": {},
        }
        for role, application in ROLES:
            source = artifacts / f"automesh-{role}-{chip}"
            required = dict(IMAGES)
            required["application.bin"] = Path(application)
            files = {}
            for dst_name, src_name in required.items():
                src = source / src_name
                if not src.is_file():
                    raise FileNotFoundError(f"Missing required {full_chip}/{role}: {src}")
                dst = target / "images" / key / role / dst_name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
                raw = dst.read_bytes()
                if not raw or (dst_name == "application.bin" and len(raw) > MAX_APP_BYTES):
                    raise ValueError(f"Invalid image length: {dst} ({len(raw)})")
                files[dst_name] = {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            profile["roles"][role] = {"files": files}
            print(f"OK {full_chip}/{role}: {files['application.bin']['size']} application bytes")
        manifest["profiles"][key] = profile
    out = target / "manifest.json"
    out.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    ns = parser.parse_args()
    stage(ns.artifacts, ns.out)



