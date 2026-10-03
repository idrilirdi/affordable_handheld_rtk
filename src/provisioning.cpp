#include "provisioning.h"

#include "config.h"

#if ENABLE_WIFI_PROVISIONING

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <math.h>

#include "battery.h"
#include "ble_server.h"
#include "fieldstats.h"
#include "gnss_lc29h.h"
#include "hotspots.h"
#include "ntrip.h"
#include "rtcm.h"

static DNSServer dns;
static WebServer web(80);
static Preferences prefs;
static bool active = false;
static uint32_t startedAt = 0;
static String ssid, pass;
static String status = "";
static bool shuttingDown = false;
static uint32_t shutdownDue = 0;
static bool timeSynced = false;
static bool debug = false;
static CredMode mode = CRED_PHONE;
static char apName[40] = "";

static GnssFix snap;
static uint32_t snapFrom = 0, snapTo = 0, snapEpochs = 0, snapAt = 0;
static SkyView snapSky;
static uint32_t lastSampleAt = 0;
static FieldStats stats;
static uint32_t bootCount = 0;
static uint32_t dlRate = 0, dlLast = 0;

// provisioningPublish() is called once per pass of the main loop, so the gap
// between its own calls is the loop period. Worth reporting: a survey sharing
// the board with an access point must be shown not to be starved, not assumed
// not to be.
static uint32_t loopLastAt = 0, loopCount = 0, loopMaxMs = 0, loopWindowAt = 0;
static uint32_t loopMaxReport = 0;
static float loopRate = 0;
static const char *profName[PROV_PROFILE_SLOTS] = {nullptr};
static uint32_t profMs[PROV_PROFILE_SLOTS] = {0};


// Zero means "shut down now", and debug mode never reaches it: the countdown
// is suspended for as long as someone is diagnosing.
static uint32_t remainingMs() {
  if (debug || PROV_ALWAYS_ON) return PROV_TIMEOUT_MS;
  uint32_t elapsed = millis() - startedAt;
  return elapsed >= PROV_TIMEOUT_MS ? 0 : PROV_TIMEOUT_MS - elapsed;
}

// The page carries its own countdown so the visitor can see the portal is
// temporary, rather than discovering it vanished mid-typing.
static String page(const String &body) {
  uint32_t left = remainingMs() / 1000;
  String h =
      "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:system-ui,sans-serif;margin:0;padding:24px 16px;"
      "background:#111;color:#eee}h1{font-size:1.2rem;margin:0 0 4px}"
      "p{color:#aaa;font-size:.9rem}select,input,button{width:100%;box-sizing:border-box;"
      "padding:12px;margin:6px 0;font-size:1rem;border-radius:8px;border:1px solid #444;"
      "background:#1c1c1c;color:#eee}button{background:#2d6cdf;border:0;font-weight:600}"
      "#t{color:#e8a33d;font-variant-numeric:tabular-nums}</style>"
      "<h1>RTK Rover setup</h1>";
  String f = "";
  if (PROV_ALWAYS_ON && !debug) {
    h += "<p style='color:#e8a33d'>Bench build: WiFi stays on and the portal "
         "reopens at every boot.</p>";
  } else if (debug) {
    h += "<p style='color:#e8a33d;border:1px solid #e8a33d;border-radius:8px;padding:10px'>"
         "<b>Debug mode: WiFi will not switch itself off.</b> This draws about "
         "35 mA on top of the survey - over half again - and costs the cell around a "
         "third of its runtime. "
         "Switch it off, or press Finish, as soon as you are done.</p>";
  } else if (!PROV_ALWAYS_ON) {
    h += "<p>WiFi closes in <span id=t>" + String(left) + "</span>s to save battery.</p>";
    f = "<script>let s=" + String(left) +
        ";setInterval(()=>{if(s>0)s--;document.getElementById('t').textContent=s;},1000);</script>";
  }
  return h + body + f;
}

// What a visitor needs in order to believe it worked: the network it is on,
// the address it was given, the signal, and a clock that could only be right
// if the connection reached the internet.
static String statusBlock() {
  String s = "<div style='background:#1c1c1c;border:1px solid #333;border-radius:8px;"
             "padding:12px;margin:12px 0;font-size:.9rem'>";
  if (WiFi.status() == WL_CONNECTED) {
    s += "<b style='color:#7bd88f'>Connected</b><br>network <b>" + WiFi.SSID() +
         "</b><br>address " + WiFi.localIP().toString() + "<br>signal " +
         String(WiFi.RSSI()) + " dBm";
    if (timeSynced) {
      time_t now = time(nullptr);
      struct tm tm;
      gmtime_r(&now, &tm);
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
      s += "<br>time <b>" + String(buf) + "</b> (from the network)";
    } else {
      s += "<br><span style='color:#e8a33d'>no time from the network yet</span>";
    }
  } else if (ssid.length()) {
    s += "<b style='color:#e06c6c'>Not connected</b><br>last tried <b>" + ssid + "</b>";
  } else {
    String stored = String(provisionedSsid());
    s += stored.length() ? "<b>Stored network:</b> " + stored + " (not connected yet)"
                         : "<b>No network stored yet.</b>";
  }
  s += "</div>";
  return s;
}

// Everything worth knowing in a field with no laptop: what the receiver
// thinks, what the base is sending, who is listening over BLE, and how much
// cell is left. Plain text, because it has to be readable on a phone.
static size_t buildPanel(char *buf, size_t max) {
  size_t n = 0;
  const GnssFix &f = snap;
  const uint32_t age = snapAt ? (millis() - snapAt) / 1000 : 0;

  n = appendf(buf, max, n, "FIX   %s  sats %u  age %lus\n",
                posTypeName(f.type), f.sats, (unsigned long)age);
  n = appendf(buf, max, n, "POS   %.7f %.7f  %.2f m\n", f.lat, f.lon,
                f.altMsl);
  n = appendf(buf, max, n, "ACC   E %.3f  N %.3f  U %.3f m  (%s)\n",
                f.lonStd, f.latStd, f.altStd, f.haveAcc ? "gst" : "none");
  n = appendf(buf, max, n, "CORR  age %.1f s  base %u\n", f.diffAge, f.baseId);
  n += snapSky.render(buf + n, max - n);
  if (n >= max) n = max - 1;
  n = appendf(buf, max, n, "UTC   %04u-%02u-%02u %02u:%02u:%02u\n", f.year,
                f.mon, f.day, f.hour, f.min, f.sec);

  double blat, blon, balt;
  if (rtcmBasePosition(&blat, &blon, &balt)) {
    n = appendf(buf, max, n, "BASE  %.7f %.7f  %.1f m", blat, blon, balt);
    // Baseline length decides what is achievable: a VRS sits on top of you, a
    // real base kilometers away does not, and the difference explains more
    // about a stubborn float than anything else on this page.
    if (f.havePos) {
      const double dN = (f.lat - blat) * 111320.0;
      const double dE = (f.lon - blon) * 111320.0 * cos(blat * M_PI / 180.0);
      n = appendf(buf, max, n, "   baseline %.1f m", sqrt(dN * dN + dE * dE));
    }
    n = appendf(buf, max, n, "\n");
  }
  n = appendf(buf, max, n, "RTCM  ok %lu  bad %lu\n",
              (unsigned long)rtcmFramesOk(), (unsigned long)rtcmFramesBad());
  n = appendf(buf, max, n, "MSGS  ");
  n += rtcmSummary(buf + n, max - n);
  if (n >= max) n = max - 1;
  n = appendf(buf, max, n, "\n");

  uint32_t dlTotal = 0, dlDropped = 0;
  bleDownlinkStats(&dlTotal, &dlDropped);
  n = appendf(buf, max, n,
              "BLE   raw %d  notify-fail %lu  down %lu B (%lu B/s)  %lu drop\n",
              bleRawActive(), (unsigned long)bleNotifyFailures(),
              (unsigned long)dlTotal, (unsigned long)dlRate, (unsigned long)dlDropped);
  if (batteryPresent())
    n = appendf(buf, max, n, "BATT  %u mV  %u%%\n", batteryMillivolts(),
                batteryPercent());
  else
    n = appendf(buf, max, n, "BATT  absent (USB powered)\n");
  n = appendf(buf, max, n, "LOOP  %.1f/s  worst gap %lu ms\n", loopRate,
              (unsigned long)loopMaxReport);
  n = appendf(buf, max, n, "PROF  ");
  for (uint8_t i = 0; i < PROV_PROFILE_SLOTS; ++i)
    if (profName[i])
      n = appendf(buf, max, n, "%s %lu  ", profName[i], (unsigned long)profMs[i]);
  n = appendf(buf, max, n, "ms worst\n");
  n = appendf(buf, max, n,
              "SYS   up %lus  heap %lu (min %lu)  epochs %lu  gnss %lu/%lu B\n",
              (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(),
              (unsigned long)ESP.getMinFreeHeap(), (unsigned long)snapEpochs,
              (unsigned long)snapFrom, (unsigned long)snapTo);
  // A brownout and a clean restart look identical from a distance - both just
  // reset the uptime - so the panel has to say which one happened.
  const char *why;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: why = "power-on"; break;
    case ESP_RST_BROWNOUT: why = "BROWNOUT"; break;
    case ESP_RST_PANIC: why = "PANIC"; break;
    case ESP_RST_INT_WDT: why = "INT WDT"; break;
    case ESP_RST_TASK_WDT: why = "TASK WDT"; break;
    case ESP_RST_WDT: why = "WDT"; break;
    case ESP_RST_SW: why = "software"; break;
    case ESP_RST_DEEPSLEEP: why = "deep sleep"; break;
    case ESP_RST_EXT: why = "external"; break;
    default: why = "unknown"; break;
  }
  n = appendf(buf, max, n, "BOOT  #%lu  last reset: %s\n", (unsigned long)bootCount, why);
  n = appendf(buf, max, n, "GNSS  %s\n",
              gnssAnswering() ? "commands acknowledged"
                              : "NO ANSWER to commands - check D6 -> module RX");
  n = appendf(buf, max, n, "MODE  %s\n",
              mode == CRED_DEVICE ? "device NTRIP" : "phone NTRIP");
  char nt[160];
  ntripStatus(nt, sizeof(nt));
  n = appendf(buf, max, n, "%s\n", nt);
  n += stats.render(buf + n, max - n);
  if (n >= max) n = max - 1;
  char closing[32];
  if (debug || PROV_ALWAYS_ON)
    snprintf(closing, sizeof(closing), "stays on");
  else
    snprintf(closing, sizeof(closing), "closes in %lus", (unsigned long)(remainingMs() / 1000));
  n = appendf(buf, max, n, "WIFI  %s  %d dBm  %s\n",
              WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "ap only",
              WiFi.RSSI(), closing);

  return n;
}

static void handleDebugText() {
  char buf[2000];
  buildPanel(buf, sizeof(buf));
  web.send(200, "text/plain", buf);
}

// The rover's network is somewhere to reach the portal, never a way to the
// internet. Offered as a gateway, it took the internet away from whatever
// joined it: a phone sent its traffic and its lookups here, which cut off the
// mapping app's corrections and, when the phone was also the rover's hotspot,
// the rover's own caster lookups that went out through it.
//
// So the DHCP offer carries no router, and the DNS server answers one name -
// PROV_HOSTNAME - and says "no such host" to everything else, promptly. A
// phone then files the network as local-only and keeps mobile data for the
// rest, and the portal stays up for as long as it is wanted. The price is that
// no page pops up by itself: the address is typed, which is a job for whoever
// administers the device, not for the people who carry it.
static void notAGateway() {
  esp_netif_t *n = WiFi.AP.netif();
  if (!n) return;
  esp_netif_dhcps_stop(n);
  dhcps_offer_t none = 0;
  esp_netif_dhcps_option(n, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &none,
                         sizeof(none));
  // The address handed out as DNS server is the netif's own DNS setting, which
  // the AP leaves at 0.0.0.0 - set it to ourselves or rtk.setup never resolves.
  esp_netif_dns_info_t self = {};
  self.ip.type = ESP_IPADDR_TYPE_V4;
  self.ip.u_addr.ip4.addr = static_cast<uint32_t>(WiFi.softAPIP());
  esp_netif_set_dns_info(n, ESP_NETIF_DNS_MAIN, &self);
  dhcps_offer_t dns = OFFER_DNS;
  esp_netif_dhcps_option(n, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dns, sizeof(dns));
  esp_netif_dhcps_start(n);
}

static void startDns() {
  dns.stop();
  dns.setErrorReplyCode(DNSReplyCode::NonExistentDomain);
  dns.start(53, PROV_HOSTNAME, WiFi.softAPIP());
}

static void setDebug(bool on) {
  debug = on;
  startDns();
  prefs.begin(PROV_NVS_NAMESPACE, false);
  prefs.putBool("debug", on);
  prefs.end();
}

static void handleRoot();

static void handleModeToggle() {
  mode = mode == CRED_DEVICE ? CRED_PHONE : CRED_DEVICE;
  prefs.begin(PROV_NVS_NAMESPACE, false);
  prefs.putUChar("mode", mode);
  prefs.end();
  ntripRestart();
  handleRoot();   // same reason as the debug toggle: no bare redirect
}

static String esc(const char *v) {
  String o;
  for (const char *p = v; *p; ++p) {
    if (*p == '\'') o += "&#39;";
    else if (*p == '<') o += "&lt;";
    else if (*p == '&') o += "&amp;";
    else o += *p;
  }
  return o;
}

// Results are otherwise kept for the life of the portal, so a hotspot switched
// on after the first scan would never appear.
static void handleScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  handleRoot();
}

static void handleCaster() {
  NtripConfig c;
  strlcpy(c.host, web.arg("host").c_str(), sizeof(c.host));
  c.port = (uint16_t)web.arg("port").toInt();
  if (!c.port) c.port = NTRIP_DEFAULT_PORT;
  strlcpy(c.mount, web.arg("mount").c_str(), sizeof(c.mount));
  strlcpy(c.user, web.arg("user").c_str(), sizeof(c.user));
  strlcpy(c.pass, web.arg("cpass").c_str(), sizeof(c.pass));
  ntripSaveConfig(c);
  ntripRestart();
  status = "Caster saved.";
  handleRoot();
}

static void handleDebugToggle() {
  setDebug(!debug);
  // Answer with the page itself. A captive-portal mini browser often will not
  // follow a bare redirect, and then pressing the button appears to do nothing.
  handleRoot();
}

static void handleRoot() {
  String body = statusBlock();
  if (status.length()) body += "<p style='color:#7bd88f'>" + status + "</p>";
  status = "";
  // The scan result, as <option>s, shared by both hotspot forms below.
  String opts;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    opts += "<option value=''>scanning - reload in a few seconds</option>";
  } else if (n <= 0) {
    // An active scan hops every channel and is exactly the kind of 2.4 GHz
    // interference a debug session is trying to observe around, so while
    // debugging it waits for the Rescan button rather than happening by itself.
    if (!debug) {
      WiFi.scanNetworks(true);   // async: the page must not block on a scan
      opts += "<option value=''>scanning - reload in a few seconds</option>";
    } else {
      opts += "<option value=''>not scanned (debug mode) - press Rescan</option>";
    }
  } else {
    // A dual-band router appears once per band; the list is for choosing a
    // network, not a radio, so the first (strongest) entry is the one shown.
    String seen = "\n";
    for (int i = 0; i < n; ++i) {
      const String id = "\n" + WiFi.SSID(i) + "\n";
      if (!WiFi.SSID(i).length() || seen.indexOf(id) >= 0) continue;
      seen += WiFi.SSID(i) + "\n";
      opts += "<option value='" + esc(WiFi.SSID(i).c_str()) + "'>" +
              esc(WiFi.SSID(i).c_str()) + "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  // Typed by hand: a hotspot switched on a moment ago, or one that is hidden.
  const String pick = "<select name=ssid>" + opts +
                      "</select><input name=ssid2 placeholder='or type the network name'>"
                      "<input name=pass type=password placeholder='password'>";

  // The default hotspot: set once, the same on every phone that has been set up.
  body += "<p style='margin:12px 0 0'><b>Default hotspot</b></p><form method=POST "
          "action=/save>" + pick + "<button>Save and connect</button></form>";

  // Anyone who would rather keep their own phone's hotspot adds it here. The
  // device joins whichever known hotspot is in range, the strongest if several.
  Hotspot extra[HOTSPOT_MAX_EXTRA];
  const size_t ne = hotspotExtras(extra, HOTSPOT_MAX_EXTRA);
  body += "<p style='margin:18px 0 0'><b>Other hotspots</b> (optional)</p>";
  for (size_t i = 0; i < ne; ++i) {
    body += "<form method=POST action=/hsdel style='display:flex;gap:8px;align-items:center'>"
            "<span style='flex:1'>" + esc(extra[i].ssid) + "</span>"
            "<input type=hidden name=i value=" + String(i) + ">"
            "<button style='background:#444;width:auto;padding:8px 14px'>Remove</button></form>";
  }
  if (ne < HOTSPOT_MAX_EXTRA)
    body += "<form method=POST action=/hsadd>" + pick + "<button style='background:#333'>"
            "Add this hotspot</button></form>";
  body += "<form method=POST action=/scan><button style='background:#333'>Rescan "
          "networks</button></form>";

  // The mode decides who uses the device's NTRIP account. Said plainly on the
  // page, so nobody is left believing the device is fetching corrections when
  // the phone is supposed to.
  NtripConfig cfg;
  ntripLoadConfig(&cfg);
  body += "<div style='background:#1c1c1c;border:1px solid #333;border-radius:8px;"
          "padding:12px;margin:18px 0 0;font-size:.9rem'><b>Corrections: ";
  if (mode == CRED_DEVICE) {
    char nt[160];
    ntripStatus(nt, sizeof(nt));
    body += "device fetches them</b><br>over the hotspot above, with the account "
            "below. Make sure no phone is using this account.<br><code>" +
            esc(nt) + "</code>";
  } else {
    // The reveal: in this mode the phone's app logs in, so it needs the
    // account in full - password included - to copy into its NTRIP settings.
    body += "phone fetches them</b><br>Enter this account in the mapping app's "
            "NTRIP settings. WiFi stays off.";
    if (ntripConfigured(cfg)) {
      body += "<br>caster <b>" + esc(cfg.host) + ":" + String(cfg.port) +
              "</b><br>mountpoint <b>" + esc(cfg.mount) + "</b><br>user <b>" +
              esc(cfg.user) + "</b><br>password <b>" + esc(cfg.pass) + "</b>";
    }
  }
  body += "</div><form method=POST action=/mode><button style='background:#333'>" +
          String(mode == CRED_DEVICE ? "Switch to: phone fetches corrections"
                                     : "Switch to: device fetches corrections") +
          "</button></form>";
  body += "<form method=POST action=/caster><p style='margin:12px 0 0'>NTRIP account "
          "stored on this device</p>"
          "<input name=host placeholder='caster host' value='" + esc(cfg.host) + "'>"
          "<input name=port inputmode=numeric placeholder='port' value='" +
          String(cfg.port) + "'>"
          "<input name=mount placeholder='mountpoint' value='" + esc(cfg.mount) + "'>"
          "<input name=user placeholder='user' value='" + esc(cfg.user) + "'>"
          "<input name=cpass placeholder='password' value='" + esc(cfg.pass) + "'>"
          "<button>Save account</button></form>";
  body += ""
          "<form method=POST action=/done>"
          "<button style='background:#444;margin-top:18px'>Finish and turn WiFi off</button>"
          "</form>"
          "<form method=POST action=/debug>"
          "<button style='background:#333'>" +
          String(debug ? "Debug on - switch off" : "Debug") + "</button></form>";
  if (debug) {
    // Filled in by the server, then kept live by script if the browser allows
    // it. A captive-portal window may run no script at all, so the numbers have
    // to be in the page as delivered, with a plain link to reload them by hand.
    char panel[2000];
    buildPanel(panel, sizeof(panel));
    body += "<p>The rover keeps surveying while this is open. Your phone keeps "
            "its mobile data too - this network is never its way to the internet, so "
            "this page does not open by itself. Reach it at <b>http://192.168.4.1</b> or "
            "<b>http://" PROV_HOSTNAME "</b>.</p>"
            "<pre id=d style='background:#000;border:1px solid #333;border-radius:8px;"
            "padding:10px;font-size:.75rem;line-height:1.35;overflow-x:auto'>" +
            String(panel) +
            "</pre><p><a style='color:#8ab4f8' href='/'>refresh</a> &middot; "
            "<a style='color:#8ab4f8' href='/d.txt'>raw</a></p>"
            "<script>setInterval(()=>fetch('/d.txt').then(r=>r.text())"
            ".then(t=>document.getElementById('d').textContent=t),1000);</script>";
  }
  web.send(200, "text/html", page(body));
}

static String pickedSsid() {
  return web.arg("ssid2").length() ? web.arg("ssid2") : web.arg("ssid");
}

static void handleHotspotAdd() {
  const String s = pickedSsid();
  if (!s.length()) status = "Choose or type a hotspot first.";
  else if (hotspotAddExtra(s.c_str(), web.arg("pass").c_str())) status = "Added " + s + ".";
  else status = "The list is full - remove one first.";
  ntripRestart();
  handleRoot();
}

static void handleHotspotDel() {
  hotspotRemoveExtra((size_t)web.arg("i").toInt());
  status = "Removed.";
  ntripRestart();
  handleRoot();
}

static void handleSave() {
  ssid = pickedSsid();
  pass = web.arg("pass");
  hotspotSetDefault(ssid.c_str(), pass.c_str());
  ntripRestart();

  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (uint32_t s0 = millis(); millis() - s0 < 6000;) {
      if (time(nullptr) > 1700000000) { timeSynced = true; break; }
      delay(200);
    }
  }

  String body = statusBlock();
  if (WiFi.status() == WL_CONNECTED) {
    body += "<p>Saved. <a style='color:#8ab4f8' href='/'>Back</a></p>"
            "<form method=POST action=/done><button>Finish and turn WiFi off</button></form>";
  } else {
    body += "<p style='color:#e06c6c'>Could not connect - check the password.</p>"
            "<p><a style='color:#8ab4f8' href='/'>try again</a></p>";
  }
  web.send(200, "text/html", page(body));
}

// Closing deliberately rather than waiting out the timeout: answer first, then
// take the radio down a moment later, so the page actually reaches the phone.
static void handleDone() {
  web.send(200, "text/html",
           page("<p style='color:#7bd88f'>WiFi is shutting down. You can close "
                "this page and reconnect your phone to its usual network.</p>"));
  shuttingDown = true;
  shutdownDue = millis() + 1500;
}

static void shutdownRadio() {
  web.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  active = false;
  // In device mode the hotspot connection is how corrections arrive, so the
  // station side stays up; only the access point goes.
  if (mode == CRED_DEVICE) {
    WiFi.mode(WIFI_STA);
    return;
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

bool provisioningBegin() {
#if !PROV_ALWAYS_ON
  pinMode(PROV_BUTTON_PIN, INPUT_PULLUP);
  delay(20);
  if (digitalRead(PROV_BUTTON_PIN) != LOW) {
    // Not wanted: leave the radio off entirely rather than idle.
    WiFi.mode(WIFI_OFF);
    return false;
  }
#endif

  // Whatever the last session was left in. The bench build starts debugging by
  // default; a field build stays quiet until someone asks for it.
  prefs.begin(PROV_NVS_NAMESPACE, false);
  debug = prefs.getBool("debug", PROV_ALWAYS_ON ? true : false);
  mode = prefs.getUChar("mode", CRED_PHONE) == CRED_DEVICE ? CRED_DEVICE : CRED_PHONE;
  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);
  prefs.end();

  WiFi.mode(WIFI_AP_STA);            // AP for the portal, STA to scan and test

  // From efuse, and specifically from the same address BLE names itself after:
  // WiFi.macAddress() is not readable before the driver starts (it returned
  // uninitialized A5A5 here), and the station address differs from the BLE one
  // anyway, which would leave one device answering to two different names.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(apName, sizeof(apName), "%s%02X%02X", PROV_AP_PREFIX, mac[4], mac[5]);

  WiFi.softAP(apName, PROV_AP_PASSWORD);
  notAGateway();
  startDns();
  WiFi.scanNetworks(true);

  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/done", HTTP_POST, handleDone);
  web.on("/debug", HTTP_POST, handleDebugToggle);
  web.on("/mode", HTTP_POST, handleModeToggle);
  web.on("/caster", HTTP_POST, handleCaster);
  web.on("/scan", HTTP_POST, handleScan);
  web.on("/hsadd", HTTP_POST, handleHotspotAdd);
  web.on("/hsdel", HTTP_POST, handleHotspotDel);
  web.on("/d.txt", handleDebugText);
  web.onNotFound(handleRoot);        // whatever the phone probes for, it gets the portal
  web.begin();

  active = true;
  startedAt = millis();
  return true;
}

void provisioningTask() {
  if (!active) return;
  if (shuttingDown && (int32_t)(millis() - shutdownDue) >= 0) { shutdownRadio(); return; }
  if (!debug && !PROV_ALWAYS_ON && remainingMs() == 0) { shutdownRadio(); return; }
  dns.processNextRequest();
  web.handleClient();
}

bool provisioningActive() { return active; }

bool provisioningDebug() { return active && debug; }

CredMode credMode() { return mode; }

bool provisioningExclusive() { return active && !debug && !PROV_ALWAYS_ON; }

void provisioningProfile(uint8_t slot, const char *name, uint32_t ms) {
  if (slot >= PROV_PROFILE_SLOTS) return;
  profName[slot] = name;
  profMs[slot] = ms;
}

void provisioningPublish(const GnssFix &fix, const SkyView &sky, uint32_t fromGnss,
                         uint32_t toGnss, uint32_t epochs) {
  const uint32_t now = millis();
  if (loopLastAt) {
    const uint32_t dt = now - loopLastAt;
    if (dt > loopMaxMs) loopMaxMs = dt;
  }
  loopLastAt = now;
  loopCount++;
  if (now - loopWindowAt >= 1000) {
    loopRate = loopCount * 1000.0f / (now - loopWindowAt);
    loopMaxReport = loopMaxMs;
    loopCount = 0;
    loopMaxMs = 0;
    loopWindowAt = now;
  }

  if (!lastSampleAt || now - lastSampleAt >= 1000) {
    const uint32_t elapsed = lastSampleAt ? now - lastSampleAt : 1000;
    uint32_t dl = 0, drop = 0;
    bleDownlinkStats(&dl, &drop);
    dlRate = (dl - dlLast) * 1000 / (elapsed ? elapsed : 1);
    dlLast = dl;
    lastSampleAt = now;
    stats.sample(fix, epochs, now);
  }
  snap = fix;
  snapSky = sky;
  snapFrom = fromGnss;
  snapTo = toGnss;
  snapEpochs = epochs;
  snapAt = millis();
}

const char *provisionedSsid() {
  static String s;
  prefs.begin(PROV_NVS_NAMESPACE, true);
  s = prefs.getString("ssid", "");
  prefs.end();
  return s.c_str();
}

const char *provisionedPass() {
  static String s;
  prefs.begin(PROV_NVS_NAMESPACE, true);
  s = prefs.getString("pass", "");
  prefs.end();
  return s.c_str();
}

#else

bool provisioningBegin() { return false; }
void provisioningTask() {}
bool provisioningActive() { return false; }
bool provisioningDebug() { return false; }
bool provisioningExclusive() { return false; }
void provisioningPublish(const GnssFix &, const SkyView &, uint32_t, uint32_t, uint32_t) {}
void provisioningProfile(uint8_t, const char *, uint32_t) {}
const char *provisionedSsid() { return ""; }
const char *provisionedPass() { return ""; }
CredMode credMode() { return CRED_PHONE; }

#endif
