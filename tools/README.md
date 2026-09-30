# Tools

Host-side helpers. `bridgetest.py` needs pyserial.

## Bridge check

    tools/bridgetest.py /dev/ttyACM0

Proves the USB-to-receiver bridge is transparent in both directions before it
is trusted with a receiver firmware image.
