#include "ble_server.h"

#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <math.h>
#include <string.h>

#include "battery.h"
#include "config.h"

#if DEBUG_USB
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

static char deviceName[32];

static NimBLEServer *server = nullptr;
static NimBLECharacteristic *chBattery = nullptr;
static NimBLECharacteristic *nusTx = nullptr;

// Written from the NimBLE host task and read from the main loop, so atomic,
// not merely volatile.
static std::atomic<bool> rawSubscribed{false};

// Subscriptions belong to a connection, not to the device. Counting them
// globally means one client leaving silences every client that stayed: the
// disconnect handler zeroes the count, and the peer still holding a
// subscription simply stops receiving, freezing on its last values with no
// error anywhere. Track them per connection handle instead.
struct Peer {
  uint16_t handle;
  bool raw;
  bool used;
};
static Peer peers[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];

static Peer *peerFor(uint16_t handle, bool create) {
  for (auto &p : peers)
    if (p.used && p.handle == handle) return &p;
  if (!create) return nullptr;
  for (auto &p : peers)
    if (!p.used) { p = {handle, false, true}; return &p; }
  return nullptr;
}

static std::atomic<uint32_t> notifyFailed{0};
static std::atomic<bool> connected{false};
static std::atomic<uint16_t> notifyMax{20};  // pre-MTU-exchange default (23-3)

static void recount() {
  bool anyRaw = false, anyPeer = false;
  for (auto &p : peers) {
    if (!p.used) continue;
    anyPeer = true;
    if (p.raw) anyRaw = true;
  }
  connected = anyPeer;
  rawSubscribed = anyRaw;
}


static StreamBufferHandle_t downlink = nullptr;
static std::atomic<uint32_t> dlTotal{0};
static std::atomic<uint32_t> dlDropped{0};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    peerFor(info.getConnHandle(), true);
    recount();
    // An RTK rover pushes more than the default 40 ms connection interval
    // carries comfortably.
    s->updateConnParams(info.getConnHandle(), BLE_CONN_MIN_INTERVAL,
                        BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY,
                        BLE_CONN_TIMEOUT);
    s->setDataLen(info.getConnHandle(), 251);
    DBG("[BLE] connected: %s\n", info.getAddress().toString().c_str());

#if ADVERTISE_WHILE_CONNECTED
    // Bench only: lets a second client attach for diagnostics alongside the
    // survey app. Off by default - see the note in config.h.
    NimBLEDevice::startAdvertising();
#endif
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
    // Only this peer's subscriptions go away. Zeroing the shared counters here
    // is what silenced every other client that was still connected.
    if (Peer *p = peerFor(info.getConnHandle(), false)) p->used = false;
    recount();
    // MTU is per connection, so only forget it once the last peer has gone.
    if (!connected) notifyMax = 20;
    DBG("[BLE] disconnected (reason 0x%02x), advertising again\n", reason);
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override {
    uint16_t payload = (mtu > 3) ? (mtu - 3) : 20;
    notifyMax = payload > BLE_NOTIFY_MAX ? BLE_NOTIFY_MAX : payload;
    DBG("[BLE] MTU %u -> notify payload %u\n", mtu, (unsigned)notifyMax.load());
  }
};

// Anything written here goes to the receiver untouched: RTCM3 frames from the
// phone's NTRIP client, and any line-oriented ASCII configuration it sends.
class DownlinkCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() == 0) return;

#if DEBUG_USB
    // First few writes, verbatim. Corrections are binary and start 0xD3; if
    // what arrives is readable text it is not corrections at all - typically a
    // caster's sourcetable or an error page, forwarded by an app that asked
    // for the wrong mountpoint.
    {
      static uint8_t shown = 0;
      if (shown < 3) {
        shown++;
        char pretty[81];
        size_t n = v.length() < 80 ? v.length() : 80;
        for (size_t i = 0; i < n; ++i) {
          uint8_t ch = v.data()[i];
          pretty[i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        pretty[n] = '\0';
        Serial.printf("[dl] %u B, first bytes %02X %02X %02X %02X | %s\n",
                      (unsigned)v.length(), v.data()[0],
                      v.length() > 1 ? v.data()[1] : 0,
                      v.length() > 2 ? v.data()[2] : 0,
                      v.length() > 3 ? v.data()[3] : 0, pretty);
      }
    }
#endif

    dlTotal.fetch_add(v.length());

    // The BLE host task must never block on a full UART FIFO. A short write
    // here means the buffer filled and corrections were truncated, which the
    // receiver would see as corrupt frames - worth counting rather than
    // silently losing.
    size_t sent = xStreamBufferSend(downlink, v.data(), v.length(), 0);
    if (sent < v.length()) dlDropped.fetch_add(v.length() - sent);
  }
};

class RawCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t sub) override {
    if (Peer *p = peerFor(info.getConnHandle(), true)) p->raw = (sub & 0x0001) != 0;
    recount();
    DBG("[BLE] raw NMEA stream %s\n", rawSubscribed ? "on" : "off");
  }
};

// "RTK-Rover-A1B2". Read from efuse rather than from NimBLE because the name
// must be known before init(): the GAP name cannot be changed afterwards.
static void buildDeviceName() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(deviceName, sizeof(deviceName), "%s-%02X%02X", BLE_DEVICE_NAME,
           mac[4], mac[5]);
}

// ---------------------------------------------------------------------------
void bleBegin() {
  downlink = xStreamBufferCreate(BLE_DOWNLINK_BUF, 1);
  buildDeviceName();

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(BLE_TX_POWER_DBM);

#if BLE_ENABLE_BONDING
  // "Just Works" for clients that insist on a bonded device. The
  // characteristics stay unencrypted, so clients that never pair still work.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
#endif

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  // Standard Battery Service: a client gets a battery level with no
  // app-specific support, and phones surface it in their own Bluetooth UI.
  NimBLEService *bat = server->createService(SVC_BATTERY_UUID);
  chBattery = bat->createCharacteristic(
      CH_BATTERY_LEVEL, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t unknown = BATTERY_UNKNOWN;
  chBattery->setValue(&unknown, 1);

  // Nordic UART: NMEA out, corrections in.
  NimBLEService *nus = server->createService(NUS_SERVICE_UUID);
  nusTx = nus->createCharacteristic(NUS_TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  nusTx->setCallbacks(new RawCallbacks());
  NimBLECharacteristic *nusRx = nus->createCharacteristic(
      NUS_RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  nusRx->setCallbacks(new DownlinkCallbacks());  // straight to the UART

  // A 31-byte advertisement cannot hold flags + a 128-bit service UUID + the
  // name (3 + 18 + 16 = 37). If the name is what gets dropped, phone Bluetooth
  // menus hide the device entirely - they list peers by name. So: name in the
  // advertisement, service UUID in the scan response.
  NimBLEAdvertisementData adv;
  adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  adv.setName(deviceName);
  adv.addTxPower();

  // Nordic UART goes in the scan response, because the apps that look for a
  // serial stream filter on it.
  NimBLEAdvertisementData scan;
  scan.addServiceUUID(NUS_SERVICE_UUID);

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(adv);
  advertising->setScanResponseData(scan);
  advertising->enableScanResponse(true);
  advertising->setMinInterval(160);  // ~100 ms, so phone scans pick it up promptly
  advertising->setMaxInterval(240);  // ~150 ms
  advertising->start();

  DBG("[BLE] advertising as \"%s\" (%s)\n", deviceName,
      NimBLEDevice::getAddress().toString().c_str());
}

bool bleRawActive() { return connected && rawSubscribed; }

size_t bleMaxChunk() { return notifyMax.load(); }

// Notifications are not guaranteed: NimBLE refuses one when it has no buffer
// for it. Counted, so that "nothing was lost" can be stated as a fact.
void bleSendRaw(const uint8_t *data, size_t len) {
  if (!bleRawActive() || len == 0) return;
  nusTx->setValue(data, len);
  if (!nusTx->notify()) notifyFailed.fetch_add(1);
}

size_t bleReadDownlink(uint8_t *buf, size_t max) {
  if (xStreamBufferBytesAvailable(downlink) == 0) return 0;
  return xStreamBufferReceive(downlink, buf, max, 0);
}

const char *bleDeviceName() { return deviceName; }

uint32_t bleNotifyFailures() { return notifyFailed.load(); }

void bleDownlinkStats(uint32_t *total, uint32_t *dropped) {
  if (total) *total = dlTotal.load();
  if (dropped) *dropped = dlDropped.load();
}

void blePublishBattery(uint8_t percent) {
  if (!chBattery) return;
  // Always keep the readable value current; notify only subscribers, and only
  // when it has actually moved - a level that crawls 1% an hour does not need
  // a notification every sample.
  static uint8_t lastSent = 0xFE;
  chBattery->setValue(&percent, 1);
  if (percent == BATTERY_UNKNOWN) return;
  if (lastSent == 0xFE ||
      (percent > lastSent ? percent - lastSent : lastSent - percent) >= BATTERY_NOTIFY_STEP) {
    lastSent = percent;
    chBattery->notify();
  }
}
