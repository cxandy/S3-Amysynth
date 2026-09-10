# AGENTS.md (S3-Amysynth dev ops)

Notes for AI agents / humans working in this repo. The important, repeatable
operational facts live here so nothing has to be re-derived every session.

## Networking / proxy (Windows host)

- The machine uses **Clash Verge** as its proxy front end. Core is
  **`verge-mihomo.exe`**, service exe:
    - GUI: `C:\Program Files\Clash Verge\clash-verge.exe`
    - DATA: `C:\Users\Rose\AppData\Local\io.github.clash-verge-rev.clash-verge-rev`
    - `C:\Users\Rose\Downloads\clash.yaml` is a subscription export (used by
      the GUI, not consumed directly by agents).
- System proxy port: **`http://127.0.0.1:7897`** (mihomo listen port).
- The proxy daemon starts at user login, but **the listen port is not always
  up**. Symptom when it is up: `Test-NetConnection 127.0.0.1 -Port 7897`
  returns `TcpTestSucceeded=True`, and `curl.exe -x http://127.0.0.1:7897
  -s -o NUL -w "%{http_code}" https://github.com` returns `200`.
- If the port is NOT up but the GUI process is running: start the core with
  e.g. `& 'C:\Program Files\Clash Verge\clash-verge.exe'` or launch the
  verge service; if it is not listening after a few seconds, verify the GUI
  is actually running with `Get-CimInstance Win32_Process -Filter
  "Name='verge-mihomo.exe'"`, then check which port it bound:
    `Get-NetTCPConnection -OwningProcess <pid> -State Listen | Select
     LocalAddress, LocalPort`
- **git push habit**: `git -C <repo> push fork HEAD:main` (direct, no
  proxy) if the local `http.proxy` repo config is empty. If it fails with a
  connection error, the network is down - but DO NOT give up and DO NOT just
  wait: Clash Verge's 7897 proxy usually still works even when direct TCP to
  `github.com:443` fails. Try:
    1. `curl.exe -x http://127.0.0.1:7897 -s -o NUL -w "%{http_code}"
       --max-time 15 https://github.com` -> want `200`. If not, bring the
       proxy up first (see below).
    2. `git -C <repo> -c http.proxy=http://127.0.0.1:7897 push fork
       HEAD:main`
  Only if the proxy is also down should you retry the direct push after a
  short sleep.
- **Bringing the proxy up (agent should do this itself, not ask the user)**:
  the GUI `clash-verge.exe` starts the core `verge-mihomo.exe`; both usually
  autostart at login but the 7897 listener may be missing or dead. To (re)start:
    1. If `Get-CimInstance Win32_Process -Filter "Name='verge-mihomo.exe'"`
       returns nothing, launch the GUI:
       `& 'C:\Program Files\Clash Verge\clash-verge.exe'`
    2. Wait a few seconds, then confirm the listener:
       `Get-NetTCPConnection -OwningProcess <verge-mihomo pid> -State Listen`
       (expect 127.0.0.1:7897; port may be 7890 if configured differently).
    3. Verify with the `curl.exe -x ...` check above.
  Note: `Test-NetConnection 127.0.0.1 -Port 7897` sometimes returns False
  even when the listener is up; trust `curl -x` as the source of truth.

## Flashing the ESP32-S3 device

- Repo: fork remote `fork` = https://github.com/cxandy/S3-Amysynth.git
  (CI builds on `fork main`). `origin` = upstream rt-rtos.
- Firmware is built by GitHub Actions (`Build` workflow, forks push `main`);
  download with:
    `gh run download <run-id> --repo cxandy/S3-Amysynth -D <destdir>`
    artefact: `<destdir>\S3-Amysynth-firmware\S3-Amysynth-merged.bin`
  Then always hash it: `Get-FileHash <bin> -Algorithm MD5`.
- Ports on this machine:
    - **COM11** = app mode (PID_8000 composite: UAC + USB CDC). Speaks the
      import protocol at **9600 baud** (NOT 115200).
    - **COM8** = ROM download mode (PID_1001, USB-Serial/JTAG).
    - Entering download mode: first try `RST boot` (software, no buttons,
      DIAG-14+); fall back to physical **BOOT+RESET** if unusable. GPIO0 is
      both the BOOT strap and the SHIFT button
      (`CONFIG_AMYSYNTH_BTN_SHIFT_GPIO 0`).
- Correct flash command (verified working with the software `RST boot` path):
  ```
  python -m esptool --chip esp32s3 --no-stub -p COM8 --baud 460800 \
    --before usb-reset --after watchdog-reset write-flash 0x0 <merged.bin>
  ```
  - `--before usb-reset` is REQUIRED for the software-RST-boot path: the ROM
    download console's CDC needs a USB-level soft reset or esptool dies with
    "Write timeout" while connecting. It keeps the FORCE_DOWNLOAD_BOOT state
    (device stays in download mode) instead of pushing back to the app.
  - `--after watchdog-reset` is REQUIRED so the device boots straight into
    the app (the two `hard_reset`/`soft_reset` variants do not work on this
    board). It leaves the D+/D- mux back to OTG so COM11 reappears.
  - After flashing, poll for the app port (COM11, PID_8000) before probing.
- Device-side protocol (COM11 @ 9600):
    - `PING\n` -> `PONG\n`
    - `GET ver\n` -> `OK:S3-Amysynth DIAG-N`
    - `GET song\n` -> `OK:song count=.. enabled=.. loop=.. | s<i>:<bars>/<mask>...`
    - `PUT <slot 1..N> <txt|mid|arr> <bars 0-2> <len>\n` -> `ACK` -> body ->
      `OK:saved to slot N` / `ERR:...`
    - `RST boot\n` -> `OK:reboot to bootloader` then reboots into ROM
      download/flash mode (software reset, no buttons). Implemented DIAG-4+
      via `RTC_CNTL_OPTION1_REG / RTC_CNTL_FORCE_DOWNLOAD_BOOT` (the earlier
      GPIO0-low approach did NOT work - pad goes input+pullup during reset).
      DIAG-14: one-shot SW_SYS_RST, NO RTC WDT armed (DIAG-13's WDT
      survived the reset it triggered and left the ROM download console in
      an infinite ~62ms reset loop - rst:0x9 RTCWDT_SYS_RST, ~35 reboots/s,
      so esptool could never connect). If ever stuck in that loop, escape
      with `%TEMP%\opencode\wdt_escape.py` (sends exact-esptool-framed
      WRITE_REG to WDTCONFIG0=0 during the banner listen window).
- Probing the port from PowerShell needs care: use
  `New-Object System.IO.Ports.SerialPort $port, 9600`, `Open()`, small
  sleeps; a bare speak/read sometimes returns empty until the device has
  settled. pySerial with `timeout=3` + `reset_input_buffer()` is the most
  reliable.

## Version discipline

- Firmware build tag: `#define IMP_VERSION_STR "DIAG-N"` in
  `components/usb_importer/usb_importer.c`. Bump on every build that changes
  behaviour; `GET ver` reports it, so the device tells you what is flashed.
- Diagnostic probes live in `%TEMP%\opencode\` (arrange_probe.py and MIDI
  fixtures `test_sections.mid`, `test_nomarker.mid`).

## Import pipeline gotchas

- `arr` (whole-song arrange) requires >= 2 section markers (text/marker
  meta); else `ERR:need at least 2 section markers ...`.
- Drum channel is **GM: 0-based ch 9 (status 0x99)**, i.e. `ARR_DRUM_CH 9`,
  loop path tests `(ch % 16) == 9`. Older files/conventions used ch 10.
- `arr_fill_note` step must be relative to section start (`rel = tick - w0`),
  not absolute tick, or any section starting after tick 0 drops all its notes.
- `IMP_MAX_BODY` = 60*1024 in usb_importer.c bounds MIDI/AMYSONG uploads;
  tokimi.mid (~80 KB) is over that (raise the cap to test it on-device).