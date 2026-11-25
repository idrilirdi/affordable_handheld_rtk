#pragma once

// ---------------------------------------------------------------------------
// GNSS receiver - Quectel LC29H(DA)
// ---------------------------------------------------------------------------
// The (DA) is an RTK rover: RTCM3 written to its UART is consumed directly,
// with no enabling command, so corrections coming down the BLE link need no
// special handling.
#define GNSS_BAUD 115200  // LC29H default

// UART pins on the XIAO ESP32-C6. TX/RX are named from our point of view, so
// they cross over to the module's TXD/RXD.
#ifndef GNSS_UART_NUM
#define GNSS_UART_NUM 1
#endif
#ifndef PIN_GNSS_RX
#define PIN_GNSS_RX 17  // in  <- module TXD  (NMEA)
#endif
#ifndef PIN_GNSS_TX
#define PIN_GNSS_TX 16  // out -> module RXD  (RTCM3 / commands)
#endif

// Run the receiver configuration sequence at boot. Set to 0 for a module whose
// message set is already saved to its flash.
#ifndef GNSS_INIT_LC29H
#define GNSS_INIT_LC29H 1
#endif

// Persist the configured message set with $PQTMSAVEPAR. Off by default: the
// sequence is cheap to repeat each boot and this writes to flash every time.
#ifndef LC29H_SAVE_TO_FLASH
#define LC29H_SAVE_TO_FLASH 0
#endif

// Navigation mode for $PAIR080.
//
// MUST be 0 (Normal) for RTK. Mode 1 ("Fitness") reads as the obvious choice
// for a handheld - the specification describes it as tuned for walking and for
// reducing noise from a carried device - but it SILENTLY DISABLES RTK: the
// module accepts RTCM, reports no differential age, and stays on a SINGLE fix
// forever. Measured on an LC29HDANR11A03S: switching from mode 1 to mode 0
// moved it from SINGLE to FLOAT within 17 seconds, corrections otherwise
// unchanged. Nothing in the documentation mentions this interaction.
//
// 4 (Stationary) is untested here and likely to behave the same way; treat any
// non-zero mode as suspect until proven against a live correction stream.
#ifndef LC29H_NAV_MODE
#define LC29H_NAV_MODE 0
#endif

// Position fix interval in ms. 1 Hz is sufficient for walking navigation
// (~1.4 m between fixes) and is asserted here rather than trusting whatever a
// module arrived configured with. The (DA) is reported to refuse rates above
// 1 Hz; on some variants a change needs $PQTMSAVEPAR and a restart.
#ifndef LC29H_FIX_INTERVAL_MS
#define LC29H_FIX_INTERVAL_MS 1000
#endif

#define GNSS_CMD_WAIT_MS 300  // per-command acknowledgement window
// While the module has not acknowledged anything, how often to try again.
#define GNSS_PROBE_MS 10000

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------
// A per-board suffix is appended (the last two octets of the radio address), so
// two rovers in the same field are distinguishable in a scan list.
#define BLE_DEVICE_NAME "RTK-Rover"

#define BLE_TX_POWER_DBM 9  // -12..+20; higher = more range, more current
#define BLE_ENABLE_BONDING 1

// --- NMEA over Nordic UART -----------------------------------------------
// The receiver's sentence stream goes out on NUS, and whatever the phone writes
// back - RTCM3 corrections - goes to the receiver. See docs/PROTOCOL.md.

// Synthesize a $GNGST from the receiver's $PQTMEPE error estimate, for apps
// that read accuracy from GST (GNSS Master among them). Only used when the
// receiver emits no GST of its own: firmware A04S and later do, and a real one
// is both better and the only one that should be in the stream.
#define SYNTH_GST 1

// Correction diagnostics as proprietary $PRTKA / $PRTKM sentences on the same
// NMEA stream, every DIAG_INTERVAL_MS. Attaching to the USB console resets this
// board, which restarts the receiver's navigation filter and destroys RTK
// convergence - so a survey cannot be watched over USB at all, and the only
// way to see what the corrections contain is to send it over the air.
// Apps ignore sentences they do not know; one showing raw NMEA displays them.
#ifndef ENABLE_DIAG_NMEA
#define ENABLE_DIAG_NMEA 1
#endif
#define DIAG_INTERVAL_MS 10000
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_CHAR_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // phone writes
#define NUS_TX_CHAR_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // phone reads

#define BLE_NOTIFY_MAX 244   // payload at MTU 247; clamped to the real MTU
// Corrections arrive in bursts, not at their average rate: an NTRIP client
// that stalls and catches up delivers a backlog in one go. 4 KB was too small
// and silently dropped roughly half of a real stream.
#define BLE_DOWNLINK_BUF 16384

// ---------------------------------------------------------------------------
// Link behavior
// ---------------------------------------------------------------------------
// Whether to keep advertising while a client is connected.
//
// Useful on the bench - a second client can attach for diagnostics while the
// survey app holds the link. Off in the field: an app that keeps scanning sees
// a connected device still advertising, concludes it is connectable, and tries
// to connect to a device it is already connected to - which drops and remakes
// the link over and over.
#ifndef ADVERTISE_WHILE_CONNECTED
#define ADVERTISE_WHILE_CONNECTED 0
#endif

// Connection parameters, in BLE units: interval 1.25 ms, timeout 10 ms.
//
// The supervision timeout is the one that bites. At 4 s a handful of missed
// connection events - another radio busy, the phone dozing, a long correction
// burst - drops the link, and the app then has to rediscover and resubscribe.
// 6 s costs nothing and rides out a hiccup. The interval stays short because
// corrections arrive in bursts and want the bandwidth.
#define BLE_CONN_MIN_INTERVAL 12   // 15 ms
#define BLE_CONN_MAX_INTERVAL 24   // 30 ms
#define BLE_CONN_LATENCY 0
#define BLE_CONN_TIMEOUT 600       // 6 s

// ---------------------------------------------------------------------------
// WiFi provisioning
// ---------------------------------------------------------------------------
// The phone normally fetches the corrections, so the device needs WiFi only for
// setup, the debug panel, and the optional device-side NTRIP client. The portal
// costs about 35 mA - over half again on top of the 62 mA the rest of the
// device draws (measured, docs/HARDWARE.md). So WiFi stays off, and the portal
// is asked for deliberately and closes itself again.
//
// Hold the BOOT button while powering on to open it. Nothing else uses that
// button once running, so this needs no extra hardware and will not happen by
// accident in a pocket.
#ifndef ENABLE_WIFI_PROVISIONING
#define ENABLE_WIFI_PROVISIONING 1
#endif

// Bench setting: bring the portal up at every boot and never time it out.
// Entering it otherwise means holding BOOT while power is applied, which needs
// a power button a test rig may not have. The bench build turns it on; keep it
// off for anything running on the cell - always-on WiFi is the largest load on
// the board by far.
#ifndef PROV_ALWAYS_ON
#define PROV_ALWAYS_ON 0
#endif

#define PROV_BUTTON_PIN 9          // BOOT on the XIAO ESP32-C6, low when held

// Mode selector: which end of the correction path holds the NTRIP account.
// Polled live rather than read at boot, so it can be changed without a reset.
// Recessed in the enclosure - a strap across a breast pocket must not be able
// to move it mid-survey.
#define PIN_MODE_SWITCH 1          // D1, INPUT_PULLUP; low = device-side NTRIP

// Status LEDs: three discrete SMD parts, one GPIO each, not an addressable
// strip. Red, yellow and green - deliberately not blue. Blue and pure-green
// dice have a forward voltage of about 3.0-3.2 V, and driven from a 3.3 V pin
// there is no headroom left: they come out dim and get dimmer as the cell
// sags. Red, yellow and standard green sit near 2 V and drive properly.
//
// Series resistors, at roughly 8 mA: 150R for red and yellow, 100R for green.
// A 5050 will take far more, but the point here is to be seen in daylight for
// a moment, not to light the field, and the cell has better uses.
#define PIN_LED_RED 21             // D3  - no fix, or corrections missing
#define PIN_LED_YELLOW 2           // D2  - acquiring, single or SBAS
#define PIN_LED_GREEN 22           // D4  - RTK float (blinking) or fixed (solid)

// Float is only shown as plain green when the receiver rates it within this
// (horizontal, from GST or EPE); worse, and yellow follows the green. If the
// position has nonetheless held within FLOAT_STEADY_SCATTER_M rms over the last
// SCATTER_WINDOW epochs (at least FLOAT_SCATTER_MIN_SAMPLES of them), the yellow
// is a long pulse instead: the estimate is probably pessimistic.
#define FLOAT_GOOD_ACC_M 1.0f
#define FLOAT_STEADY_SCATTER_M 0.5f
#define FLOAT_SCATTER_MIN_SAMPLES 10

// Point recording (see points.h): a momentary push button from D8 to GND,
// reachable through the enclosure. D8 is no strapping pin, so holding it while
// power comes on cannot change how the chip boots.
#define PIN_MARK_BUTTON 19         // D8, INPUT_PULLUP; low = pressed
#define MARK_DEBOUNCE_MS 30
#define MARK_REPEAT_MS 1500        // presses closer together count once
#define POINTS_FILE "/points.csv"
// A press starts an occupation: settle, then average and log (see points.h).
// For the averaged position alone a short occupation is plenty once fixed;
// the length matters for post-processing, where a minute of dual-frequency
// data on a short baseline is about the least that resolves reliably.
#define POINT_SETTLE_MS 10000
#define POINT_OCCUPY_MS 60000
// Log the receiver's raw observations during an occupation, for PPK.
#define POINTS_LOG_RAW 1
// What one occupation's raw log may take: MSM4 for four constellations on two
// bands runs at roughly 1-2 kB/s. Below this much free space the point is
// still taken, just without its raw log.
#define POINTS_RAW_RESERVE_BYTES ((POINT_SETTLE_MS + POINT_OCCUPY_MS) / 1000 * 2048)
// How stale "the receiver is talking" may be when a recording starts. The
// receiver reports at 1 Hz, so this tolerates one late epoch and nothing more.
#define POINTS_MAX_AGE_MS 2500
// Refuse a point rather than fill the file system to the last block.
#define POINTS_MIN_FREE_BYTES 8192

// Kept so a board with one LED still builds; it maps to the green.
#define PIN_STATUS_LED PIN_LED_GREEN
#define PROV_AP_PREFIX "RTK-Rover-"
#define PROV_AP_PASSWORD "rtkrover"  // WPA2; an open portal is reachable by anyone nearby
#define PROV_TIMEOUT_MS (10UL * 60 * 1000)   // radio off after this, used or not
// Debug mode suspends that deadline entirely - a diagnosis must not be cut off
// halfway - so it is the operator who ends the session, not the timer.
#define PROV_NVS_NAMESPACE "rtkwifi"

// The one name the portal's DNS answers. See notAGateway() in provisioning.cpp:
// a phone on the rover's network must keep its mobile data, so nothing else
// resolves to us.
#define PROV_HOSTNAME "rtk.setup"


// Device-side NTRIP (CRED_DEVICE mode). See ntrip.h for why a refusal is never
// retried: a caster that sees an account hammering it bans the account.
#define NTRIP_DEFAULT_PORT 2101
#define NTRIP_CONNECT_TIMEOUT_MS 8000    // DNS + TCP over a phone hotspot
#define NTRIP_RESPONSE_TIMEOUT_MS 10000
#define NTRIP_GGA_INTERVAL_MS 10000      // a VRS needs to know where we are
#define NTRIP_STALL_MS 30000             // connected but silent: reconnect
#define NTRIP_WIFI_RETRY_MS 15000
#define NTRIP_BACKOFF_MIN_MS 15000
#define NTRIP_BACKOFF_MAX_MS 120000
// Consecutive sessions that delivered (almost) nothing before the client stops
// for good, until the settings or the mode change or the device restarts.
#define NTRIP_MAX_FAILURES 5
#define NTRIP_GOOD_SESSION_BYTES 1024
#define NTRIP_BUFFER_BYTES 4096          // ~4 s of a busy MSM7 stream

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------
// The XIAO ESP32-C6 has no battery divider of its own: an external 100k/100k
// from the switched cell feed halves it into A0 (D0/GPIO0), so millivolts =
// analogReadMilliVolts(A0) * 2. On the switched side, it draws nothing when off.
//
// Reported to clients as the Bluetooth SIG Battery Service, which phones and
// mapping apps already understand - a level shows up without app support.
#define BATTERY_ADC_PIN A0

#define BATTERY_SAMPLE_MS 5000   // how often to sample
#define BATTERY_NOTIFY_STEP 1    // notify when the level moves by this many %

// Outside this range the reading is not a cell: on a bench board without one
// the charger still drives the BAT rail, so a plausibility check is the only way
// to tell "no battery" from "full battery".
#define BATTERY_MIN_PLAUSIBLE_MV 2500
#define BATTERY_MAX_PLAUSIBLE_MV 4400

#define BATTERY_UNKNOWN 0xFF

// Bluetooth SIG Battery Service
#define SVC_BATTERY_UUID "180F"
#define CH_BATTERY_LEVEL "2A19"  // uint8 percent

// ---------------------------------------------------------------------------
// Buffering
// ---------------------------------------------------------------------------
#define UART_RX_BUF 4096
#define UART_TX_BUF 2048
#define BRIDGE_CHUNK 512

// ---------------------------------------------------------------------------
// Debug / LED
// ---------------------------------------------------------------------------
#define DEBUG_USB 1

// Transparent USB <-> GNSS bridge, for updating the receiver's own firmware.
//
// The production board has no USB of its own - only RX, TX, GND and power - so
// once it is in a sealed enclosure the ESP32 is the only route to it. Without
// this, a fielded unit could never take a GNSS firmware update.
#define BRIDGE_ENABLE 1

// Pinned, never negotiated. The ESP32-C6 has USB Serial/JTAG rather than full
// USB-OTG, so a host baud change never reaches the application and the bridge
// cannot follow one. Flash with the 115200 download agent and stay there: a
// 4 MB image takes about six minutes instead of one, which is a fair price for
// not needing USB-OTG.
#define BRIDGE_BAUD 115200

// Typed by the operator, or sent by a script, on the USB console.
#define BRIDGE_COMMAND "$PRTKFLASH"

// Debug output drops rather than blocks (see printStats), which would silently
// corrupt a firmware image. While bridging, the host is definitely reading, so
// writes may wait.
#define BRIDGE_TX_TIMEOUT_MS 1000

// GPIO driving the load switch on the module's supply, or -1 if it is wired
// permanently on. Quectel recommends MCU control of VCC in any case, both to
// save power and to restart a module that has hung. Flashing needs the module
// freshly powered while the tool handshakes, and with no reset pin on the
// production header, cutting its power is the only way to arrange that.
//
// Active high because of the gate driver, which is also not optional. The switch
// is a high-side P-FET with its source on the cell, and the cell sits up to
// 4.2 V - a 3.3 V GPIO on the gate would leave Vgs at -0.9 V, partly on. An
// N-FET or NPN pulls the P-FET gate down when the GPIO is high.
//
// On the carrier board this is D5 (GPIO23): D3, the obvious spare, is the
// red LED.
#ifndef PIN_GNSS_POWER
#define PIN_GNSS_POWER -1
#endif
#define GNSS_POWER_ACTIVE_HIGH 1

// How long the module's supply needs to settle before its boot ROM is ready.
#define BRIDGE_POWER_SETTLE_MS 120

// Echo raw receiver bytes to the USB console. Diagnostic only - it competes
// with the status lines and is noisy at 1 Hz with GSV enabled.
#ifndef DEBUG_NMEA_ECHO
#define DEBUG_NMEA_ECHO 0
#endif
#define DEBUG_STATS_MS 5000

#define USE_STATUS_LED 1
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1  // XIAO ESP32-C6 user LED is active low
#endif
