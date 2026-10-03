#include "ntrip.h"

#include "config.h"
#include "provisioning.h"

#if ENABLE_WIFI_PROVISIONING

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <base64.h>
#include <freertos/stream_buffer.h>

#include "hotspots.h"
#include "textbuf.h"

enum class State : uint8_t {
  Off, Unconfigured, WaitWifi, WaitFix, Connecting, Streaming, Backoff, Refused, GaveUp
};

static const char *stateName(State s) {
  switch (s) {
    case State::Off: return "off (phone mode)";
    case State::Unconfigured: return "no caster stored";
    case State::WaitWifi: return "waiting for hotspot";
    case State::WaitFix: return "waiting for a position";
    case State::Connecting: return "connecting";
    case State::Streaming: return "streaming";
    case State::Backoff: return "retrying";
    case State::Refused: return "REFUSED";
    case State::GaveUp: return "STOPPED after repeated failures";
  }
  return "?";
}

static StreamBufferHandle_t rtcmBuf = nullptr;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static char gga[100] = "";          // guarded by lock
static volatile bool restart = false;

// Written by the task, read by the panel. Torn reads of these cost at most one
// odd number on a diagnostic line, so they are not locked.
static volatile State state = State::Off;
static uint32_t bytesIn = 0, dropped = 0, sessions = 0, lastDataAt = 0;
static char lastError[64] = "";

void ntripLoadConfig(NtripConfig *c) {
  Preferences p;
  p.begin(PROV_NVS_NAMESPACE, true);
  // getString returns 0 and leaves the buffer alone when the key is missing.
  if (!p.getString("n_host", c->host, sizeof(c->host))) c->host[0] = '\0';
  if (!p.getString("n_mount", c->mount, sizeof(c->mount))) c->mount[0] = '\0';
  if (!p.getString("n_user", c->user, sizeof(c->user))) c->user[0] = '\0';
  if (!p.getString("n_pass", c->pass, sizeof(c->pass))) c->pass[0] = '\0';
  c->port = p.getUShort("n_port", NTRIP_DEFAULT_PORT);
  p.end();
}

void ntripSaveConfig(const NtripConfig &c) {
  Preferences p;
  p.begin(PROV_NVS_NAMESPACE, false);
  p.putString("n_host", c.host);
  p.putUShort("n_port", c.port);
  p.putString("n_mount", c.mount);
  p.putString("n_user", c.user);
  p.putString("n_pass", c.pass);
  p.end();
}

bool ntripConfigured(const NtripConfig &c) { return c.host[0] && c.mount[0] && c.port; }

void ntripRestart() { restart = true; }

void ntripSetGga(const char *s) {
  portENTER_CRITICAL(&lock);
  strlcpy(gga, s, sizeof(gga));
  portEXIT_CRITICAL(&lock);
}

size_t ntripRead(uint8_t *buf, size_t max) {
  if (!rtcmBuf) return 0;
  return xStreamBufferReceive(rtcmBuf, buf, max, 0);
}

size_t ntripStatus(char *out, size_t max) {
  const State s = state;
  size_t n = appendf(out, max, 0, "NTRIP %s", stateName(s));
  if (s == State::Streaming && lastDataAt)
    n = appendf(out, max, n, "  last data %lus ago",
                (unsigned long)((millis() - lastDataAt) / 1000));
  n = appendf(out, max, n, "  %lu B  sessions %lu", (unsigned long)bytesIn,
              (unsigned long)sessions);
  if (dropped) n = appendf(out, max, n, "  dropped %lu B", (unsigned long)dropped);
  if (lastError[0]) n = appendf(out, max, n, "  (%s)", lastError);
  return n;
}

static void setError(const char *e) { strlcpy(lastError, e, sizeof(lastError)); }

// Sleep in small steps so a restart request is acted on promptly.
static bool waitOrRestart(uint32_t ms) {
  for (uint32_t t0 = millis(); millis() - t0 < ms;) {
    if (restart) return true;
    delay(100);
  }
  return false;
}

// Reads one header line, CRLF stripped. False on timeout or disconnect.
static bool readLine(WiFiClient &c, char *line, size_t max, uint32_t timeoutMs) {
  size_t n = 0;
  for (uint32_t t0 = millis(); millis() - t0 < timeoutMs;) {
    if (!c.connected() && !c.available()) return false;
    int ch = c.read();
    if (ch < 0) { delay(10); continue; }
    if (ch == '\n') { line[n] = '\0'; return true; }
    if (ch != '\r' && n < max - 1) line[n++] = (char)ch;
  }
  return false;
}

static bool haveGga() {
  portENTER_CRITICAL(&lock);
  const bool have = gga[0] != '\0';
  portEXIT_CRITICAL(&lock);
  return have;
}

static void sendGga(WiFiClient &c) {
  char s[sizeof(gga)];
  portENTER_CRITICAL(&lock);
  strlcpy(s, gga, sizeof(s));
  portEXIT_CRITICAL(&lock);
  if (!s[0]) return;   // no position yet: a caster rejects an empty GGA
  c.print(s);
  c.print("\r\n");
}

// One session: connect, handshake, stream until something fails. Returns true
// if the caster refused us outright, which must not be retried.
static bool session(WiFiClient &c, const NtripConfig &cfg) {
  state = State::Connecting;
  // Resolved separately so the panel can tell the two failures apart: a name
  // that will not resolve almost always means the hotspot itself has no
  // internet, while a refused connection is the caster's doing.
  IPAddress ip;
  if (!WiFi.hostByName(cfg.host, ip)) {
    setError("DNS failed - does the hotspot have internet?");
    return false;
  }
  if (!c.connect(ip, cfg.port, NTRIP_CONNECT_TIMEOUT_MS)) {
    snprintf(lastError, sizeof(lastError), "no TCP to %s:%u", ip.toString().c_str(), cfg.port);
    return false;
  }

  // NTRIP 1.0: the answer is "ICY 200 OK" followed directly by RTCM. Version
  // 2.0 would bring chunked transfer encoding for no benefit here, and every
  // caster this is meant for still speaks 1.0.
  String auth = base64::encode(String(cfg.user) + ":" + cfg.pass);
  c.printf("GET /%s HTTP/1.0\r\nUser-Agent: NTRIP RTKRover/1.0\r\n"
           "Authorization: Basic %s\r\n\r\n",
           cfg.mount, auth.c_str());

  char line[96];
  if (!readLine(c, line, sizeof(line), NTRIP_RESPONSE_TIMEOUT_MS)) {
    setError("no answer from caster");
    return false;
  }
  if (strstr(line, "SOURCETABLE")) {
    setError("mountpoint not found");
    return true;
  }
  if (strstr(line, " 401") || strstr(line, " 403")) {
    setError("login rejected");
    return true;
  }
  if (!strstr(line, " 200")) {
    snprintf(lastError, sizeof(lastError), "caster: %.36s", line);
    return false;
  }
  // An HTTP-style answer carries headers; ICY does not. Skip to the blank line.
  if (!strncmp(line, "HTTP/", 5)) {
    do {
      if (!readLine(c, line, sizeof(line), NTRIP_RESPONSE_TIMEOUT_MS)) {
        setError("headers cut off");
        return false;
      }
    } while (line[0]);
  }

  sessions++;
  setError("");
  state = State::Streaming;
  lastDataAt = millis();
  sendGga(c);
  uint32_t ggaAt = millis();

  uint8_t buf[512];
  while (!restart && credMode() == CRED_DEVICE && WiFi.status() == WL_CONNECTED) {
    const int n = c.read(buf, sizeof(buf));
    if (n > 0) {
      const size_t sent = xStreamBufferSend(rtcmBuf, buf, n, 0);
      bytesIn += n;
      if (sent < (size_t)n) dropped += n - sent;
      lastDataAt = millis();
    } else if (!c.connected()) {
      setError("caster closed the stream");
      return false;
    } else {
      delay(20);
    }
    if (millis() - ggaAt >= NTRIP_GGA_INTERVAL_MS) {
      sendGga(c);
      ggaAt = millis();
    }
    if (millis() - lastDataAt >= NTRIP_STALL_MS) {
      setError("no data, reconnecting");
      return false;
    }
  }
  return false;
}

// Every hotspot this device may join: the default first, then the optional
// list. Only this task touches it.
static Hotspot known[1 + HOTSPOT_MAX_EXTRA];
static size_t nKnown = 0;
static char joining[33] = "";

static void loadHotspots() {
  nKnown = 0;
  hotspotDefault(&known[0]);
  if (known[0].ssid[0]) nKnown = 1;
  nKnown += hotspotExtras(&known[nKnown], HOTSPOT_MAX_EXTRA);
}

// With several known, look first and join the strongest that is actually
// there, rather than trying each blind. A scan finding none of them (or failing
// mid-connect) falls back to the first, which also covers a hidden hotspot.
static void ensureStation(uint32_t *lastBegin) {
  if (!(WiFi.getMode() & WIFI_STA)) WiFi.mode((wifi_mode_t)(WiFi.getMode() | WIFI_STA));
  if (WiFi.status() == WL_CONNECTED) return;
  if (*lastBegin && millis() - *lastBegin < NTRIP_WIFI_RETRY_MS) return;
  *lastBegin = millis();

  const Hotspot *pick = &known[0];
  if (nKnown > 1) {
    const int n = WiFi.scanNetworks(false);   // blocks this task only
    int best = -1000;
    for (int i = 0; i < n; ++i)
      for (size_t k = 0; k < nKnown; ++k)
        if (WiFi.SSID(i) == known[k].ssid && WiFi.RSSI(i) > best) {
          best = WiFi.RSSI(i);
          pick = &known[k];
        }
    WiFi.scanDelete();
  }
  strlcpy(joining, pick->ssid, sizeof(joining));
  WiFi.begin(pick->ssid, pick->pass);
}

static void task(void *) {
  NtripConfig cfg;
  ntripLoadConfig(&cfg);
  loadHotspots();
  uint32_t backoff = NTRIP_BACKOFF_MIN_MS;
  uint32_t lastBegin = 0;
  uint8_t failures = 0;
  bool refused = false;

  for (;;) {
    if (restart) {
      restart = false;
      ntripLoadConfig(&cfg);
      loadHotspots();
      lastBegin = 0;
      refused = false;
      failures = 0;
      backoff = NTRIP_BACKOFF_MIN_MS;
      setError("");
    }
    if (credMode() != CRED_DEVICE) { state = State::Off; delay(500); continue; }
    if (!ntripConfigured(cfg)) { state = State::Unconfigured; delay(500); continue; }
    if (refused) { state = State::Refused; delay(500); continue; }
    if (failures >= NTRIP_MAX_FAILURES) { state = State::GaveUp; delay(500); continue; }

    if (!nKnown) { state = State::WaitWifi; setError("no hotspot stored"); delay(500); continue; }
    ensureStation(&lastBegin);
    if (WiFi.status() != WL_CONNECTED) { state = State::WaitWifi; delay(250); continue; }

    // A network caster builds the stream for the position it is sent, and
    // sends nothing until it has one. Logging in without a position only
    // produces a session that is dropped, and a run of those looks like abuse.
    if (!haveGga()) { state = State::WaitFix; delay(500); continue; }

    const uint32_t before = bytesIn;
    bool refusal;
    {
      WiFiClient c;
      refusal = session(c, cfg);
      // Closed before anything else happens, so a retry can never overlap the
      // old session - some logins allow only one at a time.
      c.stop();
    }
    if (refusal) { refused = true; continue; }
    if (restart) continue;

    // Only a session that actually delivered corrections resets the count. A
    // login that streams a couple of bytes and drops is still a failure.
    if (bytesIn - before >= NTRIP_GOOD_SESSION_BYTES) {
      failures = 0;
      backoff = NTRIP_BACKOFF_MIN_MS;
    } else {
      failures++;
    }
    state = State::Backoff;
    waitOrRestart(backoff);
    backoff = backoff * 2 > NTRIP_BACKOFF_MAX_MS ? NTRIP_BACKOFF_MAX_MS : backoff * 2;
  }
}

void ntripBegin() {
  rtcmBuf = xStreamBufferCreate(NTRIP_BUFFER_BYTES, 1);
  xTaskCreate(task, "ntrip", 6144, nullptr, 1, nullptr);
}

#else

void ntripLoadConfig(NtripConfig *c) { *c = NtripConfig{}; }
void ntripSaveConfig(const NtripConfig &) {}
bool ntripConfigured(const NtripConfig &) { return false; }
void ntripBegin() {}
void ntripRestart() {}
void ntripSetGga(const char *) {}
size_t ntripRead(uint8_t *, size_t) { return 0; }
size_t ntripStatus(char *out, size_t max) { return snprintf(out, max, "NTRIP not built"); }

#endif
