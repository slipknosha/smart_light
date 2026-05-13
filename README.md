# Smart Light ESP32-S3 Dimmer

Firmware for an ESP32-S3 zero-cross AC phase dimmer.

## Hardware pins

- `GPIO3` - `LTV_OUT`, zero-cross detector input.
- `GPIO8` - `MOC`, MOC3022 trigger transistor drive.

## Behavior

The firmware waits for each zero-cross edge, schedules a phase-delay alarm with a 1 MHz GPTimer, and sends a short gate pulse to the optotriac. Brightness ramps smoothly from `0%` to `100%`, then back to `0%`, forever.

The default timing is tuned for 50 Hz mains and auto-filters measured half-cycle time in the `7-12 ms` range, so 60 Hz should also track.

## Build and flash

```sh
. /home/vlados/esp/esp-idf/export.sh
idf.py build
idf.py -p PORT flash monitor
```

Watch the log for `brightness`, measured `half_cycle`, and zero-cross counter `zc`.
