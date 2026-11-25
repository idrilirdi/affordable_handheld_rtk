# BLE profile

Two services: Nordic UART for NMEA and corrections, and the SIG Battery
Service.

## NMEA over Nordic UART

The receiver's sentence stream goes to the phone, and corrections come back,
over the Nordic UART Service:

| UUID | Direction |
|---|---|
| `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | service |
| `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | notify, rover -> phone |
| `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | write, phone -> rover: corrections |

Sentences go out in whatever chunks the UART delivered, unreframed. The only
additions are the synthesized `$GNGST` described in the README and the
diagnostics below, and both are only ever emitted at a sentence boundary.

Everything written to the RX characteristic reaches the receiver UART verbatim:
RTCM3 frames from the app's NTRIP client, and any line-oriented ASCII
configuration it sends. Apps typically request a large MTU and stream RTCM3 in
MTU-sized chunks.

### Diagnostics

Two proprietary sentences ride the same stream every 10 s, because the USB
console cannot be used during a survey: attaching to the serial port resets the
board, and the boot-time receiver configuration restarts the navigation filter
and discards RTK convergence. Observing over USB destroys what is being
observed, so the diagnostics go over the air instead.

    $PRTKA,<age>,<base id>,<baseline m>,<frames ok>,<frames bad>*CS
    $PRTKM,<type>,<count>,<satellites>,<signal id>,...*CS

`$PRTKA` carries the age of differential corrections from GGA field 13 - an
**empty** age means the receiver is not applying corrections at all - and the
distance to the reference station decoded from RTCM `1005`. For a network VRS
that baseline should be meters, since the virtual base is synthesized at the
position the client sends upstream; kilometers means the client sent a stale or
wrong position.

`$PRTKM` repeats per message type, with the MSM satellite count and the RTCM
signal IDs present. The IDs are what decide whether a stream can fix this
receiver: it tracks the L1 and L5 bands, so **22/23/24** (L5, E5a, B2a) must
appear. A base sending only the L1 and L2 bands leaves one usable frequency,
and one frequency floats.

Clients ignore sentences they do not recognize; one showing raw NMEA displays
them.

## Battery

The SIG-assigned Battery Service, unmodified, so that clients that know nothing
else about this device still show a charge level:

| UUID | Properties | Value |
|---|---|---|
| `180F` | service | |
| `2A19` | read, notify | `uint8`, percent, 0-100 |

`0xFF` means **unknown** - a bench board without a cell, or a voltage outside the plausibility
window. It is only ever read, never notified: a client polling the
characteristic sees the unknown state, but a subscriber is not woken to be told
nothing. Notifications are sent on change, not on a timer.

The percentage is derived from a piecewise Li-ion discharge curve rather than a
linear voltage map. See docs/HARDWARE.md for the sensing path.

## Advertising

The rover **stops advertising while connected** and serves one client at a
time. An app that keeps scanning sees a connected device that is still
advertising as connectable, and tries to connect to a link it already holds,
which drops and remakes the connection over and over. `ADVERTISE_WHILE_CONNECTED`
in `config.h` turns advertising-while-connected on for the bench build, where a
second diagnostic client is worth more than that.

Connection parameters are 15-30 ms interval with a **6 s** supervision timeout.
Four seconds is short enough that an ordinary
hiccup drops the link, and the app then has to rediscover and resubscribe.

The name goes in the advertisement and the service UUID in the scan response: a
31-byte advertisement cannot hold flags plus a 128-bit UUID plus the name, and
if the name is dropped, phone Bluetooth menus hide the device entirely because
they list peers by name.
