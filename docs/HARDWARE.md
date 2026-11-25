# Hardware

## Prototype: LC29H HAT + XIAO ESP32-C6

The Raspberry-Pi-shaped LC29H boards work fine as a bench prototype - nothing
here uses a Pi, only three wires plus power.

    LC29H board                  XIAO ESP32-C6
    -------------------------    -------------------------
    TXD  (module out)      -->   GPIO17   (PIN_GNSS_RX)
    RXD  (module in)       <--   GPIO16   (PIN_GNSS_TX)
    GND                    ---   GND
    5V                     <--   5V

Both sides are 3.3 V logic, so no level shifting. TXD/RXD are named from the
module's point of view, hence the crossover.

On the Waveshare HAT, set the UART selection jumper to the position that routes
the module's UART to the 40-pin header (labeled for "control via Raspberry
Pi"). The other positions route it to the onboard USB-to-UART instead, where it
will look completely dead to the ESP32.

> Verify the header pin numbering and jumper lettering against the vendor wiki
> before wiring - the ones above come from the standard Pi header pinout and the
> product description, not from a checked datasheet.

## Testing with a bundled patch antenna

The small active patch antenna that ships with these boards is fine for
bringing the system up, but it is **not** the antenna the product is designed
around, and it fails in a specific way if used naively:

**A patch antenna needs a ground plane.** It is designed to sit on a conductive
surface roughly 70-100 mm square, which forms part of the radiating structure.
Lying loose on a desk or held in the air it loses gain and its pattern
distorts - typically several dB of C/N0, which is the difference between
reaching RTK fixed and never converging. If the antenna has a magnetic base,
put it on something flat and metallic: a tin lid, an aluminum offcut, a car
roof. This single detail will affect early results more than anything in the
firmware.

### Measured behavior, for calibrating expectations

With a multiband helix, SAPOS VRS corrections at 1 s age and roughly 28
satellites, outdoors with a decent view of the sky:

| Condition | Result |
|---|---|
| Stationary | **RTK fixed, 6-8 mm per axis**, held for minutes |
| Re-acquiring after a loss | **fixed again in ~11 s** |
| Carried, walking (first tests, antenna at chest height) | float, 2-7 m |

Later tests did better while moving: walking with the antenna on a pole, 92 %
of epochs fixed; held up in one hand in open fields, the fix held. Two things
follow, and both are about mounting rather than firmware.

**Keep the antenna in one orientation, always - including when it is set
down.** It reached fixed while lying horizontal, and lost it the moment it was
picked up and stood vertical: a sudden change of orientation moves the phase
center against every satellite at once, which is an efficient way to slip
cycles across the whole constellation. A device that lives flat and is picked
up vertical will drop its fix every single time.

**Height above the body is the lever for walking.** A carried antenna has a
torso removing a large part of the sky, and that share changes with every turn -
which is why fix is lost at corners first. Elevating the antenna above the
shoulder is what survey poles are for.

**Stop-and-go is the safe workflow.** Eleven seconds to re-fix means pausing at
each point costs almost nothing, and points are where you stop anyway. With
the antenna held up and a clear sky the fix also holds while walking.

**Firmware A04S or later gives you GST.** Before that the specification said
"GST is not supported on LC29H (BA, CA, DA, EA)" and the module rejected the
request, leaving `$PQTMEPE` as the only accuracy source. A04S added GST and ZDA
(disabled by default, enabled via `$PQTMCFGMSGRATE`), and the firmware asks for
both - preferring GST where it exists, falling back to EPE where it does not.
A04S also fixes latitude and longitude minutes occasionally reading 60, and a
time error after 2038; A05S improves RTK fixed deviation after a restart.
Quectel sends the package by email on request through their forums.

Also note `$PQTMEPE` is only trustworthy once fixed. In float it reported 7.7 m
while the measured stationary scatter was 0.09 m; in fixed it reports
millimeters and is credible. Do not judge a float solution by it - and do not
show it to a user as though it means anything.

Then: outdoors with a clear view of the sky, and away from the USB cable and
the ESP32. A windowsill is not good enough - the multipath indoors will keep it
out of RTK fixed and looks exactly like a firmware fault.

### Finding points recorded by another receiver

When the points being walked to were recorded by a different receiver, what
matters is the **relative** error between the two, not either one's absolute
accuracy.

**Use the same correction source for both.** Two RTK receivers taking
corrections from the same base or the same NTRIP mount point share most of
their error: base coordinate error, residual atmosphere, and datum all cancel
in the difference. That makes relative agreement far better than either
receiver's absolute figure. Take corrections from *different* sources and those
same terms stop canceling and reappear as a systematic offset - decimeters to
meters, constant all day, and invisible unless specifically looked for.

**Check the reference frames agree.** A service delivering ETRS89 and one
delivering ITRF/WGS84 differ by a few centimeters per year of accumulated plate
motion in Europe - tens of centimeters over a decade. Harmless for finding an
object by eye, but it is a real fixed offset, so know which frame each side is
in rather than assuming they match.

**Measure end to end.** Put a visible target on the ground, record it with the
other system, then walk to the recorded point with this one and measure the
miss distance. That single number is what the whole chain is worth, and it
shows which side's error dominates before anyone optimizes the wrong one.

### Walking to points

**Repeatability matters more than absolute accuracy.** Finding a point recorded
in an earlier session depends on the two sessions agreeing, not on either being
right in absolute terms. That is RTK's real value here.

**Fix state has to be honest, and it changes.** Passing trees or a building
drops the solution from fixed to float or single, and it then re-converges.
GGA reports the fix quality every epoch, so an app sees the transition rather
than a stale "RTK" label.

**Stop-start motion is the normal case**, since the user slows and stops as
they close on a point. This is why `$PAIR070` static hold is asserted off: with
it enabled, the position freezes at low speed, so a navigation display would
lie exactly when it matters most.

**1 Hz is settled as sufficient** - at walking pace that is ~1.4 m between
fixes, and the last few meters to a point are closed by eye anyway. So the (DA)
is the right variant and there is no reason to pay for an (EA), whose selling
point is higher update rates.

`LC29H_FIX_INTERVAL_MS` is asserted at 1000 ms rather than left to whatever a
module arrived configured with, for the same reason `$PAIR070` is asserted off.
Lower it if that assumption ever changes; the (DA) is reported to refuse rates
above 1 Hz, so watch the boot transcript for an `,ERROR` reply.

### Carried by a person, in the open

An open field removes the hard part - no canopy, no buildings, so little
multipath. What is left is the carrier, and a human body is a substantial
absorber at L-band.

- **Get the antenna above the body**, not in a hand at waist height. On a cap,
  clipped to a shoulder strap, or on a short pole. The difference between
  chest-height-in-hand and above-the-head is easily several dB.
- **Take the ground plane with it.** A patch antenna held away from any
  conductive surface is working against its own design. Tape it to a ~100 mm
  metal disc and mount that: cap, pole, or a plate on a backpack.
- **Keep it level.** A patch radiates upward from its face; tilting it trades
  away low-elevation satellites on one side. This is one of the reasons the
  product uses a helix, which tolerates tilt far better - so a carried patch
  will understate what the final device does.
- **Walk steadily, and note where you stop.** RTK holds better through smooth
  motion than through stop-start.

**Navigation mode must stay at 0 (Normal), even though the device is carried.**
`$PAIR080` mode 1 ("Fitness") is described in the specification as tuned for
walking and running, and it is the obvious choice here - but it silently
disables RTK. The module still accepts RTCM and still reports satellites, it
simply never applies corrections: no differential age, single forever.
Measured on an LC29HDANR11A03S, switching mode 1 -> mode 0 moved it from
single to float within 17 seconds with the correction stream otherwise
untouched. No documentation mentions this.

The cost is that the position is filtered for driving dynamics rather than
walking, which is a far smaller penalty than having no RTK at all.

Rough expectations once corrections are flowing:

    SINGLE within ~30-60 s of cold start
    FLOAT, then FIXED, within a couple of minutes

If it stays on single, check that corrections are actually arriving before
suspecting the receiver: the `[STAT]` console line counts bytes written to the
GNSS UART, and that number must climb while an app is streaming RTCM.

Do not use the patch as the baseline when comparing helix antennas later. Its
ground-plane requirement is exactly why it is unsuitable for a handheld
enclosure, so it cannot tell you anything useful about the helix choice.

## Prototype wiring: HAT, cell, and controls

The HAT is 65.0 x 30.5 mm with Phi 2.75 mounting holes on a 58.0 x 23.5 mm
pattern. The production board is 32 x 28 mm, so the enclosure should be laid
out around the smaller one and the prototype allowed to overhang.

Set the onboard jumper block to **B (Pi ~ LC29H)** so the module's UART reaches
the 40-pin header rather than the onboard USB bridge.

This is the prototype the design is proven on. The carrier PCB keeps the same
connections, so the firmware does not notice the difference.

                    ,-----------------------------------------------.
                    |  LC29H(DA) HAT   (jumper block -> B, Pi~LC29H) |
                    |                                               |
                    |  U2 RT9193-33: 5V pin -> 3V3 -> module        |
                    |  BAT1 (underside) holds V_BCKP while off      |
                    |                                               |
                    |   TXD      RXD      GND      5V        SMA    |
                    `----|--------|--------|--------|---------|-----'
                         |        |        |        |         |
                         |        |        |        |      helix antenna
                         |        |        |        |      (active, biased
                         v        ^        |        ^       up the coax)
    ,--------------------|--------|--------|--------|--------------------.
    |                   D7       D6       GND      |                     |
    |                                              |                     |
    |   XIAO ESP32-C6                              |    [P-FET] <- D5    |
    |                                              |     optional,       |
    |   BAT+ <------------------------------------ + --- PIN_GNSS_POWER  |
    |   BAT- <------------------------------------ | -----.              |
    |                                              |      |              |
    |   feed --[100k]--+--[100k]--> GND            |      |              |
    |                  '--> A0                     |      |              |
    |   D1 <-- [mode switch] --> GND               |      |              |
    |   D2/D3/D4 --> R --> yellow/red/green LED    |      |              |
    |   D8 <-- [mark button] --> GND               |      |              |
    |   D9 = BOOT, provisioning entry              |      |              |
    |                                              |      |              |
    |   USB-C: charge + flash + GNSS bridge        |      |              |
    |   5V pin --> TP4056 IN+                      |      |              |
    `----------------------------------------------|------|-------------'
                                                   |      |
                                           [power switch] |
                                                   |      |
                                        TP4056 OUT+    OUT-
                                        TP4056 module with protection
                                        B+             B-
                                                   |      |
                                        18650, welded nickel tabs

Only the unregulated cell feed makes the long journey to the HAT; its own
regulator and decoupling are already at the module end, which is where Quectel
wants them.

The cell is fixed, and charged in place: USB-C on the XIAO feeds its 5V pin,
which feeds the TP4056's input, so one port charges, flashes and bridges. The
TP4056 module's own USB-C carries no data and stays unused. The load hangs off
OUT, behind the module's protection, never off B+.

### Connections

The header is labeled from the Pi's side: H_TXD is the Pi transmitting, so it
is the module's input. Verified with `tools/bridgetest.py`.

| from | to | note |
| ---- | -- | ---- |
| HAT header pin 10 (H_RXD, module TXD) | XIAO D7 / GPIO17 | `PIN_GNSS_RX` |
| HAT header pin 8 (H_TXD, module RXD) | XIAO D6 / GPIO16 | `PIN_GNSS_TX` |
| HAT header pin 6 | XIAO GND | |
| HAT header pin 2 | cell feed, or XIAO 5V on the bench | the module needs it even with USB in |
| 18650 + / - | TP4056 B+ / B- | welded tabs, module mounted at the cell |
| XIAO 5V pin | TP4056 IN+ (IN- to GND) | charging from the XIAO's USB-C |
| TP4056 OUT+ | power switch -> XIAO BAT+ | the XIAO has its own reverse protection |
| TP4056 OUT+ | power switch -> HAT 5V pin | the HAT's own RT9193 regulates; see below |
| (optional) | P-FET in that feed, gate driver from D5 | `PIN_GNSS_POWER` for the bridge |
| TP4056 OUT- | XIAO BAT-, common ground | |
| switched feed | 100k/100k divider -> XIAO A0 | `BATTERY_ADC_PIN`, 4.2 V -> 2.1 V |
| mode switch | XIAO D1 / GPIO1 to GND | `INPUT_PULLUP`, polled live |
| red LED | XIAO D3 / GPIO21 -> 150R -> anode, cathode to GND | `PIN_LED_RED` |
| yellow LED | XIAO D2 / GPIO2 -> 150R -> anode, cathode to GND | `PIN_LED_YELLOW` |
| green LED | XIAO D4 / GPIO22 -> 150R -> anode, cathode to GND | `PIN_LED_GREEN` |
| mark button | XIAO D8 / GPIO19 to GND, momentary | `PIN_MARK_BUTTON`, `INPUT_PULLUP`; records a point |

### Powering the receiver from the cell

Settled from the Waveshare schematic rather than guessed. The relevant part of
the power block:

    USB_5V --> U2  RT9193-33PB  --> 3V3    (1 uF in, 100 nF out, 22 nF bypass)
    USB_5V --> U7  RT9166A-28PXL --> 2V8   (level shifters, VDD_EXT)
    3V3 --[D2 1N4148WS]--[R5 1K]--> V_BCKP --> BAT1

`USB_5V` is simply the header's two 5V pins tied to the micro-USB VBUS.
**Nothing on the HAT actually needs 5 V** - the only consumers are those two
LDOs and the CP2102N, which this design does not use.

So the cell feeds the 5V pin directly and U2 does the regulating. Because the
RT9193 is a low-dropout part (its 220 mV figure is at the full 300 mA; at our
~80 mA it is far less) it holds regulation until the cell is essentially flat,
and the module's floor is 3.1 V rather than 3.3 V:

    cell 4.2 V -> 3.3 V        cell 3.3 V -> ~3.25 V
    cell 3.6 V -> 3.3 V        cell 3.2 V -> ~3.15 V, still in spec
                               cell 3.0 V -> out of spec, but the XIAO's own
                                             cutoff is ~2.9 V anyway

That covers the whole usable cell range **with no switching converter anywhere
in the GNSS supply**, which is what Quectel ask for:

> "It is not recommended to use a switching DC-DC power supply."

Note this rules out two tempting alternatives. A boost to 5 V would put a
switcher in the supply; and the XIAO's own 3V3 rail is no better, because it
comes from an SGM6029C, which is also a buck.

No additional regulator is needed. If `PIN_GNSS_POWER` is wanted - the
USB-to-GNSS bridge uses it to power-cycle the module for flashing, and Quectel
recommends MCU control of VCC regardless - it becomes a high-side P-FET in the
cell-to-HAT feed, driven by D5 through an N-FET gate driver.

### The backup domain is already on the HAT

`BAT1`, on the underside, is trickle-charged from 3V3 through D2 and R5 (1 K),
with the 4.7 uF/100 nF/3.3 pF network at `V_BCKP` that Quectel specifies. So the
prototype needs nothing added for a warm restart: the receiver keeps
ephemeris, almanac and RTC while switched off, and hot-starts in seconds
instead of cold-starting.

**It is a charging circuit, so fit a rechargeable ML1220 - never a CR1220.**
A primary cell in a charging holder is a hazard. Check whether yours is
populated; the footprint is on the bottom side, so it is easy to miss.

### Controls are recessed, deliberately

The unit clips to a vest breast pocket, where backpack straps, seatbelts and
harnesses cross. A latching power button that a strap can press is a unit that
is silently off when someone needs it, and a mode switch that a knock can move
changes the correction source mid-survey. Recess both below the surface, or
shroud them.

The **latching** button is chosen over a soft-latch FET circuit on purpose: no
quiescent drain, nothing to leave half-on, and no firmware failure can strand a
device in a bag. Clean shutdown is not needed, because every persistent flag -
debug mode, WiFi and NTRIP settings - is written to NVS when it changes rather
than at power-off.

The switch sits after the charger's output, so **a switched-off unit still
charges**.

### If the HAT has no backup cell

Without a held-up `V_BCKP` the receiver cold-starts after every power-off -
losing ephemeris, almanac and RTC, which costs minutes of reacquisition. On a
board without `BAT1`, or with it empty, a supercapacitor charged through a
resistor from the 3.3 V rail does the same job: `V_BCKP` accepts 2.2-3.6 V and
draws about 25 uA.

    0.22 F   over a 3.3 -> 2.2 V droop   ~3 hours
    1 F      over the same droop         ~12 hours

### Open items, to be measured rather than assumed

1. What the module's VCC actually sits at with the cell near empty. The math
   above says it stays above 3.1 V; confirm it with a meter at ~3.2 V in.
2. The TP4056's charge current and end-of-charge voltage on the final wiring.
3. Is `BAT1` populated on the underside of your HAT? The circuit is there; the
   cell may not be.
4. ~~Total system current~~ - measured 2026-10-03, XIAO + HAT together on
   5 V USB through an inline meter, module acquiring indoors:

   | state | current |
   | ----- | ------- |
   | BLE only, phone linked (field build) | 62 mA |
   | WiFi AP + BLE (bench build, phone on the portal or not) | 97 mA |

   The portal costs about 35 mA whether or not anyone is connected, which is
   why the field build turns the radio off. From a ~3000 mAh 18650 that is
   roughly 35-45 h on BLE. Still to do: the same on the cell, where the XIAO's
   buck sees ~3.7 V rather than 5 V.
5. Whether running on the cell costs any C/N0 against the USB reference above.
   Record `CNO` and `ELEV` on USB, then on battery, same antenna and spot,
   minutes apart. Any noise the power path injects at L1 shows up directly in
   dB - which is the only measurement that actually settles it.

## Target: standalone build

    XIAO ESP32-C6  --UART--  LC29H(DA) module  --SMA--  quad-helix antenna
          |
       BAT pads  --  switch  --  TP4056 with protection  --  18650 cell
          |
       USB-C (charge + flash + bridge)

Points worth settling before committing to a layout:

**Antenna.** A quad-helix needs no ground plane, which is what makes a compact
handheld enclosure viable at all - a patch antenna would need a substantial
metal plane under it and would dictate the whole shape. Active antennas need
bias voltage; check the module board supplies it on the SMA center pin, and at
what voltage.

**Power budget.** Measured, not estimated - see "Total system current" above:
the whole device, XIAO + HAT + antenna, draws **62 mA** on BLE (field build)
and 97 mA with the WiFi portal up (bench build), at 5 V on USB. The datasheet
sums this replaced (100-210 mA) were pessimistic by a factor of two to three.

On the cell the XIAO's buck sees ~3.7 V instead of 5 V and draws more current
for the same power, so allow up to ~85 mA until that is measured too. An 18650
of 3000 mAh then gives roughly **35-45 hours** - several field days. Ample
margin, so the cell can be chosen for size rather than capacity.

**Regulation.** The 18650 runs 3.0-4.2 V. The XIAO regulates its own rail from
the BAT pads, and the HAT's RT9193 regulates the module's 3.3 V from the same
feed - see "Powering the receiver from the cell". No extra regulator.

**Charging.** A TP4056 module with built-in protection charges the fixed cell
at 1 A from the XIAO's USB-C, about 3-4 hours for 3000 mAh, and protects it at
4.28 V over, 3.0 V under and 3 A over current. It has no power path, so while
charging the load draws through the charger, which at well under 100 mA only
shifts the end of charge slightly. Its temperature input is not wired, so
charge indoors: a lithium cell must never be charged below 0 C.

**Sensing and reporting.** An external 100k/100k divider on the switched feed
brings half the cell voltage to **A0** (D0/GPIO0), and the firmware doubles it
(`analogReadMilliVolts(A0) * 2`). Two consequences:

- **D0/GPIO0 is not a free pin.** It is the battery sense input, so anything
  else that wants a GPIO has to go elsewhere.
- **The divider sits after the switch**, so it draws nothing from a shelved
  unit.

Voltage is turned into a percentage against a piecewise Li-ion discharge curve,
not a linear 3.0-4.2 V map - a linear map reads roughly 50% when a cell is
actually near 20%, because the chemistry's discharge plateau is nowhere near
the middle of the voltage span. Samples are averaged and then smoothed
(exponential, quarter weight), because the cell voltage sags under the GNSS
module's draw and an unsmoothed reading walks several percent between samples.

The result is published as the SIG **Battery Service 0x180F**, characteristic
**Battery Level 0x2A19** (read + notify, one byte of percent). Any client that
understands standard BLE - phone OS battery display included - picks it up
without knowing anything else about the device. A reading outside a plausibility
window (2.5-4.4 V) is reported as unknown rather than as a wrong number, which
is what a bench board on USB without a cell reports.

The cell is built in, which drives a few details:

- **Unprotected flat-top cells from a known maker, spot-welded.** Never solder
  to a cell: the heat damages it. Pure nickel strip, 8 mm x 0.15 mm.
- **Protection right at the cell.** The TP4056 module sits against the cell, so
  the short tabs are the only unprotected conductors.
- **An insulator ring on the positive end**, so the tab cannot touch the can,
  which is negative.
- **Mechanical retention.** The cell is held in a printed cradle, not by its
  tabs.

**Antenna placement.** Keep the helix as far from the ESP32-C6 and the USB
connector as the enclosure allows: 2.4 GHz BLE and the GNSS L1/L5 bands are far
apart, but switching noise and USB harmonics are broadband. A ground pour under
the module and short antenna coax help more than distance alone.

## WiFi, and why it is almost always off

The phone normally fetches corrections, so the device needs WiFi only for
setup, the debug panel, and the optional device-side NTRIP client over the
phone's hotspot. The portal draws about 35 mA - over half again on top of the
62 mA the device needs without it. So it stays off, and credentials are entered
through a captive portal that has to be asked for and closes itself again:

- **Hold BOOT while powering on.** Nothing else uses that button once running,
  so no extra hardware and no pocket triggering.
- The device raises `RTK-Rover-xxxx` (WPA2). Joining it makes the phone open
  the setup page by itself: a DNS server answers every lookup with the device's
  own address, so the connectivity check Android and iOS perform lands on our
  page and they offer it as a sign-in.
- Pick a network from the scanned list, enter the password, and it stores them,
  connects to verify, and reports back.
- **Ten minutes and the radio shuts down**, used or not. The page shows the
  countdown so nobody is surprised by it disappearing.

Credentials live in NVS, so this is a once-per-network job.


## Enclosure

Nothing here needs a machined part. A printed shell in PETG or ASA (PLA creeps
in a car in summer) with:

- a cradle that holds the cell, with the charger module beside it
- a shelf for the module board, antenna centered on top and pointing skyward
- a cutout for USB-C, and holes for the power switch, mode switch and mark button
- a breast-pocket clip, or a mount for a pole

Field use means dew, mud and rain. The antenna itself is typically IP67, but
the printed shell is not: give the USB port an overhang, and give the shell's
seam a gasket groove, or at minimum a lip that sheds water away from it. Nothing here is waterproof by default.

Print the antenna platform flat and level: a quad-helix wants its axis vertical
for a clear view of the sky, and a few degrees of tilt costs low-elevation
satellites.
