# First boot: open Wi-Fi setup and administrator key

When there is **no saved home Wi-Fi network**, the AutoMesh gateway broadcasts
\`AutoMesh-XXXXXX\` with **no Wi-Fi password**. Use a phone or PC nearby to join
this network and open **http://192.168.4.1/**.

1. On the very first installation, enter an administrator password of **at
   least 6 Unicode characters, no more than 64 UTF-8 bytes**. Twelve or more
   characters is recommended. Confirm it.
2. The gateway stores a **salted PBKDF2-HMAC-SHA256 derived hash**, never the
   plaintext password, in its **own NVS**: namespace \`meshweb\`, key
   \`admin_v1\`. It persists through power cycles and application-only updates.
3. Log in with the chosen password, scan available 2.4 GHz Wi-Fi networks, and
   connect the gateway to a router. The setup access point then disappears.
   The gateway hosts the same panel over its new router-assigned LAN address.
4. Existing gateways with the legacy generated 32-hex admin token preserve
   that token. Sign in with it and use **Сменить ключ администратора** to
   replace it; the old token is removed from NVS on successful change.

**Security caution:** An open setup AP is *not private*. Anyone within radio
range can connect, and the first person to set an administrator password can
claim the gateway. The initial web session is HTTP, not HTTPS, so a nearby
attacker may intercept data. Commission the gateway in a trusted environment,
set the password immediately, and connect to a WPA2/WPA3 home router. Do not
expose the gateway's HTTP port to the internet.

The web API for first setup is \`GET /api/bootstrap\` and
\`POST /api/admin/setup\` (JSON: \`{"key":"..."}\`). The setup POST is permitted
only while the gateway has **no saved Wi-Fi** *and* **no existing administrator**.
Subsequent administrative APIs require the \`X-Automesh-Admin\` header.
Authenticated key changes use \`POST /api/admin/change\`.

A key change does **not** erase registered Mesh nodes, device names, or the
saved router configuration. Erasing the NVS partition, on the other hand,
also erases Mesh keys and should not be used as an ordinary password reset.
For a lost key, use a deliberate recovery procedure with a verified Flash
backup rather than a blanket erase.

This change does **not** automatically reset an existing installation to the
unprotected first-boot state. If the gateway already has saved router Wi-Fi,
it continues in STA mode. This behavior requires a device-level test before
deployment.

