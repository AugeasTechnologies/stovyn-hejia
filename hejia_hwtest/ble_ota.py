#!/usr/bin/env python3
"""
ble_ota.py — flash a firmware .bin to the Stovyn Hejia board over Bluetooth LE.

The hardware-test firmware advertises as 'Stovyn-HWTEST' and exposes a small OTA
GATT service. This uploads the image to the board's INACTIVE OTA slot and reboots
into it. A bad/partial upload never bricks the board: the boot slot only switches
on a verified END, otherwise the board keeps running the current image.

Setup:  pip install bleak
Usage:  python ble_ota.py firmware.bin
        python ble_ota.py firmware.bin --name Stovyn-HWTEST --fast

Get the .bin from an arduino-cli build:
    arduino-cli compile --fqbn esp32:esp32:esp32s3:...,PartitionScheme=app3M_fat9M_16MB \\
        --output-dir ./build hejia_hwtest
    # -> ./build/hejia_hwtest.ino.bin  (this is the file to pass here)
"""
import asyncio, struct, sys, argparse
from bleak import BleakScanner, BleakClient

SVC  = "e7c1b100-2f3a-4b6d-9c11-000000000001"
CTRL = "e7c1b101-2f3a-4b6d-9c11-000000000001"
DATA = "e7c1b102-2f3a-4b6d-9c11-000000000001"
STAT = "e7c1b103-2f3a-4b6d-9c11-000000000001"
OP_BEGIN, OP_END, OP_ABORT = 0x01, 0x02, 0x03


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware", help="path to the .bin to flash")
    ap.add_argument("--name", default="Stovyn-HWTEST")
    ap.add_argument("--fast", action="store_true",
                    help="write-without-response (faster; a dropped chunk fails safe at END, then retry)")
    args = ap.parse_args()

    with open(args.firmware, "rb") as f:
        img = f.read()
    size = len(img)
    if size == 0:
        sys.exit("[ble-ota] empty image")
    print(f"[ble-ota] image {args.firmware}: {size} bytes")

    print(f"[ble-ota] scanning for '{args.name}' (blue LED should be pulsing)…")
    dev = await BleakScanner.find_device_by_name(args.name, timeout=20.0)
    if not dev:
        sys.exit(f"[ble-ota] '{args.name}' not found — is the board powered and done with its self-test?")

    done = asyncio.Event()
    last = {"s": ""}
    def on_stat(_, raw: bytearray):
        s = bytes(raw).decode(errors="replace")
        last["s"] = s
        print(f"\n[board] {s}")
        if s.startswith(("DONE", "ABORT", "ERR")):
            done.set()

    print(f"[ble-ota] connecting to {dev.address}…")
    async with BleakClient(dev) as client:
        await client.start_notify(STAT, on_stat)
        mtu = getattr(client, "mtu_size", 23) or 23
        chunk = max(20, mtu - 3)
        print(f"[ble-ota] connected, MTU={mtu}, chunk={chunk}, mode={'fast/WNR' if args.fast else 'acked'}")

        # BEGIN + 4-byte little-endian size
        await client.write_gatt_char(CTRL, bytes([OP_BEGIN]) + struct.pack("<I", size), response=True)
        await asyncio.sleep(0.3)

        sent = 0
        for i in range(0, size, chunk):
            part = img[i:i + chunk]
            await client.write_gatt_char(DATA, part, response=not args.fast)
            sent += len(part)
            if (i // chunk) % 16 == 0:
                print(f"[ble-ota] {sent}/{size} ({100 * sent // size}%)", end="\r", flush=True)
        print(f"[ble-ota] {size}/{size} (100%) sent            ")

        # END -> board verifies, sets boot slot, reboots
        await client.write_gatt_char(CTRL, bytes([OP_END]), response=True)
        try:
            await asyncio.wait_for(done.wait(), timeout=30)
        except asyncio.TimeoutError:
            print("[ble-ota] no final status — board likely rebooted into the new image.")

    result = last["s"] or "(board rebooted)"
    print(f"[ble-ota] result: {result}")
    sys.exit(0 if result.startswith(("DONE", "")) and not result.startswith(("ABORT", "ERR")) else 1)


if __name__ == "__main__":
    asyncio.run(main())
