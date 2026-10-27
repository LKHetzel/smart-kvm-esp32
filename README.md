# TESmart HDK403-P23 ESP32 Controller

ESP32 firmware for controlling a TESmart HDK403-P23 (4 PCs × 3 monitors) KVM over the network. It will likely work with other TESmart models too (see [Compatibility](#compatibility)). The ESP32 connects to the KVM's UART service port. It provides a web page for everyday switching and an HTTP API for tools such as [Bitfocus Companion](https://bitfocus.io/companion).

Features:

- Switch any output to any of the four PCs, or switch all outputs at once
- Named PCs and up to 8 custom presets, set up from the web page
- Front-panel input changes are picked up and shown on the page
- Separate login for the web page and bearer token for the API
- Firmware updates over Wi-Fi after the first USB flash
- Settings export, import and factory reset
- Serial tools for working with the KVM protocol: raw hex sender, baud setting, live log

## Compatibility

This firmware was developed and tested on the HDK403-P23. It will likely work with other TESmart KVMs that have a UART service port, since they appear to share the same `AA BB …` command family. TESmart's manual for a 2-PC triple-monitor model documents the same switch commands. Check these differences before relying on it:

- **Baud rate:** the HDK403-P23 uses 9600, but the 2-PC model's manual says 38400. Change it on the Advanced page.
- **Frame ending:** the HDK403-P23 needs a checksum as the last byte. The manual shows a fixed `EE` instead. The firmware always sends the checksum. If your model only accepts `EE`, change the last byte in `sendKvm()` in `src/main.cpp`.
- **Number of PCs and outputs:** the page offers 4 PCs and 3 outputs. On a model with fewer, the extra buttons do nothing.
- **Front-panel reports:** these may differ or be missing, which would leave the page's routing display stale after a front-panel change.

To check a new model, use the Advanced page:

1. Try each baud rate and watch the serial log while you press a front-panel input button. Clean frames starting `AA BB` mean the rate is right.
2. Send a switch command with and without **Add checksum byte**, for example `AA BB 03 01 01`, and see whether the KVM switches.

The [protocol notes](#hdk403-p23-serial-protocol-as-observed) below show what to compare against.

If your model behaves differently, please [open an issue](https://github.com/LKHetzel/smart-kvm-esp32/issues). Include:

- the model number
- the baud rate that works
- whether commands need the checksum or `EE`
- the frames from the serial log when you press front-panel buttons

We'll add support for it as a build-time option: a separate PlatformIO environment in `platformio.ini`, selected with `pio run -e <model>`. That way each model gets the right messaging without code changes. Reports from models that work as-is are welcome too, so they can be listed here.

## Hardware

- An ESP32 dev board. The project targets the generic `esp32dev` (ESP32-WROOM-32) board.
- A 3.5 mm TRRS breakout or terminal adapter for the KVM's service port
- Three jumper wires, plus a USB cable for the first flash

### Wiring

The KVM's 3.5 mm TRRS service port carries 3.3 V TTL serial:

| TRRS contact | KVM signal | Connect to |
|---|---|---|
| Tip | VCC 3.3 V | **Leave disconnected** |
| Ring 1 | KVM TX | ESP32 GPIO16 (RX) |
| Ring 2 | KVM RX | ESP32 GPIO17 (TX) |
| Sleeve | GND | ESP32 GND |

Don't use RS-232 voltage levels, and don't connect the TRRS tip to the ESP32's 3.3 V rail. Power the ESP32 from its own USB supply.

To use different pins, change `KVM_TX_PIN` and `KVM_RX_PIN` in `src/main.cpp`.

## Building and flashing

This project is best used with [PlatformIO](https://platformio.org/). It relies on PlatformIO features: `board_build.embed_txtfiles` to build the web pages into the firmware, and the `include/` folder for secrets. It won't build as-is in the Arduino IDE.

You can use PlatformIO two ways:

- **VS Code:** install the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode), then open this folder. The extension installs the toolchain on first use.
- **Command line:** install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), for example with `pipx install platformio`, to get the `pio` command.

### 1. Configure

Clone the repository and copy the secrets template:

```bash
git clone https://github.com/LKHetzel/smart-kvm-esp32.git
cd smart-kvm-esp32
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h`. It's gitignored, so your credentials stay out of version control.

| Setting | Purpose |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | Your 2.4 GHz Wi-Fi network |
| `API_TOKEN` | Bearer token for `/api/*`. Generate one with `openssl rand -hex 16`. Leaving it blank or as `CHANGE_ME_TOKEN` **turns off API authentication**. |
| `UI_USERNAME`, `UI_PASSWORD` | Login for the web page. Change the placeholder password. Leaving it blank turns off the login. |

Optional settings at the top of `src/main.cpp`:

| Setting | Default | Purpose |
|---|---|---|
| `HOSTNAME` | `kvm-controller` | mDNS name (`kvm-controller.local`) and Wi-Fi hostname |
| `FW_VERSION` | | Shown on the web page. Bump it for each release. |
| `DEFAULT_KVM_BAUD` | `9600` | Used until a different rate is saved from the Advanced page |
| `KVM_TX_PIN`, `KVM_RX_PIN` | `17`, `16` | UART pins |

### 2. First flash (USB)

Connect the ESP32 over USB, then:

```bash
pio run -t upload      # build and flash
pio device monitor     # watch the serial log (Ctrl+C to exit)
```

In VS Code, use the **Upload** (→) and **Serial Monitor** (plug icon) buttons on the PlatformIO status bar.

The serial log shows the IP address once Wi-Fi connects:

```text
WiFi connected: 192.168.1.50
mDNS: http://kvm-controller.local/
TESmart ESP32 controller v0.7.0 (...) ready, KVM serial 9600 baud
```

If Wi-Fi can't connect within 30 seconds, the ESP32 restarts and tries again.

On Linux, if the upload can't open the serial port, add yourself to the serial group and log in again. That's `dialout` on Debian/Ubuntu and `uucp` on Arch.

### 3. Later updates (over Wi-Fi)

Build without flashing:

```bash
pio run
```

Then open **Advanced → Firmware update** on the web page and upload `.pio/build/esp32dev/firmware.bin`. The controller installs it and restarts. The page reloads when it's back. Names, presets and other settings are kept.

## Using it

### Web page

Open `http://kvm-controller.local/`, or use the IP address if your network doesn't resolve `.local` names. The browser asks for the web login once and remembers it.

The main page has:

- **Presets:** one button per saved preset. The button matching the current routing is highlighted.
- **All → PC:** switch every output to one PC.
- **Outputs:** switch each of the three outputs on its own. The current PC is highlighted.

The routing display refreshes every 10 seconds.

### Advanced page

Open it with the **Advanced** button at the bottom of the main page.

- **PC names:** rename PC1–PC4. The names appear on every button.
- **Presets:** add, edit and delete up to 8 presets. Each sets every output to a PC, or leaves it unchanged.
- **Backup & reset:**
  - **Export:** downloads the PC names, presets and baud rate as a JSON file.
  - **Import:** restores such a file.
  - **Factory reset:** erases all saved settings and restarts.

  None of these touch the Wi-Fi details, API token or web login, because those are built into the firmware.
- **KVM serial:** change the baud rate, send raw hex (with the checksum added automatically, if you choose), and run the status query.
- **Serial log:** the last 40 frames sent and received, updated every second.
- **Firmware update:** see [Later updates](#3-later-updates-over-wi-fi).

A new controller starts with no presets and the names PC1–PC4.

## API

External tools use `/api/*` with the bearer token:

```text
Authorization: Bearer YOUR_TOKEN
```

| Endpoint | Action |
|---|---|
| `GET /health` | Liveness check, no auth |
| `GET /api/status` | Current state, as shown below |
| `POST /api/preset/<n>` | Apply preset `n`, numbered from 1 in the order shown on the Advanced page |
| `POST /api/all/pc<1-4>` | Switch every output to one PC |
| `POST /api/output/<1-3>/pc/<1-4>` | Switch one output |
| `POST /api/query` | Send the manual's status query and return any raw reply as hex. The HDK403-P23 hasn't answered it. |

Responses:

```json
{"ok":true,"action":"preset-1","state":{
  "version":"0.7.0","build":"...","uptime":1234,"hostname":"kvm-controller","ip":"192.168.1.50","rssi":-60,
  "pcs":["PC1","PC2","PC3","PC4"],
  "presets":[{"name":"My preset","outputs":[1,1,2]}],
  "outputs":{"1":1,"2":1,"3":2}}}
```

`/api/status` returns `{"ok":true,"state":{...}}`. In `outputs`, `0` means unknown. Otherwise the value is the PC number. In a preset's `outputs`, `0` means "leave unchanged". Errors return `{"ok":false,"error":"..."}` with a 4xx or 5xx status.

Deleting a preset renumbers the presets after it, so update any Companion buttons that point at those numbers.

### Companion

Use the Generic HTTP module's POST action, with the `Authorization` header shown above:

- `http://kvm-controller.local/api/preset/1`
- `http://kvm-controller.local/api/all/pc2`
- `http://kvm-controller.local/api/output/3/pc/1`

### Web page endpoints

The built-in pages use `/ui/*` with the web login instead of the token. Every `/api` endpoint above also exists under `/ui`. These exist only under `/ui`:

| Endpoint | Form fields |
|---|---|
| `POST /ui/config/pcs` | `pc1`–`pc4`. A blank name resets to the default. |
| `POST /ui/config/preset/save` | `index` (0 for new), `name`, `o1`–`o3` (0–4) |
| `POST /ui/config/preset/delete` | `index` |
| `GET /ui/config/export` | Downloads the settings JSON |
| `POST /ui/config/import` | `baud`, `pc1`–`pc4`, `presets` (the count), then `p<n>name` and `p<n>o1`–`p<n>o3` for each preset. All fields are validated before anything is saved. |
| `POST /ui/config/reset` | None. Factory reset, then restart. |
| `POST /ui/serial/send` | `hex` (up to 64 bytes) and `checksum=1` to append the checksum. Returns any reply received within 120 ms. |
| `POST /ui/serial/baud` | `baud`: 9600, 19200, 38400, 57600 or 115200 |
| `GET /ui/serial/log?since=<id>` | None. Returns log entries newer than `id`. |
| `POST /ui/update` | Multipart firmware upload |

`POST /ui/*` requests must include an `X-Requested-With` header. Browsers won't add that header to a request from another site, so other websites can't use your saved login.

## Security

- Traffic is plain HTTP, so the token and web login can be read by anyone watching the network. Keep the controller on a trusted LAN or VLAN, and don't expose it to the internet.
- The API token only allows switching, status and the status query. Configuration, serial tools and firmware updates need the web login.

## Routing state

The controller doesn't read the routing back from the KVM. The status query never gets an answer. Instead it tracks state from two sources:

- **Its own commands:** every switch it sends updates the state.
- **Front-panel reports:** the KVM reports input button presses, and the controller picks them up.

Changes made with the IR remote or keyboard hotkeys haven't been tested, and may not be reported. Until the next switch, the page may show stale routing after such a change. State is saved in flash, so it survives restarts. A factory reset clears it.

## HDK403-P23 serial protocol (as observed)

TESmart's published UART table for its 2-PC model doesn't match the HDK403-P23. Here's what has been confirmed on this unit:

- 9600 baud, 8N1
- Frames are `AA BB <type> <arg1> <arg2> <checksum>`. The checksum is the low byte of the sum of the first five bytes. Frames ending in `EE`, as the manual shows, are ignored.
- **Switching:** `AA BB 03 <output> <pc> <sum>`. `output` is 1–3, or `00` for all outputs. `pc` counts from 0, so `00` is PC1. Confirmed for each output with PC1 and PC2, and for all outputs with all four PCs. A single output to PC3 or PC4 hasn't been tested yet.
- **Front-panel report:** the KVM sends `AA BB 82 01 <pc> <sum>` when an input button is pressed. Confirmed for all four inputs. It sometimes sends the same frame in response to a switch command, so the firmware ignores reports for 1 second after sending a switch.
- **Not working:** the status query (`06 02 03`) never gets a reply. The buzzer toggle (`04 02 xx`) does nothing, and changing the buzzer setting in the KVM's menu sends nothing over serial.

## Project layout

```text
platformio.ini            Board, framework and embedded web files
include/secrets.example.h Template for include/secrets.h (gitignored)
src/main.cpp              Firmware
web/index.html            Main page
web/advanced.html         Advanced page
web/app.css, web/app.js   Shared styles and helpers
```

The files in `web/` are embedded into the firmware at build time, so changing a page only needs a rebuild and upload.
