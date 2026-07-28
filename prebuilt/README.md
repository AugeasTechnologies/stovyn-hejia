# Prebuilt images

Ready-to-flash firmware for the Stovyn bring-up test. Pick the variant that matches which
parts are populated on the board.

| File | Use when | Flash how |
|---|---|---|
| `hejia_hwtest-piezo-merged.bin` | board **without** the I2S speaker/mic (piezo only) | first flash, whole image at `0x0` |
| `hejia_hwtest-speaker-mic-merged.bin` | board **with** the I2S amp + speaker + mic populated | first flash, whole image at `0x0` |
| `hejia_hwtest-piezo.bin` | same as piezo, **app only** | Bluetooth OTA (`ble_ota.py` / bench app) |
| `hejia_hwtest-speaker-mic.bin` | same as speaker+mic, **app only** | Bluetooth OTA (`ble_ota.py` / bench app) |

The `-merged` files contain the bootloader + partition table + app in one image — flash the
whole thing to offset `0x0`. The plain files are the app image only, for over-the-air updates
onto a board that already runs this firmware.

## First flash (whole image, via the UART bridge on GPIO43/44)

There is no USB on this board (the camera occupies the native-USB pins), so flash through a
CH340/CP2102 UART bridge. With Espressif `esptool`:

```
esptool --chip esp32s3 -p COMx write_flash 0x0 hejia_hwtest-piezo-merged.bin
```

If auto-reset is not wired: hold **BOOT**, tap **RESET**, release BOOT, then run the command.

## Over-the-air update (app-only image)

Once a board already runs this firmware and is advertising as `Stovyn-HWTEST`:

```
python ../hejia_hwtest/ble_ota.py hejia_hwtest-piezo.bin
```

or use the Android bench app's "Firmware OTA" button and pick the `.bin`.

---

Built from `hejia_hwtest/hejia_hwtest.ino` for `esp32:esp32:esp32s3` with
`PartitionScheme=app3M_fat9M_16MB` (two 3 MB OTA app slots). The speaker+mic variant adds
`-DUSE_I2S_SPEAKER=1 -DUSE_I2S_MIC=1`.
