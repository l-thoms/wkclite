# AGENTS.md

Testing/verification workflow for **WKCLite** (ESP32-S3, dual-screen video OSD handheld):
**UVC screen capture + serial key injection**.

Run commands from the repository root.

Two principles keep this document from rotting:

1. **Machine specifics are not written down** — the serial port, capture device, ffmpeg binary,
   ESP-IDF installation and screenshot folder are resolved at runtime, or the user is asked (§0).
2. **The application's menus/settings/pages are not written down either** — they change as features
   are added. This document instead points at the code that defines them (§0.1) and describes how
   to discover the current layout from the screen and from key presses (§5).

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
| Serial character → key mapping, GPIO0 gesture | `main/io/serialport.c` |
| Visible labels for every key | `resources_raw/translations.json` |
| Settings fields / persistence | `main/profile/settings.c` |
| Flash partitions, resource images | `partitions.csv`, root `CMakeLists.txt` |

> A screenshot or the running device always wins over prose: if the current UI differs from what
> this document implies, adapt the test — nothing here needs to be edited for that.

---

## 1. Build & flash

```bash
idf.py build                 # compile
idf.py -p <PORT> app-flash   # app only
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

If the process name no longer matches, find whatever holds `<PORT>` instead:
`fuser <PORT>` / `lsof <PORT>` (Linux, macOS), `Get-Process | Where-Object { $_.Modules... }` or
Device Manager (Windows).

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
| `x` | OSD visibility | `c` | Screen index
| `c` / `v` | Screen orientation select

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
5. navigate to the state under test (§5 explains how to find it without a fixed map)
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

## 5. Navigating the UI without a fixed map

Menu entries, their order and even the number of menus change as the product evolves. Do **not**
encode a menu path with a fixed number of key presses. Instead:

### 5.1 Invariants of the menu framework

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

### 5.2 Discovery procedure (use this instead of a hard-coded path)

1. Send `e` (or the current MENU equivalent from `main/io/serialport.c`), capture, and read the
   visible entries from the screenshot.
2. Count the DOWN presses needed to reach the entry whose label matches the target. Labels come
   from `resources_raw/translations.json`, so the same text can be searched there to confirm.
3. Press OK, capture, and repeat from step 1 for the next level.
4. To go back, press EXIT the same number of times as levels entered (verify with a capture), and
   remember that the last EXIT leaves the page.
5. Keep each step's screenshot: it documents the path that was actually taken for that run.

### 5.3 Consequences for tests

- Never assert on `"index 3 of the settings root"`; assert on the visible label or on the state
  after the action.
- If an entry moves or is renamed, only the discovery steps change — nothing in this document has
  to be edited.
- When the UI gains features, extend tests by discovery; when the framework's behaviours in §5.1
  change, that is when this section should be updated.

---

## 6. Testing discipline

- **Tests modify persisted settings.** Restore whatever you changed and verify it afterwards by
  re-reading the settings page (its fields live in `main/profile/settings.c`). Leave the device on
  the home page.
- Values that are regenerated on every boot (e.g. a random session code) need no restore.
- Do not treat existing behaviour of `main/ui/camera.c` as something to change; the camera page has
  been explicitly declared off-limits for UI refactors.
- After UI changes, regress every registered page (see `main/main.c`) in all three orientations
  (horizontal / vertical / vertical tiled). In multi-screen modes check both screens — the capture
  may show only one (see §3).
- Screenshots show only the composed OSD output — not key latency, flicker, or whether a refresh
  was actually triggered. For "does it refresh" questions, ask the user to confirm on the real
  device/App, or compare two consecutive frames.
- When several machines/adapters are in use, state which `<PORT>`/`<UVC_DEVICE>` a run used so the
  result can be reproduced.
