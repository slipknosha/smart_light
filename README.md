# Smart Light Firmware

ESP32-S3 firmware for a BLE-controlled, zero-cross synchronized AC phase dimmer.

> [!WARNING]
> This firmware is intended for a PCB that switches **220-230 VAC mains**.
> Bring-up and testing must be done with proper isolation, enclosure, fusing, and mains-safety practice.

## Related Repositories

This project is split into two repositories:

- **Firmware**: <https://github.com/slipknosha/smart_light/tree/develop>
- **PCB / KiCad hardware**: <https://github.com/slipknosha/smart_light_pcb/tree/develop>

This repository contains only the ESP-IDF application. The schematic, PCB layout, BOM, and manufacturing files live in the PCB repository.

## Hardware Target

The firmware targets the custom ESP32-S3 dimmer PCB from the hardware repository.

Important schematic nets:

| Function | Schematic net | ESP32-S3 GPIO | Notes |
|---|---:|---:|---|
| Zero-cross detector input | `LTV_OUT` | `GPIO1` | From LTV-814 optocoupler |
| Triac trigger output | `MOC` | `GPIO2` | Drives BC847B + MOC3052M |
| Boot button | `ESP_IO0` | `GPIO0` | ESP32 bootloader mode |
| Reset / enable | `ESP_EN` | `EN` | Reset circuit |
| USB native D- | `ESP_D-` | `USB_D-` / `GPIO19` | USB-C flashing |
| USB native D+ | `ESP_D+` | `USB_D+` / `GPIO20` | USB-C flashing |

Power-stage summary:

- AC input: `J1` / `220V`
- Load output: `J2` / `Load`
- Isolated 5 V supply: Mean Well `IRM-05-5`
- 3.3 V regulator: `LM1117-3.3`
- Zero-cross detector: `LTV-814`
- Optotriac driver: `MOC3052M`
- Power triac: `BTB16-600B`

## Firmware Features

- ESP-IDF project for ESP32-S3.
- BLE peripheral named `SmartLight`.
- Just Works BLE pairing/bonding.
- Encrypted GATT read/write for the dimmer characteristic.
- Zero-cross interrupt on `GPIO1`.
- Triac gate pulse generation on `GPIO2` using GPTimer.
- Linear, discrete dimming level range: `0..100000`.
- No gamma curve, lookup table, gradient, or perceptual remapping.
- Last dimming level is saved to NVS after 10 seconds without a new BLE write.
- Saved level is restored after power cycling.

## Source Layout

```text
main/
├── main.c           # app_main, NVS persistence, module wiring
├── ble_service.c    # NimBLE GATT server, pairing, BLE protocol
├── ble_service.h
├── dimmer.c         # zero-cross ISR, GPTimer phase delay, triac gate pulse
├── dimmer.h
└── CMakeLists.txt
```

## BLE API

Device name:

```text
SmartLight
```

Custom GATT service:

```text
7b5e0001-4c3f-4b3d-9a57-8d47b4ef0001
```

Level characteristic:

```text
7b5e0002-4c3f-4b3d-9a57-8d47b4ef0001
```

Characteristic properties:

- read
- write
- write without response
- notify
- encrypted access required

### Value Format

The level is a **binary unsigned integer**, not ASCII text.

Allowed range:

```text
0..100000
```

Byte order:

```text
little-endian
```

Recommended write format is 4 bytes, `uint32_t` little-endian.
The firmware also accepts shorter 1-3 byte little-endian writes when the value fits.

Examples for nRF Connect hex writes:

```text
0      -> 00 00 00 00
1      -> 01 00 00 00
1000   -> E8 03 00 00
10000  -> 10 27 00 00
50000  -> 50 C3 00 00
100000 -> A0 86 01 00
```

Read returns 4 bytes little-endian.

### Android Example

```kotlin
val level = 100000

val payload = ByteBuffer
    .allocate(4)
    .order(ByteOrder.LITTLE_ENDIAN)
    .putInt(level)
    .array()

characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
characteristic.value = payload
gatt.writeCharacteristic(characteristic)
```

Do not write `"100000"` as a string. That would send ASCII bytes, not the binary integer expected by the firmware.

## Dimming Behavior

At each accepted zero-cross event:

1. The firmware reads the current level.
2. `0` disables gate pulses completely.
3. `100000` fires near the beginning of the half-cycle.
4. Intermediate values are mapped linearly to the delay window.
5. A short gate pulse is sent to the MOC driver.

The timer resolution is 1 MHz. The firmware measures half-cycle timing and filters valid values in the `7-12 ms` range, so it can track 50 Hz and 60 Hz mains.

## Persistence

The level is saved in ESP32 NVS:

```text
namespace: light
key:       level100k
type:      u32
```

Writes are delayed by 10 seconds after the last BLE update to avoid unnecessary flash wear while a slider is moving.

To clear the saved level, erase flash/NVS:

```sh
idf.py erase-flash
```

## Build and Flash

Activate ESP-IDF:

```sh
. /home/vlados/esp/esp-idf/export.sh
```

Build:

```sh
idf.py build
```

Flash and monitor over native USB:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

Useful monitor lines:

- `BLE advertising as SmartLight`
- `BLE connection encrypted`
- `BLE level set to .../100000`
- `Saved level .../100000 to flash`
- `No accepted zero-cross edges on GPIO1...`

## Bring-Up Notes

1. First test the board from USB only.
2. Verify the 3.3 V rail and ESP32-S3 USB flashing.
3. Confirm `SmartLight` appears in nRF Connect.
4. Pair/bond with the device.
5. Write low values first, then increase gradually.
6. Only test the AC stage with safe insulation and a suitable resistive load.

## License

See the project repository for license details.
