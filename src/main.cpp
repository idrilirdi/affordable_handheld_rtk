// RTK GNSS rover -> phone, over Bluetooth LE.
//
//   receiver TX --> UART --> NMEA --> Nordic UART ---> phone
//                        \-> parse: LEDs, points, diagnostics
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
#include "diag.h"
#include "gnss_lc29h.h"
#include "nmea.h"
#include "bridge.h"
#include "provisioning.h"
#include "status_led.h"
#include "ntrip.h"
#include "points.h"
#include "uplink_split.h"
#include "rtcm.h"
#include "scatter.h"

static HardwareSerial GnssSerial(GNSS_UART_NUM);
static NmeaParser parser;
// Spread of the float position only: single and fixed positions would mix two
// different solutions into one number.
static Scatter floatScatter;

static UplinkSplit uplinkSplit;
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
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);

  // Lamp check at power-up: each lamp alone, in board order, so a dead or
  // swapped LED is obvious before anyone trusts what they show.
  const int lamps[] = {PIN_LED_RED, PIN_LED_YELLOW, PIN_LED_GREEN};
  for (int pin : lamps) {
    digitalWrite(pin, HIGH);
    delay(400);
    digitalWrite(pin, LOW);
  }
}

// The three status lamps. Driven from the same fix the rest of the firmware
// reads, so what a bystander sees and what the panel says cannot disagree.
//
// The portal pattern only while the portal has the device to itself. With the
// rover surveying alongside it - the bench build, debug mode - the fix is what
// matters, and a portal pattern on top hid it entirely.
static void statusLedTask() {
  Lamps l;
  // The answer to a button press outranks the status for its second.
  if (!pointsLamps(&l)) l = statusPattern(
      statusOf(parser.fix(), parser.fix().diffAge, batteryPercent(),
               provisioningExclusive(), floatScatter.rms(FLOAT_SCATTER_MIN_SAMPLES)),
      millis());
  digitalWrite(PIN_LED_RED, l.red);
  digitalWrite(PIN_LED_YELLOW, l.yellow);
  digitalWrite(PIN_LED_GREEN, l.green);
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

  static uint8_t in[BRIDGE_CHUNK];
  size_t got = 0;
  while (GnssSerial.available() && got < limit) in[got++] = (uint8_t)GnssSerial.read();
  if (got == 0) return false;
  bytesFromGnss += got;
  // RTCM out of the receiver - only while a point is being recorded - goes to
  // the point log; everything below sees the NMEA alone.
  n = uplinkSplit.feed(in, got, buf, pointsOnRtcm);
  if (n == 0) return true;

  // Parse unconditionally, even with nobody listening: the parser must stay in
  // sync with the sentence stream, and a client that subscribes mid-burst then
  // gets a complete fix immediately rather than after the next epoch.
#if DEBUG_NMEA_ECHO
  Serial.write(buf, n);
#endif

  uint8_t updated = parser.feed(buf, n);
  if (updated & (NMEA_UPD_POS | NMEA_UPD_STATE)) epochs++;
  // On every GGA, so losing the fix withdraws the position as well.
  if (updated & (NMEA_UPD_POS | NMEA_UPD_STATE)) ntripSetGga(parser.lastGga());
  if (updated & (NMEA_UPD_POS | NMEA_UPD_STATE)) {
    const GnssFix &f = parser.fix();
    if (f.havePos && f.type == PosType::Float) floatScatter.add(f.lat, f.lon);
    else floatScatter.clear();
    pointsOnEpoch(f);
  }
  // Raw mirror is a straight passthrough - it must not be reframed, so it goes
  // out in whatever chunks the UART delivered.
  bleSendRaw(buf, n);

#if ENABLE_DIAG_NMEA
  // Diagnostics ride the same stream, and only at a sentence boundary for the
  // same reason the synthesized GST does.
  static uint32_t lastDiag = 0;
  if (bleRawActive() && buf[n - 1] == '\n' && millis() - lastDiag >= DIAG_INTERVAL_MS) {
    lastDiag = millis();
    char line[96];
    for (uint8_t i = 0; i < 1 + rtcmTypeCount(); ++i) {
      size_t len = diagSentence(line, sizeof(line), i, parser.fix());
      if (!len) break;
      bleSendRaw((const uint8_t *)line, len);
    }
  }
#endif

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

    // One source, chosen by the mode, never both: two streams interleaved at
    // chunk boundaries would split frames and corrupt both. In device mode
    // anything the phone writes is discarded unread rather than queued.
    size_t n;
    if (credMode() == CRED_DEVICE) {
      uint8_t junk[64];
      while (bleReadDownlink(junk, sizeof(junk))) {}
      n = ntripRead(buf, room);
    } else {
      n = bleReadDownlink(buf, room);
    }

    if (n == 0) break;

    rtcmFeed(buf, n);
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
                "frames ok %lu bad %lu  notify-fail %lu\n",
                bleRawActive() ? "streaming" : "waiting",
                (unsigned long)epochs, posTypeName(f.type), f.sats,
                f.latStd, f.lonStd, f.altStd,
                batt, (unsigned long)bytesFromGnss, (unsigned long)bytesToGnss,
                (unsigned long)dl, (unsigned long)dlDrop,
                (unsigned long)rtcmFramesOk(), (unsigned long)rtcmFramesBad(),
                (unsigned long)bleNotifyFailures());

  // What the corrections actually contain. A stream can be intact and still be
  // unusable: the rover tracks L1 and L5, so RTCM signal IDs 22/23/24 (the L5
  // band) are what let it fix. A base sending only the L1 and L2 bands leaves
  // one usable frequency, and one frequency floats.
  char age[24];
  if (f.diffAge >= 0) snprintf(age, sizeof(age), "%.1f s", f.diffAge);
  else snprintf(age, sizeof(age), "none");
  // Where the base actually is. For a network VRS that should be meters from
  // us, because the VRS is synthesized at the position we send upstream - so a
  // baseline in kilometers means the client sent a stale or wrong position,
  // and no amount of correction quality will fix that.
  char base[64];
  double bLat, bLon, bAlt;
  if (rtcmBasePosition(&bLat, &bLon, &bAlt) && f.havePos) {
    // Equirectangular is ample at these distances and costs no trigonometry
    // beyond one cosine.
    const double kMetersPerDeg = 111320.0;
    double dy = (bLat - f.lat) * kMetersPerDeg;
    double dx = (bLon - f.lon) * kMetersPerDeg * cos(f.lat * M_PI / 180.0);
    snprintf(base, sizeof(base), "%u @ %.6f,%.6f baseline %.0f m", f.baseId,
             bLat, bLon, sqrt(dx * dx + dy * dy));
  } else {
    snprintf(base, sizeof(base), "%u (position unknown)", f.baseId);
  }
  char msgs[320];
  rtcmSummary(msgs, sizeof(msgs));
  Serial.printf("[RTCM] corr age %s  base %s\n  [RTCM] %s\n", age, base, msgs);
  if (credMode() == CRED_DEVICE) {
    char nt[160];
    ntripStatus(nt, sizeof(nt));
    Serial.printf("[%s]\n", nt);
  }
}
#endif

void setup() {
  ledInit();

  // Before anything else: holding BOOT at power-on asks for the WiFi portal,
  // and that decision has to be made while the button is still held. When it
  // is not wanted the radio is switched off rather than left idle.
  const bool portal = provisioningBegin();

  // Unconditional, even when nothing is printed: the USB console is the only
  // way to reach the GNSS module for a firmware update once this is sealed in
  // an enclosure, and a build that cannot be bridged cannot be updated.
  Serial.begin(115200);
  // Debug output must never be able to stall a survey: drop it instead of
  // waiting for a host that may not be listening. The bridge restores a real
  // timeout while it is active, where dropping would corrupt an image.
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
  bridgeBegin(GnssSerial);
  gnssInit(GnssSerial);

  batteryBegin();
  pointsBegin();
  bleBegin();
  ntripBegin();

  DBG("\n[SYS] %s - UART%d @ %d (RX=GPIO%d, TX=GPIO%d)\n", bleDeviceName(),
      GNSS_UART_NUM, GNSS_BAUD, PIN_GNSS_RX, PIN_GNSS_TX);
  if (portal && PROV_ALWAYS_ON)
    DBG("[SYS] WiFi portal open (bench build: stays up, does not time out)\n");
  else if (portal)
    DBG("[SYS] WiFi portal open for %lu s\n", (unsigned long)(PROV_TIMEOUT_MS / 1000));
  else if (provisionedSsid()[0])
    DBG("[SYS] WiFi credentials stored for '%s' (radio off)\n", provisionedSsid());
}

void loop() {
  // Before anything else, and regardless of what else the device is doing: a
  // unit that cannot be put into bridge mode cannot have its receiver updated.
  if (bridgeActive()) {
    bridgeTask();
    return;
  }
  if (bridgePoll()) return;

  const uint32_t tLoopStart = millis();

  // A deliberate, bounded provisioning session owns the device: the radio is
  // busy and nobody is surveying. In debug mode, and on the bench build, the
  // portal only rides along and the survey has to keep running underneath it.
  if (provisioningActive()) {
    provisioningTask();
    if (provisioningExclusive()) {
      // Still polled, so a press gets its red "not saved" rather than nothing.
      pointsTask();
      ledTask();
      statusLedTask();
      return;
    }
    provisioningPublish(parser.fix(), parser.sky(), bytesFromGnss, bytesToGnss, epochs);
  }

  // Where the time actually goes, section by section.
  static uint32_t worst[PROV_PROFILE_SLOTS] = {0};
  uint32_t mark = tLoopStart;
  auto lap = [&](uint8_t slot, const char *name) {
    const uint32_t now = millis();
    if (now - mark > worst[slot]) worst[slot] = now - mark;
    mark = now;
    provisioningProfile(slot, name, worst[slot]);
  };

  lap(0, "portal");
  gnssInitTask(GnssSerial);
  // Raw observations only while a point is being recorded.
  static bool rawOn = false;
  if (pointsWantRaw() != rawOn) {
    rawOn = pointsWantRaw();
    gnssSetRawOutput(GnssSerial, rawOn);
  }
  bool busy = pumpUplink();
  lap(1, "uplink");
  busy |= pumpDownlink();
  lap(2, "downlink");
  ledTask();
  statusLedTask();
  lap(3, "led");
  batteryTask();
  lap(4, "batt");
  blePublishBattery(batteryPercent());
  lap(5, "blebatt");
#if DEBUG_USB
  printStats();
#endif
  lap(6, "stats");
  pointsTask();
  lap(7, "points");

  // Yield when idle: a tight loop() starves the idle task on this core and
  // eventually trips the task watchdog.
  if (!busy) delay(1);
}
