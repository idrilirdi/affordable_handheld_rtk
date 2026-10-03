#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nmea.h"
#include "textbuf.h"

// A rolling summary of how the RTK engine has been behaving.
//
// A single instant says nothing useful about an RTK engine: what matters in the
// field is how much of the last two minutes it held each solution, how often it
// threw one away, and whether the corrections kept up. Two minutes at 1 Hz is
// long enough to catch a re-convergence cycle and short enough to react to a
// change of sky while you are still standing in it.
//
// Deliberately free of Arduino and of any clock of its own: the caller decides
// when a second has passed, which is also what makes it testable on the host.
#define STAT_WINDOW 120

class FieldStats {
 public:
  // Call about once per second, passing the current millisecond clock.
  // `epochs` is a free-running counter of completed fixes; only its increments
  // are used. The interval is measured rather than assumed: a loop that stalls
  // must show up as a longer window, not as a window that silently lies.
  void sample(const GnssFix &fix, uint32_t epochs, uint32_t nowMs);

  // Human-readable block, newline terminated. Returns bytes written.
  size_t render(char *out, size_t max) const;

  uint16_t count() const { return n_; }

  // Real seconds covered by the window, which is not n_ unless the caller
  // really did manage one sample per second.
  float spanSeconds() const;

 private:
  struct Sample {
    uint8_t type;
    uint8_t epochs;   // epochs completed since the previous sample
    uint16_t dtMs;    // real time since the previous sample, saturating
    float hAcc;       // meters, horizontal; negative when unknown
    float age;        // seconds, correction age; negative when absent
    float east;       // meters from the first fix of the window
    float north;
    bool havePos;
  };
  Sample ring_[STAT_WINDOW];
  uint16_t n_ = 0, head_ = 0;
  uint32_t lastEpochs_ = 0;
  uint32_t lastAt_ = 0;
  bool primed_ = false;
  // Local tangent plane, fixed at the first fix seen. Positions are kept as
  // meters from it so a float carries centimeters instead of being swallowed
  // by the magnitude of a degree of longitude.
  double refLat_ = 0, refLon_ = 0;
  bool haveRef_ = false;
};
