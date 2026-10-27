# TESmart HDK403-P23 ESP32 Controller

Small ESP32 network-to-UART controller for a TESmart HDK403-P23 matrix KVM.

## Wiring

TESmart 3.5 mm TRRS service/UART port:

- Tip: VCC 3.3 V — leave disconnected
- Ring 1: KVM_TX — connect to ESP32 GPIO16 (RX)
- Ring 2: KVM_RX — connect to ESP32 GPIO17 (TX)
- Sleeve: GND — connect to ESP32 GND

UART: 3.3 V TTL, 9600 baud, 8N1 (confirmed on the HDK403-P23; the 2-PC model's manual says 38400).

Do **not** use RS-232 voltage levels and do not connect the TRRS 3.3 V tip to the ESP32 3.3 V rail.

## HDK403-P23 serial protocol (as observed)

The TESmart manual for the 2-PC model doesn't match this unit. Here's what has been confirmed on the HDK403-P23:

- 9600 baud, 8N1
- Frames are `AA BB <type> <arg1> <arg2> <checksum>`. The checksum is the low byte of the sum of the first five bytes. Frames that end in `EE`, as the manual shows, are ignored.
- `AA BB 03 <output> <pc> <sum>` switches outputs. `output` is 1–3, or `00` for all outputs, and `pc` counts from 0, so `00` is PC1. Confirmed for each output with PC1 and PC2, and for all outputs with all four PCs. Switching a single output to PC3 or PC4 hasn't been tested yet.
- The KVM sends `AA BB 82 01 <pc> <sum>` when an input button on the front panel is pressed. `pc` counts from 0, and all four inputs are confirmed. The controller uses it to update its state, and the web page shows the change at its next refresh, within 10 seconds. The KVM sometimes sends the same report in response to the controller's own commands, so reports arriving within 1 second of a switch command are ignored.
- The status query (`06 02 03`) has never received a reply.
- The buzzer toggle from the manual (`04 02 xx`) does nothing on this model, so the firmware doesn't offer it.

## PC names and presets

You can rename PC1–PC4 and build up to 8 presets on the Advanced page. Each preset sets each output to one of the four PCs, or leaves it Unchanged. Both are saved on the controller and survive restarts and firmware updates. A new controller starts with no presets and the names `PC1`–`PC4`.

## Build

Copy the secrets template and fill in your Wi-Fi settings, API token and web login (`include/secrets.h` is gitignored):

```bash
cp include/secrets.example.h include/secrets.h
pio run -t upload
pio device monitor
```

Default hostname: `kvm-controller.local` (set `HOSTNAME` in `src/main.cpp`)

If Wi-Fi can't connect within 30 seconds of boot, the ESP32 restarts and tries again.

The first flash must be over USB. After that you can upload `.pio/build/esp32dev/firmware.bin` from the Advanced page. Bump `FW_VERSION` in `src/main.cpp` for each release, so the page shows which build is running.

Project layout:

- `src/main.cpp`: firmware
- `web/`: the built-in pages (`index.html`, `advanced.html`, shared `app.css` and `app.js`). They're embedded into the firmware through `board_build.embed_txtfiles` in `platformio.ini`, so a normal build or firmware upload includes them.
- `include/secrets.h`: credentials (gitignored)

## Web interface

Open `http://kvm-controller.local/`. The browser asks for the web login (`UI_USERNAME` / `UI_PASSWORD` in `secrets.h`) once and remembers it. The page doesn't use the API token. It calls the `/ui/*` endpoints, which take the same login. Leave `UI_PASSWORD` blank to turn the login off.

`/advanced` has tools for bring-up and maintenance:

- **KVM serial:** set the baud rate (applied immediately and saved on the controller), send raw hex such as `AA BB 03 01 00 EE`, and run the status query.
- **Serial log:** the last 40 transmit/receive entries, including any bytes the KVM sends by itself. It updates every second.
- **Firmware update:** upload a new `firmware.bin` over Wi-Fi.
- **Backup & reset:**
  - **Export:** downloads a settings file with the PC names, presets and baud rate.
  - **Import:** loads such a file and replaces all three.
  - **Factory reset:** erases every saved setting and restarts.

  None of these touch the Wi-Fi details, API token or web login, which are compiled into the firmware.

`POST /ui/*` requests must also send an `X-Requested-With` header. Browsers won't add that header to a request from another site, so other websites can't use your saved login to switch the KVM.

## API

External clients (Companion etc.) use `/api/*`. If `API_TOKEN` is configured, send:

```text
Authorization: Bearer YOUR_TOKEN
```

Every endpoint below also exists under `/ui/...` for the built-in page. Those use the web login instead of the token.

Endpoints:

- `GET /health` (no auth)
- `GET /api/status`
- `POST /api/preset/<n>`: apply preset number `n`, counting from 1 in the order shown on the Advanced page
- `POST /api/all/pc<1-4>`: switch every output to one PC
- `POST /api/output/<1-3>/pc/<1-4>`: switch one output
- `POST /api/query`

Responses look like `{"ok":true,"action":"preset-1","state":{"version":...,"build":...,"uptime":<seconds>,"hostname":...,"ip":...,"rssi":...,"pcs":["PC1","PC2"],"presets":[{"name":"My preset","outputs":[1,1,2]},...],"outputs":{"1":1,"2":1,"3":2}}}`; `/api/status` returns `{"ok":true,"state":{...}}`. Output values: 0 unknown, otherwise the PC number (1–4).

Traffic is plain HTTP, so the token and the web login are visible on the network. Keep the device on a trusted LAN.

These exist only under `/ui/*`, with the web login, and aren't exposed to `/api` clients:

- `POST /ui/config/pcs` with form fields `pc1`–`pc4`. A blank name resets to the default (`PC1` and so on).
- `POST /ui/config/preset/save` with form fields `index` (0 for a new preset), `name`, `o1`, `o2`, `o3`. Each output is 0 for unchanged, or a PC number from 1 to 4.
- `POST /ui/config/preset/delete` with form field `index`
- `GET /ui/config/export`: settings as a JSON file download
- `POST /ui/config/import` with form fields `baud`, `pc1`–`pc4`, `presets` (the count), then `p<n>name` and `p<n>o1`–`p<n>o3` for each preset. The Advanced page converts an exported file into these fields. Everything is validated before anything is saved.
- `POST /ui/config/reset`: factory reset. It erases the names, presets, baud rate and routing state, then restarts.

- `POST /ui/serial/send` with form field `hex=AA BB ...` (up to 64 bytes). Returns the bytes sent and any reply received within 120 ms.
- `POST /ui/serial/baud` with form field `baud=` set to 9600, 19200, 38400, 57600 or 115200
- `GET /ui/serial/log?since=<id>`: log entries newer than `id`
- `POST /ui/update`: multipart firmware upload. The controller restarts once the update is installed.

The `/api/query` endpoint sends the TESmart manual's connection-status query and returns the raw response bytes as hex. The HDK403-P23 hasn't replied to it so far.

## Companion

Use Generic HTTP actions:

- First preset: `POST http://kvm-controller.local/api/preset/1`
- Second preset: `POST http://kvm-controller.local/api/preset/2`

Use the same Bearer token header if authentication is enabled.

## State

The firmware remembers the last routes it commanded in ESP32 NVS. This is controller-side state, not authoritative KVM readback. Front-panel input buttons send a report that the controller picks up. Changes made from the remote or keyboard hotkeys haven't been tested, and may leave `/api/status` stale.
