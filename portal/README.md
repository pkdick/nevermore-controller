# Wi-Fi configuration portal (web UI)

Served by the Pico 2 W's configuration hotspot at http://192.168.4.1/ (see `src/wifi/portal.cpp`).

Based on the web UI of [esp_wifi_manager](https://github.com/tuanpmt/esp_wifi_manager)
by Tuan PM (MIT, see `LICENSE`), commit `20f77d7`. The REST API it uses (`/api/wifi/...`)
is reimplemented by the firmware. Changes from upstream:

- French text, "Nevermore · Wi-Fi" title.
- Signal bars & quality (%) for every scanned network, strongest first.
- Errors are shown instead of only logged; "connecting" state shown.
- Saved networks are listed in the order they're tried, and refreshed after connecting.
- Only pre-compressed assets are emitted (`deleteOriginFile`), the firmware serves them gzipped.

## Building

The firmware embeds `dist/` (committed), so building the firmware doesn't need Node.
After editing `src/`:

```bash
npm install
npm run build
```

Then rebuild the firmware.
