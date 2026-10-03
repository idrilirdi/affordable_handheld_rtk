#pragma once

// The hotspots the rover may join in device mode.
//
// One default - a hotspot with the same name and password on every phone
// that has been set up for it - plus an optional list for anyone who would
// rather keep their own phone's hotspot. Units can be shared, so the
// list is per device, not per person: whoever wants their own hotspot adds it
// to the device they are carrying.
//
// Stored in NVS. Each call opens its own handle, so the portal and the NTRIP
// task may both use these without sharing state.

#include <stddef.h>
#include <stdint.h>

#define HOTSPOT_MAX_EXTRA 10

struct Hotspot {
  char ssid[33];
  char pass[65];
};

// The default hotspot, empty ssid when unset.
void hotspotDefault(Hotspot *h);
void hotspotSetDefault(const char *ssid, const char *pass);

// The optional list. Returns how many were written into `out`.
size_t hotspotExtras(Hotspot *out, size_t max);
// Adds or, for a name already listed, replaces. False when the list is full.
bool hotspotAddExtra(const char *ssid, const char *pass);
void hotspotRemoveExtra(size_t index);
