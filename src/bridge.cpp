#include "bridge.h"

#include "config.h"

#if BRIDGE_ENABLE

#include <Arduino.h>

static HardwareSerial *gnss = nullptr;
static bool active = false;
static bool powered = true;

// An unpowered module must not be driven. UART TX idles high, and 3.3 V on the
// RX pin of a module with no supply flows through its input protection into
// its rail - enough to half-power it, which can stop it resetting cleanly into
// the boot ROM and stresses the pin. So the lines are released before the
// supply goes, and the UART reattached only once it is back.
static void releaseLines() {
  gnss->end();
  pinMode(PIN_GNSS_TX, INPUT);
  pinMode(PIN_GNSS_RX, INPUT);
}

static void attachLines() {
  gnss->begin(BRIDGE_BAUD, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);
}

static void modulePower(bool on) {
#if PIN_GNSS_POWER >= 0
  digitalWrite(PIN_GNSS_POWER, (on == !!GNSS_POWER_ACTIVE_HIGH) ? HIGH : LOW);
#else
  (void)on;   // wired permanently on; the operator power-cycles by hand
#endif
  powered = on;
}

void bridgeBegin(HardwareSerial &g) {
  gnss = &g;
#if PIN_GNSS_POWER >= 0
  pinMode(PIN_GNSS_POWER, OUTPUT);
  modulePower(true);
#endif
}

static void enter() {
  // Debug output is configured to drop rather than block, which would quietly
  // put holes in a firmware image. The host is reading throughout a flash, so
  // here writes are allowed to wait.
  Serial.setTxTimeoutMs(BRIDGE_TX_TIMEOUT_MS);
  Serial.printf("\n[BRIDGE] transparent at %d baud. Reset the board to leave.\n",
                BRIDGE_BAUD);
  Serial.flush();

  // Held off until the host speaks. The boot ROM only answers the handshake in
  // a short window after power-up, so letting the tool's first byte trigger the
  // power-up removes the race that would otherwise have to be won by hand.
#if PIN_GNSS_POWER >= 0
  releaseLines();
  modulePower(false);
#else
  // No supply switch: the module stays powered and the operator resets it.
  gnss->end();
  attachLines();
#endif
  active = true;
}

bool bridgePoll() {
  if (active) return true;
  if (!Serial || !Serial.available()) return false;

  static char line[32];
  static uint8_t n = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[n] = '\0';
      n = 0;
      if (!strcmp(line, BRIDGE_COMMAND)) {
        enter();
        return true;
      }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    } else {
      n = 0;   // overlong: not the command, start again
    }
  }
  return false;
}

bool bridgeActive() { return active; }

void bridgeTask() {
  static uint8_t buf[256];

  if (Serial.available()) {
    if (!powered) {
      // First contact from the host: bring the module up so its boot ROM is
      // listening while the tool is still sending its handshake. Supply first,
      // lines second - the reverse of how they were taken away.
      modulePower(true);
      delay(BRIDGE_POWER_SETTLE_MS);
      attachLines();
    }
    size_t n = 0;
    while (Serial.available() && n < sizeof(buf)) buf[n++] = (uint8_t)Serial.read();
    gnss->write(buf, n);
  }

  if (gnss->available()) {
    size_t n = 0;
    while (gnss->available() && n < sizeof(buf)) buf[n++] = (uint8_t)gnss->read();
    Serial.write(buf, n);
  }
}

#else

void bridgeBegin(HardwareSerial &) {}
bool bridgePoll() { return false; }
bool bridgeActive() { return false; }
void bridgeTask() {}

#endif
