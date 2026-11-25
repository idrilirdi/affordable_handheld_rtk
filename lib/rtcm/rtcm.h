#pragma once

// RTCM3 inspection on the downlink.
//
// The byte counters prove we forwarded what the phone gave us; they say nothing
// about whether those bytes still form valid frames, nor whether the frames
// carry what this receiver can actually use. A stream can be perfectly intact
// and still never produce a fix - if it corrects signals the rover does not
// track, the rover applies what overlaps and floats forever.
//
// So beyond the CRC check, this tallies message types and, for MSM messages,
// decodes the satellite and signal masks. That is what distinguishes "the
// corrections are not arriving" from "the corrections are arriving and are
// useless to this receiver".

#include <stddef.h>
#include <stdint.h>

void rtcmFeed(const uint8_t *data, size_t len);

uint32_t rtcmFramesOk();
uint32_t rtcmFramesBad();

// The reference-station position from message 1005/1006, as geodetic degrees
// and meters. False until one has been seen. Worth checking rather than
// assuming: a network VRS synthesizes its base at the position the client
// sends upstream, so a stale or wrong NMEA position puts the virtual base
// somewhere else entirely - and a long baseline floats no matter how good the
// corrections look.
bool rtcmBasePosition(double *lat, double *lon, double *alt);

// Per-message-type detail, for building a diagnostic sentence. Returns false
// once the index is past the last type seen.
uint8_t rtcmTypeCount();
bool rtcmTypeInfo(uint8_t index, uint16_t *type, uint32_t *count, uint8_t *sats,
                  uint32_t *sigMask);

// One line describing every message type seen, with satellite counts and RTCM
// signal IDs for MSM. Truncated to fit; returns the length written.
size_t rtcmSummary(char *out, size_t n);
