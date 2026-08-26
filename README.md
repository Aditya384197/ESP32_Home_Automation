# ESP32 Smart Home

Internet Edition offline-first home automation controller with local scheduling and optional cloud remote access.

## At a glance

- **Offline-first:** the AP, local web control and physical switches continue working when the home Wi-Fi/Internet is unavailable.
- **STA + AP:** the ESP32 stays available locally while automatically reconnecting to the configured home Wi-Fi.
- **Manual offline mode:** a single tap in the app pauses Wi-Fi/cloud reconnect attempts entirely; tapping again resumes them.
- **Secure remote control:** the cloud API uses HTTPS, device bearer tokens and user authentication; relay commands are queued and acknowledged by the ESP32.
- **5 relays / 5 switches:** Relay 1-3 remain fixed; Relay 4-5 are optional and can be configured from the local settings page.
- **Schedules:** up to 64 schedules per device. Each relay has independent weekly entries; either action can optionally include an automatic reversal after a set duration (0-1439 minutes). Schedules are downloaded to the ESP32 and executed locally, so an Internet outage does not stop scheduled operation. Days use a 7-bit mask (Sun=bit 0 ... Sat=bit 6; `127` = every day).
- **Local scheduler:** cached schedules execute independently of cloud availability, with NTP time synchronization whenever home Wi-Fi is available.

## Firmware flash

This firmware has no over-the-air update capability. Every update is done with a direct cable flash of all three files at these addresses:

| File | Address |
|---|---:|
| `bootloader.bin` | `0x1000` |
| `partition-table.bin` | `0x8000` |
| `smart_home.bin` | `0x10000` |

All three files must be written together on every flash. There is no OTA endpoint, no firmware-upload page and no remote update path anywhere in this project.

## Hardware

Default relay GPIOs:

`16, 17, 18, 19, 21` -> Relay 1-5

Default physical switch GPIOs:

`32, 33, 25, 26, 27` -> Switch 1-5

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
7. On the ESP32 local Settings -> Internet Connection, enter the home Wi-Fi SSID/password (applies immediately, no restart).
8. On the ESP32 local Settings -> Remote Access, enter the Cloud API URL, Device ID and Device token (applies immediately, no restart).

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

## Cost note

The reference backend is designed to start on Cloudflare's low/zero-cost tiers for a small personal deployment. Actual limits and pricing can change, so check the provider's current plan before production use.

## Behavior notes

- Wi-Fi SSID/password and Cloud credentials are two independent settings pages; either can be saved on its own and both apply immediately without a restart.
- Once Wi-Fi connects, the local page is also reachable at the ESP32's home-network IP address (shown under Settings -> Internet Connection), in addition to the AP.
- Hidden SSIDs are supported because the station connects by the manually supplied SSID/password and does not pin a channel or BSSID.
- Wi-Fi reconnects automatically after loss; the station scans again rather than assuming the previous router channel.
- Manual offline mode overrides automatic reconnection until the user re-enables it from the app.
- The local scheduler runs in its own task and is independent of HTTPS/cloud polling.
- Up to 64 weekly schedule events are cached on the ESP32. Each event has relay, time, ON/OFF action, weekday mask, optional auto-reversal duration and enabled state.
- Schedule execution continues during temporary Wi-Fi/Internet/cloud outages, and is fully paused only if the device is manually placed in offline mode or the clock has not yet synced via NTP.
- NTP sync starts automatically the moment Wi-Fi connects, whether that happens at boot or is configured later while the device is already running.
- Cloud schedules are refreshed from the backend when the device reconnects.

> **Schedule clock:** the firmware uses India Standard Time (IST, UTC+05:30) and synchronizes with NTP as soon as Wi-Fi is connected.

## Important behavior

**Local control and schedules do not depend on Cloudflare.** Cloudflare is an optional remote-access layer. If the Internet disappears, local AP/web control, physical switches and already-cached schedules continue operating. When Wi-Fi/Internet returns, the device reconnects and resumes authenticated cloud polling automatically, unless the user has manually forced offline mode.

For a first setup, entering only the home Wi-Fi SSID and password is sufficient. Cloud URL/Device ID/Device Token are only required when remote access is desired.

## Local scheduler behavior

Schedules are stored in ESP32 NVS and execute from the device clock. Cloud polling is not in
the scheduler execution path. A temporary Wi-Fi/Internet outage therefore does not stop already
cached schedules. When Wi-Fi returns and NTP is available, the device clock is corrected and the
next scheduled events continue from the configured weekly rules. After a reboot, a valid time
source (NTP) is required before a time-based schedule can execute accurately.
