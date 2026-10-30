# TokenTV

**Your Claude plan usage, live on a GeekMagic SmallTV. Fully standalone: no PC, no server, no bridge.**

TokenTV turns a ~$20 GeekMagic SmallTV Ultra (or HelloCubic Lite) into a desk gauge for your Claude Pro/Max
limits. The ESP8266 inside logs in to your Claude account itself, refreshes its own tokens and polls the usage
endpoint over TLS. It shows the 5-hour session window and the weekly limit, with reset times, either as two
rings or as bars with a pace marker.

It is built on top of the [GeekMagic Open Firmware](https://github.com/Times-Z/GeekMagic-Open-Firmware) by
Times-Z, so you keep its web UI, OTA updates, GIF player and rescue mode.

```
 ┌──────────────────────────────┐
 │ ● Claude               22:47 │
 │ ──────────────────────────── │
 │    ╭──────╮      ╭──────╮    │
 │   │  31%   │    │  23%   │   │
 │    ╰──────╯      ╰──────╯    │
 │   5 Stunden        Woche     │
 │   in 2h 14m      Di 09:59    │
 │                              │
 │   aktualisiert vor 2 min     │
 └──────────────────────────────┘
```

The bar layout adds a pace marker (how much of the window has passed), the reserve against an even pace and a
projection whether the limit lasts until the reset:

```
 ┌─────────────────────────────────┐
 │ ● Claude                  22:47 │
 │ ─────────────────────────────── │
 │ 5 Stunden                   31% │
 │ █████████░░░░░░░░░│░░░░░░░░░░░░ │
 │ 69% frei        Reset in 2h 14m │
 │ ● 24% Reserve  reicht bis Reset │
 │ Woche                       23% │
 │ ███████░░│░░░░░░░░░░░░░░░░░░░░░ │
 │ 77% frei        Reset in 4d 12h │
 │ ● 9% Reserve   reicht bis Reset │
 │      aktualisiert vor 2 min     │
 └─────────────────────────────────┘
```

## Features

- **Standalone:** on-device OAuth login (PKCE) with automatic token refresh. Your Claude Code session on the PC
  is not touched; the device gets its own token pair.
- **Live usage:** polls every 5 minutes, honours `Retry-After`, backs off on errors.
- **Two layouts:** ring gauges, or bars with reserve and a projection to the reset. Both are green / amber /
  red (amber also when the server reports a warning severity). Switch on the `/claude` page.
- **Time marker:** a white tick on rings and bars marks how much of the window has passed, so you see at a
  glance whether you are ahead of an even pace. Can be turned off on the `/claude` page.
- **Reset times:** countdown for the session window, weekday and time for the weekly reset (local time zone).
  The API reports e.g. `10:59:59.95`; times are rounded to the minute and snapped to a quarter hour when within
  two minutes, so this shows as 11:00.
- **Proper TLS:** certificates are validated against pinned root CAs (GTS Root R4, ISRG Root X1/X2).
- **Fits into 80 KB of RAM:** see [Memory notes](#memory-notes) for what it took.
- **Web page** at `http://<device>/claude` for login, status, layout, manual refresh and logout. It is served
  from flash, so no filesystem update is needed. The home page is served from flash too, so the menu has a
  *Claude Usage* entry even with the upstream filesystem image.

## Hardware

| Device                | MCU     | Display            | Status          |
| --------------------- | ------- | ------------------ | --------------- |
| GeekMagic SmallTV Ultra | ESP8266 | 240×240 ST7789   | tested          |
| GeekMagic HelloCubic Lite | ESP8266 | 240×240 ST7789 | should work (same board) |

The device must already run the GeekMagic Open Firmware, or be flashed with it once over serial. See the
[upstream documentation](docs/open-firmware.md) for teardown, wiring and first-time flashing. **Make a
flash backup before you start.**

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                      # builds .pio/build/esp12e/firmware.bin
```

For a first install over serial, create `data/config.json` from `data/config-smartTV.example` (Wi-Fi and an
API token of your choice), then `pio run -t uploadfs && pio run -t upload`.

## Flash over the air

If the device already runs the open firmware, upload the firmware image. You can use the web UI
(*Firmware / FS Update* → Firmware) or curl:

```bash
curl -H "Authorization: Bearer <API_TOKEN>" \
     -F "file=@.pio/build/esp12e/firmware.bin" \
     http://<device-ip>/api/v1/ota/fw
```

Only the firmware is needed; the LittleFS image can stay as it is.

**Lost the API token?** Power-cycle the device three times within 20 seconds each to enter rescue mode. It opens
the access point `GeekMagic` (password `$str0ngPa$$w0rd`, IP `192.168.4.1`). There you can upload firmware
without a token (`POST /api/v1/rescue/ota`, field `firmware`) and set a new token
(`POST /api/v1/rescue/token` with `{"token":"..."}`). Leave rescue mode with `POST /api/v1/rescue/reboot`. A plain
power cycle is not enough, because the boot counter is persisted.

## Set up

1. Open `http://<device-ip>/token.html` and save your API token in the browser.
2. Open `http://<device-ip>/claude` and click **Login starten**. A Claude authorization page opens in a new tab.
3. Approve, then copy the code shown there (format `code#state`), paste it on the device page and click
   **Code bestätigen**.
4. After a few seconds the display switches to the gauges.

Until you log in, the display shows the login URL.

## Configuration

| What               | Where                                          | Default                        |
| ------------------ | ---------------------------------------------- | ------------------------------ |
| Time zone          | build flag `-DCLAUDE_TZ="..."` (POSIX TZ rule) | `CET-1CEST,M3.5.0,M10.5.0/3`   |
| Poll interval      | `FETCH_INTERVAL_MS` in `src/claude/ClaudeUsage.cpp` | 5 min                     |
| OAuth scope        | *Erweitert* on the login page                  | `user:profile` (enough for usage) |
| Warn / critical    | `WARN_PERCENT` / `CRIT_PERCENT` in `src/claude/ClaudeScreen.cpp` | 70 % / 90 %  |
| Layout             | `/claude` page, stored as `claude_layout` in `config.json` (0 rings, 1 bars) | rings |
| Time marker        | `/claude` page, stored as `claude_pace` in `config.json` | on |
| Pace windows       | `SESSION_WINDOW_S` / `WEEKLY_WINDOW_S` in `src/claude/ClaudeScreen.cpp` | 5 h / 7 d |

## API

All endpoints need `Authorization: Bearer <API_TOKEN>`, except the page itself.

| Method | Path                         | Purpose                                                    |
| ------ | ---------------------------- | ---------------------------------------------------------- |
| GET    | `/claude`                    | Login and status page                                      |
| GET    | `/api/v1/claude/status`      | State, usage, layout, time marker, token expiry, heap, last reset and crash record |
| POST   | `/api/v1/claude/display`     | Screen settings, saved in `config.json`: `{"layout":0}` rings or `1` bars, `{"pace":false}` hides the time marker; both fields optional |
| POST   | `/api/v1/claude/login/start` | Starts a PKCE login, returns the authorize URL (`{"scope":...}` optional) |
| POST   | `/api/v1/claude/login/finish`| Queues the pasted code; the exchange runs in the background (`login_error` in status) |
| POST   | `/api/v1/claude/refresh`     | Fetch usage now                                            |
| POST   | `/api/v1/claude/logout`      | Forget the tokens                                          |
| GET    | `/api/v1/claude/diag`        | Network check from the device: gateway, DNS, TCP, HTTP     |

## How it works

```
 boot ── Wi-Fi ── DNS fallback ── NTP (or HTTP Date fallback)
                                      │
 /claude page ── PKCE login ──► platform.claude.com/v1/oauth/token ──► tokens in EEPROM (obfuscated)
                                      │
 every 5 min ──► api.anthropic.com/api/oauth/usage ──► five_hour / seven_day / limits[] ──► display
                 (refresh 5 min before expiry, retry once on 401)
```

The OAuth client, endpoints and PKCE parameters are the same ones Claude Code uses (checked against Claude Code
2.1.312).

## Memory notes

An ESP8266 has about 80 KB of RAM, and a BearSSL handshake normally wants a 16 KB receive buffer plus a 6 KB
stack. To make the TLS calls fit next to the web server, the firmware does the following:

- **4 KB TLS receive buffer.** Neither host negotiates max fragment length, but both send small records. The
  largest measured record is the 3.4 KB certificate chain of `platform.claude.com`; usage data arrives in about
  1.4 KB records.
- **Root CAs are loaded per host and only during a request,** instead of staying in RAM.
- **`scripts/patch_gfx_progmem.py`** moves about 7 KB of YCbCr lookup tables in *GFX Library for Arduino* to
  flash. They hang off a virtual method, so the linker cannot drop them, but this firmware never decodes JPEG.
- **The EEPROM RAM mirror (2 KB)** is only allocated while secure storage reads or writes.
- **The token exchange runs from `loop()`,** not inside the HTTP handler, so no browser connection holds heap
  during the handshake.

Typical numbers: about 30 KB free heap idle, and TLS calls succeed repeatedly.

## Troubleshooting

| Symptom | Cause / fix |
| ------- | ----------- |
| `Waiting for time sync` | No NTP and no HTTP. Check internet access with `/api/v1/claude/diag`. |
| `dns0: (IP unset)` in diag | The DHCP server gives no DNS server (e.g. the Windows mobile hotspot). The firmware then uses 1.1.1.1 / 8.8.8.8 automatically. |
| `low memory` / `Unable to allocate memory for SSL` | Too little heap for TLS. Stop GIF playback and reload; check `free_heap` in status. |
| Device reboots | Read `last_crash` in status and decode with `xtensa-lx106-elf-addr2line -pfiaC -e .pio/build/esp12e/firmware.elf <addrs>`. |
| `HTTP 403` on usage | Scope too narrow. Log in again with `user:profile user:inference` (login page → *Erweitert*). |
| `Login abgelaufen` on the display | The refresh token was revoked. Log in again. |
| Web pages show no data, log has `Unauthorized request` | The browser holds an old API token. On *API Token*, enter the current token in the top field and click **Save**. *Change Token* changes the token on the device and needs the old one. |

## Changes to the base firmware

Besides the Claude module (`src/claude/`, `src/web/ClaudeApi.cpp`), TokenTV fixes a few things in the base
firmware:

- An API token set via the web UI or rescue mode is now persisted. Before, it was lost on reboot.
- A DNS fallback is used when DHCP provides none.
- CPU runs at 160 MHz for faster TLS.
- *GFX Library for Arduino* is pinned to 1.6.4, and its YCbCr tables are moved to PROGMEM.
- Secure storage frees its EEPROM buffer when idle.
- `/` and `/index.html` are served from flash (`include/web/IndexPage.h`, a copy of `data/web/index.html`).
- `config.json` has extra `claude_layout` and `claude_pace` fields.

## Disclaimer

TokenTV uses **unofficial, undocumented** endpoints and presents itself with Claude Code's public OAuth client
ID. They can change or break at any time, and using them from a third-party device may conflict with Anthropic's
terms. It is meant for watching your own usage, at your own risk. This project is not affiliated with Anthropic
or GeekMagic.

Tokens are stored obfuscated, not encrypted, in the device's flash (see the upstream notes on secure storage).
Anyone with physical access or your API token can get at them. Use **Abmelden** on the device page to delete
them; you can also revoke the session in your Claude account settings.

## Credits and license

- [Times-Z/GeekMagic-Open-Firmware](https://github.com/Times-Z/GeekMagic-Open-Firmware): the open firmware this
  is based on.
- Inspired by [geekmagic-stats](https://github.com/jimmystridh/geekmagic-stats),
  [claude-code-stats](https://lib.rs/crates/claude-code-stats) and
  [ESP-ClaudeUsageMonitor](https://github.com/ximian1984/ESP-ClaudeUsageMonitor).

GPL-3.0-or-later, see [LICENSE](LICENSE).
