# Parts and cost per unit

Prices as researched in September-October 2026, small quantities, before shipping
and VAT. **Treat every figure as an estimate to confirm at order time** - GNSS
modules and antennas in particular move a lot, and only the two marked
"verified" were read from a vendor page during this exercise.

## Sourcing

Two paths, and the gap between them is most of the unit cost.

**Distributor** (SparkFun, gnss.store, Waveshare, The Pi Hut): known variant,
stated specifications, returnable. The two figures below marked *verified* were
read from vendor pages.

**AliExpress**: the same LC29H boards and helix antennas are listed for
substantially less. Worth using - with two specific things to check, because
both failure modes are silent and expensive.

### Check the module variant, not the listing title

Marketplace listings routinely bundle every LC29H variant under one title -
"LC29H AA BA DA EA" - with the variant as a dropdown, and the wrong one is
useless here:

    LC29H(DA)   RTK only          <- ideal
    LC29H(BA)   RTK + DR           <- also fine, see below
    LC29H(CA)   DR only, no RTK    - useless here
    LC29H(AA)   plain GNSS, no RTK - useless as a product, useful as a test rig
    LC29H(BS)   base station       - outputs raw observations, never a rover fix

A **(BA) is a superset of a (DA)**, not a lesser part: the application note
states "LC29H (BA) supports DR and RTK" while "(DA) only supports RTK", and
both accept the same RTCM 10403.3 input. This firmware runs on either
unmodified - buy whichever is cheaper.

Its extra dead-reckoning simply will not engage in a handheld: BA/CA integrate
an IMU and are specified for vehicle mounting, and the note is explicit that
"manually holding the device is not acceptable" and that DR is only used once
calibration reports complete. Handheld, that never happens, so DR stays out of
the way rather than degrading the fix.

The reverse swap is not possible at all - a (DA) has no IMU, so no firmware
could give it DR.

An **(AA) cannot be configured into an RTK rover**: it has no RTK engine to
enable. The protocol specification never mentions (AA) in an RTK context, the
DR&RTK application note covers only (BA, CA, DA), and (AA) is grouped with the
LC79H(AL) throughout - a different firmware line. Changing that would mean
replacing the firmware, not configuring it.

It is still a useful board to own, though: as a **development receiver** it
exercises this entire firmware end to end - UART, parsing, BLE,
bonding, app integration - and the only difference is that the position type
only ever reports a single (non-RTK) fix. Worth validating the whole pipeline on an
(AA) before spending on a rover-capable module.

The firmware prints the module's own answer to `$PQTMVERNO` at boot, which
returns a firmware string naming the variant. That is the acceptance test for a
marketplace module: flash, power on, read the console. Do it before wiring
anything into an enclosure.

### Check the antenna by behavior, not by its specification

Antenna listings claim bands and gain figures freely, and an antenna that is
really L1-only, or has a poor axial ratio, still produces a plausible-looking
single-point fix - it just converges slowly, or not at all, and drops out of
RTK fixed under canopy. There is no way to tell from the listing.

The practical test: buy one cheap antenna and one known-good one, and compare
**time to first RTK fixed** and **how long it holds fixed** from the same spot,
against the same correction stream. That difference is the entire reason for
building an RTK device, so it is worth one extra antenna to measure rather than
assume.

Also budget for EU import VAT and possible duty on marketplace orders, which
narrows the gap somewhat on the more expensive items.


## Per unit

The cell is fixed inside the unit and charged in place over the XIAO's USB-C,
through a TP4056 charger module with built-in protection. There is no
removable cell, no sled and no external bay charger. One USB port does
everything: the module's own USB-C, which carries no data, stays unused.

    USB-C (XIAO) -> XIAO 5V pin -> TP4056 IN+ / IN-
    TP4056 B+ / B- <-> 18650 (welded tabs)
    TP4056 OUT+ -> power switch -> XIAO BAT pad and HAT 5V pin
    TP4056 OUT- -> common ground

The load hangs off OUT, never B+, or the protection is bypassed. The module has
no temperature input (its TEMP pin is grounded), so charge indoors only, never
below 0 °C.

| Part | Qty | Unit | Note |
|---|---|---|---|
| Quectel LC29H(DA) board (Waveshare HAT or similar) | 1 | **~€29** AliExpress / €45-70 distributor | Verify the variant with `$PQTMVERNO` before it goes into a build. A (BA) works equally well. |
| Multiband L1/L2/L5 active quad-helix antenna, SMA | 1 | **~€25** AliExpress / €69.99 distributor | Distributor reference verified (gnss.store ELT0014): 28-30 dBic, 3.3-16 V, 18 g, 59.7 x Ø27.5 mm. No ground plane needed. Verify a marketplace antenna by behavior, above. |
| Seeed XIAO ESP32-C6 | 1 | $5.20 | **Verified** (Seeed Studio). |
| TP4056 charger module with protection, USB-C | 1 | €0.50-1 | The six-pad version (IN, B, OUT) with DW01-type protection: charges at 1 A (about 3-4 h for 3000 mAh) to 4.2 V, protects at 4.28 V over, 3.0 V under and 3 A over current. No load sharing: while charging, the load draws through the charger, which only shifts the end of charge slightly at this ~85 mA load. On arrival, check a full charge stops at 4.15-4.2 V on B+/B-. |
| 18650 cell, **unprotected flat-top**, ~3000-3500 mAh (built with DMEGC INR18650-30P from NKON) | 1 | €2.25-6 | Brand cells only (Samsung, Molicel, LG, Sony/Murata, DMEGC) from a reputable seller (e.g. NKON, Akkuteile); marketplace capacity claims are routinely fictional. Flat top, because the tabs are welded. |
| Pure nickel strip, 8 mm x 0.15 mm | ~8 cm | <€0.50 | Two tabs, welded to the cell, soldered to the charger module. Pure nickel, not nickel-plated steel. |
| Insulator ring for the 18650 positive end | 1 | €0.05 | Keeps the positive tab off the can's rim, which is negative. |
| Power switch, latching | 1 | €0.50-1 | In OUT+, so a switched-off unit still charges. |
| Momentary push button, mark | 1 | €0.30-1 | D8, records a point. |
| Slide switch, mode | 1 | €0.30 | D1. |
| LED 1206: red, yellow, green | 3 | €0.05 | On the PCB, under holes or light pipes in the shell. 3 mm through-hole on wires for the hand-wired unit. |
| Resistor 0805, 150 Ω | 3 | €0.01 | LED series resistors, for LEDs of ~2 V. A true green (~3 V, check with the meter's diode mode) needs 47-68 Ω instead. |
| Resistor 0805, 100 kΩ | 2 | €0.01 | Cell voltage divider to A0. |
| Capacitor 0805, 100 nF | 2 | €0.01 | A0 to ground, and the GNSS supply input. |
| Capacitor 0805, 10 µF, ≥10 V, X5R/X7R | 1 | €0.02 | GNSS supply input. |
| SMA pigtail or bulkhead, as the layout needs | 1 | €3-6 | |
| Wire, heatshrink, Kapton tape, M2 screws | - | €1-2 | |
| Filament, printed shell | - | €1-2 | ~30-50 g in PETG. |

A polyfuse of about 1.5 A in series with the cell is optional extra
protection. The module already cuts off a short. Optional footprints on the
PCB: an AO3401 P-FET (SOT-23) with a 100 kΩ gate pull-up, for switching the
receiver from D5 (`PIN_GNSS_POWER`), and the polyfuse in 1206.

## Totals

    antenna, quad-helix L1/L2/L5 (AliExpress)   ~€25
    LC29H(DA) board (AliExpress)                ~€29
    XIAO ESP32-C6                               ~€5
    cell                                        €2-6
    charger module with protection              ~€1
    switches, button, LEDs, passives,
      nickel, SMA lead, fasteners, filament     €7-13
                                                -----------
                                                €71-79 per unit

Call it **~€75 per unit**, or ~€125-170 buying the antenna and module from
Western distributors instead.

Composition at those prices:

    antenna + module   €54    ~72%
    cell + charger     €5-7   ~7-9%
    incidentals        €7-13  ~9-17%
    XIAO               €5     ~7%

## Tools and consumables

| Item | Cost | Note |
|---|---|---|
| Spot welder, supercapacitor type (e.g. AWithZ UF20B or UKF10) | €110-130 | For the cell tabs. Never solder to the cell itself. A capacitor welder has no internal lithium cell to wear out, and also handles copper for heavier packs. |
| Soldering station, flux, solder | - | For everything except the cell. Lead-free (SAC305) wire, to match the paste. |
| Solder paste, SAC305, no-clean, Type 3-4 | €10-15 | For the passives with hot air. Fridge, ~6 months once opened. Never mix with leaded or bismuth solder. |
| Multimeter | - | Per-unit checks: cell voltage, charger end-of-charge voltage, current draw, LED forward voltage. |
| USB-C cable and USB power meter | €10-15 | Charge current and system current per unit. |
| Kapton tape, heatshrink assortment | €10 | |
| Hot-air rework station (858D class, or an iron + hot-air combo) | €40-90 | SMD passives with solder paste, and heatshrink. Not a paint-stripper heat gun, which blows small parts away. On a cell wrap: widest nozzle, ~200 °C, keep moving, never heat a cell above ~60 °C. |

## Where the cost actually goes

At ~€75 a unit, the interesting question is no longer how to make it cheaper but
where a few more euros buy the most:

1. **A better antenna is the only upgrade that matters.** A €70 distributor
   helix against a €25 marketplace one is a €45 decision on a €75 device - the
   single largest lever on time-to-fix and on holding RTK fixed under canopy,
   which is the entire point of the device. Build two units and A/B them; at
   these prices the experiment costs less than guessing wrong.
2. **The module is nearly free to upgrade.** (BA) and (DA) are interchangeable
   here, so take whichever is cheaper on the day.
3. **At volume, the breakout is the waste.** The bare module on a small custom
   PCB carrying the charger, cell protection, switch and SMA footprint would
   cost less than the €29 breakout alone and delete most of the hand-wiring.
   That PCB comes after the first unit is hand-wired and proven.

The compute is a rounding error at ~7%.

## Cheaper receivers?

At ~€29 the LC29H breakout looks to be at or near the floor for a *ready-to-use*
dual-band RTK board; it is routinely described as the most cost-effective
L1/L5 RTK part currently available, and the established alternatives
(u-blox ZED-F9P, Unicore UM980/UM982) are several times more expensive, not
less.

Candidates worth a look, none price-verified here:

| Part | Form | Note |
|---|---|---|
| SIMCom SIM66MD | bare LCC/SMT | L1+L5 with onboard RTK. Needs your own PCB. |
| Allystar TAU1302 | module / board | Dual-band RTK; sold by DATAGNSS among others. |
| Bare `LC29HDAMD` | bare LCC | The same module without the breakout. |

**The criterion that matters more than price: does the module compute an RTK
fix onboard, or only output raw observations?** Plenty of cheap "RTK" parts do
the latter and expect a host running RTKLIB to solve. This device has no such
host - the phone provides corrections, not a solver - so a raw-observation-only
module is useless here. It is the same trap as the (BS) variant, one level up.

### Whether a swap is worth it

The module is ~39% of a ~€75 build, so even a *free* module only reaches ~€46,
and a €15 one saves about €14 a unit. Against that, a different receiver means
a new init sequence, possibly a third accuracy message to parse, and full
re-verification against each app. At prototype volumes that is poor value
compared with spending the same effort on antenna selection and the enclosure.

Where a cheaper part genuinely pays is at volume and *together with* a custom
PCB: a bare module carrying the charger, cell protection, switch and SMA
footprint deletes the breakout, the SMA lead, and most of the hand-wiring at
once - a far larger saving than the chip swap alone.

Should it come to that, the change is contained by design: receiver specifics
live in one file (`src/gnss_lc29h.cpp`) behind a single init call, and the
parser already handles two different accuracy sources (GST and `$PQTMEPE`), so
adding a third is a contained edit rather than a rewrite.

## Not included

- NTRIP corrections. The phone provides them, so this is a service cost, not a
  hardware one. In Germany SAPOS is free for many use cases, but an account is
  limited to one device at a time, so several units in the field at once need
  an account each or another correction source.
- The phone.
- A pole mount or tripod adapter, if surveying rather than walking.
