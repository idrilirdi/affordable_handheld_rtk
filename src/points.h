#pragma once

// Recording points.
//
// A press starts an occupation rather than taking a single epoch:
//
//   settle  POINT_SETTLE_MS     the carrier steadies the pole; nothing is
//                               averaged, but raw logging has already begun
//   record  POINT_OCCUPY_MS     every epoch is averaged (see PointAverager),
//                               and the receiver's raw observations are logged
//
// then the averaged point is appended to /points.csv, and the raw log kept
// beside it as /raw/<point>_<UTC>.rtcm3 for post-processing (PPK) when the
// live corrections were missing or poor. Converted to RINEX .obs/.nav on a PC
// - see tools/rtcm2rinex.sh.
//
// The same button steers the occupation: pressed while settling it cancels,
// pressed while recording it finishes early with what it has. The portal
// offers the same three actions.
//
// The lamps, instead of the status pattern, while a recording runs:
//
//   green and yellow alternating   settling
//   green blip, twice a second     recording, RTK fixed
//   yellow blip, twice a second    recording, not fixed (yet)
//   red blip, twice a second       recording, no position
//
// and when it ends:
//
//   three green flashes   saved, RTK fixed
//   three yellow flashes  saved, but not fixed - check before relying on it
//   one long red          nothing saved

#include <stddef.h>
#include <stdint.h>

#include "nmea.h"
#include "points_format.h"
#include "status_led.h"

// Mounts the flash file system (formatting it on first use) and sets up the
// button.
void pointsBegin();

// Every new solution, as it is parsed.
void pointsOnEpoch(const GnssFix &fix);

// Every RTCM3 frame the receiver sends, whole and CRC-checked.
void pointsOnRtcm(const uint8_t *frame, size_t len);

// Polls the button and runs the occupation. Call from loop().
void pointsTask();

// Whether the receiver should be sending raw observations right now. The
// caller switches it, since it owns the UART.
bool pointsWantRaw();

enum class PointPhase : uint8_t { Idle, Settling, Recording };
PointPhase pointsPhase();
uint32_t pointsPhaseLeftMs();
const char *pointsCurrentName();   // the point being recorded, or ""
unsigned pointsCurrentSamples();   // epochs averaged so far

enum class MarkResult : uint8_t {
  Started, Saved, NotFixed, NoPosition, Canceled, Full, Error
};

// What the button does: start, cancel or finish, depending on the phase.
void pointsPress();

MarkResult pointsStart();
void pointsFinish();               // end the recording now, saving what it has
void pointsCancel();               // end it without saving anything

// How the last occupation ended, in a sentence for the portal; empty before
// the first.
const char *pointsLastOutcome();

uint32_t pointsCount();

// The last recorded line, without its newline; empty if there is none.
const char *pointsLast();

// Deletes every point and every raw log. Names start again from P0001.
// Refused while a recording runs.
bool pointsClear();

// Feeds the whole point file, converted, to `emit` in chunks.
void pointsExport(PointsFormat f, void (*emit)(const char *data, size_t len));

// The raw logs: each one's file name and size, in no particular order.
void pointsRawList(void (*each)(const char *name, size_t bytes, void *ctx), void *ctx);

// Streams one raw log to `emit`. False if `name` is not one of them; with no
// `emit`, only answers that.
bool pointsRawSend(const char *name, void (*emit)(const char *data, size_t len));

// While a recording runs or its outcome is showing, the lamps it wants; false
// otherwise, and the normal status pattern applies.
bool pointsLamps(Lamps *l);
