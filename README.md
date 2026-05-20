# Clawdmeter — ESP32-C6 fork

## Het verhaal

Als AI-liefhebber zag ik [Hermann's originele Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) langs komen en wilde ik er meteen één voor mijn bureau. Klein dashboardje dat live je Claude Code usage laat zien — perfect.

Helaas: het **verkeerde kastje besteld**. Hermann's project draait op de Waveshare **ESP32-S3**-Touch-AMOLED-2.16, en ik had de **ESP32-C6** versie liggen. Niet hetzelfde — andere CPU (RISC-V vs Xtensa), geen PSRAM (512KB SRAM totaal), ander display-IC (SH8601 i.p.v. CO5300), andere pinout, andere buttons.

In plaats van het terugsturen heb ik samen met **Claude Code** de hele firmware geport. Wat werkt nu op de C6:
- SH8601 driver met de vendor-init sequence (zonder dit blijft het paneel zwart).
- Per-strip QSPI push die de display niet verwart (CS-toggle bug is écht).
- Splash-animatie herontworpen van 460KB CPU-canvas naar een 20×20 LVGL image met 24× zoom — past in 800 bytes.
- Snapshot/screenshot uit (past niet zonder PSRAM).
- Knoppen opnieuw gemapped: BOOT (links), KEY2 (rechts), KEY1 (PWR).
- IMU-rotatie-as transform omdat de QMI8658 fysiek anders gemonteerd zit.
- Touch coords meegedraaid met het scherm zodat swipes blijven werken in elke oriëntatie.

## Wat ik daarna toegevoegd heb

Naast de port nog twee features die het ding écht functioneel maken:

### 1. Tweede pagina: Extra usage / Monthly spend

Swipe op het Usage scherm **van rechts naar links** → je komt op een nieuwe Details-pagina die je month-to-date API spend laat zien (precies wat in `console.anthropic.com → Settings → Billing` staat als "Extra usage"). Inclusief budget en progress-bar.

Data komt uit Anthropic's OAuth usage endpoint (`/api/oauth/usage`) — dezelfde token die Claude Code al gebruikt, geen aparte admin key nodig. Daemon haalt 'm op, stuurt 'm over BLE naar het kastje.

Swipe terug naar rechts om weer op het Usage scherm te komen.

### 2. Pomodoro focus-timer

Met de rechterknop kan je een focus-sessie starten:

| Knop-actie | Wat 't doet |
|---|---|
| 1× tikken | Shift+Tab (Claude Code mode toggle, zoals voorheen) |
| 2× snel achter elkaar | 25-minuten focus-sessie |
| 3× | 60-minuten sessie |
| 4× | 90-minuten sessie |

Fullscreen arc-countdown met `MM:SS` in het midden. Als de tijd op is: paneel flasht naar max brightness, "Done!" verschijnt, en het kastje **typt automatisch `/clear` + Enter naar Claude Code** zodat je in een schone conversatie begint. PWR-knop tijdens de timer = annuleren.

Beide features werken alleen als de daemon aan staat en je met Claude bent ingelogd — zie hieronder.

## Hoe verbind je je Claude account?

De daemon op je Mac leest je Claude Code OAuth-token (uit `~/.claude/.credentials.json` of de macOS Keychain entry `Claude Code-credentials`) en pollt elke 60s de Anthropic API voor rate-limit + spend data. Die data wordt over BLE naar het kastje gestuurd.

**Eerste keer setup:**

1. Zorg dat je in **Claude Code** ingelogd bent op je Mac:
   ```bash
   claude login
   ```
   (Of via de Claude Code app — installer leidt door OAuth flow.)

2. Daemon installeren:
   ```bash
   cd Clawdmeter
   ./install-mac.sh
   ```
   Dit maakt een Python venv met `bleak` en `httpx`, installeert een LaunchAgent die automatisch start, en vraagt om Bluetooth-permissie voor Terminal.

3. Pair het kastje:
   - System Settings → Bluetooth → klik **Connect** naast "Claude Controller".
   - Daemon vindt 'm binnen ~30 seconden automatisch en begint data te sturen.

4. Verifieer dat 't werkt:
   ```bash
   tail -F ~/Library/Logs/claude-usage-daemon.out.log
   ```
   Je moet om de minuut een regel zien als:
   ```
   Sending: {"s":12,"sr":155,"w":35,"wr":3015,"st":"allowed","ok":true,"eu":12.34,"em":50.0,"cu":"EUR"}
   ```
   - `s` / `sr` = session % + reset (minuten) — Usage scherm bovenste balk
   - `w` / `wr` = weekly % + reset — Usage scherm onderste balk
   - `eu` / `em` / `cu` = extra usage, budget, currency — Details scherm

**Account swappen?** Log uit en in via Claude Code (`claude logout` / `claude login`). De daemon pakt automatisch de nieuwe token op bij de volgende poll.

**BLE adres reset** (bv. na firmware-flash op een ander board):
```bash
rm ~/.config/claude-usage-monitor/ble-address
```

## De originele beschrijving

A small ESP32 dashboard I made for my desk to keep an eye on Claude Code usage.

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) and pairs with my laptop over Bluetooth, the splash screen plays pixel-art Clawd animations that get
busier when your usage rate climbs. The two side buttons send Space and
Shift+Tab over BLE HID for Claude Code's voice mode and mode-toggle shortcuts.

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

The Clawd animations come from [claudepix](https://claudepix.vercel.app), [@amaanbuilds](https://x.com/amaanbuilds)'s library of pixel-art Clawd sprites, check it out, it's lovely.

## Screens

The device boots into the splash and stays there until you press the middle (PWR) button, which cycles between Usage and Bluetooth. Tap the screen anywhere (except the Reset zone on the Bluetooth screen) to flip back to the splash; tap again to dismiss it.

|              Splash               |              Usage              |                Bluetooth                |
| :-------------------------------: | :-----------------------------: | :-------------------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) | ![Bluetooth](screenshots/bluetooth.png) |
|   Splash; touch-toggle anytime    | Session and weekly utilization  |    Connection status and bond reset     |

While the splash is up, the middle button cycles animations instead of screens. The firmware also auto-rotates every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

## Hardware

Two supported boards (both 480×480, same chassis, same peripherals):

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) — ESP32-S3R8 (dual-core Xtensa, 8MB PSRAM), **CO5300** AMOLED, CST9220 touch, AXP2101 PMU, QMI8658 IMU
- [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm) — ESP32-C6 (single-core RISC-V, no PSRAM, 8MB flash), **SH8601** AMOLED, CST9217 touch, AXP2101 PMU, QMI8658 IMU. Cheaper, smaller power envelope; same firmware features.

Plus:
- USB-C cable for flashing firmware and charging
- 3.7V Li-Po battery (MX1.25 2-pin connector, optional)

### Build flavours

`firmware/platformio.ini` defines two environments:

| Env                          | Board   | Notes                                                      |
| ---------------------------- | ------- | ---------------------------------------------------------- |
| `waveshare_amoled_216_s3`    | S3 2.16 | Original target. PSRAM-backed splash, screenshot QA cmd.   |
| `waveshare_amoled_216_c6`    | C6 2.16 | SH8601 driver, vendor init, no-PSRAM splash, screenshot off. |

Build a specific board: `pio run -d firmware -e waveshare_amoled_216_s3` (or `_c6`). The flash helper auto-picks the S3 env; for C6 use `pio run -e waveshare_amoled_216_c6 -t upload --upload-port /dev/cu.usbmodem<XXXX>` (find the port with `ls /dev/cu.usbmodem*`).

## Prerequisites

- Linux (tested on Ubuntu) or macOS
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `curl`, `bluetoothctl`, `busctl` (BlueZ Bluetooth stack)
- macOS: `python3` (the installer sets up a venv with `bleak` and `httpx`)
- Claude Code with an active subscription

## macOS installation

The macOS host pieces — Python daemon, LaunchAgent, and flash helper — were ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson). Thanks Chris!

### Flash the firmware

```bash
./flash-mac.sh                       # auto-detects /dev/cu.usbmodem*
./flash-mac.sh /dev/cu.usbmodem1101  # or pass an explicit USB serial port
```

### Pair the device

After flashing, open **System Settings → Bluetooth** and click *Connect* next to "Clawdmeter". The daemon will discover it on its next scan (~30 s).

### Install the daemon

The daemon reads your Claude OAuth token from the macOS Keychain (service `Claude Code-credentials`), polls usage every 60 s, and pushes it to the display over BLE.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `bleak` and `httpx`, renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and loads it. The first run is launched interactively so macOS prompts for Bluetooth permission.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

## Linux installation

### Flash the firmware

```bash
cd firmware
pio run -t upload --upload-port /dev/ttyACM0
```

### Pair the device

After flashing, the device advertises as "Claudemeter". Pair it once:

```bash
# Scan for the device
bluetoothctl scan le

# When "Claude Controller" appears, pair and trust it
bluetoothctl pair F4:12:FA:C0:8F:E5    # use your device's MAC
bluetoothctl trust F4:12:FA:C0:8F:E5
```

The MAC address is shown on the Bluetooth screen — press the middle (PWR) button to cycle to it.

### Install the daemon

The daemon polls your Claude usage every 60 seconds and sends it to the display over BLE.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

## How it works

1. The daemon reads your Claude Code OAuth token from `~/.claude/.credentials.json`.
2. It makes a minimal API call to `api.anthropic.com/v1/messages` — one token of Haiku, basically free.
3. The usage numbers come straight out of the response headers (`anthropic-ratelimit-unified-5h-utilization` and friends).
4. The daemon connects to the ESP32 over BLE and writes a JSON payload to the GATT RX characteristic.
5. The firmware parses it and updates the LVGL dashboard.
6. The firmware also tracks the rate of change of session % over a 5-minute window and picks splash animations from the matching mood group.
7. The two side buttons are independent of all of this — they send Space and Shift+Tab as BLE HID keyboard input to the paired host directly.

## Physical buttons

The board has three side buttons. Left and right do the same thing on every screen; the middle button is screen-aware.

| Button           | S3 GPIO      | C6 GPIO       | Function                                                       |
| ---------------- | ------------ | ------------- | -------------------------------------------------------------- |
| **Left**         | GPIO 0 (BOOT)| GPIO 9 (BOOT) | Hold to send Space (Claude Code voice-mode push-to-talk)       |
| **Middle** (PWR) | AXP2101 PKEY | AXP2101 PKEY  | Cycle screens (Usage ↔ Bluetooth); on splash, cycle animations |
| **Right**        | GPIO 18      | GPIO 10 (KEY2)| Press to send Shift+Tab (Claude Code mode toggle)              |

Space and Shift+Tab go out as standard BLE HID keyboard reports, so they trigger in whatever window has focus on the paired host — not just Claude Code.

## ESP32-C6 specifics

The C6 port keeps all features of the S3 build, but a few things behave differently because of the smaller chip and different display IC:

- **No PSRAM.** Total SRAM is ~512KB. The 480×480 splash canvas was replaced with a 20×20 RGB565 image that LVGL scales 24× at draw time (`lv_image_set_scale`, antialias off). Saves ~460KB.
- **SH8601 vendor init.** The C6 board uses the SH8601 display controller; the bare `Arduino_SH8601` lib init leaves the panel dark. `main.cpp` runs the Waveshare vendor init sequence (`sh8601_vendor_init()`) right after `gfx->begin()`.
- **Streamed per-strip push.** Per-strip `draw16bitRGBBitmap` calls blanked the SH8601 panel (CS toggles between calls confuse this panel revision). The flush path was rewritten to `startWrite → writeAddrWindow → writePixels → endWrite` so each LVGL strip is one clean QSPI transaction.
- **No screenshot serial command.** The host-side `./screenshot.sh` QA flow needs a 460KB framebuffer that doesn't fit in C6 SRAM — disabled on this build. The command returns `SCREENSHOT_UNAVAILABLE_NO_PSRAM`.
- **USB Serial/JTAG only.** No native USB OTG — the C6 build sets `ARDUINO_USB_MODE=1` so `Serial` resolves to `HWCDCSerial` (without this SensorLib won't compile).
- **Bigger partition table.** Firmware is ~1.4MB so the C6 env uses `huge_app.csv` (3MB app slot, no OTA) on the 8MB flash.

### Fixing wrong-direction auto-rotation

The QMI8658 IMU is mounted at a different physical orientation on the C6 board. If the screen rotates the wrong way when you tilt the device, edit `firmware/src/imu.cpp` and change `CLAWD_IMU_AXES` to one of the 8 presets (0–7) listed in the comment block, or set `-DCLAWD_IMU_AXES=<n>` in `platformio.ini`. One of them will line up.

## BLE protocol

The device advertises a custom GATT service alongside the standard HID keyboard service:

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| **HID Service**            | `00001812-0000-1000-8000-00805f9b34fb` |

JSON payload format (written to RX):

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true }
```

Fields: `s` = session %, `sr` = session reset (minutes), `w` = weekly %, `wr` = weekly reset (minutes), `st` = status, `ok` = success flag.

## Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## Converting Lucide icons

The UI uses a small set of [Lucide](https://lucide.dev) icons (bluetooth + battery states) converted to RGB565 / RGB565A8 C arrays for LVGL.

```bash
node tools/png_to_lvgl.js assets/icon_bluetooth_48.png icon_bluetooth_data ICON_BLUETOOTH_WIDTH ICON_BLUETOOTH_HEIGHT
```

Default tint is white (`0xFFFFFF`); Lucide PNGs ship as black-on-transparent and would render invisible against the dark UI without it. Pass `--no-tint` for pre-coloured artwork like the logo. Battery icons use RGB565A8 (alpha plane) so they blend cleanly over the splash; the rest are baked RGB565 over the panel colour. Paste the converter output into `firmware/src/icons.h`.

## Splash animations

The animations come from [claudepix.vercel.app](https://claudepix.vercel.app),
a library of Clawd sprites. `tools/scrape_claudepix.js` evaluates the
site's JavaScript in a Node VM to pull out frame data and palettes, then
`tools/convert_to_c.js` turns everything into RGB565 C arrays and writes
`firmware/src/splash_animations.h`.

To re-pull (e.g. when the source library updates):

```bash
node tools/scrape_claudepix.js
node tools/convert_to_c.js
pio run -d firmware -t upload
```

See `tools/README.md` for details.

## Credits

- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app). Frame data and palettes scraped + converted by the tooling in `tools/`.
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
