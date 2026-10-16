# AGENTS.md

Notes for coding agents: the bench, the recipes, and the constraints
that are not visible in the code.

## Bench

- Board on `/dev/cu.usbmodem5CBC3977031` (COM port, CH9102): DevKitC-1,
  16 MB flash, 8 MB PSRAM, neither used beyond 4 MB and 0. The sensor is
  on the native USB port; the OTG jumper is soldered, so that port
  supplies 5 V. The ESP cannot switch that 5 V off.
- The board and this machine share a LAN with internet. Device:
  `airmonitor-dd94.local`, currently `192.168.1.70`. MQTT broker:
  `192.168.1.102:1883`. Never change the Mac's network settings.
- `~/esp32s3-dump/firmware-16mb.bin` is the board's factory firmware.
  Do not delete it. Restore with `esptool write-flash 0x0 <file>`.
- ESP-IDF v5.5.5 in `~/esp/esp-idf`. `esptool` v5 via pipx (dash-style
  commands: `write-flash`, `read-flash`, `erase-region`).

## Recipes

Build:

```bash
source ~/esp/esp-idf/export.sh
IDF_SKIP_CHECK_SUBMODULES=1 idf.py build
```

`IDF_SKIP_CHECK_SUBMODULES=1` skips a git check that needs the network.
After adding or removing a component: `idf.py reconfigure`.

Flash the app only:

```bash
esptool --chip esp32s3 -p /dev/cu.usbmodem5CBC3977031 -b 115200 \
  --before default_reset --after hard_reset --connect-attempts 15 \
  write-flash 0x10000 build/esp32_zytemp.bin
```

115200 only; 460800 and 921600 fail with "Invalid head of packet".
Settings (`nvs`, 0x9000, 0x6000) and history (0x310000, 0x10000)
survive an app flash. Before a run that overwrites them, `read-flash`
both and write them back afterwards. `erase-region 0x9000 0x6000`
gives the factory state.

Console capture (`esp_idf_monitor` needs a TTY and resets the target on
attach; `--no-reset` to watch a running device; `cat`/`stty` on the
port hold the chip in reset):

```bash
(script -q /dev/null python -m esp_idf_monitor \
    -p /dev/cu.usbmodem5CBC3977031 -b 115200 build/esp32_zytemp.elf \
    > "$TMPDIR/mon.log" 2>&1 &)
perl -e 'sleep 25'; pkill -f esp_idf_monitor
LC_ALL=C sed 's/\x1b\[[0-9;]*m//g' "$TMPDIR/mon.log" | LC_ALL=C grep zytemp
```

`LC_ALL=C` is needed: the log has bytes that make `sed` and `grep` fail
with "illegal byte sequence". If `script` fails with "Operation not
supported on socket", run it again in a new shell call.

Live pages: headless Chrome from the sandbox cannot reach the LAN
(`ERR_ADDRESS_UNREACHABLE`), `curl` can. Save the API answers with
`curl` and open a copy of the page with `window.fetch` redirected to
those files.

Self-test build (its own build directory, because the CMake cache keeps
`-D` flags):

```bash
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build-mqtt \
  -DSDKCONFIG=build-mqtt/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.selftest" \
  -DAPP_SELFTEST_NTP=1 -DAPP_SELFTEST_MQTT=1 build
```

Log lines `SELFTEST PASS/FAIL <name>`, summary `SELFTEST: N passed, M
failed`. Phases that save settings reboot the device, so a run spans
several boots; capture at least 170 s. Defines are listed in
`main/CMakeLists.txt`.

Host tests: `cd test/host && make test` and `node test/ui/ui_smoke.js`.

Release: push a tag `v*`. `.github/workflows/release.yml` builds with
`espressif/esp-idf-ci-action`, merges bootloader, partition table and
app into `airmonitor.bin` (offset 0, DIO, 4 MB), attaches both binaries
to the GitHub Release and deploys `flasher/` plus the binaries to GitHub
Pages. Pages must be set to "GitHub Actions" once in the repo settings.
`flasher/manifest.json` is the ESP Web Tools manifest; `__VERSION__` is
replaced by the tag. Local equivalent of the merge: `esptool merge-bin`
with the same offsets.

## Constraints

- The image is built for 4 MB flash, DIO, no PSRAM, so one binary runs
  on any ESP32-S3 module. The partition table ends at 0x320000. Do not
  reintroduce `CONFIG_SPIRAM`; nothing needs it.
- `CONFIG_USB_HOST_RESET_RECOVERY_MS=300` in `sdkconfig.defaults` is
  required. With Wi-Fi active the default reset is too fast and the
  sensor never enumerates.
- Blocking HID control transfers only from the `zytemp` session task.
  Their completion event comes from the `hid-host` task; from a
  `hid-host` callback they wait on themselves for 5 s.
- `attach()` must read the report descriptor. The sensor sends nothing
  until it has been read. `SET_IDLE` STALLs; the driver returns `ESP_OK`
  for rejected control requests.
- The sensor reports wrong CO₂ for ~25 s after power-up with valid
  checksums: 4000 ppm, then 70–90 ppm. Hence `co2_warmup_ms`.
- Only `wifi_mgr` calls `esp_wifi_*`. esp_wifi event handlers run on
  `sys_evt` with a 2304-byte stack: they set flags and post messages;
  AP start/stop, `esp_wifi_connect()` and mode changes run on `wifi-mgr`.
- `settings_load()` never fails; do not wrap it in `ESP_ERROR_CHECK`.
- `reset_early_init()` runs before anything opens NVS.
- MQTT: `esp_mqtt_client_publish/subscribe` only from `mqtt_pub`.
- `fresh.h` is the only freshness rule.
- The API has no authentication and CORS `*` by decision. Secrets are
  reported only as `*_set` booleans.
- `managed_components/` is downloaded; do not edit it.
- Pages are self-contained HTML, no CDN: the captive portal has no
  internet. Style: flat, one accent `#FF6F3F`, hairlines, uppercase
  section labels, no cards, no explanatory copy.
- `usb_host_hid` can leak the device if the sensor is unplugged during
  a control transfer in `attach()` (~10 ms); only a reboot recovers.
  No USB hubs. `zytemp_start()` does not roll back a partial init.
- SPDX header in every source file, C11, 4 spaces, `snake_case`.
  Comments only for what the code does not say. Commits in English,
  body says why.
