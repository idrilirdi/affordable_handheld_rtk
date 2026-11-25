#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nmea.h"

// What the three lamps say, and when.
//
// Read at a glance, on a chest, in daylight. That rules out anything subtle:
// one lamp at a time, and a pattern slow enough to recognize without counting.
//
// The lamps pulse rather than hold. A steady LED at 8 mA is a tenth of the
// whole device's draw for no benefit - a 60 ms flash every two seconds is
// visible in sunlight and averages under a quarter of a milliamp.

enum class Status : uint8_t {
  NoFix,          // nothing yet, or the receiver has lost it
  Acquiring,      // tracking, no position
  Single,         // position, no corrections
  Diff,           // code differential, usually SBAS
  Float,          // RTK, ambiguities unresolved, accuracy 1 m or better
  FloatPoor,      // float, but the receiver claims worse than 1 m
  FloatSteady,    // float claiming worse than 1 m, yet the position holds still
  Fixed,          // RTK fixed
  NoCorrections,  // had corrections, stopped getting them - the loud one
  Provisioning,   // WiFi portal is up
  LowBattery,
};

struct Lamps {
  bool red;
  bool yellow;
  bool green;
};

// Which lamps are lit at `tMs` into a repeating cycle. Pure, so the pattern can
// be tested on a host without an LED or a receiver.
Lamps statusPattern(Status s, uint32_t tMs);

// What the rover's state amounts to. `correctionAge` is negative when none are
// arriving at all. `floatScatterM` is the measured spread of the float position
// (see scatter.h), negative when there is not enough of it to judge.
Status statusOf(const GnssFix &fix, float correctionAge, uint8_t batteryPercent,
                bool provisioning, float floatScatterM = -1.0f);
