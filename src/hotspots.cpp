#include "hotspots.h"

#include "config.h"

#if ENABLE_WIFI_PROVISIONING

#include <Preferences.h>
#include <string.h>

// The default keeps the keys it always had, so a device set up before the list
// existed keeps its hotspot.
void hotspotDefault(Hotspot *h) {
  Preferences p;
  p.begin(PROV_NVS_NAMESPACE, true);
  if (!p.getString("ssid", h->ssid, sizeof(h->ssid))) h->ssid[0] = '\0';
  if (!p.getString("pass", h->pass, sizeof(h->pass))) h->pass[0] = '\0';
  p.end();
}

void hotspotSetDefault(const char *ssid, const char *pass) {
  Preferences p;
  p.begin(PROV_NVS_NAMESPACE, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

// Packed into one blob rather than a key per entry: removing from the middle
// is then a rewrite, not a renumbering.
struct Blob {
  uint8_t n;
  Hotspot h[HOTSPOT_MAX_EXTRA];
};

static void load(Blob *b) {
  Preferences p;
  p.begin(PROV_NVS_NAMESPACE, true);
  if (p.getBytes("hs_extra", b, sizeof(*b)) != sizeof(*b) || b->n > HOTSPOT_MAX_EXTRA) b->n = 0;
  p.end();
}

static void store(const Blob &b) {
  Preferences p;
  p.begin(PROV_NVS_NAMESPACE, false);
  p.putBytes("hs_extra", &b, sizeof(b));
  p.end();
}

// About 1 kB each: on the heap rather than a task stack, and never static,
// since the portal and the NTRIP task may both be in here at once.
size_t hotspotExtras(Hotspot *out, size_t max) {
  Blob *b = new Blob;
  load(b);
  size_t n = b->n < max ? b->n : max;
  memcpy(out, b->h, n * sizeof(Hotspot));
  delete b;
  return n;
}

bool hotspotAddExtra(const char *ssid, const char *pass) {
  if (!ssid[0]) return false;
  Blob *b = new Blob;
  load(b);
  size_t i = 0;
  while (i < b->n && strcmp(b->h[i].ssid, ssid)) ++i;
  bool ok = true;
  if (i == b->n) {
    if (b->n >= HOTSPOT_MAX_EXTRA) ok = false;
    else b->n++;
  }
  if (ok) {
    strlcpy(b->h[i].ssid, ssid, sizeof(b->h[i].ssid));
    strlcpy(b->h[i].pass, pass, sizeof(b->h[i].pass));
    store(*b);
  }
  delete b;
  return ok;
}

void hotspotRemoveExtra(size_t index) {
  Blob *b = new Blob;
  load(b);
  if (index < b->n) {
    memmove(&b->h[index], &b->h[index + 1], (b->n - index - 1) * sizeof(Hotspot));
    b->n--;
    store(*b);
  }
  delete b;
}

#else

void hotspotDefault(Hotspot *h) { h->ssid[0] = h->pass[0] = '\0'; }
void hotspotSetDefault(const char *, const char *) {}
size_t hotspotExtras(Hotspot *, size_t) { return 0; }
bool hotspotAddExtra(const char *, const char *) { return false; }
void hotspotRemoveExtra(size_t) {}

#endif
