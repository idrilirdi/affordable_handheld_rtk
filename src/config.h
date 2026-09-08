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
