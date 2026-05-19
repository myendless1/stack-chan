# StackChan Counter Demo

This is a minimal ESP-IDF firmware demo for StackChan/CoreS3.

Behavior:

- On boot, the screen shows `0`.
- Each screen tap increments the number by 1.

## Requirements

- ESP-IDF 5.x
- Internet access during the first build so ESP-IDF Component Manager can fetch `m5stack/m5unified`
- StackChan connected by USB

## Build And Flash

From this `demo` directory:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

If your serial port is different, replace `/dev/ttyACM0`, for example:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Notes

Flashing this demo replaces the currently installed StackChan firmware. Keep a copy of the original firmware if you need to restore it later.
