# ESP32 Smart Home V2

Internet Edition built on the stable V1 offline controller.

### V2 at a glance

- **Offline-first:** the AP, local web control and physical switches continue working when the home Wi-Fi/Internet is unavailable.
- **STA + AP:** the ESP32 can stay available locally while automatically reconnecting to the configured home Wi-Fi.
- **Secure remote control:** the cloud API uses HTTPS, device bearer tokens and user authentication; relay commands are queued and acknowledged by the ESP32.
- **5 relays / 5 switches:** Relay 1–3 remain fixed; Relay 4–5 are optional and can be configured from the local settings page.
- **Schedules:** up to 20 schedules per device. Schedules are downloaded to the ESP32 and executed locally, so an Internet outage does not stop scheduled operation. Days use a 7-bit mask (Sun=bit 0 … Sat=bit 6; `127` = every day).
- **Remote OTA:** an authenticated admin can queue an HTTPS firmware URL. The ESP32 fetches it over TLS and switches to the new OTA partition only after a successful write/end check.
- **Local OTA:** the existing password-protected local OTA remains available.
- **No history/logging in V2:** intentionally deferred to V2.1.

## Initial firmware flash

| File | Address |
|---|---:|
| `bootloader.bin` | `0x1000` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xF000` |
| `offline_smart_home.bin` | `0x20000` |

For normal local OTA, upload only the application `.bin` through the device OTA page.

## Hardware

Default relay GPIOs:

`16, 17, 18, 19, 21` → Relay 1–5

Default physical switch GPIOs:

`32, 33, 25, 26, 27` → Switch 1–5

Each physical switch is wired between its GPIO and **GND**. The firmware uses the ESP32 internal pull-up, debounces the input, and changes the corresponding relay only after a stable transition.

> **Mains safety:** relay contacts and 230 V AC wiring must be installed/enclosed by a qualified person. The ESP32 switch inputs are low-voltage GPIOs and must never be connected directly to mains voltage.

## Internet Edition setup

The Internet layer is supplied in `server/` and uses **Cloudflare Workers + D1** as the reference backend.

1. Create a D1 database and put its ID in `server/wrangler.toml`.
2. Apply `server/schema.sql` to the database.
3. Set the Worker secrets `ADMIN_EMAIL` and `ADMIN_PASSWORD`.
4. Deploy the Worker and note its HTTPS URL.
5. Sign in to the dashboard as the administrator.
6. Create a device. The dashboard returns a **device token once**; keep it private.
7. On the ESP32 local Settings → Internet Connection, enter:
   - Home Wi-Fi SSID/password
   - Cloud API URL
   - Device ID
   - Device token
8. Save and allow the ESP32 to restart and reconnect.

The ESP32 never needs inbound Internet port-forwarding. It makes an outbound HTTPS connection to the backend.

### Cloudflare commands

From `server/`:

```text
npm install -D wrangler
npx wrangler d1 create smart_home
npx wrangler d1 execute smart_home --remote --file=schema.sql
npx wrangler secret put ADMIN_EMAIL
npx wrangler secret put ADMIN_PASSWORD
npx wrangler deploy
```

Replace `REPLACE_WITH_D1_DATABASE_ID` in `wrangler.toml` with the database ID returned by Wrangler.

## Remote OTA

Remote OTA is intentionally restricted to an **HTTPS firmware URL** and an authenticated administrator. The device receives the OTA command only through its authenticated device channel. For production deployments, enable the platform's strongest firmware security options (Secure Boot/Flash Encryption) in addition to TLS and application authentication.

## Cost note

The reference backend is designed to start on Cloudflare's low/zero-cost tiers for a small personal deployment. Actual limits and pricing can change, so check the provider's current plan before production use.

## Version scope

**V2:** remote control, STA/AP operation, automatic reconnect, authentication, multiple users/permissions, device status, local schedules, remote OTA.

**V2.1:** notifications and any history/logging features remain intentionally deferred.

> **Schedule clock:** V2 currently initializes the device schedule clock to India Standard Time (IST, UTC+05:30) and synchronizes it with NTP when STA is connected. A future release can make timezone selection configurable.
