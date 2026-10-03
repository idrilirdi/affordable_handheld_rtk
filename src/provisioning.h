#pragma once

#include "nmea.h"

// WiFi credentials, entered once through a captive portal.
//
// The radio is off unless this mode is running. The portal costs about 35 mA
// on this board - measured, over half again on top of the 62 mA the device
// draws without it - and the phone normally fetches the corrections. So the
// portal is entered deliberately, holds the radio up for a bounded time, and
// shuts it down again whether or not anyone used it.
//
// Entered by holding the BOOT button at power-on. Nothing else uses that
// button once the firmware is running, so it needs no extra hardware and
// cannot be triggered by accident in a pocket.

#include <stdbool.h>
#include <stdint.h>

// Call once in setup(), before the radio is needed. Returns true if the portal
// was started, in which case provisioningTask() must be called from loop().
bool provisioningBegin();

// Runs the portal. Does nothing when the portal is not active.
void provisioningTask();

// True while the portal is up.
bool provisioningActive();

// True while the portal is in debug mode, in which the rover keeps surveying
// and the caller must go on running its normal loop.
bool provisioningDebug();

// True only while the portal is holding the device to itself. When it is false
// the caller must keep running its normal loop even if the portal is up.
bool provisioningExclusive();

// Worst observed time, in milliseconds, spent in one named part of the
// caller's loop. Reported verbatim on the debug panel. A stalled loop is not
// cosmetic: nothing is published over BLE while it is stalled, and a client
// that stops hearing positions reports exactly that - lost signal.
#define PROV_PROFILE_SLOTS 8
void provisioningProfile(uint8_t slot, const char *name, uint32_t ms);

// Hand the portal the current state so its debug panel has something to show.
// Only worth calling while provisioningDebug() is true.
void provisioningPublish(const GnssFix &fix, const SkyView &sky, uint32_t fromGnss,
                         uint32_t toGnss, uint32_t epochs);

// Which end of the correction path uses the device's NTRIP account.
//
//   CRED_PHONE  - the phone's mapping app runs NTRIP with the account the
//                 device reports, and sends corrections over BLE. WiFi stays
//                 off: the low-power mode.
//   CRED_DEVICE - the device joins the phone's hotspot and fetches the
//                 corrections itself; the account never leaves it.
//
// Either way the account is bound to the device, not to whoever carries it.
// Chosen on the portal and kept across power cycles.
enum CredMode : uint8_t { CRED_PHONE = 0, CRED_DEVICE = 1 };
CredMode credMode();

// Stored credentials, for a later connection attempt. Empty when unset.
const char *provisionedSsid();
const char *provisionedPass();
