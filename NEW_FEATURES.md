

## Embedded gateway web UI (v0.2.0-dev)

The gateway firmware now contains its own Wi-Fi SoftAP and HTTP management panel.
No PC or Python web server is required for accessing this page once flashed.

- A unique Wi-Fi name of the form `AutoMesh-XXXXXX` is derived from the gateway MAC.
- The individual 16-character Wi-Fi passphrase and 32-character admin token
  are generated on the device, saved in NVS and shown on its **physical USB console**.
- Connect your phone/PC to that Wi-Fi, then open `http://192.168.4.1/`.
- Enter the **admin token**, not the Wi-Fi passphrase, to view registered devices.
- The panel supports discovery, approval with a per-device static OOB secret,
  denial and a Mesh ping operation. **There is no automatic acceptance.**
- Flashing firmware from the web panel remains disabled until tested BLE Mesh
  DFU and OTA integrity checks are implemented.
- The current panel is for a local WPA2 AP test network; do not expose HTTP
  endpoints directly to the Internet. Use an authenticated HTTPS/VPN bridge later.

## Status LED

For the tested ESP32-C3 SuperMini board variant, the onboard indicator is
GPIO8, **active-low**. Do not wire external hardware that drives this strapping
pin low during boot.

- Solid on: the node is already provisioned and has received a Mesh health
  request from the gateway within the last 35 seconds.
- Blink (~1 Hz): never provisioned, gateway unreachable or health timeout.
- Gateway LED: solid only after a registered device answers; otherwise blinks.
- Gateway sends authenticated Generic OnOff Get as the health check, not as a
  command to change the status LED.
- On C3 the demonstration OnOff model no longer controls the onboard status
  LED. Its state remains independent of the network indicator.

**Other ESP32 board models often use other GPIOs, active polarity or a
WS2812 RGB LED. The LED driver must be configured per board and is not
automatically portable across every ESP32 family.**

## Hardware test status

- COM13 (ESP32-C3 Super Mini) successfully flashed with onboard LED version,
  Mesh stack and OOB reported initialized through serial diagnostics.
- Gateway image with embedded web panel is built separately; it must be tested
  on the COM12 board when that device is connected.
- Neither mesh registration of the two physical units nor gateway online-status
  acknowledgment is yet confirmed end-to-end.

## Gateway Wi-Fi commissioning: v0.2.1-dev

**Reason for change:** Espressif ESP-BLE-MESH FAQ documents Wi-Fi station coexistence as the supported option. Simultaneous SoftAP + continuous BLE Mesh provisioning may not be reliable. Previous v0.2.0-dev started both; the radio did not appear in a phone Wi-Fi list despite logs saying SoftAP started.

The gateway now has two modes:

1. **First boot / no saved Wi-Fi credentials:** SoftAP ONLY, with Mesh disabled. WPA2 SSID AutoMesh-XXXXXX, password and admin key persist in the gateway NVS. Open http://192.168.4.1/ from a device connected to this SoftAP. Enter admin key, then enter SSID and WPA2 password of a **2.4 GHz router network** in the local setup form. No router password needs to be sent through ChatGPT.
2. **After Wi-Fi is configured:** ESP32 restarts and connects to that Wi-Fi as STA with Bluetooth Mesh enabled. Find the LAN address on the physical USB serial console (line "AutoMesh web panel ready: http://.../"). The management webpage is hosted on the gateway itself, not on the PC.

Credentials are stored locally in NVS; do not include them in GitHub repositories, logs, or screenshots. The management HTTP is intended only for an isolated trusted LAN in development; do not publish to the Internet without HTTPS and proper authentication.

To re-enter setup, a dedicated protected config-reset function is still needed. Until it is implemented, avoid erasing NVS: it also stores device keys and Mesh configuration. Do not change flash partitions or erase the entire chip just to reset Wi-Fi.

**Hardware check (Nov 14, 2026):** COM12 re-flashed application only with v0.2.1-dev, verified flash hash, boot log confirmed SoftAP, DHCP and embedded web service, and explicitly reported "SETUP ONLY: Mesh disabled". No boot errors were observed. Wi-Fi SSID visibility from another radio still requires an on-site check. COM13 node firmware was left unchanged.

## Wi-Fi scanning and switching — v0.2.3-dev

The embedded gateway web panel supports **Wi-Fi scanning in both setup and normal operation**.

- In setup, the gateway uses **Wi-Fi AP+STA with Bluetooth Mesh suspended**.
- In normal operation, the gateway uses **Wi-Fi STA + Bluetooth Mesh**. Scan requests can briefly increase Wi-Fi latency or Mesh radio contention.
- After logging into the embedded web UI, open **Настройки Wi-Fi шлюза** and click **Сканировать Wi-Fi**.
- Results display SSID, signal level (RSSI dBm and approximate percent), channel, and whether the network requires a password.
- Click a discovered network to fill its SSID; enter the password and submit. Open Wi-Fi networks can be selected without a password (not recommended for security).
- Only explicit user action initiates a scan. The ESP32 task scans asynchronously and releases Wi-Fi scan records afterward.
- Available API endpoints require the per-device admin key: POST /api/wifi/scan, GET /api/wifi/networks, POST /api/wifi.
- Wi-Fi changes are **staged** in NVS, then the gateway reboots and attempts to connect. On a successful DHCP lease the new settings become permanent. If connection fails for 45 seconds, the gateway restores the previous network settings (or returns to its setup AP if none existed). This does not erase Mesh keys.
- Never put Wi-Fi passwords, OOB keys or administrator tokens in repository documentation.

**Hardware test:** Built with ESP-IDF v5.5.3, installed by app-only flash on COM12, hash verified, STA Wi-Fi and BLE Mesh initialized, gateway announced a LAN URL from DHCP. Actual on-air scan results must still be confirmed by clicking Scan from a logged-in browser; the protected endpoint was not automatically exercised.

## Node identification / Locate (v0.3.0-dev)

- Each registered node has its own "Найти" button in the built-in gateway page.
- Button sends an authenticated, unicast BLE Mesh vendor-model message to that node's Mesh address. The selected node acknowledges the request and rapidly blinks the built-in LED for 15 seconds (approximately 4 Hz); other nodes are not changed.
- At the end of the 15-second locate interval, the node resumes the regular status LED policy: solid if gateway reachable, slow blinking if offline.
- The node's vendor server must be bound to the AppKey, in addition to the standard Generic OnOff Server.
- The gateway automatically sends that bind step when onboarding a newly approved node.
- The Bluetooth SIG vendor company ID currently uses the Espressif example CID as a prototype/testing only; a released product must use its own assigned Company Identifier.
- If an older node was registered before this new vendor model was added, it may need reconfiguration/binding. Do not erase NVS to fix it without backup.
- POST /api/locate means "send queued", not physical LED blink verified. Look for ACK in gateway logs and observe the LED.

## Windows USB installer

- Standalone AutoMesh-Installer.exe plus Install-AutoMesh.cmd and Check-AutoMesh.cmd in a ZIP archive under the Windows Downloads folder.
- Detects connected USB COM ports, reads SoC type, MAC and flash size using esptool, selects only compatible ESP32-C3 SuperMini 4MB images.
- A prior AutoMesh gateway/node role can be detected from a full flash backup; for new boards an explicit role selection is required (node default), because chip type does not reveal intended role.
- Backups are stored in Windows LocalAppData ESP32-AutoMesh Backups; originals and credentials MUST NOT be uploaded to public repositories.
- Before write: user confirmation of physical board profile, read-back of full flash, file SHA-256 verification, final write confirmation. Optional NVS erasure is separately confirmed and off by default.
- Test: Standalone EXE correctly detected both COM12 and COM13; SHA-256 passed for both image roles. EXE was also exercised through a complete 4MB backup of COM13 and deliberately cancelled before writing. The complete first-install write path was not exercised on a fresh third board.


