#pragma once

// NTRIP client for the device-side correction mode.
//
// In CRED_DEVICE mode the rover joins the phone's hotspot and fetches its own
// corrections with the account stored on it, so the account stays bound to
// the device no matter who carries it. In CRED_PHONE mode this does nothing
// and the radio stays off.
//
// Runs in its own task: DNS, TCP connect and the caster's handshake all block,
// for seconds on a weak hotspot, and the main loop must never stall on them -
// nothing is published over BLE while it does. The task hands RTCM to the
// main loop through a stream buffer, and the main loop writes it to the
// receiver exactly as it does corrections arriving over BLE.
//
// Casters ban accounts that hammer them, and some logins allow one session at
// a time. So a rejected login or an unknown mountpoint stops the client until
// the settings change - it never retries a refusal - and every other failure
// backs off, up to two minutes between attempts.

#include <stddef.h>
#include <stdint.h>

struct NtripConfig {
  char host[64];
  uint16_t port;
  char mount[48];
  char user[64];
  char pass[64];
};

// Stored on the device, in the portal's NVS namespace. Empty fields when unset.
void ntripLoadConfig(NtripConfig *c);
void ntripSaveConfig(const NtripConfig &c);
bool ntripConfigured(const NtripConfig &c);

// Starts the task. Call once from setup().
void ntripBegin();

// The settings or the mode changed: drop any session, clear a refusal, and
// start again with what is stored now.
void ntripRestart();

// The latest GGA with a position, for the caster. A network solution (VRS)
// synthesizes its base at this position, so it must be current.
void ntripSetGga(const char *sentence);

// RTCM received from the caster, for the receiver. Returns 0 when none waits.
size_t ntripRead(uint8_t *buf, size_t max);

// One line for the debug panel and the console.
size_t ntripStatus(char *out, size_t max);
