# Air Monitor

ESP32-S3 firmware that reads a Holtek USB-zyTemp CO₂ and temperature
sensor over USB and serves the readings as a web page, a JSON API and
MQTT with Home Assistant discovery.

## Hardware

- Any ESP32-S3 board with at least 4 MB of flash; PSRAM is not needed.
  The sensor plugs into the native USB port, which must supply 5 V (on
  the DevKitC-1 close the USB-OTG solder jumper). The second USB-C port
  powers the board and carries the console.
- Holtek USB-zyTemp, USB `04d9:a052`. Found inside the TFA AirCO2ntrol
  Mini and similar desktop meters.

## Install a ready-made firmware

Open <https://grakovne.github.io/esp_32_holtek-usb-zy-temp-driver/> in
Chrome or Edge, connect the board's COM port, press Install. The page
also has the image and the command for `esptool`. Every tagged release
publishes `airmonitor.bin` (complete image for offset 0) and
`esp32_zytemp.bin` (app only, offset 0x10000) on the Releases page.

## Build from source

ESP-IDF v5.5.x with the `esp32s3` target.

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX -b 115200 flash monitor
```

One image for the web flasher:

```bash
esptool --chip esp32s3 merge-bin -o build/airmonitor.bin \
  --flash-mode dio --flash-size 4MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp32_zytemp.bin
```

The first build downloads `usb_host_hid` and `mdns` from the component
registry. Flash at 115200: boards with a CH9102 USB-UART bridge fail at
higher rates with "Invalid head of packet".

Tests:

```bash
cd test/host && make test        # appconf validation, history ring
node test/ui/ui_smoke.js         # page JS against mocked API answers
```

## Use

With no Wi-Fi saved the device opens an access point `AirMonitor-XXXX`
(the last four hex digits of its MAC). Join it from a phone; the setup
page opens by itself on most phones, otherwise open
`http://192.168.4.1/`. Pick your network, enter the password, save. The
device restarts and answers at `http://airmonitor-xxxx.local/`, or at
the IP your router gave it.

Pages: Dashboard (current CO₂ and temperature with zones, two 24-hour
charts), Settings (Wi-Fi, MQTT, device name, NTP server, restart,
factory reset), Device Info.

Zones: CO₂ green below 1000 ppm, yellow to 1400, red above (GOST
30494-2011). Temperature green at 20–24 °C, yellow at 18–20 and 24–26,
blue below 18, red above 26.

### API

JSON over HTTP, no authentication, `Access-Control-Allow-Origin: *`.

| Method | Path | |
| --- | --- | --- |
| GET | `/api/v1/current` | current readings, `null` when older than 30 s |
| GET | `/api/v1/history` | 144 ten-minute averages, oldest first |
| GET | `/api/v1/status` | sensor, Wi-Fi, MQTT, time, chip, firmware |
| GET | `/api/v1/config` | settings; passwords as `*_set` flags only |
| POST | `/api/v1/config` | save and restart; omitted fields keep their value |
| GET | `/api/v1/wifi/scan` | cached scan; `?refresh=1` starts a new one |
| POST | `/api/v1/reboot` | restart |
| POST | `/api/v1/factory-reset` | erase settings and restart |

```json
GET /api/v1/current
{
  "timestamp": 1760620800, "time_synced": true, "uptime_s": 5231,
  "sensor": { "connected": true, "warming_up": false },
  "co2": { "ppm": 612, "age_s": 2 },
  "temperature": { "celsius": 23.98, "age_s": 1 }
}
```

### MQTT

Plain numbers to `airmonitor/<id>/co2` and `airmonitor/<id>/temperature`
every 30 s by default (5 to 3600 s). `airmonitor/<id>/availability`
carries `online`/`offline` with a last will. With Discovery on, Home
Assistant adds one device with two sensors and gets the configs again
after it restarts. TLS with your own CA certificate or the built-in
bundle; "skip certificate verification" disables the check entirely.

### Factory reset

The red button on the Settings page, or hold the board's BOOT button
for 5 s. Settings are erased, history is kept, the device comes back as
the setup access point.

## Behavior worth knowing

- CO₂ is held back for 30 s after the sensor connects: the sensor
  reports wrong values for about 25 s after power-up.
- History and MQTT readings start after the clock has synced over NTP.
- Every settings change restarts the device.
- One sensor, no USB hubs. Resetting the board does not reset the
  sensor: the board cannot cut its power.
- No HTTPS, no login, no OTA; updates go over USB.

## License

MIT, see `LICENSE`.
