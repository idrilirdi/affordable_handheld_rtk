// Quectel LC29H(DA) - configure as an RTK rover at boot, so the receiver
// needs no PC-side setup before a survey.
//
// The LC29H family speaks two proprietary command sets carried as NMEA
// sentences: $PQTM (Quectel) and $PAIR (the Airoha chipset underneath). Both
// require a real NMEA checksum, so it is computed here rather than hardcoded.

#include "gnss_lc29h.h"

#include <string.h>

#include "config.h"

#if DEBUG_USB
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

#if GNSS_INIT_LC29H

// Message set for an RTK rover feeding a phone app over NMEA:
//
//   GGA      position, fix quality, satellites   (required)
//   ZDA      UTC date and time, 4-digit year     (preferred time source)
//   RMC      UTC date and time                   (fallback)
//   GST      per-axis standard deviations            (A04S firmware and later)
//   PQTMEPE  north/east/down error estimate           (fallback for older ones)
//   GSA/GSV  satellite detail; 1 Hz max on (DA) whatever rate is configured
//   GLL/VTG  redundant here - disabled to save UART bandwidth
//
// GST arrived in firmware LC29HDANR11A04S: before that the protocol
// specification stated "GST is not supported on LC29H (BA, CA, DA, EA)" and
// the module rejected the request outright. Both are asked for, and the parser
// prefers GST where it exists - it reports genuine per-axis standard
// deviations, where $PQTMEPE is an estimate that read 7.7 m against 0.09 m of
// measured scatter. An older module simply answers ERROR to the GST line and
// carries on with EPE.
//
// The <MsgVer> field is mandatory for $PQTM messages - omit it and the module
// answers ERROR - hence the trailing ",2" on the PQTMEPE line.
#define LC29H_STR2(x) #x
#define LC29H_STR(x) LC29H_STR2(x)

static const char *const kCommands[] = {
    // Navigation mode. Keep this at 0: mode 1 ("Fitness") disables RTK on this
    // module - see the warning in config.h.
    "PAIR080," LC29H_STR(LC29H_NAV_MODE),
    // Static navigation hold OFF. With it on, the reported position freezes
    // below a speed threshold - which for a survey device means standing still
    // to record a point can log a stale position. The factory default is 0
    // (disabled), but $PQTMSAVEPAR persists settings, so a module that has been
    // configured by someone else can arrive with it enabled. Assert it.
    "PAIR070,0",
    "PQTMCFGFIXRATE,W," LC29H_STR(LC29H_FIX_INTERVAL_MS),
    "PQTMCFGMSGRATE,W,GGA,1",
    "PQTMCFGMSGRATE,W,ZDA,1",
    "PQTMCFGMSGRATE,W,RMC,1",
    "PQTMCFGMSGRATE,W,GSA,1",
    "PQTMCFGMSGRATE,W,GSV,1",
    "PQTMCFGMSGRATE,W,GLL,0",
    "PQTMCFGMSGRATE,W,VTG,0",
    "PQTMCFGMSGRATE,W,GST,1",
    "PQTMCFGMSGRATE,W,PQTMEPE,1,2",
};

static bool answering = false;

// NMEA checksum: XOR of everything between '$' and '*'. Returns true if the
// module acknowledged it, accepted or not - either way it heard us. The NMEA
// stream does not count: it flows whether or not our TX reaches the module.
static bool sendCmd(HardwareSerial &ser, const char *body, uint32_t waitMs) {
  uint8_t cs = 0;
  for (const char *p = body; *p; ++p) cs ^= (uint8_t)*p;

  char line[128];
  int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
  if (n <= 0 || n >= (int)sizeof(line)) {
    DBG("[GNSS] command too long, skipped: %s\n", body);
    return false;
  }

  while (ser.available()) ser.read();  // discard anything already queued
  ser.write((const uint8_t *)line, n);
  ser.flush();
  DBG("[>] $%s*%02X\n", body, cs);

  // The acknowledgement is interleaved with the ordinary NMEA stream, so scan
  // the window for it rather than assuming the first line back is the reply.
  // Two acknowledgement styles: $PQTM commands answer ",OK" / ",ERROR", while
  // $PAIR commands answer "$PAIR001,<CommandID>,<Result>" with 0 for success.
  char resp[512];
  size_t len = 0;
  bool ok = false, err = false, version = false;
  char code = '?';
  for (uint32_t t0 = millis(); millis() - t0 < waitMs;) {
    while (ser.available() && len < sizeof(resp) - 1) resp[len++] = (char)ser.read();
    resp[len] = '\0';

    if (strstr(resp, ",OK")) { ok = true; break; }
    if (strstr(resp, "$PQTMVERNO,")) { version = true; }
    if (strstr(resp, ",ERROR")) { err = true; break; }

    const char *ack = strstr(resp, "$PAIR001,");
    if (ack) {
      const char *result = strchr(ack + 9, ',');
      if (result && result[1]) {
        code = result[1];
        if (code == '0') ok = true; else err = true;
        break;
      }
    }
    delay(5);
  }

  if (ok) {
    DBG("[<] OK\n");
  } else if (err) {
    DBG("[<] rejected (result %c): %s\n", code, body);
  } else if (len) {
    // Not every command answers ",OK" - $PQTMVERNO replies with the version
    // string itself, and that is exactly what we wanted to see.
    for (size_t i = 0; i < len; ++i)
      if (resp[i] == '\r' || resp[i] == '\n') resp[i] = ' ';
    DBG("[<] %s\n", resp);
  } else {
    DBG("[<] (no reply)\n");
  }
  return ok || err || version;
}

static void configure(HardwareSerial &ser) {
  bool heard = false;
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i)
    heard |= sendCmd(ser, kCommands[i], GNSS_CMD_WAIT_MS);

#if LC29H_SAVE_TO_FLASH
  heard |= sendCmd(ser, "PQTMSAVEPAR", 2000);
#endif
  answering = heard;
}

void gnssInit(HardwareSerial &ser) {
  DBG("\n[GNSS] LC29H rover init @ %d baud\n", GNSS_BAUD);

  delay(1500);  // the module emits a boot banner and ignores commands until done
  while (ser.available()) ser.read();

  // Returns the firmware version string, which names the variant - the quickest
  // way to confirm a (DA) rover rather than a (BS), which is a base station and
  // never produces a rover fix.
  sendCmd(ser, "PQTMVERNO", 1000);
  configure(ser);

  if (answering)
    DBG("[GNSS] ready - RTCM3 written to this UART is consumed directly\n");
  else
    DBG("[GNSS] NO ANSWER to any command - check the wire to the module's RX (D6)\n");
}

bool gnssAnswering() { return answering; }

void gnssInitTask(HardwareSerial &ser) {
  static uint32_t lastProbe = 0;
  if (answering || millis() - lastProbe < GNSS_PROBE_MS) return;
  lastProbe = millis();
  // Blocks for the probe window, and the bytes it reads are lost to the parser.
  // Only ever while the module is not hearing us, when nothing works anyway.
  if (!sendCmd(ser, "PQTMVERNO", 500)) return;
  DBG("[GNSS] module answers now - running setup again\n");
  configure(ser);
}

#else
void gnssInit(HardwareSerial &) {}
bool gnssAnswering() { return true; }
void gnssInitTask(HardwareSerial &) {}
#endif
