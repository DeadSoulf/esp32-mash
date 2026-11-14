#!/usr/bin/env python3
# coding: utf-8
"""ESP32 AutoMesh initial installer for Windows.
Detects ESP32 chip/flash via esptool, chooses only exact-compatible binaries.
Backs up ALL flash before first installation. Does not erase NVS without opt-in.
Standalone EXE build bundles Python/esptool/pyserial and images.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import esptool
from serial.tools import list_ports

RELEASE_DIR = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
USER_ROOT = Path.home() / "AppData" / "Local" / "ESP32-AutoMesh"
SUPPORTED_USB_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}
HEX_ROLE = {"gateway": "Шлюз", "node": "Узел"}
FLASH_MAP = {
    "bootloader.bin": "0x0000",
    "partitions.bin": "0x8000",
    "otadata.bin": "0xf000",
    "application.bin": "0x20000",
}


def line(message=""):
    print(message, flush=True)


def sdk_command(arguments: list[str], capture=False) -> str:
    """Call esptool inside both Python source and frozen Windows EXE."""
    stream = io.StringIO()
    try:
        if capture:
            with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
                esptool.main(arguments)
            return stream.getvalue()
        esptool.main(arguments)
        return ""
    except (SystemExit, Exception) as exc:
        tail = stream.getvalue()[-450:]
        raise RuntimeError(f"esptool: {type(exc).__name__}: {exc} {tail}") from exc


def discover_ports() -> list[str]:
    ports = list_ports.comports()
    matches = [
        p.device for p in ports
        if p.vid in SUPPORTED_USB_VIDS
        or (p.vid is None and "USB" in (p.description or "").upper())
    ]
    return sorted(set(matches), key=lambda x: (int(re.search(r"\d+$", x).group())
                                           if re.search(r"\d+$", x) else 9999))


def identify(port: str) -> dict | None:
    try:
        text = sdk_command(["--port", port, "--baud", "115200", "chip-id"], capture=True)
        name = re.search(r"Chip type:\s*(ESP32[-\w]*)", text, re.IGNORECASE)
        mac = re.search(r"MAC:\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})", text, re.IGNORECASE)
        if not name or not mac:
            return None
        info = sdk_command(["--port", port, "--baud", "115200", "flash-id"], capture=True)
        flash = re.search(r"(?:Detected flash size|Flash size):\s*(\d+)\s*(MB|KB)", info, re.IGNORECASE)
        if not flash:
            return None
        size = int(flash.group(1)) * (1024 * 1024 if flash.group(2).upper() == "MB" else 1024)
        return {"port": port, "chip": name.group(1).upper(),
                "mac": mac.group(1).lower(), "flash": size}
    except Exception as exc:
        line(f"  {port}: нет ответа / порт занят ({type(exc).__name__}: {str(exc)[:500]})")
        return None


def read_manifest() -> dict:
    path = RELEASE_DIR / "manifest.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != 2 or not isinstance(data.get("profiles"), dict):
        raise RuntimeError("Неизвестный формат manifest.json")
    return data


def matching_profile(manifest: dict, device: dict) -> tuple[str, dict] | None:
    matches = [
        (name, profile) for name, profile in manifest["profiles"].items()
        if profile.get("chip", "").upper() == device["chip"].upper()
        and profile.get("flash_size_bytes") == device["flash"]
    ]
    if len(matches) == 1:
        return matches[0]
    return None


def role_assets(profile_name: str, profile: dict, role: str) -> dict[str, Path]:
    # Profile names come exclusively from our bundled manifest. Do not permit
    # absolute paths or traversal in a damaged or edited manifest.
    if not re.fullmatch(r"[a-zA-Z0-9_-]{3,64}", profile_name):
        raise ValueError("Некорректное имя профиля в manifest.json")
    filemap = profile["roles"][role]["files"]
    relative = Path("images") / profile_name / role
    resolved = {}
    for filename in FLASH_MAP:
        meta = filemap[filename]
        p = RELEASE_DIR / relative / filename
        if not p.is_file():
            raise FileNotFoundError(f"Нет образа {p}")
        raw = p.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if len(raw) != meta["size"] or digest.lower() != meta["sha256"].lower():
            raise ValueError(f"Контрольная сумма файла {filename} не совпала. Запись запрещена.")
        if filename == "application.bin":
            info = sdk_command(["image-info", str(p)], capture=True)
            expected_chip = profile["chip"]
            if f"Detected image type: {expected_chip}" not in info:
                raise ValueError(
                    f"Неподходящая архитектура прошивки: ожидался {expected_chip}"
                )
            expected_mb = profile["flash_size_bytes"] // 1048576
            if not re.search(rf"Flash size:\s*{expected_mb}MB", info):
                raise ValueError(
                    f"Неподходящий Flash-размер в прошивке: ожидалось {expected_mb}MB"
                )
        resolved[filename] = p
    return resolved


def existing_role(backup: Path) -> str | None:
    # Distinguish previous AutoMesh application from binary strings.
    content = backup.read_bytes()
    if b"AUTOMESH_WEB" in content and b"Wi-Fi STA" in content:
        return "gateway"
    if b"AUTOMESH_NODE_OOB" in content:
        return "node"
    return None


def backup_flash(device: dict) -> Path:
    folder = USER_ROOT / "Backups"
    folder.mkdir(parents=True, exist_ok=True)
    safe_mac = device["mac"].replace(":", "-")
    name = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = folder / f"esp32-{safe_mac}-{name}-{device['flash'] // 1048576}MB.bin"
    line(f"\nРезервная копия всех {device['flash'] // 1048576} МБ Flash:")
    line(f"  {backup}")
    sdk_command(["--port", device["port"], "--baud", "460800", "read-flash",
                 "0x0", hex(device["flash"]), str(backup)])
    if not backup.is_file() or backup.stat().st_size != device["flash"]:
        raise RuntimeError("Резервное копирование не завершено. Запись запрещена.")
    digest = hashlib.sha256(backup.read_bytes()).hexdigest()
    line(f"Резервная копия проверена. SHA-256: {digest}")
    return backup


def prompt_device(devices: list[dict], chosen_port: str | None) -> dict:
    if not devices:
        raise RuntimeError("Подходящие ESP32 на COM-портах не найдены.")
    if chosen_port:
        selected = next((x for x in devices if x["port"].upper() == chosen_port.upper()), None)
        if not selected:
            raise RuntimeError(f"Порт {chosen_port} не определён как ESP32.")
        return selected
    if len(devices) == 1:
        return devices[0]
    while True:
        try:
            index = int(input(f"Номер устройства для прошивки (1–{len(devices)}): ").strip())
            if 1 <= index <= len(devices):
                return devices[index-1]
        except (ValueError, EOFError):
            pass
        line("Нужно ввести номер устройства из списка.")


def choose_role(previous: str | None, requested: str | None) -> str:
    if requested:
        if previous and requested != previous:
            line("ВНИМАНИЕ: указана другая роль, чем у найденной прошивки AutoMesh.")
        return requested
    if previous:
        line(f"Роль определена по ранее установленной прошивке: {HEX_ROLE[previous]}")
        return previous
    line("\nНовый контроллер: роль по модели микросхемы определить невозможно.")
    line("По умолчанию будет установлен УЗЕЛ. Для создания нового шлюза введите G.")
    v = input("Роль: N (узел, по умолчанию) / G (шлюз): ").strip().upper()
    return "gateway" if v == "G" else "node"


def flash_device(device: dict, profile: dict, files: dict[str, Path], reset_nvs=False) -> None:
    port = device["port"]
    if reset_nvs:
        line("ОЧИСТКА NVS 0x9000..0xEFFF (после резервного копирования)...")
        sdk_command(["--port", port, "--baud", "460800", "erase-region", "0x9000", "0x6000"])
    # Chip type and flash config are verified against manifest before writing.
    flash_mb=profile["flash_size_bytes"]//1048576
    params = ["--port", port, "--baud", "460800",
              "--chip", profile["esptool_chip"],
              "write-flash", "--flash-mode", profile["flash_mode"],
              "--flash-freq", profile["flash_freq"],
              "--flash-size", str(flash_mb)+"MB"]
    for name, offset in FLASH_MAP.items():
        params += [offset, str(files[name])]
    line("\nЗапись bootloader + partition table + OTA metadata + app...")
    sdk_command(params)
    line("\nГотово: esptool завершил запись и проверку контрольных сумм.")


def main() -> int:
    parser = argparse.ArgumentParser(description="AutoMesh: первичная установка прошивки ESP32")
    parser.add_argument("--port", help="например COM13")
    parser.add_argument("--role", choices=["gateway", "node"], help="принудительный выбор роли")
    parser.add_argument("--check", action="store_true", help="только проверка оборудования и образов, без записи")
    parser.add_argument("--self-test", action="store_true", help="проверить все встроенные образы без COM-портов")
    args = parser.parse_args()

    line("ESP32 AutoMesh — установщик для Windows (v0.3.2-dev)")
    if args.self_test:
        manifest = read_manifest()
        count = 0
        for name, profile in manifest["profiles"].items():
            for role in ("gateway", "node"):
                if role not in profile.get("roles", {}):
                    continue
                files = role_assets(name, profile, role)
                count += 1
                line(f"  {profile['chip']} / {role}: {len(files)} образа, SHA-256 и тип чипа проверены")
        if count < 4:
            raise RuntimeError("Пакет неполный: отсутствуют образы шлюза/узла для C3 или S3")
        line(f"Самопроверка выполнена: {count} совместимых комплектов")
        return 0
    line("Поиск USB-устройств ESP32...")
    found = []
    for port in discover_ports() if args.port is None else [args.port]:
        d = identify(port)
        if d:
            found.append(d)
            line(f"  [{len(found)}] {d['port']}: {d['chip']}, Flash {d['flash']//1048576} МБ, MAC {d['mac']}")
    if not found:
        line("Не найдены доступные ESP32. Проверьте USB-кабель и драйвер.")
        return 2
    manifest = read_manifest()
    selected = prompt_device(found, args.port)
    match = matching_profile(manifest, selected)
    if match is None:
        line("\nНЕТ совместимого бинарного образа для этого чипа и размера Flash.")
        line(f"Чип: {selected['chip']}; Flash: {selected['flash']//1048576} МБ.")
        line("Установщик не будет записывать прошивку другого семейства ESP32.")
        return 3
    profile_name,profile=match
    line(f"\nСовместимый профиль: {profile['board_profile']}")
    line(f"Светодиод: {profile['led_profile']}")
    line("Чип и размер Flash определены автоматически.")
    line("Один только ID чипа не позволяет определить GPIO встроенного светодиода.")
    if args.check:
        for role in ("gateway", "node"):
            role_assets(profile_name, profile, role)
            line(f"  {HEX_ROLE[role]}: файлы и SHA-256 проверены.")
        line("Режим проверки: никакие изменения в Flash не выполнены.")
        return 0

    answer = input("Подтвердите профиль платы (ДА/YES): ").strip().upper()
    if answer not in ("ДА", "YES", "Y"):
        line("Операция отменена: не подтверждён профиль платы.")
        return 4
    # Read before write, even if user previously flashed other firmware.
    backup = backup_flash(selected)
    role = choose_role(existing_role(backup), args.role)
    files = role_assets(profile_name, profile, role)
    line(f"\nПлата: {selected['chip']} на {selected['port']}")
    line(f"Роль: {HEX_ROLE[role]}")
    line(f"Исходная Flash сохранена: {backup}")
    line("Начальная установка затронет загрузчик, таблицу разделов, OTA и приложение.")
    line("NVS с прежними сетевыми и Mesh-ключами пока НЕ очищается.")
    reset_nvs = input("Очистить NVS после резервного копирования? (нет/да): ").strip().lower() == "да"
    line("Команда перезапишет содержимое платы. Это нельзя отменить без восстановления backup.")
    if input("Для подтверждения введите ПРОШИТЬ (или FLASH): ").strip().upper() not in ("ПРОШИТЬ", "FLASH"):
        line("Отменено, резервная копия осталась на диске.")
        return 5
    flash_device(selected, profile, files, reset_nvs=reset_nvs)
    line("\nПосле загрузки: шлюз создаёт Wi-Fi для настройки или подключается к роутеру.")
    line("Узел появляется в Mesh только после разрешения в веб-панели и ввода OOB.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nОперация прервана.", flush=True)
        sys.exit(130)
    except Exception as exc:
        print(f"\nОШИБКА: {exc}", flush=True)
        print("Если была начата запись, используйте резервную копию для восстановления.", flush=True)
        sys.exit(1)

