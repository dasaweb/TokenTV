# CLAUDE.md

TokenTV: Claude plan usage on a GeekMagic SmallTV (ESP8266, 240×240 ST7789), built on top of the GeekMagic
Open Firmware. PlatformIO + Arduino core 3.1.x (espressif8266), C++17.

## Commands

```bash
pio run                                   # build -> .pio/build/esp12e/firmware.bin
pio run -t upload                         # serial flash (first install only)
curl -H "Authorization: Bearer $TOKEN" -F "file=@.pio/build/esp12e/firmware.bin" http://$IP/api/v1/ota/fw
curl -H "Authorization: Bearer $TOKEN" http://$IP/api/v1/claude/status     # state, heap, last_crash
curl -H "Authorization: Bearer $TOKEN" http://$IP/api/v1/claude/diag       # gateway/DNS/TCP/HTTP from device
```

OTA reboots the device 5 s after upload. The first ~45 s after boot can be blocked by the NTP client when there
is no network time.

On WSL2 the device is usually not reachable from Linux (NAT). Use Windows `curl.exe` from `/mnt/c` with a
Windows path for `-F file=@C:\...`.

## Layout

- `src/claude/ClaudeUsage.cpp`: OAuth PKCE login, token refresh, usage polling, DNS and time fallbacks, HTTPS
  helper.
- `src/claude/ClaudeScreen.cpp`: display. It redraws fully on `ClaudeUsage::revision()` changes and partially
  every wall-clock minute. Fonts come from U8g2 and are rendered by Arduino_GFX (transparent, so clear rects
  first).
- `src/claude/CrashLog.cpp`: `custom_crash_callback` writes reason and stack code addresses to RTC memory
  (block 32; RescueMode uses block 0).
- `src/web/ClaudeApi.cpp`: `/claude` page (`include/web/ClaudePage.h`, PROGMEM, Alpine.js) and
  `/api/v1/claude/*`. It also serves `/` from `include/web/IndexPage.h`, a copy of `data/web/index.html`, so
  devices with an old filesystem image get the Claude menu entry. Keep both in sync.
- Screen layouts (`ClaudeScreen::LAYOUT_*`): rings or bars, each with an optional time (pace) marker; stored as
  `claude_layout` / `claude_pace` in `config.json`, set via `POST /api/v1/claude/display`. With the marker on,
  the minute tick redraws the rings too.
- `include/claude/ClaudeCerts.h`: root CAs per host. Regenerate when the chains change
  (`openssl s_client -showcerts`).
- `scripts/patch_gfx_progmem.py`: pre-build patch that moves Arduino_GFX YCbCr tables to flash.
- Upstream code: `src/web/Api.cpp`, `src/config/*`, `src/display/*`, `src/boot/RescueMode.cpp`. Upstream docs
  are in `docs/open-firmware.md`.

## Constraints that bite

- **Heap is the limit.** About 30 KB are free idle, and a TLS request needs about 18 KB: the 6.2 KB BearSSL stack
  thunk, the 4 KB receive buffer, and the client and x509 contexts. `operator new` failure inside
  WiFiClientSecure means `abort()`, which shows up as reset reason "Software/System restart". Check `free_heap`
  and `last_crash` after any change that adds RAM. Keep big constants `PROGMEM`; on the ESP8266 a plain
  `static const` goes to RAM.
- **Do not do TLS inside an HTTP handler.** Queue the work and run it from `loop()` (see `exchangeCode()`).
- **TLS receive buffer 4096:** measured max record 3415 B. If a handshake fails after a CDN change, re-measure
  with `openssl s_client -msg` before raising it.
- **Time:** cert validation needs wall-clock time. NTP runs in UTC and `configTime(0, 0, ...)` resets TZ on every
  sync, so call `setTZ(CLAUDE_TZ)` before `localtime_r`. The HTTP `Date` header is the fallback.
- **SecureStorage** commits the whole 2 KB EEPROM on every `put()`. Avoid writes in hot paths.
- **Watchdog** is enabled with WDTO_2S. Blocking code must `yield()`/`delay()`; network calls do that internally.

## Claude endpoints (unofficial)

- Authorize: `https://claude.com/cai/oauth/authorize` with client `9d1c250a-e61b-44d9-88ed-5944d1962f5e`,
  redirect `https://platform.claude.com/oauth/code/callback`, `code=true`, S256 PKCE.
- Token: `POST https://platform.claude.com/v1/oauth/token`, JSON body (`authorization_code` / `refresh_token`).
- Usage: `GET https://api.anthropic.com/api/oauth/usage`, `anthropic-beta: oauth-2025-04-20`. It returns
  `limits[]` (`session`, `weekly_all`, with percent, severity, resets_at) and legacy `five_hour`/`seven_day`.

## Style

Match the upstream code: trailing return types, `constexpr` named constants (magic numbers are fine inside the
`NOLINTBEGIN` drawing blocks), anonymous namespaces for file-local state, `Logger::warn` for anything that should
show up in `/api/v1/logs` (only WARN and ERROR are kept). UI texts on the device and the /claude page are
German.
