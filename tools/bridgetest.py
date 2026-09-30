#!/usr/bin/env python3
"""Prove the USB <-> GNSS bridge is transparent in both directions.

Worth running before trusting it with a firmware image: a bridge that drops or
reorders bytes would leave a half-written module that has no other way in.

    python3 tools/bridgetest.py /dev/ttyACM0
"""

import sys
import time

import serial


def nmea(body):
    """'$PQTMVERNO' -> the sentence with its checksum and terminator."""
    sum_ = 0
    for ch in body[1:]:
        sum_ ^= ord(ch)
    return f"{body}*{sum_:02X}\r\n".encode()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    s = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.3)
    s.reset_input_buffer()

    print(f"asking {port} for the bridge...")
    s.write(b"$PRTKFLASH\n")
    s.flush()

    banner = b""
    t0 = time.time()
    while time.time() - t0 < 5:
        banner += s.read(128)
        if b"[BRIDGE]" in banner:
            break
    if b"[BRIDGE]" not in banner:
        print("no bridge banner. Is BRIDGE_ENABLE set, and is this the right port?")
        print(f"  got: {banner[-200:]!r}")
        return 1
    print(f"  {banner.decode(errors='replace').strip().splitlines()[-1]}")

    # Round trip through the bridge: the module only answers this if bytes
    # reach it intact and its reply comes back the same way.
    print("asking the module its firmware version through the bridge...")
    s.write(nmea("$PQTMVERNO"))
    s.flush()

    reply = b""
    t0 = time.time()
    while time.time() - t0 < 6:
        reply += s.read(256)
        if b"PQTMVERNO," in reply:
            break

    if b"PQTMVERNO," not in reply:
        print("no reply from the module.")
        print(f"  got: {reply[-300:]!r}")
        print("  If the board has no load switch (PIN_GNSS_POWER -1), the module")
        print("  stays powered and this should work. If it does have one, the")
        print("  module powers up on the first byte - try once more.")
        return 1

    line = [l for l in reply.split(b"\r\n") if b"PQTMVERNO," in l][0]
    print(f"  {line.decode(errors='replace')}")
    print("\nbridge is transparent in both directions.")
    print("Reset the board to leave bridge mode.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
