// RTK GNSS rover -> phone, over Bluetooth LE.
//
//   receiver TX --> UART --> NMEA --> Nordic UART ---> phone
//                        \-> parse: GST synthesis
//   receiver RX <-- UART <----------- corrections <--- phone
//
// The phone runs a GNSS app that reads an NMEA sentence stream over Nordic
// UART (GNSS Master, SW Maps, Lefebure) and typically shares the position with
// every other app as a mock location. See docs/PROTOCOL.md.
//
// The correction path is byte-transparent: whatever the phone writes - RTCM3
// frames from its own NTRIP client, or ASCII configuration lines - is handed to
// the receiver untouched.

#include <Arduino.h>
#include <math.h>

#include "battery.h"
#include "ble_server.h"
#include "config.h"
#include "gnss_lc29h.h"
#include "nmea.h"

static HardwareSerial GnssSerial(GNSS_UART_NUM);
static NmeaParser parser;
static uint32_t bytesFromGnss = 0;
static uint32_t bytesToGnss = 0;
static uint32_t epochs = 0;

#if DEBUG_USB
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Status LED: slow blink while waiting for a phone, brief blip once linked.
// ---------------------------------------------------------------------------
#if USE_STATUS_LED && defined(LED_BUILTIN)
static inline void ledSet(bool on) {
#if LED_ACTIVE_LOW
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
#else
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
#endif
}
static void ledInit() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledSet(false);
}

static void ledTask() {
  const bool linked = bleRawActive();
  const uint32_t period = linked ? 2000 : 600;
  const uint32_t on = linked ? 60 : 300;
  ledSet((millis() % period) < on);
}
#else
static void ledInit() {}
static void ledTask() {}
#endif

// ---------------------------------------------------------------------------
// Receiver -> phone
// ---------------------------------------------------------------------------
static bool pumpUplink() {
  static uint8_t buf[BRIDGE_CHUNK];
  size_t n = 0;

  size_t limit = bleMaxChunk();
  if (limit > sizeof(buf)) limit = sizeof(buf);
  if (limit == 0) limit = sizeof(buf);

  while (GnssSerial.available() && n < limit) buf[n++] = (uint8_t)GnssSerial.read();
  if (n == 0) return false;
  bytesFromGnss += n;

  // Parse unconditionally, even with nobody listening: the parser must stay in
  // sync with the sentence stream, and a client that subscribes mid-burst then
  // gets a complete fix immediately rather than after the next epoch.
#if DEBUG_NMEA_ECHO
  Serial.write(buf, n);
#endif

  uint8_t updated = parser.feed(buf, n);
  if (updated & (NMEA_UPD_POS | NMEA_UPD_STATE)) epochs++;
  // Raw mirror is a straight passthrough - it must not be reframed, so it goes
  // out in whatever chunks the UART delivered.
  bleSendRaw(buf, n);

#if SYNTH_GST
  // Only ever at a sentence boundary: a UART chunk can end mid-sentence, and
  // injecting there would corrupt the sentence the client is assembling.
  if ((updated & NMEA_UPD_ACC) && !parser.haveRealGst() && bleRawActive() &&
      buf[n - 1] == '\n') {
    char gst[128];
    size_t g = nmeaBuildGst(gst, sizeof(gst), parser.fix());
    if (g) bleSendRaw((const uint8_t *)gst, g);
  }
#endif
  return true;
}

// ---------------------------------------------------------------------------
// Phone -> receiver
// ---------------------------------------------------------------------------
static bool pumpDownlink() {
  uint8_t buf[BRIDGE_CHUNK];
  bool moved = false;

  // Drain until the queue is empty or the UART will not take more. One chunk
  // per loop iteration was not enough: corrections arrive in bursts rather than
  // at their average rate, and the queue overflowed while we trickled it out.
  for (;;) {
    size_t room = GnssSerial.availableForWrite();
    if (room == 0) break;  // let the UART drain, resume next loop
    if (room > sizeof(buf)) room = sizeof(buf);

    size_t n = bleReadDownlink(buf, room);

    if (n == 0) break;

    GnssSerial.write(buf, n);
    bytesToGnss += n;
    moved = true;
  }
  return moved;
}

// ---------------------------------------------------------------------------
#if DEBUG_USB
static void printStats() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < DEBUG_STATS_MS) return;
  last = now;

  // Nothing is attached in the field, and a USB-CDC write with no host does
  // not vanish - it blocks until it times out. These are the two longest lines
  // the firmware emits, so on battery they buy nothing and cost main-loop time
  // that belongs to the receiver and the radio.
  //
  // `!Serial` is not enough on its own: a port that is merely enumerated - a
  // laptop with no terminal open, which is how this is usually bench-tested -
  // reads as connected, and every write then blocks until it times out. That
  // cost 4 s per stats line, measured, during which nothing was published over
  // BLE at all. So refuse to write unless the buffer can actually take it.
  if (!Serial || Serial.availableForWrite() < 256) return;

  const GnssFix &f = parser.fix();
  uint32_t dl = 0, dlDrop = 0;
  bleDownlinkStats(&dl, &dlDrop);
  char batt[24];
  if (batteryPresent())
    snprintf(batt, sizeof(batt), "%u%% %umV", batteryPercent(), batteryMillivolts());
  else
    snprintf(batt, sizeof(batt), "no battery");

  Serial.printf("[STAT] %s  epochs %lu  %s  sats %u  acc %.3f/%.3f/%.3f m  "
                "batt %s  rx %lu B  tx %lu B  dl %lu B (dropped %lu)  "
                "notify-fail %lu\n",
                bleRawActive() ? "streaming" : "waiting",
                (unsigned long)epochs, posTypeName(f.type), f.sats,
                f.latStd, f.lonStd, f.altStd,
                batt, (unsigned long)bytesFromGnss, (unsigned long)bytesToGnss,
                (unsigned long)dl, (unsigned long)dlDrop,
                (unsigned long)bleNotifyFailures());

}
#endif

void setup() {
  ledInit();

  Serial.begin(115200);
  // Debug output must never be able to stall a survey: drop it instead of
  // waiting for a host that may not be listening.
  Serial.setTxTimeoutMs(0);
#if DEBUG_USB
  // On USB-CDC boards the port only exists once the host enumerates it, well
  // after boot. Bounded, so a headless unit still starts.
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 1500;) delay(10);
#endif

  GnssSerial.setRxBufferSize(UART_RX_BUF);
  GnssSerial.setTxBufferSize(UART_TX_BUF);
  GnssSerial.begin(GNSS_BAUD, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);

  // Before the radio, not after: the receiver echoes its replies, and a phone
  // that connected early would otherwise be fed the command transcript.
  gnssInit(GnssSerial);

  batteryBegin();
  bleBegin();

  DBG("\n[SYS] %s - UART%d @ %d (RX=GPIO%d, TX=GPIO%d)\n", bleDeviceName(),
      GNSS_UART_NUM, GNSS_BAUD, PIN_GNSS_RX, PIN_GNSS_TX);
}

void loop() {
  gnssInitTask(GnssSerial);
  bool busy = pumpUplink();
  busy |= pumpDownlink();
  ledTask();
  batteryTask();
  blePublishBattery(batteryPercent());
#if DEBUG_USB
  printStats();
#endif

  // Yield when idle: a tight loop() starves the idle task on this core and
  // eventually trips the task watchdog.
  if (!busy) delay(1);
}
