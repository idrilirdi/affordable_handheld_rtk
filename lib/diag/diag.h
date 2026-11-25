#pragma once

// Correction diagnostics as NMEA, so they can be read over the air.
//
// The USB console cannot be used to watch a survey: attaching to the serial
// port resets this board, which restarts the receiver's navigation filter and
// throws away RTK convergence - the measurement is destroyed by the act of
// observing it. These sentences carry the same information out over the link
// the phone is already using, where reading costs nothing.
//
// Proprietary "$P" sentences, so a client that does not know them ignores them
// and one that shows raw NMEA displays them.
//
//   $PRTKA,<age>,<baseId>,<baseline m>,<ok>,<bad>*CS
//   $PRTKM,<type>,<count>,<sats>,<signal IDs...>*CS     one per message type
//
// An empty age field means the receiver is not applying corrections at all.

#include <stddef.h>
#include <stdint.h>

struct GnssFix;

// Builds the next sentence in the cycle, or returns 0 when the cycle is done.
// Call with index 0, 1, 2 ... until it returns 0.
size_t diagSentence(char *out, size_t max, uint8_t index, const GnssFix &fix);
