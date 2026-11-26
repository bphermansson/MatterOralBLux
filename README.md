# MatterOralBLux

One ESP32-C6 firmware that combines a VEML7700 lux meter with an Oral-B iO
bluetooth monitor:

- **Lux** is exposed as a Matter Light Sensor and shows up in Google Home.
- **Brushing** (and brush battery) is sent to Telegram.
- No OTA; the app uses a single 3 MB factory partition.

## What it exposes

| Where | Data |
| ----- | ---- |
| Matter endpoint 1 (Light Sensor) | `IlluminanceMeasurement.MeasuredValue` |
| Telegram | Brush state changes (`running`, `idle`, `charging`, ...); idle messages include battery percentage when available |
| Telegram | Battery percentage when it changes |

The lux reading is unchanged from
[MatterLuxMeter](https://github.com/patriknoren/MatterLuxMeter): the raw lux
value is converted to Matter's logarithmic measured value.

## Hardware

- ESP32-C6, 4 MB flash
- VEML7700 light sensor on I2C
- I2C SDA/SCL pins are configured in
  `idf.py menuconfig` → `Device Configuration` → `I2C SDA Pin` / `I2C SCL Pin`
- Optional: the brush's BLE address can be fixed in `menuconfig` → `Oral-B` →
  `Brush BLE address filter`. Leave it empty to accept the first device that
  advertises a valid Oral-B iO payload.

## Network and BLE ownership

Matter owns both the Wi-Fi station and the NimBLE host:

- Wi-Fi credentials are provisioned from Google Home during Matter
  commissioning and stored in NVS. There are no hard-coded Wi-Fi settings, and
  the firmware never calls `esp_wifi_init()` itself.
- Telegram messages are queued and wait until the Matter-managed station has
  an IP address. If the device has never been commissioned, Telegram will stay
  quiet until it is added to Google Home.
- Matter also starts NimBLE for CHIPoBLE commissioning. `oralb_ble_init()`
  attaches to that running host instead of initializing NimBLE a second time,
  then runs a passive Oral-B scan in the background.
- `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING` must stay **disabled**. If enabled,
  Matter shuts BLE down after commissioning and the brush would stop being
  monitored.
- The scan uses a 100 ms interval with a 50 ms window (50 % duty cycle)
  instead of the controller default 100 %, so Wi-Fi/Matter and BLE do not
  fight over the 2.4 GHz radio.
- While the brush is idle/charging, the firmware briefly connects once per
  `APP_POLL_*_MINUTES` and reads FF02/FF04/FF05 for battery data.

## Telegram setup

Set the bot token and chat id in `idf.py menuconfig` → `Telegram`:

- `ESP_TELEGRAM_TOKEN` – bot token from BotFather
- `ESP_TELEGRAM_CHAT_ID` – numeric chat/group id (groups start with `-`)

Messages are sent over HTTPS using the embedded `telegram_certificate.pem`
(GoDaddy root CA).

## Build

ESP-IDF 6.0.2 and esp-matter 1.6.1 are assumed:

```sh
. /path/to/esp-idf/export.sh
export ESP_MATTER_PATH=/path/to/esp-matter
. "$ESP_MATTER_PATH/export.sh"
idf.py build
```

The factory app partition is 3 MB (`partitions.csv`). The `nvs` and `fctry`
offsets match the original MatterLuxMeter layout, so existing commissioning
data can survive an app-only reflash.

## Flash

Erase and flash the firmware:

```sh
idf.py erase-flash -p /dev/ttyACM0
idf.py flash -p /dev/ttyACM0
```

Then flash one of the generated factory partitions at `0x1E000`:

```sh
esptool.py -p /dev/ttyACM0 write_flash 0x1E000 \
  out/mfg/device1/fff2_8001/<uuid>/<uuid>-partition.bin
```

Each device directory contains its own QR code and onboarding codes:

- `device1`: discriminator 3841, passcode 20202026
- `device2`: discriminator 3842, passcode 20202026

If the device was already commissioned, flashing only the app (without
`erase-flash`) keeps the fabric/NVS data, including the Wi-Fi credentials used
by Telegram.

Watch the log with:

```sh
idf.py monitor -p /dev/ttyACM0
```

Expected messages include `attaching to the existing NimBLE host`,
`scanning for Oral-B advertisements`, `sent: Brush: running...`, and
`Lux: ... -> Matter: ...`.

## Kconfig

`Oral-B` menu:

- `APP_BRUSH_ADDR` – optional BLE address filter
- `APP_POLL_CHARGING_MINUTES` – battery poll interval while charging (default 10)
- `APP_POLL_IDLE_MINUTES` – battery poll interval while idle (default 60)

`Telegram` menu:

- `ESP_TELEGRAM_TOKEN`
- `ESP_TELEGRAM_CHAT_ID`

`Device Configuration` keeps the original lux/I2C options.

## Notes

- `main/CMakeLists.txt` intentionally has no `REQUIRES`/`PRIV_REQUIRES`.
  ESP-IDF then lets `main` see every component; adding requirements would
  hide the esp-matter/CHIP headers.
- `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` is reduced to 4; the device
  creates one dynamic endpoint (the light sensor).
- The `main` component embeds `telegram_certificate.pem` with
  `EMBED_TXTFILES`; the symbol names are
  `_binary_telegram_certificate_pem_start/end`.
