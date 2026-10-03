# Tools

Host-side helpers. Python 3; `plotwalk.py` needs matplotlib, `bridgetest.py`
needs pyserial.

## Walked tests

A walked test answers whether losing the fix follows the sky or something
else. The rover's debug panel (WiFi portal, `http://192.168.4.1/d.txt`) is the
only view into a running survey - attaching USB resets the board and restarts
the receiver's navigation filter - so the test is recorded over WiFi.

    tools/walk.sh [RTK-Rover-XXXX]       # record logs/walk-<date>.csv, Ctrl-C to stop
    tools/plotwalk.py logs/walk-<date>.csv --out walk.png   # fix state against C/N0

`walk.sh` joins the rover's setup network with NetworkManager when given its
name, keeps the laptop awake, and runs `logpanel.py`, which polls the panel
once a second and writes one CSV row per poll. A failed poll still writes a
row, with `ok` set to 0: gaps are data, they show where the link died.

## Bridge check

    tools/bridgetest.py /dev/ttyACM0

Proves the USB-to-receiver bridge is transparent in both directions before it
is trusted with a receiver firmware image.
