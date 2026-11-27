# AGENTS.md

Working agreement for **WKCLite** (ESP32-S3, dual-screen video OSD handheld): the
**testing/verification workflow** (UVC capture + serial key injection) and the **UI design rules**
that every UI change must follow.

Run commands from the repository root.

Three principles keep this document from rotting:

1. **Machine specifics are not written down** — the serial port, capture device, ffmpeg binary,
   ESP-IDF installation and screenshot folder are resolved at runtime, or the user is asked (§0).
2. **The application's menus/settings/pages are not written down either** — they change as features
   are added. This document instead points at the code that defines them (§0.1) and describes how
   to discover the current layout from the screen and from key presses (§6).
3. **UI design rules are stable requirements, not current layout** — §5 states the rules; the code
   references say where they are currently implemented.

---

## 0. Resolve the machine environment first — ask the user when unknown

| Item | How to discover it | If it cannot be determined |
| --- | --- | --- |
| ESP-IDF | `idf.py --version`, `echo $IDF_PATH` (`$env:IDF_PATH` on Windows) | **Ask the user** how to activate their ESP-IDF environment |
| Serial port (`<PORT>`) | list ports (below) | **Ask the user** |
| Capture device (`<UVC_DEVICE>`) | list capture devices (§3) | **Ask the user** |
| ffmpeg (`<FFMPEG>`) | `ffmpeg -version` / `where ffmpeg` / `command -v ffmpeg` | **Ask the user** for the path |
| Screenshot folder (`<SHOT_DIR>`) | OS temp dir | Use a temp directory; no need to ask |
| Target chip | `sdkconfig` (`CONFIG_IDF_TARGET`) | ESP32-S3 for this project |

Rules:

- Never hard-code a port name, device name, install path or user name.
- Prefer detection; when it is ambiguous (several serial ports, several video devices, ffmpeg
  missing) **ask the user** instead of guessing.
- Activating ESP-IDF differs per installation (an `export.sh`-style script, an `idf.py` wrapper, a
  PowerShell profile). Do not assume a fixed path; if the environment is not active, ask.

List serial ports:

```powershell
# Windows
[System.IO.Ports.SerialPort]::GetPortNames()
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description
```

```bash
# Linux
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
# macOS
ls /dev/tty.* /dev/cu.* 2>/dev/null
```

### 0.1 Sources of truth in the repository (read them, do not memorize)

| Concern | Where |
| --- | --- |
| Registered pages, boot-complete log line | `main/main.c` |
| Home / main menu entries | `main/ui/menu.c` |
| Settings page: menu trees, entries, special screens | `main/ui/settings.c` (the `ui_settings_build_*_menu` functions) |
| Camera page menus | `main/ui/camera.c` |
| Offline (GPIO0) menu | `main/ui/offline_menu.c` |
| Common menu widget behaviour, key codes, page API | `main/ui/ui_common.c`, `main/ui/ui_common.h`, `main/ui/shell.c`, `main/ui/shell.h` |
| Formats, resolutions, colour/layer semantics | `main/display/display_common.h`, `main/display/graphics.c` |
| Buffers, compositing, I2S, vsync compensation | `main/display/display_control.c`, `main/display/display_control.h` |
| Serial character → key mapping, GPIO0 gesture | `main/io/serialport.c` |
| Quick settings/actions tables (what the phone sees) | `main/protocol/quicksettings.c` |
| Visible labels for every key | `resources_raw/translations.json` |
| Settings fields / persistence | `main/profile/settings.c` |
| Flash partitions, resource images | `partitions.csv`, root `CMakeLists.txt` |

> A screenshot or the running device always wins over prose: if the current UI differs from what
> this document implies, adapt the test — nothing here needs to be edited for that.

---

## 1. Build & flash

```bash
idf.py build                 # compile
idf.py -p <PORT> app-flash   # app only, normal iterations
idf.py -p <PORT> flash       # everything (bootloader + partition table + app + images)
```

### 1.1 Speed up flashing

The root `CMakeLists.txt` builds and flashes the FATFS raw images (currently `data_static` from
`resources/` and `data_fonts` from `fonts/` — re-read the file, the set may change) together with
the project because of `FLASH_IN_PROJECT`:

```cmake
fatfs_create_rawflash_image(data_static resources FLASH_IN_PROJECT)
fatfs_create_rawflash_image(data_fonts  fonts     FLASH_IN_PROJECT)
```

Those images are re-flashed during every full `idf.py flash` that dominates the flash time.
Partition table or resource files changed (see `partitions.csv`) always needs the full `idf.py flash`. Otherwise use `app-flash`.

### 1.2 Free the serial port before flashing

A leftover monitor keeps the port open and flashing fails with `esptool ... failed` /
`Serial port ...: FAILED`. ESP-IDF's monitor currently runs as `idf_monitor` and spawns several
python processes — kill all of them. The user may also be running a monitor in their own terminal,
so mention it before killing.

```powershell
# Windows
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -match 'idf_monitor' } |
  Stop-Process -Force -ErrorAction SilentlyContinue
```

```bash
# Linux / macOS
pgrep -af idf_monitor
pkill -f idf_monitor
```

If the process name no longer matches, find whatever holds `<PORT>` instead: `fuser <PORT>` /
`lsof <PORT>` (Linux, macOS), Device Manager or `Get-Process` (Windows).

After flashing the board resets itself; poll the log for the boot-complete line (currently
`Entering Mainloop`, defined in `main/main.c`) with a generous timeout instead of sleeping a fixed
time. Start `idf.py -p <PORT> monitor` only when needed (it holds the port).

---

## 2. Serial key injection

`main/io/serialport.c` maps received characters to UI key codes, so the UI can be driven by
"typing" into the serial port. **Read that file for the current mapping**; the table below is only a
snapshot of what it looked like when this document was written.

| Char | Key | Char | Key |
| --- | --- | --- | --- |
| `w` | UP | `a` | LEFT |
| `s` | DOWN | `d` | RIGHT |
| `Space` | OK | `q` | EXIT |
| `e` | MENU | `z` | GPIO0 key |
| `x` | OSD visibility | `c` | Screen index |
| `v` / `b` | Screen orientation select | `n` | Revert output mode to falling edge |

- GPIO0 gesture (currently: three `z` within 0.5 s = long press, one or two = short presses) is
  implemented in the same file.
- Port settings: 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control.
- Open the port with **DTR and RTS deasserted**; otherwise the chip may be reset or held in reset.
- Reset the board by pulsing RTS (~150 ms), then reopen the port in normal mode — or press the
  board's reset button if the adapter does not wire DTR/RTS.
- Leave 500–800 ms between key presses; after a page/menu transition wait an extra 1–1.5 s before
  capturing.

Any available transport is fine; do not hard-code a path:

```python
# Portable (pyserial), Windows/macOS/Linux
import serial, time
port = serial.Serial("<PORT>", 115200, timeout=0.2)
port.dtr = False; port.rts = False
for key in ["e", "d", " "]:
    port.write(key.encode())
    time.sleep(0.6)
```

```powershell
# Windows PowerShell (.NET SerialPort)
$port = New-Object System.IO.Ports.SerialPort <PORT>,115200,None,8,one
$port.DtrEnable = $false; $port.RtsEnable = $false; $port.ReadTimeout = 200
$port.Open()
$port.Write('e'); Start-Sleep -Milliseconds 600
```

```bash
# Linux / macOS without extra libraries
stty -F <PORT> 115200 cs8 -cstopb -parenb -crtscts clocal -hupcl
printf 'e' > <PORT>; sleep 0.6
```

---

## 3. UVC screen capture

ffmpeg is the same everywhere; only the **input backend** differs.

```bash
# Windows (dshow)
<FFMPEG> -hide_banner -loglevel error -f dshow -rtbufsize 256M \
         -i "video=<UVC_DEVICE>" -frames:v 1 -y <SHOT_DIR>/shot.png

# macOS (avfoundation; <UVC_INDEX> from the device list, video-only form <index>:none)
<FFMPEG> -hide_banner -loglevel error -f avfoundation -rtbufsize 256M \
         -i "<UVC_INDEX>:none" -frames:v 1 -y <SHOT_DIR>/shot.png

# Linux (v4l2)
<FFMPEG> -hide_banner -loglevel error -f v4l2 -rtbufsize 256M \
         -i /dev/video<N> -frames:v 1 -y <SHOT_DIR>/shot.png
```

List capture devices:

```bash
# Windows
<FFMPEG> -hide_banner -list_devices true -f dshow -i dummy
# macOS
<FFMPEG> -hide_banner -f avfoundation -list_devices true -i ""
# Linux
v4l2-ctl --list-devices            # if v4l2-utils is installed
ls /dev/video*
```

Notes:

- Resolution/format depend on the capture card; do not assume a fixed size in assertions.
- **The capture path lags by 1–2 frames (~1 s).** After a state change take two shots ~0.7–1 s
  apart and trust the second one. Naming convention: `xxx.png` plus `xxx_b.png` for the second.
- Device busy / empty capture usually means another application (OBS, a browser tab, another
  ffmpeg) is holding the device — ask the user to release it.
- A garbled `No Signal` / format-mismatch frame means the cable is loose or the video format was
  just switched; wait and capture again.
- **When several screens are composed into one output (tiled/stacked mode) the capture may show
  only one of them.** If so, find the capture/screen-output setting in the UI (its text is listed
  in `resources_raw/translations.json`, the item itself in `main/ui/settings.c`) and switch it,
  then capture again. Rotated content in such modes is expected.

---

## 4. Standard verification loop

```
1. resolve <PORT>, <UVC_DEVICE>, <FFMPEG>, <SHOT_DIR>     (ask the user if unknown)
2. idf.py build
3. free the serial port (§1.2) and flash (§1, §1.1)
4. reset via RTS pulse; poll the log for the boot-complete line, then wait ~3 s
5. navigate to the state under test (§6 explains how to find it without a fixed map)
6. capture two screenshots per relevant state, use the second
7. inspect the PNGs
```

Keep the resolved values in variables so a run can be reproduced on another machine:

```powershell
$port   = '<PORT>'
$uvc    = '<UVC_DEVICE>'
$ffmpeg = '<FFMPEG>'
$shots  = '<SHOT_DIR>'
```

---

## 5. UI design rules (mandatory for every UI change)

### 5.1 Coordinate system: PAL is the base, NTSC is 5/6 of it

- Design and lay out in **PAL coordinates**: 384 × 288 (`DISPLAY_WIDTH_PAL` / `DISPLAY_HEIGHT_PAL`).
- NTSC is exactly **1/6 smaller** (320 × 240), so convert instead of hard-coding a second layout:
  `display_coordinate_compensation()`, `display_vector_compensation()`,
  `display_rect_compensation()`, `display_line_compensation()` in `main/display/graphics.c`
  currently apply `round(value / 12 * 10)` for NTSC and identity for PAL.
- Convert **per screen**, because the two screens may run different formats: the drawing API takes
  `formats[0]` / `formats[1]` and each rect/vector is compensated separately.
- Sizes are compensated as well as positions (a 240-wide board is 200 wide in NTSC). During
  incremental commits the region has to grow by the compensated shadow — 1 absolute pixel, or 2
  base (PAL) pixels.
- In non-horizontal orientations with NTSC the visible height is cropped (`ntsc_vert()` in
  `graphics.c`), so keep content away from the extreme edges.

### 5.2 OSD buffering: two PSRAM layers + one internal-RAM frame per screen

Per screen (`main/display/display_control.c`):

| Buffer | Memory | Role |
| --- | --- | --- |
| `primary_main_buffer` / `secondary_main_buffer` | SPIRAM | page drawing layer (screens 0 / 1) |
| `primary_overlay_buffer` / `secondary_overlay_buffer` | SPIRAM | toast layer (screens 2 / 3) |
| `primary_frame_buffer` / `secondary_frame_buffer` | internal RAM | composited output read by the I2S DMA |

- Draw into a PSRAM layer, then **commit** the changed region with
  `display_update(screen_index, orientation, &range)`, which merges main/overlay into the internal
  frame buffer for that range.
- Colour semantics (`display_color_t`): `TRANSPARENT = 0b01` shows the main layer, `BLACK = 0b10`
  and `WHITE = 0b11` mean "overlay wins" — the overlay is a paint-over layer, not a second page
  layer.
- **Commit incrementally.** Only these events require a whole-screen refresh: opening a page,
  video format or orientation change, closing/switching a popup, and a language change (the shell
  already raises `on_refresh` for the last three; see §5.5).
- Draw pages on the **main** layer (0/1). The overlay (2/3) belongs to the shell's toast; do not
  paint page content there. Popups are pages too: draw them on the main layer and let the shell
  refresh the page when a popup closes or switches.

### 5.3 Unified page style

- Background: **transparent** for every page (the video shows through). Only the toast has a black
  background.
- Primary colour: **white**. Highlight corner radius: **2px**. Borders: **2 px** stroke, **6 px** corner radius (in base coordinates,
  compensated per screen) — `display_draw_rounded_rect(..., stroke = 2)` /
  `display_fill_rounded_rect(..., corner_radius)`.
- Shadow: every element except the toast gets a black copy offset by **(1, 1) absolute pixels**
  (draw the shape in black after translating by (1, 1), then the white shape on top; see the item
  rendering in `main/ui/ui_common.c`).
- Highlighted elements only need their highlight background redrawn; all other elements must be
  redrawn completely.

### 5.4 Do not disturb the vsync synchronisation while drawing

The I2S transmit offset is auto-compensated from the CPU-cycle deltas measured in the vsync ISR
(`vsync_isr()` in `display_control.c`). Long PSRAM accesses during drawing corrupt those
measurements and make the OSD visibly shift up/down.

- Wrap long/full-screen drawing sequences in `display_control_operate_hang_request()` …
  `display_control_operate_hang_release()`. The counter is nesting-safe and mutex-protected; while
  it is non-zero the ISR treats the measurements as invalid. Always release it afterwards
  (`main/ui/camera.c` uses this around its composite draw).
- Repeated drawing calls automatically call `display_control_record_operate_time()`, which also
  invalidates the measurement window; for known-long operations outside drawing use
  `display_control_cancel_operate_time(duration)`.

### 5.5 Keep the phone in sync (both directions)

- When a UI action changes state that is exposed to the client, notify it immediately:
  `protocol_ble_notify_update_command(category, name)` for a single value,
  `protocol_ble_notify_update_table(category, name)` for a table. The tables and their item names
  live in `main/protocol/quicksettings.c`; if a value is listed there, the notification is
  mandatory.
- Conversely, when the phone changes something that affects the visible page, the page must update.
  The settings write path already calls `quicksettings_check_settings_home()` and the shell raises
  `on_refresh` on `QUEUE_EVENT_REFRESH`. New pages must implement `on_refresh` idempotently —
  rebuild derived data and re-read the settings instead of caching values from `on_show`.

### 5.6 Keep key handlers small — one function per entry

- Use the shared menu widgets for standard lists. `on_key_event` keeps only the generic framework
  (movement, activation); each entry's behaviour is a function (the `ui_menu_action_t` style used by
  `main/ui/camera.c` and the `ui_settings_*_action` functions in `main/ui/settings.c`).
- For pages with custom states, dispatch from a small state switch to small functions — do not grow
  a large inline branch tree inside the key event.

### 5.7 Screen symmetry between the two displays

- **Horizontal / vertical**: the two screens may differ only in video format. Everything else
  (layout, content, behaviour) must be identical — achieve it by converting per screen (§5.1).
- **Vertical tiled**: the two screens are two independent vertical screens; never rely on
  cross-screen continuity. A "looks like one tall screen" impression is welcome (avoid icons at the
  exact centre, allow each side its own status bar), but every element must still be correct when
  each half is viewed alone.

### 5.8 All text must be translatable

- Every visible string goes through `wkc_translations_get_string(key)`, with the key present in
  `resources_raw/translations.json` for **all** supported languages.
- Do not build sentences from fragments that bake in word order or spacing.
- No space between CJK and Latin letters/digits (`接口USB`, not `接口 USB`).

### 5.9 Verify on hardware

Every UI change must be checked through the real pipeline — build/flash, serial keys and UVC
captures (§1–§4) — in all three orientations. Reading the code is not verification.

### 5.10 XGA direct drive (planned — keep the code ready)

`main/display/display_common.h` already documents the target design; keep new code compatible:

- XGA keeps the CVBS icons, fonts and I2S transport but changes the OSD resolution:
  **512 × 384** for horizontal/vertical (2× downscale, PAL resources) and **340 × 256** for
  vertical tiled (3× downscale, NTSC resources; the buffer stride is padded to 344 for 8-pixel
  alignment — see `DISPLAY_WIDTH_XGA_VERTICAL_TILED*`).
- XGA is enabled by probing an external screen controller (FPGA) over I2C. Once enabled **both**
  screens are XGA; there is no per-screen format switching and no CVBS connectivity fallback
  (sync-loss detection stays).
- XGA allocates only the primary buffer set; the secondary frame buffer is not allocated.
  Draw call signatures do not change: in non-tiled orientations, secondary (screen 1) draw calls
  are ignored; in vertical tiled, the two screens' draw calls map onto the top and bottom halves of
  the primary buffer.
- Consequence for UI code: keep drawing in terms of `(screen_index, orientation)` and never assume
  that per-screen buffers are distinct or that both screens are visible at once.

---

## 6. Navigating the UI without a fixed map

Menu entries, their order and even the number of menus change as the product evolves. Do **not**
encode a menu path with a fixed number of key presses. Instead:

### 6.1 Invariants of the menu framework

These are properties of the shared implementation (`main/ui/ui_common.c`, `main/ui/shell.c`) and
are far more stable than any menu content:

- The current menu is drawn on screen and its highlighted entry is visible — **the screen is the
  map**.
- UP/DOWN move the selection; LEFT/RIGHT change the value of a picker (pickers usually wrap);
  OK activates a switch or enters the selected entry.
- EXIT leaves the current level. Whether it pops one level or closes everything is page-specific —
  observed behaviour wins (in the settings page it pops one level; on the root it leaves the page).
- Sub-menus remember their own selection; the parent level is restored on EXIT.
- A page (not a menu) can host special full-screen states (editors, confirmations) that behave like
  a separate level: navigation keys act on the state, not on a list.

### 6.2 Discovery procedure (use this instead of a hard-coded path)

1. Send `e` (or the current MENU equivalent from `main/io/serialport.c`), capture, and read the
   visible entries from the screenshot.
2. Count the DOWN presses needed to reach the entry whose label matches the target. Labels come
   from `resources_raw/translations.json`, so the same text can be searched there to confirm.
3. Press OK, capture, and repeat from step 1 for the next level.
4. To go back, press EXIT the same number of times as levels entered (verify with a capture), and
   remember that the last EXIT leaves the page.
5. Keep each step's screenshot: it documents the path that was actually taken for that run.

### 6.3 Consequences for tests

- Never assert on `"index 3 of the settings root"`; assert on the visible label or on the state
  after the action.
- If an entry moves or is renamed, only the discovery steps change — nothing in this document has
  to be edited.
- When the UI gains features, extend tests by discovery; when the framework's behaviours in §6.1
  change, that is when this section should be updated.

---

## 7. Testing discipline

- **Tests modify persisted settings.** Restore whatever you changed and verify it afterwards by
  re-reading the settings page (its fields live in `main/profile/settings.c`). Leave the device on
  the home page.
- Values that are regenerated on every boot (e.g. a random session code) need no restore.
- Do not treat existing behaviour of `main/ui/camera.c` as something to change; the camera page has
  been explicitly declared off-limits for UI refactors.
- After UI changes, regress every registered page (see `main/main.c`) in all three orientations
  (horizontal / vertical / vertical tiled) and check the UI rules of §5 — transparency, border
  width/radius, shadow, translations, phone notifications, incremental refresh. In multi-screen
  modes check both screens; the capture may show only one (see §3).
- Screenshots show only the composed OSD output — not key latency, flicker, or whether a refresh
  was actually triggered. For "does it refresh" questions, ask the user to confirm on the real
  device/App, or compare two consecutive frames.
- When several machines/adapters are in use, state which `<PORT>`/`<UVC_DEVICE>` a run used so the
  result can be reproduced.
